/*
 * Copyright (c) 2000-2005 The Regents of The University of Michigan
 * Copyright (c) 2008 The Hewlett-Packard Development Company
 * Copyright (c) 2013 Advanced Micro Devices, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "sim/eventq.hh"

#include <cassert>
#include <iostream>
#include <iterator>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "base/logging.hh"
#include "base/trace.hh"
#include "cpu/smt.hh"
#include "debug/Checkpoint.hh"

namespace gem5
{

Tick simQuantum = 0;

//
// Main Event Queues
//
// Events on these queues are processed at the *beginning* of each
// cycle, before the pipeline simulation is performed.
//
uint32_t numMainEventQueues = 0;
std::vector<EventQueue *> mainEventQueue;
__thread EventQueue *_curEventQueue = NULL;
bool inParallelMode = false;

EventQueue *
getEventQueue(uint32_t index)
{
    while (numMainEventQueues <= index) {
        numMainEventQueues++;
        mainEventQueue.push_back(
            new EventQueue(csprintf("MainEventQueue-%d", index)));
    }

    return mainEventQueue[index];
}

#ifndef NDEBUG
Counter Event::instanceCounter = 0;
#endif

Event::~Event()
{
    assert(!scheduled());
    flags = 0;
}

const std::string
Event::name() const
{
    return csprintf("Event_%s", instanceString());
}


void
Event::acquire()
{
    if (flags.isSet(Event::Managed))
        acquireImpl();
}

void
Event::release()
{
    if (flags.isSet(Event::Managed))
        releaseImpl();
}

void
Event::acquireImpl()
{
}

void
Event::releaseImpl()
{
    if (!scheduled())
        delete this;
}

void
EventQueue::insert(Event *event)
{
    const BinKey key(event->when(), event->priority());
    auto &first = pending.first;
    auto &later = pending.later;
    Event **bin;
    if (first) {
        if (*event < *first) {
            later.emplace_hint(later.begin(),
                               BinKey(first->when(), first->priority()),
                               first);
            first = nullptr;
            bin = &first;
        } else if (*event == *first) {
            bin = &first;
        } else {
            bin = &later.try_emplace(key, nullptr).first->second;
        }
    } else if (later.empty() || key < later.begin()->first) {
        bin = &first;
    } else {
        bin = &later.try_emplace(key, nullptr).first->second;
    }
    event->prevInBin = nullptr;
    event->nextInBin = *bin;
    if (*bin)
        (*bin)->prevInBin = event;
    *bin = event;
}

void
EventQueue::remove(Event *event)
{
    assert(event->queue == this);
    Event *next = event->nextInBin;
    if (event->prevInBin) {
        event->prevInBin->nextInBin = next;
    } else if (event == pending.first) {
        pending.first = next;
    } else {
        auto &later = pending.later;
        auto bin = later.begin();
        if (bin != later.end() && bin->second != event)
            bin = later.find(BinKey(event->when(), event->priority()));
        if (bin == later.end() || bin->second != event)
            panic("event not found!");
        if (next)
            bin->second = next;
        else
            later.erase(bin);
    }
    if (next)
        next->prevInBin = event->prevInBin;
    event->nextInBin = nullptr;
    event->prevInBin = nullptr;
}

Event *
EventQueue::serviceOne()
{
    std::lock_guard<EventQueue> lock(*this);
    return serviceOneLocked();
}

Event *
EventQueue::serviceOneLocked()
{
    Event *event = getHead();
    remove(event);
    event->flags.clear(Event::Scheduled);

    // handle action
    if (!event->squashed()) {
        // forward current cycle to the time when this event occurs.
        setCurTick(event->when());
        if (debug::Event)
            event->trace("executed");
        event->process();
        if (event->isExitEvent()) {
            assert(!event->flags.isSet(Event::Managed) ||
                   !event->flags.isSet(Event::IsMainQueue)); // would be silly
            return event;
        }
    } else {
        event->flags.clear(Event::Squashed);
    }

    event->release();

    return NULL;
}

void
Event::serialize(CheckpointOut &cp) const
{
    SERIALIZE_SCALAR(_when);
    SERIALIZE_SCALAR(_priority);
    short _flags = flags;
    SERIALIZE_SCALAR(_flags);
}

void
Event::unserialize(CheckpointIn &cp)
{
    assert(!scheduled());

    UNSERIALIZE_SCALAR(_when);
    UNSERIALIZE_SCALAR(_priority);

    FlagsType _flags;
    UNSERIALIZE_SCALAR(_flags);

    // Old checkpoints had no concept of the Initialized flag
    // so restoring from old checkpoints always fail.
    // Events are initialized on construction but original code
    // "flags = _flags" would just overwrite the initialization.
    // So, read in the checkpoint flags, but then set the Initialized
    // flag on top of it in order to avoid failures.
    assert(initialized());
    flags = _flags;
    flags.set(Initialized);

    // need to see if original event was in a scheduled, unsquashed
    // state, but don't want to restore those flags in the current
    // object itself (since they aren't immediately true)
    if (flags.isSet(Scheduled) && !flags.isSet(Squashed)) {
        flags.clear(Squashed | Scheduled);
    } else {
        DPRINTF(Checkpoint, "Event '%s' need to be scheduled @%d\n",
                name(), _when);
    }
}

void
EventQueue::checkpointReschedule(Event *event)
{
    // It's safe to call insert() directly here since this method
    // should only be called when restoring from a checkpoint (which
    // happens before thread creation).
    if (event->flags.isSet(Event::Scheduled)) {
        // Restore the debug queue identity as well as the index. Unserialized
        // events have their tick and flags restored, but not their queue.
#ifndef NDEBUG
        event->queue = this;
#endif
        insert(event);
    }
}
void
EventQueue::dump() const
{
    cprintf("============================================================\n");
    cprintf("EventQueue Dump  (cycle %d)\n", curTick());
    cprintf("------------------------------------------------------------\n");

    if (empty())
        cprintf("<No Events>\n");
    for (Event *event = pending.first; event; event = event->nextInBin)
        event->dump();
    for (const auto &[key, first] : pending.later)
        for (Event *event = first; event; event = event->nextInBin)
            event->dump();

    cprintf("============================================================\n");
}

bool
EventQueue::debugVerify() const
{
    std::unordered_map<const Event *, bool> seen;
    auto check = [&](const Event *first) {
        const Event *prev = nullptr;
        for (const Event *event = first; event; event = event->nextInBin) {
            if (event->prevInBin != prev || *event != *first ||
                !seen.emplace(event, true).second)
                return false;
            prev = event;
        }
        return true;
    };
    if (!check(pending.first))
        return false;
    for (const auto &[key, first] : pending.later) {
        if (!first || BinKey(first->when(), first->priority()) != key ||
            (pending.first && *pending.first >= *first) || !check(first))
            return false;
    }
    return true;
}

void
EventQueue::swapPendingEvents(PendingEvents &saved)
{
    std::swap(pending.first, saved.first);
    pending.later.swap(saved.later);
}

void
dumpMainQueue()
{
    for (uint32_t i = 0; i < numMainEventQueues; ++i) {
        mainEventQueue[i]->dump();
    }
}


const char *
Event::description() const
{
    return "generic";
}

void
Event::trace(const char *action)
{
    // This is just a default implementation for derived classes where
    // it's not worth doing anything special.  If you want to put a
    // more informative message in the trace, override this method on
    // the particular subclass where you have the information that
    // needs to be printed.
    DPRINTF(Event, "%s %s %s @ %d\n",
            description(), instanceString(), action, when());
}

const std::string
Event::instanceString() const
{
#ifndef NDEBUG
    return csprintf("%d", instance);
#else
    return csprintf("%#x", (uintptr_t)this);
#endif
}

void
Event::dump() const
{
    cprintf("Event %s (%s)\n", name(), description());
    cprintf("Flags: %#x\n", flags);
#ifdef EVENTQ_DEBUG
    cprintf("Created: %d\n", whenCreated);
#endif
    if (scheduled()) {
#ifdef EVENTQ_DEBUG
        cprintf("Scheduled at  %d\n", whenScheduled);
#endif
        cprintf("Scheduled for %d, priority %d\n", when(), _priority);
    } else {
        cprintf("Not Scheduled\n");
    }
}

EventQueue::EventQueue(const std::string &n)
    : objName(n), _curTick(0)
{
}

void
EventQueue::asyncInsert(Event *event)
{
    async_queue_mutex.lock();
    async_queue.push_back(event);
    async_queue_mutex.unlock();
}

void
EventQueue::handleAsyncInsertions()
{
    assert(this == curEventQueue());
    async_queue_mutex.lock();

    while (!async_queue.empty()) {
        insert(async_queue.front());
        async_queue.pop_front();
    }

    async_queue_mutex.unlock();
}

} // namespace gem5
