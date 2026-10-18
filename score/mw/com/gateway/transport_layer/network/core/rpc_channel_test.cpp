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
#include "score/mw/com/gateway/transport_layer/network/core/rpc_channel.h"

#include "score/mw/com/gateway/transport_layer/network/core/loopback_bus.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace score::mw::com::gateway::network
{
namespace
{

using namespace std::chrono_literals;

FrameKind KindOfFrame(const std::uint8_t* data, std::size_t size)
{
    return (size >= 4U) ? static_cast<FrameKind>(data[3]) : FrameKind::kAck;
}

/// Records dispatched frames and lets tests wait for a number of them.
class Recorder
{
  public:
    void operator()(const Frame& frame)
    {
        std::lock_guard<std::mutex> lock{mutex_};
        frames_.push_back(frame);
        cv_.notify_all();
    }

    bool WaitFor(std::size_t count, std::chrono::milliseconds timeout = 2000ms)
    {
        std::unique_lock<std::mutex> lock{mutex_};
        return cv_.wait_for(lock, timeout, [this, count]() {
            return frames_.size() >= count;
        });
    }

    std::vector<Frame> Frames()
    {
        std::lock_guard<std::mutex> lock{mutex_};
        return frames_;
    }

  private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<Frame> frames_;
};

RpcChannelConfig FastConfig()
{
    RpcChannelConfig config{};
    config.ack_timeout = 50ms;
    config.max_attempts = 5U;
    return config;
}

class RpcChannelTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        auto pair = LoopbackBus::CreatePair();
        bus_a_ = std::move(pair.first);
        bus_b_ = std::move(pair.second);
        a_ = std::make_unique<RpcChannel>(*bus_a_, FastConfig(), 1U);
        b_ = std::make_unique<RpcChannel>(*bus_b_, FastConfig(), 2U);
    }

    void TearDown() override
    {
        a_.reset();
        b_.reset();
    }

    void StartBoth()
    {
        ASSERT_EQ(a_->Start([this](const Frame& f) {
            recorder_a_(f);
        }),
                  ChannelStatus::kOk);
        ASSERT_EQ(b_->Start([this](const Frame& f) {
            recorder_b_(f);
        }),
                  ChannelStatus::kOk);
    }

    std::unique_ptr<LoopbackBus> bus_a_;
    std::unique_ptr<LoopbackBus> bus_b_;
    std::unique_ptr<RpcChannel> a_;
    std::unique_ptr<RpcChannel> b_;
    Recorder recorder_a_;
    Recorder recorder_b_;
};

TEST_F(RpcChannelTest, SendBeforeStartFails)
{
    EXPECT_EQ(a_->SendRequest(OfferService{"x"}), ChannelStatus::kNotStarted);
    EXPECT_EQ(a_->SendNotification(UpdateNotification{}), ChannelStatus::kNotStarted);
    EXPECT_EQ(a_->SendEventSample("i", "e", nullptr, 0U), ChannelStatus::kNotStarted);
}

TEST_F(RpcChannelTest, RequestIsAckedAndDispatchedOnce)
{
    StartBoth();
    EXPECT_EQ(a_->SendRequest(ProvideService{"svc", {{"e", 4U, 4U}}}), ChannelStatus::kOk);
    ASSERT_TRUE(recorder_b_.WaitFor(1U));
    const auto frames = recorder_b_.Frames();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(std::get<ProvideService>(frames[0].body).instance, "svc");
    EXPECT_EQ(frames[0].header.session, 1U);
    EXPECT_EQ(a_->Statistics().retries, 0U);
    // Acks are consumed by the channel, never dispatched.
    std::this_thread::sleep_for(50ms);
    EXPECT_TRUE(recorder_a_.Frames().empty());
}

