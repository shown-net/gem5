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
    auto bin = bins.begin();
    // Most CPU and service events are inserted into or ahead of the first
    // bin. Preserve that constant-time path rather than searching the tree.
    if (bin != bins.end() && key > bin->first)
        bin = bins.lower_bound(key);
    event->prevInBin = nullptr;
    if (bin != bins.end() && bin->first == key) {
        Event *top = bin->second;
        event->nextBin = top->nextBin;
        event->nextInBin = top;
        top->prevInBin = event;
        bin->second = event;
    } else {
        event->nextBin = bin == bins.end() ? nullptr : bin->second;
        event->nextInBin = nullptr;
        bin = bins.emplace_hint(bin, key, event);
    }

    if (bin == bins.begin())
        head = event;
    else
        std::prev(bin)->second->nextBin = event;
}

void
EventQueue::remove(Event *event)
{
    assert(event->queue == this);
    auto bin = event == head ? bins.begin() :
        bins.find(BinKey(event->when(), event->priority()));
    if (bin == bins.end())
        panic("event not found!");
    if (event->prevInBin) {
        event->prevInBin->nextInBin = event->nextInBin;
        if (event->nextInBin)
            event->nextInBin->prevInBin = event->prevInBin;
    } else {
        if (bin->second != event)
            panic("event not found!");
        Event *next = event->nextInBin;
        Event *next_bin = event->nextBin;
        if (next) {
            next->prevInBin = nullptr;
            next->nextBin = next_bin;
            bin->second = next;
        }
        if (bin == bins.begin())
            head = next ? next : next_bin;
        else
            std::prev(bin)->second->nextBin = next ? next : next_bin;
        if (!next)
            bins.erase(bin);
    }
    event->nextBin = nullptr;
    event->nextInBin = nullptr;
    event->prevInBin = nullptr;
}

Event *
EventQueue::serviceOne()
{
    std::lock_guard<EventQueue> lock(*this);
    Event *event = head;
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
    else {
        Event *nextBin = head;
        while (nextBin) {
            Event *nextInBin = nextBin;
            while (nextInBin) {
                nextInBin->dump();
                nextInBin = nextInBin->nextInBin;
            }

            nextBin = nextBin->nextBin;
        }
    }

    cprintf("============================================================\n");
}

bool
EventQueue::debugVerify() const
{
    std::unordered_map<long, bool> map;

    Tick time = 0;
    short priority = 0;
    bool first = true;
    auto indexed = bins.begin();

    Event *nextBin = head;
    while (nextBin) {
        if (indexed == bins.end() || indexed->second != nextBin ||
            indexed->first != BinKey(nextBin->when(), nextBin->priority())) {
            cprintf("event bin index inconsistent!");
            return false;
        }
        Event *prevInBin = nullptr;
        Event *nextInBin = nextBin;
        while (nextInBin) {
            if (nextInBin->prevInBin != prevInBin ||
                *nextInBin != *nextBin) {
                cprintf("event bin links inconsistent!");
                return false;
            }
            if (!first && nextInBin->when() < time) {
                cprintf("time goes backwards!");
                nextInBin->dump();
                return false;
            } else if (!first && nextInBin->when() == time &&
                       nextInBin->priority() < priority) {
                cprintf("priority inverted!");
                nextInBin->dump();
                return false;
            }

            if (map[reinterpret_cast<long>(nextInBin)]) {
                cprintf("Node already seen");
                nextInBin->dump();
                return false;
            }
            map[reinterpret_cast<long>(nextInBin)] = true;

            time = nextInBin->when();
            priority = nextInBin->priority();
            first = false;

            prevInBin = nextInBin;
            nextInBin = nextInBin->nextInBin;
        }

        nextBin = nextBin->nextBin;
        ++indexed;
    }

    return indexed == bins.end();
}

Event*
EventQueue::replaceHead(Event* s)
{
    Event* t = head;
    head = s;
    bins.clear();
    for (Event *bin = head; bin; bin = bin->nextBin) {
        const auto result = bins.emplace(BinKey(bin->when(), bin->priority()),
                                         bin);
        if (!result.second)
            panic("duplicate event queue bin during restore");
        Event *prev = nullptr;
        for (Event *event = bin; event; event = event->nextInBin) {
            event->prevInBin = prev;
            prev = event;
        }
    }
    return t;
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
    : objName(n), head(NULL), _curTick(0)
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
