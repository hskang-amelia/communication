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
// Probes for the shared-engine reentrancy hazard behind mw::com method calls (communication#767).
//
// One SharedResourceEngine serves client replies AND server callbacks on a single callback thread
// (the mw::com Runtime wires proxy and skeleton sides to the same engine). These probes pin down
// two facts an adapter or binding on top of message_passing must design around:
//
//  1. SendWaitReply refuses to run on the callback thread itself (EAGAIN) — the visible guard.
//  2. SendWaitReply issued from ANY OTHER thread while a server callback blocks the callback
//     thread cannot complete until that callback returns: the reply has no thread to arrive on.
//     Offloading the blocking call to a worker therefore bypasses guard (1) and converts the
//     detectable error into a silent hang.
#include <gtest/gtest.h>

#include "score/message_passing/unix_domain/unix_domain_client_factory.h"
#include "score/message_passing/unix_domain/unix_domain_server_factory.h"

#include "score/message_passing/i_server_connection.h"

#include <unistd.h>

#include <atomic>
#include <optional>
#include <cerrno>
#include <chrono>
#include <future>
#include <string>
#include <thread>

namespace score::message_passing
{
namespace
{

using namespace std::chrono_literals;

constexpr std::chrono::seconds kFutureWaitTimeout{5};
/// Long enough that a scheduler hiccup cannot fake the "no progress" observation.
constexpr std::chrono::milliseconds kBlockedHandlerHold{1500};

class ReentrancyProbeFixture : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        service_identifier_ = "reentrancy_probe_" + std::to_string(::getpid());
        protocol_config_ = ServiceProtocolConfig{service_identifier_, 1024U, 1024U, 1024U};
        client_config_ = IClientFactory::ClientConfig{1U, 1U, false, true, false};

        server_factory_.emplace();
        // Same engine for server and both clients: the mw::com Runtime constellation.
        client_factory_.emplace(server_factory_->GetEngine());
    }

    void TearDown() override
    {
        blocked_client_.reset();
        worker_client_.reset();
        if (server_)
        {
            server_->StopListening();
            server_.reset();
        }
    }

    void StartServer(MessageCallback sent_with_reply_callback)
    {
        server_ = server_factory_->Create(protocol_config_, server_config_);
        ASSERT_TRUE(server_);
        auto connect_callback = [](IServerConnection&) -> void* {
            return nullptr;
        };
        auto disconnect_callback = [](IServerConnection&) {};
        auto sent_callback = [](IServerConnection&, score::cpp::span<const std::uint8_t>) -> score::cpp::blank {
            return {};
        };
        ASSERT_TRUE(server_
                        ->StartListening(std::move(connect_callback), std::move(disconnect_callback),
                                         std::move(sent_callback), std::move(sent_with_reply_callback))
                        .has_value());
    }

    score::cpp::pmr::unique_ptr<IClientConnection> StartClient()
    {
        auto client = client_factory_->Create(protocol_config_, client_config_);
        EXPECT_TRUE(client);
        std::promise<void> ready;
        auto ready_future = ready.get_future();
        client->Start(
            [&ready](IClientConnection::State state) {
                if (state == IClientConnection::State::kReady)
                {
                    ready.set_value();
                }
            },
            IClientConnection::NotifyCallback{});
        EXPECT_EQ(ready_future.wait_for(kFutureWaitTimeout), std::future_status::ready);
        return client;
    }

    std::string service_identifier_;
    ServiceProtocolConfig protocol_config_{};
    IClientFactory::ClientConfig client_config_{};
    IServerFactory::ServerConfig server_config_{};

    std::optional<UnixDomainServerFactory> server_factory_;
    std::optional<UnixDomainClientFactory> client_factory_;
    score::cpp::pmr::unique_ptr<IServer> server_;
    score::cpp::pmr::unique_ptr<IClientConnection> blocked_client_;
    score::cpp::pmr::unique_ptr<IClientConnection> worker_client_;
};

/// Shared state for the probes; the server callback captures one pointer to stay within the
/// score::cpp::callback inline capacity.
struct ProbeState
{
    IClientConnection* nested_client{nullptr};
    std::promise<score::os::Error> guard_result;
    std::promise<void> handler_entered;
    std::shared_future<void> handler_release;
};