TEST_F(RpcChannelTest, NotificationsAndSamplesKeepOrder)
{
    StartBoth();
    constexpr std::uint8_t kCount{100U};
    for (std::uint8_t i = 0U; i < kCount; ++i)
    {
        const std::uint8_t value = i;
        ASSERT_EQ(a_->SendEventSample("svc", "evt", &value, 1U), ChannelStatus::kOk);
    }
    ASSERT_TRUE(recorder_b_.WaitFor(kCount));
    const auto frames = recorder_b_.Frames();
    for (std::uint8_t i = 0U; i < kCount; ++i)
    {
        const auto& sample = std::get<EventSample>(frames[i].body);
        ASSERT_EQ(sample.payload.size(), 1U);
        EXPECT_EQ(sample.payload[0], i);
    }
}

TEST_F(RpcChannelTest, LostRequestIsResent)
{
    std::atomic<int> requests_seen{0};
    bus_a_->SetFaultHook([&requests_seen](const std::uint8_t* data, std::size_t size) {
        if (KindOfFrame(data, size) == FrameKind::kOfferService && requests_seen.fetch_add(1) == 0)
        {
            return LoopbackBus::Fault::kDrop;
        }
        return LoopbackBus::Fault::kDeliver;
    });
    StartBoth();
    EXPECT_EQ(a_->SendRequest(OfferService{"svc"}), ChannelStatus::kOk);
    ASSERT_TRUE(recorder_b_.WaitFor(1U));
    EXPECT_EQ(a_->Statistics().retries, 1U);
    std::this_thread::sleep_for(100ms);
    EXPECT_EQ(recorder_b_.Frames().size(), 1U);
}

TEST_F(RpcChannelTest, LostAckCausesResendButSingleDispatch)
{
    std::atomic<int> acks_seen{0};
    bus_b_->SetFaultHook([&acks_seen](const std::uint8_t* data, std::size_t size) {
        if (KindOfFrame(data, size) == FrameKind::kAck && acks_seen.fetch_add(1) == 0)
        {
            return LoopbackBus::Fault::kDrop;
        }
        return LoopbackBus::Fault::kDeliver;
    });
    StartBoth();
    EXPECT_EQ(a_->SendRequest(StopOfferService{"svc"}), ChannelStatus::kOk);
    std::this_thread::sleep_for(100ms);
    EXPECT_EQ(recorder_b_.Frames().size(), 1U);
    EXPECT_EQ(b_->Statistics().duplicates_dropped, 1U);
    EXPECT_EQ(a_->Statistics().retries, 1U);
}

TEST_F(RpcChannelTest, DuplicatedDeliveryDispatchedOnce)
{
    bus_a_->SetFaultHook([](const std::uint8_t*, std::size_t) {
        return LoopbackBus::Fault::kDuplicate;
    });
    StartBoth();
    EXPECT_EQ(a_->SendRequest(RegisterNotification{{"svc", WireElementKind::kEvent, "e"}}), ChannelStatus::kOk);
    std::this_thread::sleep_for(100ms);
    EXPECT_EQ(recorder_b_.Frames().size(), 1U);
    EXPECT_GE(b_->Statistics().duplicates_dropped, 1U);
}

TEST_F(RpcChannelTest, TimesOutWhenPeerIsAbsent)
{
    ASSERT_EQ(a_->Start([](const Frame&) {}), ChannelStatus::kOk);
    const auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(a_->SendRequest(OfferService{"svc"}), ChannelStatus::kTimeout);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_GE(elapsed, 5 * 50ms);
    EXPECT_EQ(a_->Statistics().retries, 4U);
}

TEST_F(RpcChannelTest, PeerStartingLateIsReachedByRetries)
{
    ASSERT_EQ(a_->Start([](const Frame&) {}), ChannelStatus::kOk);
    auto result = std::async(std::launch::async, [this]() {
        return a_->SendRequest(OfferService{"svc"});
    });
    std::this_thread::sleep_for(120ms);
    ASSERT_EQ(b_->Start([this](const Frame& f) {
        recorder_b_(f);
    }),
              ChannelStatus::kOk);
    EXPECT_EQ(result.get(), ChannelStatus::kOk);
    EXPECT_TRUE(recorder_b_.WaitFor(1U));
}

