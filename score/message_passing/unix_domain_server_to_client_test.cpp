/********************************************************************************
 * Copyright (c) 2025 Contributors to the Eclipse Foundation
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
#include <gtest/gtest.h>

#include "score/message_passing/unix_domain/unix_domain_client_factory.h"
#include "score/message_passing/unix_domain/unix_domain_server_factory.h"

#include "score/message_passing/i_server_connection.h"

#include <algorithm>
#include <cerrno>
#include <future>
#include <thread>

namespace score::message_passing
{
namespace
{

using namespace ::testing;

void stdout_handler(const score::cpp::handler_parameters& param)
{
    std::cout << "In " << param.file << ":" << param.line << " " << param.function << " condition " << param.condition
              << " >> " << param.message << std::endl;
}

std::chrono::seconds kFutureWaitTimeout{5};

// param:
// - false: client and server use different engines with different background threads
// - true: client and server use share the engine and the background thread
class ServerToClientTestFixtureUnix : public ::testing::Test, public testing::WithParamInterface<bool>
{
  public:
    void SetUp() override
    {
        score::cpp::set_assertion_handler(stdout_handler);

        std::string test_prefix{"test_prefix_"};
        test_prefix += std::to_string(::getpid()) + "_";
        service_identifier_ = test_prefix + "1";
        protocol_config_ = ServiceProtocolConfig{service_identifier_, 1024, 1024, 1024};
        client_config_ = IClientFactory::ClientConfig{1, 1, false, true, false};

        server_connections_started_ = 0;
        server_connections_finished_ = 0;

        SetupClientPromises();
    }

    void TearDown() override
    {
        client_.reset();
        if (server_)
        {
            server_->StopListening();
            EXPECT_EQ(server_connections_finished_, server_connections_started_);
            server_.reset();
        }
    }

  protected:
    struct Promises
    {
        std::promise<void> ready;
        std::promise<void> stopping;
        std::promise<void> stopped;
    };

    struct Futures
    {
        std::future<void> ready;
        std::future<void> stopping;
        std::future<void> stopped;
    };

    void SetupClientPromises()
    {
        promises_ = Promises{};
        futures_ =
            Futures{promises_.ready.get_future(), promises_.stopping.get_future(), promises_.stopped.get_future()};
    }

    void WhenServerAndClientFactoriesConstructed(bool server_first = true, bool same_engine = true)
    {
        if (server_first)
        {
            server_factory_.emplace();
            if (same_engine)
            {
                client_factory_.emplace(server_factory_->GetEngine());
            }
            else
            {
                client_factory_.emplace();
            }
        }
        else
        {
            client_factory_.emplace();
            if (same_engine)
            {
                server_factory_.emplace(client_factory_->GetEngine());
            }
            else
            {
                server_factory_.emplace();
            }
        }
    }

    void WhenServerCreated()
    {
        server_ = server_factory_->Create(protocol_config_, server_config_);
        ASSERT_TRUE(server_);
    }

    void WhenRefusingServerStartsListening()
    {
        auto connect_callback = [](IServerConnection&) {
            std::cout << "RefusingConnectCallback" << std::endl;
            return score::cpp::make_unexpected(score::os::Error::createUnspecifiedError());
        };
        EXPECT_TRUE(server_->StartListening(connect_callback).has_value());
    }

    void WhenEchoServerStartsListening()
    {
        auto connect_callback = [this](IServerConnection& connection) -> void* {
            std::cout << "EchoConnectCallback " << &connection << std::endl;
            ++server_connections_started_;
            return nullptr;
        };
        auto disconnect_callback = [this](IServerConnection& connection) {
            std::cout << "EchoDisconnectCallback " << &connection << std::endl;
            ++server_connections_finished_;
        };
        auto sent_callback = [](IServerConnection& connection,
                                score::cpp::span<const std::uint8_t> message) -> score::cpp::blank {
            std::cout << "EchoSentCallback " << &connection << std::endl;
            connection.Notify(message);
            return {};
        };
        auto sent_with_reply_callback = [](IServerConnection& connection,
                                           score::cpp::span<const std::uint8_t> message) -> score::cpp::blank {
            std::cout << "EchoSentWithReplyCallback " << &connection << std::endl;
            connection.Reply(message);
            return {};
        };
        EXPECT_TRUE(
            server_->StartListening(connect_callback, disconnect_callback, sent_callback, sent_with_reply_callback)
                .has_value());
    }

    void WhenClientStarted(bool delete_on_stop = false)
    {
        delete_on_stop_ = delete_on_stop;
        client_ = client_factory_->Create(protocol_config_, client_config_);
        ASSERT_TRUE(client_);
        const auto state_callback = [this](auto state) {
            std::cout << "StateCallback " << static_cast<std::int32_t>(state) << std::endl;
            if (state == IClientConnection::State::kReady)
            {
                std::cout << "StateCallback Ready " << std::endl;
                promises_.ready.set_value();
            }
            else if (state == IClientConnection::State::kStopping)
            {
                std::cout << "StateCallback Stopping " << static_cast<std::int32_t>(client_->GetStopReason())
                          << std::endl;
                promises_.stopping.set_value();
            }
            else if (state == IClientConnection::State::kStopped)
            {
                std::cout << "StateCallback Stopped " << static_cast<std::int32_t>(client_->GetStopReason())
                          << std::endl;
                if (delete_on_stop_)
                {
                    std::lock_guard<std::mutex> guard(client_mutex_);
                    client_.reset();
                }
                promises_.stopped.set_value();
            }
        };

        std::lock_guard<std::mutex> guard(client_mutex_);
        client_->Start(state_callback, IClientConnection::NotifyCallback{});
    }

    void WhenClientStartedRestartingFromCallback(std::uint32_t retry_count)
    {
        retry_count_ = retry_count;
        client_ = client_factory_->Create(protocol_config_, client_config_);
        ASSERT_TRUE(client_);
        const auto state_callback = [this](auto state) {
            std::cout << "StateCallback " << static_cast<std::int32_t>(state) << std::endl;
            if (state == IClientConnection::State::kReady)
            {
                std::cout << "StateCallback Ready " << std::endl;
            }
            else if (state == IClientConnection::State::kStopping)
            {
                std::cout << "StateCallback Stopping " << static_cast<std::int32_t>(client_->GetStopReason())
                          << std::endl;
            }
            else if (state == IClientConnection::State::kStopped)
            {
                std::cout << "StateCallback Stopped " << static_cast<std::int32_t>(client_->GetStopReason())
                          << std::endl;
                if (retry_count_ > 0)
                {
                    --retry_count_;
                    client_->Restart();
                    return;
                }
                promises_.stopped.set_value();
            }
        };

        std::lock_guard<std::mutex> guard(client_mutex_);
        client_->Start(state_callback, IClientConnection::NotifyCallback{});
    }

    void WaitClientConnected()
    {
        ASSERT_EQ(futures_.ready.wait_for(kFutureWaitTimeout), std::future_status::ready);
    }

    void WaitClientStopping()
    {
        ASSERT_EQ(futures_.stopping.wait_for(kFutureWaitTimeout), std::future_status::ready);
    }

    void WaitClientStoppedExpectStatusStopped()
    {
        ASSERT_EQ(futures_.stopped.wait_for(kFutureWaitTimeout), std::future_status::ready);
        EXPECT_EQ(client_->GetState(), IClientConnection::State::kStopped);
    }

    void WaitClientStoppedExpectClientDeleted()
    {
        ASSERT_EQ(futures_.stopped.wait_for(kFutureWaitTimeout), std::future_status::ready);

        std::lock_guard<std::mutex> guard(client_mutex_);
        EXPECT_EQ(client_.get(), nullptr);
    }

    void WhenClientRestarted()
    {
        ASSERT_EQ(client_->GetState(), IClientConnection::State::kStopped);
        SetupClientPromises();
        client_->Restart();
    }

    void ExpectClientStillConnecting()
    {
        EXPECT_NE(futures_.ready.wait_for(std::chrono::milliseconds(10)), std::future_status::ready);

        std::lock_guard<std::mutex> guard(client_mutex_);
        EXPECT_EQ(client_->GetState(), IClientConnection::State::kStarting);
    }

    void WithStandardEchoServerSetup()
    {
        WhenServerAndClientFactoriesConstructed(false, GetParam());
        WhenClientStarted();

        ExpectClientStillConnecting();

        WhenServerCreated();
        WhenEchoServerStartsListening();

        WaitClientConnected();
    }

    void WhenClientSendsMessageItReceivesEchoReply()
    {
        std::array<std::uint8_t, 6> message = {1, 2, 3, 4, 5, 6};
        std::array<std::uint8_t, 256> reply_buffer = {};
        {
            std::promise<bool> promise;
            auto reply_callback =
                [&promise, &message](
                    score::cpp::expected<score::cpp::span<const std::uint8_t>, score::os::Error> message_expected) {
                    if (!message_expected.has_value())
                    {
                        promise.set_value(false);
                        return;
                    }
                    auto reply_message = message_expected.value();
                    promise.set_value(message.size() == static_cast<std::size_t>(reply_message.size()));
                };
            auto reply_expected = client_->SendWithCallback(message, reply_callback);
            ASSERT_TRUE(reply_expected.has_value());
            auto future = promise.get_future();
            ASSERT_EQ(future.wait_for(kFutureWaitTimeout), std::future_status::ready);
            EXPECT_TRUE(future.get());
        }
        {
            auto reply_expected = client_->SendWaitReply(message, reply_buffer);
            ASSERT_TRUE(reply_expected.has_value());
            auto reply = reply_expected.value();
            EXPECT_EQ(reply_buffer.data(), reply.data());
            EXPECT_EQ(message.size(), reply.size());
        }
    }

    Promises promises_;
    Futures futures_;

    IServerFactory::ServerConfig server_config_{};
    IClientFactory::ClientConfig client_config_{};
    std::optional<UnixDomainServerFactory> server_factory_;
    std::optional<UnixDomainClientFactory> client_factory_;

    score::cpp::pmr::unique_ptr<IServer> server_;
    std::mutex client_mutex_;
    score::cpp::pmr::unique_ptr<IClientConnection> client_;
    std::uint32_t server_connections_started_;
    std::uint32_t server_connections_finished_;

    std::string service_identifier_;
    ServiceProtocolConfig protocol_config_;

    bool delete_on_stop_{false};
    std::uint32_t retry_count_{0};
};

TEST_P(ServerToClientTestFixtureUnix, RefusingServerStartingFirst)
{
    WhenServerAndClientFactoriesConstructed(true, GetParam());
    WhenServerCreated();
    WhenRefusingServerStartsListening();

    WhenClientStarted();

    WaitClientStoppedExpectStatusStopped();
}

TEST_P(ServerToClientTestFixtureUnix, RefusingServerStartingLater)
{
    WhenServerAndClientFactoriesConstructed(false, GetParam());
    WhenClientStarted();

    ExpectClientStillConnecting();

    WhenServerCreated();
    WhenRefusingServerStartsListening();

    WaitClientStoppedExpectStatusStopped();
}

TEST_P(ServerToClientTestFixtureUnix, RefusingServerStartingLaterClientDeleted)
{
    WhenServerAndClientFactoriesConstructed(false, GetParam());
    WhenClientStarted(true);

    ExpectClientStillConnecting();

    WhenServerCreated();
    WhenRefusingServerStartsListening();

    WaitClientStoppedExpectClientDeleted();
}

TEST_P(ServerToClientTestFixtureUnix, RefusingServerStartingLaterClientRestarting)
{
    WhenServerAndClientFactoriesConstructed(false, GetParam());
    WhenClientStartedRestartingFromCallback(3);

    ExpectClientStillConnecting();

    WhenServerCreated();
    WhenRefusingServerStartsListening();

    WaitClientStoppedExpectStatusStopped();
    EXPECT_EQ(retry_count_, 0);
}

TEST_P(ServerToClientTestFixtureUnix, EchoServerStartingLaterForcedStop)
{
    WhenServerAndClientFactoriesConstructed(false, GetParam());
    WhenClientStarted();

    ExpectClientStillConnecting();

    WhenServerCreated();
    WhenEchoServerStartsListening();

    WaitClientConnected();

    client_->Stop();
    WaitClientStopping();
    WaitClientStoppedExpectStatusStopped();
}

TEST_P(ServerToClientTestFixtureUnix, EchoServerSetup)
{
    WithStandardEchoServerSetup();

    WhenClientSendsMessageItReceivesEchoReply();

    client_->Stop();
    WaitClientStoppedExpectStatusStopped();
}

TEST_P(ServerToClientTestFixtureUnix, EchoServerClientRestart)
{
    WithStandardEchoServerSetup();

    WhenClientSendsMessageItReceivesEchoReply();

    client_->Stop();
    WaitClientStoppedExpectStatusStopped();

    WhenClientRestarted();
    WaitClientConnected();

    WhenClientSendsMessageItReceivesEchoReply();

    client_->Stop();
    WaitClientStoppedExpectStatusStopped();
}

INSTANTIATE_TEST_SUITE_P(UnixDomain, ServerToClientTestFixtureUnix, testing::Values(false, true));

// Regression test reproducing the scenario from eclipse-score/communication#767:
//   App1 (offers Foo)                         App2 (offers Bar)
//        | --------- 1. BarMethod() --------->|
//        |                                    | - handling BarMethod
//        | <-------- 2. FooMethod() ----------|
//   - handling FooMethod                      |
//        | --------- 3. FooMethod returns --->|
//        |                                    |
//        | <-------- 4. BarMethod returns ----|
//
// Step 2 is a SendWaitReply() call made from *within* the BarMethod server callback - i.e. from the same thread
// that also has to process step 3's reply. Bar's server and the outgoing connection used for step 2 are put on
// the SAME engine/thread here (mirroring one real process sharing one engine across its client and server
// connections), which is exactly the configuration the bug report is about.
TEST(NestedMethodCallReentrancyTest, BarMethodCallingFooMethodFromItsOwnCallbackSucceeds)
{
    score::cpp::set_assertion_handler(stdout_handler);

    const std::string prefix = "test_nested_" + std::to_string(::getpid()) + "_";
    // ServiceProtocolConfig::identifier is a non-owning string_view, so these need to outlive it.
    const std::string identifier_foo = prefix + "foo";
    const std::string identifier_bar = prefix + "bar";
    const ServiceProtocolConfig protocol_config_foo{identifier_foo, 1024, 1024, 1024};
    const ServiceProtocolConfig protocol_config_bar{identifier_bar, 1024, 1024, 1024};
    const IClientFactory::ClientConfig client_config{1, 1, false, true, false};
    const IServerFactory::ServerConfig server_config{};

    auto connect_callback = [](IServerConnection&) -> score::cpp::expected<UserData, score::os::Error> {
        return nullptr;
    };
    auto disconnect_callback = [](IServerConnection&) {};

    // "App1": offers Foo, on its own engine/thread (a separate process in reality).
    UnixDomainServerFactory app1_server_factory;
    UnixDomainClientFactory app1_client_factory;

    auto foo_server = app1_server_factory.Create(protocol_config_foo, server_config);
    ASSERT_TRUE(foo_server);
    auto foo_sent_with_reply_callback = [](IServerConnection& connection,
                                           score::cpp::span<const std::uint8_t> message) -> score::cpp::blank {
        connection.Reply(message);  // Foo just echoes its request back
        return {};
    };
    ASSERT_TRUE(foo_server->StartListening(connect_callback, disconnect_callback, {}, foo_sent_with_reply_callback)
                    .has_value());

    // "App2": offers Bar. Its server and its own outgoing client connection to Foo share ONE engine/thread.
    UnixDomainServerFactory app2_server_factory;
    UnixDomainClientFactory app2_client_factory(app2_server_factory.GetEngine());

    auto foo_client = app2_client_factory.Create(protocol_config_foo, client_config);
    ASSERT_TRUE(foo_client);
    std::promise<void> foo_client_ready;
    std::promise<void> foo_client_stopped;
    foo_client->Start(
        [&foo_client_ready, &foo_client_stopped](auto state) {
            if (state == IClientConnection::State::kReady)
            {
                foo_client_ready.set_value();
            }
            else if (state == IClientConnection::State::kStopped)
            {
                foo_client_stopped.set_value();
            }
        },
        IClientConnection::NotifyCallback{});
    ASSERT_EQ(foo_client_ready.get_future().wait_for(kFutureWaitTimeout), std::future_status::ready);

    const std::array<std::uint8_t, 6> nested_message = {9, 9, 9, 9, 9, 9};
    auto bar_sent_with_reply_callback = [&foo_client, &nested_message](
                                            IServerConnection& connection,
                                            score::cpp::span<const std::uint8_t> /* message */) -> score::cpp::blank {
        std::array<std::uint8_t, 256> nested_reply_buffer{};
        // The reentrant call this regression test is about: we're running on app2_server_factory's engine
        // thread, which is the very same thread foo_client also uses.
        auto nested_result = foo_client->SendWaitReply(nested_message, nested_reply_buffer);
        if (!nested_result.has_value())
        {
            const std::uint8_t error_marker =
                static_cast<std::uint8_t>(nested_result.error().GetOsDependentErrorCode());
            connection.Reply(score::cpp::span<const std::uint8_t>{&error_marker, 1});
            return {};
        }
        connection.Reply(nested_result.value());
        return {};
    };

    auto bar_server = app2_server_factory.Create(protocol_config_bar, server_config);
    ASSERT_TRUE(bar_server);
    auto bar_listen_result =
        bar_server->StartListening(connect_callback, disconnect_callback, {}, bar_sent_with_reply_callback);
    ASSERT_TRUE(bar_listen_result.has_value()) << "errno " << bar_listen_result.error().GetOsDependentErrorCode();

    auto bar_client = app1_client_factory.Create(protocol_config_bar, client_config);
    ASSERT_TRUE(bar_client);
    std::promise<void> bar_client_ready;
    std::promise<void> bar_client_stopped;
    bar_client->Start(
        [&bar_client_ready, &bar_client_stopped](auto state) {
            if (state == IClientConnection::State::kReady)
            {
                bar_client_ready.set_value();
            }
            else if (state == IClientConnection::State::kStopped)
            {
                bar_client_stopped.set_value();
            }
        },
        IClientConnection::NotifyCallback{});
    ASSERT_EQ(bar_client_ready.get_future().wait_for(kFutureWaitTimeout), std::future_status::ready);

    // App1's own (non-nested) call, step 1 in the diagram above.
    const std::array<std::uint8_t, 6> message = {1, 2, 3, 4, 5, 6};
    std::array<std::uint8_t, 256> reply_buffer{};
    auto result = bar_client->SendWaitReply(message, reply_buffer);
    ASSERT_TRUE(result.has_value()) << "expected the nested FooMethod call from within BarMethod's own callback to "
                                       "succeed instead of failing with EAGAIN (communication#767)";
    auto reply = result.value();
    ASSERT_EQ(static_cast<std::size_t>(reply.size()), nested_message.size());
    EXPECT_TRUE(std::equal(reply.begin(), reply.end(), nested_message.begin()));

    auto bar_client_stopped_future = bar_client_stopped.get_future();
    bar_client->Stop();
    ASSERT_EQ(bar_client_stopped_future.wait_for(kFutureWaitTimeout), std::future_status::ready);

    auto foo_client_stopped_future = foo_client_stopped.get_future();
    foo_client->Stop();
    ASSERT_EQ(foo_client_stopped_future.wait_for(kFutureWaitTimeout), std::future_status::ready);

    bar_server->StopListening();
    foo_server->StopListening();
}

