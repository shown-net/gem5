/*
 * Copyright (c) 2026
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

#include <gtest/gtest.h>

#include <vector>

#include "sim/eventq.hh"

namespace gem5
{
namespace
{

class RecordingEvent : public Event
{
  public:
    RecordingEvent(int id, std::vector<int> &order, Priority priority = 0)
        : Event(priority), id(id), order(order)
    {}

    void process() override { order.push_back(id); }

  private:
    int id;
    std::vector<int> &order;
};

class EventQueueTest : public ::testing::Test
{
  protected:
    EventQueue queue{"event queue test"};

    void SetUp() override { curEventQueue(&queue); }
    void TearDown() override
    {
        inParallelMode = false;
        EXPECT_TRUE(queue.empty());
        curEventQueue(nullptr);
    }
};

TEST_F(EventQueueTest, OrderCancellationAndReschedule)
{
    std::vector<int> order;
    RecordingEvent first(1, order), middle(2, order), last(3, order);
    RecordingEvent earlier(4, order), priority(5, order, -1), later(6, order);

    queue.schedule(&first, 10);
    queue.schedule(&middle, 10);
    queue.schedule(&last, 10);
    queue.schedule(&earlier, 5);
    queue.schedule(&priority, 10);
    queue.schedule(&later, 20);
    queue.deschedule(&middle); // Remove the interior of a same-key bin.
    queue.reschedule(&first, 10); // Rescheduling puts it at the top.
    ASSERT_TRUE(queue.debugVerify());
    queue.serviceEvents(20);
    EXPECT_EQ(order, (std::vector<int>{4, 5, 1, 3, 6}));
    EXPECT_TRUE(queue.debugVerify());
}

TEST_F(EventQueueTest, DetachedQueueCanBeRestored)
{
    std::vector<int> order;
    RecordingEvent one(1, order), two(2, order), three(3, order);
    RecordingEvent temporary(4, order), inserted(5, order);

    queue.schedule(&one, 10);
    queue.schedule(&two, 10);
    queue.schedule(&three, 20);
    EventQueue::PendingEvents saved;
    queue.swapPendingEvents(saved);
    EXPECT_TRUE(queue.empty());
    queue.schedule(&temporary, 5);
    queue.serviceEvents(5);
    EXPECT_TRUE(queue.empty());
    queue.swapPendingEvents(saved);
    queue.setCurTick(0); // Ruby restores the original clock after warmup.
    queue.deschedule(&one);
    queue.schedule(&inserted, 10);
    ASSERT_TRUE(queue.debugVerify());
    queue.serviceEvents(20);
    EXPECT_EQ(order, (std::vector<int>{4, 5, 2, 3}));
}

TEST_F(EventQueueTest, RemovingAndMovingNonHeadBins)
{
    std::vector<int> order;
    RecordingEvent first(1, order), old_top(2, order), top(3, order);
    RecordingEvent removed_bin(4, order), last(5, order);
    queue.schedule(&first, 1);
    queue.schedule(&old_top, 2);
    queue.schedule(&top, 2);
    queue.schedule(&removed_bin, 3);
    queue.schedule(&last, 4);
    queue.deschedule(&top); // A non-head bin retains an older event.
    queue.deschedule(&removed_bin); // Remove a whole non-head bin.
    queue.reschedule(&old_top, 4); // Move between existing bins.
    queue.reschedule(&first, 5); // The former head moves after all others.
    ASSERT_TRUE(queue.debugVerify());
    queue.serviceEvents(5);
    EXPECT_EQ(order, (std::vector<int>{2, 5, 1}));
}

TEST_F(EventQueueTest, AsyncMergePreservesSameKeyOrder)
{
    std::vector<int> order;
    RecordingEvent local(1, order), async_one(2, order), async_two(3, order);
    queue.schedule(&local, 10);
    inParallelMode = true;
    queue.schedule(&async_one, 10, true);
    queue.schedule(&async_two, 10, true);
    queue.handleAsyncInsertions();
    ASSERT_TRUE(queue.debugVerify());
    queue.serviceEvents(10);
    EXPECT_EQ(order, (std::vector<int>{3, 2, 1}));
}

TEST_F(EventQueueTest, CallbackInsertsAheadOfRemainingEvents)
{
    std::vector<int> order;
    RecordingEvent first(1, order), inserted(2, order), later(3, order);
    EventFunctionWrapper callback([&] { queue.schedule(&inserted, 10); },
                                  "insert another event");

    queue.schedule(&first, 10);
    queue.schedule(&later, 20);
    queue.schedule(&callback, 10);
    queue.serviceEvents(20);
    EXPECT_EQ(order, (std::vector<int>{2, 1, 3}));
}

} // anonymous namespace
} // namespace gem5