TEST_F(RpcChannelTest, RequestFromDispatcherWhileCallerWaitsDoesNotDeadlock)
{
    // Destination gateway behaviour: while handling ProvideService it registers update notifications back at the
    // source, which is still blocked in its own SendRequest(ProvideService).
    ASSERT_EQ(a_->Start([this](const Frame& f) {
        recorder_a_(f);
    }),
              ChannelStatus::kOk);
    std::promise<ChannelStatus> nested_result{};
    ASSERT_EQ(b_->Start([this, &nested_result](const Frame& f) {
        if (std::holds_alternative<ProvideService>(f.body))
        {
            nested_result.set_value(b_->SendRequest(RegisterNotification{{"svc", WireElementKind::kEvent, "e"}}));
        }
    }),
              ChannelStatus::kOk);

    EXPECT_EQ(a_->SendRequest(ProvideService{"svc", {}}), ChannelStatus::kOk);
    auto future = nested_result.get_future();
    ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(future.get(), ChannelStatus::kOk);
    ASSERT_TRUE(recorder_a_.WaitFor(1U));
    EXPECT_TRUE(std::holds_alternative<RegisterNotification>(recorder_a_.Frames()[0].body));
}

TEST_F(RpcChannelTest, StopUnblocksPendingSender)
{
    RpcChannelConfig slow{};
    slow.ack_timeout = 5s;
    slow.max_attempts = 1U;
    auto channel = std::make_unique<RpcChannel>(*bus_a_, slow, 3U);
    ASSERT_EQ(channel->Start([](const Frame&) {}), ChannelStatus::kOk);
    auto result = std::async(std::launch::async, [&channel]() {
        return channel->SendRequest(OfferService{"svc"});
    });
    std::this_thread::sleep_for(50ms);
    const auto start = std::chrono::steady_clock::now();
    channel->Stop();
    EXPECT_EQ(result.get(), ChannelStatus::kNotStarted);
    EXPECT_LT(std::chrono::steady_clock::now() - start, 1s);
}

TEST_F(RpcChannelTest, RestartedPeerWithNewSessionIsNotDeduplicated)
{
    StartBoth();
    EXPECT_EQ(a_->SendRequest(OfferService{"first"}), ChannelStatus::kOk);
    ASSERT_TRUE(recorder_b_.WaitFor(1U));

    // Simulate a restart of gateway A: new channel, new session, sequence starts at 1 again.
    a_->Stop();
    a_ = std::make_unique<RpcChannel>(*bus_a_, FastConfig(), 99U);
    ASSERT_EQ(a_->Start([](const Frame&) {}), ChannelStatus::kOk);
    EXPECT_EQ(a_->SendRequest(OfferService{"second"}), ChannelStatus::kOk);
    ASSERT_TRUE(recorder_b_.WaitFor(2U));
    EXPECT_EQ(std::get<OfferService>(recorder_b_.Frames()[1].body).instance, "second");
}

TEST_F(RpcChannelTest, FullDispatchQueueDefersRequestUntilDrained)
{
    RpcChannelConfig small = FastConfig();
    small.max_dispatch_queue = 1U;
    small.max_attempts = 40U;
    b_ = std::make_unique<RpcChannel>(*bus_b_, small, 2U);

    std::mutex gate_mutex;
    std::condition_variable gate_cv;
    bool open_gate = false;
    ASSERT_EQ(a_->Start([](const Frame&) {}), ChannelStatus::kOk);
    ASSERT_EQ(b_->Start([&](const Frame& f) {
        std::unique_lock<std::mutex> lock{gate_mutex};
        gate_cv.wait(lock, [&open_gate]() {
            return open_gate;
        });
        recorder_b_(f);
    }),
              ChannelStatus::kOk);

    // First frame blocks the dispatcher, second fills the queue, third (a request) overflows and is not acked.
    const std::uint8_t byte{1U};
    ASSERT_EQ(a_->SendEventSample("i", "e", &byte, 1U), ChannelStatus::kOk);
    std::this_thread::sleep_for(30ms);
    ASSERT_EQ(a_->SendEventSample("i", "e", &byte, 1U), ChannelStatus::kOk);
    std::this_thread::sleep_for(30ms);
    auto request = std::async(std::launch::async, [this]() {
        return a_->SendRequest(OfferService{"svc"});
    });
    std::this_thread::sleep_for(150ms);
    EXPECT_GE(b_->Statistics().queue_overflows, 1U);
    {
        std::lock_guard<std::mutex> lock{gate_mutex};
        open_gate = true;
    }
    gate_cv.notify_all();
    EXPECT_EQ(request.get(), ChannelStatus::kOk);
    ASSERT_TRUE(recorder_b_.WaitFor(3U));
    EXPECT_TRUE(std::holds_alternative<OfferService>(recorder_b_.Frames()[2].body));
}