// Probes a narrower case than #767's own example: what happens if the nested call routes back through the
// SAME ClientConnection object that is already waiting for a reply (e.g. a handler that calls the very same
// peer/method again), rather than through an independent connection like the FooMethod one above. Before the
// docs/design-notes.md §2.5 multi-slot prototype, SendWaitReply always queued a second call behind an
// already-busy connection instead of sending it, which for a *nested* call was a genuine deadlock (see §2.3):
// the queued call can only be dispatched once the first one's waiting_for_reply_ is released, which only happens
// once the *handler* replies - but the handler is itself stuck waiting on the queued call. §2.3's fix rejected
// this immediately with EDEADLK rather than queuing (verified: failed at the very first reentry). §2.5's
// multi-slot table gives a nested call a few independently-correlated slots to use instead of queuing behind the
// busy one - so this same self-recursive call should now genuinely proceed several levels deep (one per slot)
// before hitting the (now higher, but still finite) capacity limit. Run off the main thread with a bound, and
// deliberately leak everything on the timeout path instead of letting destructors (which themselves wait on the
// engine) hang the whole test binary.
TEST(NestedMethodCallReentrancyTest, SelfRecursiveCallProceedsUntilSlotsExhaustThenFailsFast)
{
    score::cpp::set_assertion_handler(stdout_handler);

    const std::string identifier = "test_nested_self_" + std::to_string(::getpid());
    auto* const protocol_config = new ServiceProtocolConfig{identifier, 1024, 1024, 1024};
    const IClientFactory::ClientConfig client_config{1, 1, false, true, false};
    const IServerFactory::ServerConfig server_config{};

    auto connect_callback = [](IServerConnection&) -> score::cpp::expected<UserData, score::os::Error> {
        return nullptr;
    };
    auto disconnect_callback = [](IServerConnection&) {};

    // The server and its own outgoing "call myself again" connection deliberately share one engine/thread.
    auto* const server_factory = new UnixDomainServerFactory();
    auto* const self_client_factory = new UnixDomainClientFactory(server_factory->GetEngine());

    auto* const self_client = new score::cpp::pmr::unique_ptr<IClientConnection>(
        self_client_factory->Create(*protocol_config, client_config));
    ASSERT_TRUE(*self_client);

    // Each hop increments a depth byte carried in the message payload, so the test can tell how far recursion
    // actually got if it doesn't simply hang.
    auto sent_with_reply_callback = [self_client](IServerConnection& connection,
                                                  score::cpp::span<const std::uint8_t> message) -> score::cpp::blank {
        const std::uint8_t depth = message.empty() ? std::uint8_t{0} : message[0];
        const std::array<std::uint8_t, 1> next_message = {static_cast<std::uint8_t>(depth + 1)};
        std::array<std::uint8_t, 16> nested_reply_buffer{};
        auto nested_result = (*self_client)->SendWaitReply(next_message, nested_reply_buffer);
        if (!nested_result.has_value())
        {
            const std::array<std::uint8_t, 2> error_reply = {
                static_cast<std::uint8_t>(nested_result.error().GetOsDependentErrorCode()), depth};
            connection.Reply(error_reply);
            return {};
        }
        connection.Reply(nested_result.value());
        return {};
    };

    auto* const server =
        new score::cpp::pmr::unique_ptr<IServer>(server_factory->Create(*protocol_config, server_config));
    ASSERT_TRUE(*server);
    ASSERT_TRUE(
        (*server)->StartListening(connect_callback, disconnect_callback, {}, sent_with_reply_callback).has_value());

    std::promise<void> self_client_ready;
    (*self_client)
        ->Start(
            [&self_client_ready](auto state) {
                if (state == IClientConnection::State::kReady)
                {
                    self_client_ready.set_value();
                }
            },
            IClientConnection::NotifyCallback{});
    ASSERT_EQ(self_client_ready.get_future().wait_for(kFutureWaitTimeout), std::future_status::ready);

    // A separate, unrelated top-level client (its own engine) kicks off the chain with a plain, non-nested call.
    auto* const outer_client_factory = new UnixDomainClientFactory();
    auto* const outer_client = new score::cpp::pmr::unique_ptr<IClientConnection>(
        outer_client_factory->Create(*protocol_config, client_config));
    ASSERT_TRUE(*outer_client);
    std::promise<void> outer_client_ready;
    (*outer_client)
        ->Start(
            [&outer_client_ready](auto state) {
                if (state == IClientConnection::State::kReady)
                {
                    outer_client_ready.set_value();
                }
            },
            IClientConnection::NotifyCallback{});
    ASSERT_EQ(outer_client_ready.get_future().wait_for(kFutureWaitTimeout), std::future_status::ready);

    std::promise<score::cpp::expected<std::array<std::uint8_t, 2>, score::os::Error>> call_done;
    auto call_done_future = call_done.get_future();
    std::thread caller([outer_client, &call_done]() {
        const std::array<std::uint8_t, 1> initial_message = {0};
        std::array<std::uint8_t, 2> reply_buffer{};
        auto result = (*outer_client)->SendWaitReply(initial_message, reply_buffer);
        if (!result.has_value())
        {
            call_done.set_value(score::cpp::make_unexpected(result.error()));
            return;
        }
        std::array<std::uint8_t, 2> reply{};
        std::copy(result.value().begin(), result.value().end(), reply.begin());
        call_done.set_value(reply);
    });

    const auto status = call_done_future.wait_for(std::chrono::seconds(3));
    if (status != std::future_status::ready)
    {
        ADD_FAILURE() << "self-recursive SendWaitReply on the same connection hung instead of failing fast - the "
                         "nested-pump fix does not protect this case (only distinct connections, like #767's own "
                         "Foo/Bar example, are safe)";
        caller.detach();
        return;  // deliberately leak server/clients/factories above: their destructors would also hang
    }

    caller.join();
    auto result = call_done_future.get();
    ASSERT_TRUE(result.has_value()) << "errno " << result.error().GetOsDependentErrorCode();
    EXPECT_EQ(result.value()[0], EDEADLK) << "expected capacity exhaustion to still fail fast with EDEADLK, same "
                                             "as before the multi-slot prototype - it's a higher ceiling, not an "
                                             "unbounded one";
    // 1 primary slot + kMaxNestedPendingCalls (4, client_connection.h) auxiliary slots = depth 5 is the first
    // reentry that can no longer get a slot of its own. Before the §2.5 multi-slot prototype this was 1 - the
    // very first reentry already found the (only) slot busy and failed immediately.
    EXPECT_EQ(result.value()[1], 5)
        << "expected self-recursion to genuinely proceed through several independently-correlated slots (one per "
           "level) before hitting the now-higher capacity limit, not fail at the first reentry as before §2.5";

    (*outer_client)->Stop();
    (*self_client)->Stop();
    (*server)->StopListening();

    delete outer_client;
    delete outer_client_factory;
    delete server;
    delete self_client;
    delete self_client_factory;
    delete server_factory;
    delete protocol_config;
}

}  // namespace
}  // namespace score::message_passing