/// Fact 1: the upstream guard. A SendWaitReply issued on the engine callback thread (here: from
/// inside a server callback) is refused with EAGAIN instead of deadlocking.
TEST_F(ReentrancyProbeFixture, SendWaitReplyOnCallbackThread_IsRefusedWithEagain)
{
    ProbeState state{};
    auto guard_future = state.guard_result.get_future();

    StartServer([&state](IServerConnection& connection,
                         score::cpp::span<const std::uint8_t> message) -> score::cpp::blank {
        // Engine callback thread: try the nested synchronous call, as a naive binding would.
        std::uint8_t reply_buffer[8]{};
        const auto nested = state.nested_client->SendWaitReply(message, reply_buffer);
        if (!nested.has_value())
        {
            state.guard_result.set_value(nested.error());
        }
        score::cpp::ignore = connection.Reply(message);
        return {};
    });

    blocked_client_ = StartClient();
    worker_client_ = StartClient();
    state.nested_client = worker_client_.get();

    const std::uint8_t request[]{0x01U};
    std::uint8_t reply[8]{};
    ASSERT_TRUE(blocked_client_->SendWaitReply(request, reply).has_value());

    ASSERT_EQ(guard_future.wait_for(kFutureWaitTimeout), std::future_status::ready);
    EXPECT_EQ(guard_future.get().GetOsDependentErrorCode(), EAGAIN);
}

/// Fact 2: the hazard a worker offload creates. While a server callback blocks the engine callback
/// thread, a SendWaitReply from a separate worker thread passes the guard but cannot complete —
/// its reply can only be delivered by the blocked thread. It completes immediately once the
/// callback returns. A handler that waits for the worker's result instead of timing out therefore
/// deadlocks permanently, with no error from either side.
TEST_F(ReentrancyProbeFixture, SendWaitReplyFromWorkerWhileCallbackBlocksEngine_MakesNoProgress)
{
    std::promise<void> handler_may_return;
    ProbeState state{};
    state.handler_release = std::shared_future<void>{handler_may_return.get_future()};
    std::atomic<bool> worker_done_while_handler_blocked{false};
    std::atomic<bool> worker_succeeded{false};

    StartServer([&state](IServerConnection& connection,
                         score::cpp::span<const std::uint8_t> message) -> score::cpp::blank {
        if (message.size() == 1U)  // the blocking request; worker requests are 2 bytes
        {
            state.handler_entered.set_value();
            // Stand-in for "handler waits on a Future fulfilled by the worker's nested call".
            // The wait is bounded only so the test can prove the order of events; an unbounded
            // wait here is the permanent deadlock.
            score::cpp::ignore = state.handler_release.wait_for(kBlockedHandlerHold);
        }
        score::cpp::ignore = connection.Reply(message);
        return {};
    });

    blocked_client_ = StartClient();
    worker_client_ = StartClient();

    // Blocked client occupies the engine callback thread via the server callback.
    std::thread blocker{[this] {
        const std::uint8_t request[]{0x01U};
        std::uint8_t reply[8]{};
        score::cpp::ignore = blocked_client_->SendWaitReply(request, reply);
    }};
    ASSERT_EQ(state.handler_entered.get_future().wait_for(kFutureWaitTimeout), std::future_status::ready);

    // Worker thread: nested synchronous call, as the adapter's bounded-worker design would issue.
    // It passes the EAGAIN guard (different thread) and then waits for a reply that cannot arrive.
    std::thread worker{[this, &worker_done_while_handler_blocked, &worker_succeeded, &state] {
        const std::uint8_t request[]{0x02U, 0x02U};
        std::uint8_t reply[8]{};
        const auto result = worker_client_->SendWaitReply(request, reply);
        worker_succeeded = result.has_value();
        // Ready only AFTER the handler was released -> false if we got here while it still held
        // the engine thread.
        worker_done_while_handler_blocked = state.handler_release.wait_for(0ms) != std::future_status::ready;
    }};

    // The deadlock observation: the worker makes no progress while the handler holds the thread.
    std::this_thread::sleep_for(kBlockedHandlerHold / 2);
    EXPECT_FALSE(worker_done_while_handler_blocked);
    EXPECT_FALSE(worker_succeeded);

    // Release the handler; only now can the engine thread deliver the worker's reply.
    handler_may_return.set_value();
    worker.join();
    blocker.join();

    EXPECT_TRUE(worker_succeeded);
    EXPECT_FALSE(worker_done_while_handler_blocked);
}

}  // namespace
}  // namespace score::message_passing