TEST_F(RpcChannelTest, ConcurrentSendersAllSucceed)
{
    StartBoth();
    constexpr int kThreads{8};
    constexpr int kPerThread{25};
    std::vector<std::thread> threads{};
    std::atomic<int> failures{0};
    for (int t = 0; t < kThreads; ++t)
    {
        threads.emplace_back([this, t, &failures]() {
            for (int i = 0; i < kPerThread; ++i)
            {
                if (a_->SendRequest(OfferService{std::to_string(t) + "/" + std::to_string(i)}) != ChannelStatus::kOk)
                {
                    failures.fetch_add(1);
                }
            }
        });
    }
    for (auto& thread : threads)
    {
        thread.join();
    }
    EXPECT_EQ(failures.load(), 0);
    EXPECT_TRUE(recorder_b_.WaitFor(static_cast<std::size_t>(kThreads * kPerThread)));
    EXPECT_EQ(recorder_b_.Frames().size(), static_cast<std::size_t>(kThreads * kPerThread));
}

TEST_F(RpcChannelTest, GarbageIsCountedAndIgnored)
{
    StartBoth();
    const std::vector<std::uint8_t> garbage{1U, 2U, 3U};
    ASSERT_EQ(bus_a_->Publish(garbage.data(), garbage.size()), BusStatus::kOk);
    EXPECT_EQ(a_->SendRequest(OfferService{"svc"}), ChannelStatus::kOk);
    EXPECT_EQ(b_->Statistics().decode_failures, 1U);
    ASSERT_TRUE(recorder_b_.WaitFor(1U));
}

TEST_F(RpcChannelTest, PublishFailureFollowedByTimeoutReportsTimeout)
{
    // First send fails on the bus (endpoint closed), later sends succeed but the peer never acks.
    std::atomic<int> sends{0};
    bus_a_->SetFaultHook([&sends](const std::uint8_t*, std::size_t) {
        sends.fetch_add(1);
        return LoopbackBus::Fault::kDrop;
    });
    ASSERT_EQ(a_->Start([](const Frame&) {}), ChannelStatus::kOk);
    EXPECT_EQ(a_->SendRequest(OfferService{"svc"}), ChannelStatus::kTimeout);
    EXPECT_EQ(sends.load(), 5);
}

TEST_F(RpcChannelTest, StopFromDispatcherThenRestartWorks)
{
    std::promise<void> stopped{};
    ASSERT_EQ(a_->Start([](const Frame&) {}), ChannelStatus::kOk);
    ASSERT_EQ(b_->Start([this, &stopped](const Frame&) {
        b_->Stop();  // Stop from inside the dispatcher must not deadlock or terminate.
        stopped.set_value();
    }),
              ChannelStatus::kOk);
    EXPECT_EQ(a_->SendRequest(OfferService{"svc"}), ChannelStatus::kOk);
    ASSERT_EQ(stopped.get_future().wait_for(2s), std::future_status::ready);

    // The channel can be started again afterwards.
    ASSERT_EQ(b_->Start([this](const Frame& f) {
        recorder_b_(f);
    }),
              ChannelStatus::kOk);
    EXPECT_EQ(a_->SendRequest(OfferService{"again"}), ChannelStatus::kOk);
    EXPECT_TRUE(recorder_b_.WaitFor(1U));
}

TEST(RpcChannelSessionTest, GeneratedSessionIdsAreNonZeroAndDistinct)
{
    const auto a = RpcChannel::GenerateSessionId();
    const auto b = RpcChannel::GenerateSessionId();
    EXPECT_NE(a, 0U);
    EXPECT_NE(a, b);
}

}  // namespace
}  // namespace score::mw::com::gateway::network
