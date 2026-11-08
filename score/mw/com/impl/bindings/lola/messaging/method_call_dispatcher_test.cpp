/********************************************************************************
 * Copyright (c) 2026 Contributors to the Eclipse Foundation
 *
 * See the NOTICE file(s) distributed with this work for additional
 * information regarding copyright ownership.
 *
 * This program and the accompanying materials are made available under the
 * terms of the Apache License Version 2.0 which is available at
 * https://www.apache.org/licenses/LICENSE-2.0
 *
 * SPDX-License-Identifier: Apache-2.0
 ********************************************************************************/
#include "score/mw/com/impl/bindings/lola/messaging/method_call_dispatcher.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <vector>

namespace score::mw::com::impl::lola
{
namespace
{

using namespace std::chrono_literals;
using Job = MethodCallDispatcher::Job;

TEST(MethodCallDispatcherTest, RunsJobsInOrderOnOneThread)
{
    std::mutex mutex{};
    std::vector<int> order{};
    std::promise<void> done{};
    MethodCallDispatcher dispatcher{1U, 1U, 8U};
    for (int i = 0; i < 5; ++i)
    {
        Job job{[&, i](bool run) {
            ASSERT_TRUE(run);
            std::lock_guard<std::mutex> lock{mutex};
            order.push_back(i);
            if (i == 4)
            {
                done.set_value();
            }
        }};
        ASSERT_TRUE(dispatcher.Submit(job));
    }
    ASSERT_EQ(done.get_future().wait_for(5s), std::future_status::ready);
    EXPECT_EQ(order, (std::vector<int>{0, 1, 2, 3, 4}));
}

TEST(MethodCallDispatcherTest, SubmitFailsWhenQueueIsFullAndLeavesJobWithCaller)
{
    std::promise<void> release{};
    auto released = release.get_future().share();
    std::promise<void> started{};
    MethodCallDispatcher dispatcher{1U, 1U, 1U};
    Job blocker{[&](bool) {
        started.set_value();
        released.wait();
    }};
    ASSERT_TRUE(dispatcher.Submit(blocker));
    started.get_future().wait();

    Job queued{[](bool) {}};
    ASSERT_TRUE(dispatcher.Submit(queued));

    bool rejected_ran{false};
    Job rejected{[&](bool run) {
        rejected_ran = !run;
    }};
    EXPECT_FALSE(dispatcher.Submit(rejected));
    rejected(false);
    EXPECT_TRUE(rejected_ran);
    release.set_value();
}

TEST(MethodCallDispatcherTest, StopDropsQueuedJobsUnrun)
{
    std::atomic<int> ran{0};
    std::promise<void> release{};
    auto released = release.get_future().share();
    {
        MethodCallDispatcher dispatcher{1U, 1U, 4U};
        std::promise<void> started{};
        Job blocker{[&](bool) {
            started.set_value();
            released.wait();
        }};
        ASSERT_TRUE(dispatcher.Submit(blocker));
        started.get_future().wait();
        Job queued{[&](bool) {
            ++ran;
        }};
        ASSERT_TRUE(dispatcher.Submit(queued));
        std::thread releaser{[&release]() {
            std::this_thread::sleep_for(50ms);
            release.set_value();
        }};
        dispatcher.Stop();
        releaser.join();
        Job late{[](bool) {}};
        EXPECT_FALSE(dispatcher.Submit(late));
    }
    EXPECT_EQ(ran.load(), 0);
}

TEST(MethodCallDispatcherTest, BlockedJobLendsItsSlotToASpareThread)
{
    std::promise<void> inner_done{};
    auto inner = inner_done.get_future().share();
    std::promise<bool> outer_result{};
    MethodCallDispatcher dispatcher{1U, 2U, 4U};
    Job outer{[&](bool) {
        EXPECT_EQ(MethodCallDispatcher::Current(), &dispatcher);
        const BlockingCallScope blocking{};
        ASSERT_TRUE(blocking.Allowed());
        Job nested{[&](bool) {
            inner_done.set_value();
        }};
        ASSERT_TRUE(dispatcher.Submit(nested));
        outer_result.set_value(inner.wait_for(5s) == std::future_status::ready);
    }};
    ASSERT_TRUE(dispatcher.Submit(outer));
    auto result = outer_result.get_future();
    ASSERT_EQ(result.wait_for(10s), std::future_status::ready);
    EXPECT_TRUE(result.get());
}

TEST(MethodCallDispatcherTest, WithoutSpareThreadBlockingIsRefused)
{
    std::promise<bool> allowed{};
    MethodCallDispatcher dispatcher{1U, 1U, 4U};
    Job job{[&](bool) {
        const BlockingCallScope blocking{};
        allowed.set_value(blocking.Allowed());
    }};
    ASSERT_TRUE(dispatcher.Submit(job));
    EXPECT_FALSE(allowed.get_future().get());
}

TEST(MethodCallDispatcherTest, ScopeOutsideDispatcherIsAllowed)
{
    EXPECT_EQ(MethodCallDispatcher::Current(), nullptr);
    const BlockingCallScope blocking{};
    EXPECT_TRUE(blocking.Allowed());
}

TEST(MethodCallDispatcherTest, BaseThreadsLimitConcurrency)
{
    std::atomic<int> active{0};
    std::atomic<int> peak{0};
    std::atomic<int> finished{0};
    MethodCallDispatcher dispatcher{2U, 4U, 8U};
    for (int i = 0; i < 6; ++i)
    {
        Job job{[&](bool) {
            const int now = ++active;
            int seen = peak.load();
            while ((now > seen) && !peak.compare_exchange_weak(seen, now))
            {
            }
            std::this_thread::sleep_for(20ms);
            --active;
            ++finished;
        }};
        ASSERT_TRUE(dispatcher.Submit(job));
    }
    for (int i = 0; (i < 500) && (finished.load() < 6); ++i)
    {
        std::this_thread::sleep_for(10ms);
    }
    EXPECT_EQ(finished.load(), 6);
    EXPECT_EQ(peak.load(), 2);
}

}  // namespace
}  // namespace score::mw::com::impl::lola
