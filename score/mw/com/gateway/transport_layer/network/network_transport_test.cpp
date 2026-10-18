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
#include "score/mw/com/gateway/transport_layer/network/network_transport.h"

#include "score/mw/com/gateway/gateway_application/gateway_core_mock.h"
#include "score/mw/com/gateway/gateway_application/gateway_error.h"
#include "score/mw/com/gateway/transport_layer/network/core/loopback_bus.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <vector>

namespace score::mw::com::gateway::network
{
namespace
{

using namespace std::chrono_literals;
using ::testing::_;
using ::testing::Invoke;
using ::testing::NiceMock;

constexpr auto kInstance{"/vehicle/speed_service"};

score::mw::com::InstanceSpecifier Specifier()
{
    return score::mw::com::InstanceSpecifier::Create(std::string{kInstance}).value();
}

RpcChannelConfig FastConfig()
{
    RpcChannelConfig config{};
    config.ack_timeout = 50ms;
    config.max_attempts = 4U;
    return config;
}

/// Two NetworkTransports connected by a LoopbackBus pair, each with a mocked GatewayCore: "source" is the gateway
/// in the domain that provides the service, "destination" the one that forwards it to its local consumers.
class NetworkTransportTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        auto buses = LoopbackBus::CreatePair();
        source_ = std::make_unique<NetworkTransport>(source_core_, std::move(buses.first), FastConfig());
        destination_ = std::make_unique<NetworkTransport>(destination_core_, std::move(buses.second), FastConfig());
        ASSERT_TRUE(source_->Setup().has_value());
        ASSERT_TRUE(destination_->Setup().has_value());
    }

    void TearDown() override
    {
        source_.reset();
        destination_.reset();
    }

    /// Replaces both transports with ones that use the given type id configurations.
    void Recreate(TypeIdConfig source_ids, TypeIdConfig destination_ids)
    {
        source_.reset();
        destination_.reset();
        auto buses = LoopbackBus::CreatePair();
        source_ = std::make_unique<NetworkTransport>(
            source_core_, std::move(buses.first), FastConfig(), std::move(source_ids));
        destination_ = std::make_unique<NetworkTransport>(
            destination_core_, std::move(buses.second), FastConfig(), std::move(destination_ids));
        ASSERT_TRUE(source_->Setup().has_value());
        ASSERT_TRUE(destination_->Setup().has_value());
    }

    /// Requests are dispatched in order: once this OfferService has reached the destination core, every request
    /// sent before it has been dispatched too.
    void SendOfferAndWait()
    {
        std::promise<void> offered{};
        EXPECT_CALL(destination_core_, OfferService(_)).WillOnce(Invoke([&offered](auto) {
            offered.set_value();
            return score::Result<void>{};
        }));
        ASSERT_TRUE(source_->OfferService(Specifier()).has_value());
        ASSERT_EQ(offered.get_future().wait_for(2s), std::future_status::ready);
    }

    NiceMock<GatewayCoreMock> source_core_;
    NiceMock<GatewayCoreMock> destination_core_;
    std::unique_ptr<NetworkTransport> source_;
    std::unique_ptr<NetworkTransport> destination_;
};

TEST_F(NetworkTransportTest, IsACopyingTransport)
{
    EXPECT_FALSE(source_->IsMemorySharingSupported());
}

TEST_F(NetworkTransportTest, ProvideServiceReachesRemoteCoreWithEventMetadata)
{
    std::promise<void> called{};
    EXPECT_CALL(destination_core_, ProvideService(_, _))
        .WillOnce(Invoke(
            [&called](score::mw::com::InstanceSpecifier specifier, std::vector<score::mw::com::EventInfo> events) {
                EXPECT_EQ(specifier.ToString(), kInstance);
                EXPECT_EQ(events.size(), 2U);
                if (events.size() == 2U)
                {
                    EXPECT_EQ(events[0].name, "speed");
                    EXPECT_EQ(events[0].data_type_meta_info.size, 8U);
                    EXPECT_EQ(events[0].data_type_meta_info.alignment, 8U);
                    EXPECT_EQ(events[1].name, "status");
                }
                called.set_value();
                return score::Result<void>{};
            }));

    const std::vector<score::mw::com::EventInfo> events{
        score::mw::com::impl::EventInfo{"speed", score::mw::com::impl::DataTypeMetaInfo{8U, 8U}},
        score::mw::com::impl::EventInfo{"status", score::mw::com::impl::DataTypeMetaInfo{1U, 1U}},
    };
    EXPECT_TRUE(source_->ProvideService(Specifier(), events).has_value());
    EXPECT_EQ(called.get_future().wait_for(2s), std::future_status::ready);
}

TEST_F(NetworkTransportTest, OfferAndStopOfferReachRemoteCoreInOrder)
{
    std::promise<void> stopped{};
    ::testing::InSequence sequence{};
    EXPECT_CALL(destination_core_, OfferService(_)).WillOnce(Invoke([](score::mw::com::InstanceSpecifier) {
        return score::Result<void>{};
    }));
    EXPECT_CALL(destination_core_, StopOfferService(_)).WillOnce(Invoke([&stopped](score::mw::com::InstanceSpecifier) {
        stopped.set_value();
    }));

    EXPECT_TRUE(source_->OfferService(Specifier()).has_value());
    EXPECT_TRUE(source_->StopOfferService(Specifier()).has_value());
    EXPECT_EQ(stopped.get_future().wait_for(2s), std::future_status::ready);
}

TEST_F(NetworkTransportTest, UpdateNotificationRegistrationFlowsBackToSource)
{
    std::promise<void> registered{};
    std::promise<void> unregistered{};
    EXPECT_CALL(source_core_, RegisterUpdateNotification(_, impl::ServiceElementType::EVENT, std::string{"speed"}))
        .WillOnce(Invoke([&registered](auto, auto, auto) {
            registered.set_value();
            return score::Result<void>{};
        }));
    EXPECT_CALL(source_core_, UnregisterUpdateNotification(_, impl::ServiceElementType::EVENT, std::string{"speed"}))
        .WillOnce(Invoke([&unregistered](auto, auto, auto) {
            unregistered.set_value();
            return score::Result<void>{};
        }));

    EXPECT_TRUE(destination_->RegisterUpdateNotification(Specifier(), impl::ServiceElementType::EVENT, "speed"));
    EXPECT_EQ(registered.get_future().wait_for(2s), std::future_status::ready);
    EXPECT_TRUE(destination_->UnregisterUpdateNotification(Specifier(), impl::ServiceElementType::EVENT, "speed"));
    EXPECT_EQ(unregistered.get_future().wait_for(2s), std::future_status::ready);
}

TEST_F(NetworkTransportTest, ForwardSampleDataDeliversBytesToRemoteCore)
{
    const std::vector<std::uint8_t> sample{1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U};
    std::promise<std::vector<std::uint8_t>> received{};
    EXPECT_CALL(destination_core_, ReceiveSampleData(_, impl::ServiceElementType::EVENT, std::string{"speed"}, _))
        .WillOnce(Invoke([&received](auto, auto, auto, score::cpp::span<const std::uint8_t> data) {
            // The span is only valid during the call.
            received.set_value(std::vector<std::uint8_t>{data.begin(), data.end()});
            return score::Result<void>{};
        }));

    EXPECT_TRUE(source_
                    ->ForwardSampleData(Specifier(),
                                        impl::ServiceElementType::EVENT,
                                        "speed",
                                        score::cpp::span<const std::uint8_t>{sample.data(), sample.size()})
                    .has_value());
    auto future = received.get_future();
    ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(future.get(), sample);
}

TEST_F(NetworkTransportTest, ForwardSampleDataSupportsEventsOnly)
{
    const std::uint8_t byte{0U};
    EXPECT_FALSE(
        source_
            ->ForwardSampleData(
                Specifier(), impl::ServiceElementType::FIELD, "f", score::cpp::span<const std::uint8_t>{&byte, 1U})
            .has_value());
}

TEST_F(NetworkTransportTest, NestedRequestFromRemoteCoreDoesNotDeadlock)
{
    // The destination GatewayApplication re-registers active subscriptions while it handles ProvideService.
    std::promise<void> registered{};
    EXPECT_CALL(destination_core_, ProvideService(_, _)).WillOnce(Invoke([this](auto, auto) {
        EXPECT_TRUE(destination_->RegisterUpdateNotification(Specifier(), impl::ServiceElementType::EVENT, "speed"));
        return score::Result<void>{};
    }));
    EXPECT_CALL(source_core_, RegisterUpdateNotification(_, _, _)).WillOnce(Invoke([&registered](auto, auto, auto) {
        registered.set_value();
        return score::Result<void>{};
    }));

    EXPECT_TRUE(source_->ProvideService(Specifier(), {}).has_value());
    EXPECT_EQ(registered.get_future().wait_for(2s), std::future_status::ready);
}

TEST_F(NetworkTransportTest, RemoteCoreErrorIsOnlyLoggedRemotely)
{
    // Ack semantics (same as the sample hypervisor transport): the ack confirms delivery, not the remote result.
    std::promise<void> called{};
    EXPECT_CALL(destination_core_, OfferService(_)).WillOnce(Invoke([&called](auto) {
        called.set_value();
        return score::Result<void>{score::MakeUnexpected(GatewayErrorc::kSkeletonOfferFailed)};
    }));
    EXPECT_TRUE(source_->OfferService(Specifier()).has_value());
    EXPECT_EQ(called.get_future().wait_for(2s), std::future_status::ready);
}

TEST_F(NetworkTransportTest, RequestFailsWhenPeerIsDown)
{
    destination_->Shutdown();
    EXPECT_FALSE(source_->OfferService(Specifier()).has_value());
    EXPECT_EQ(source_->Statistics().retries, FastConfig().max_attempts - 1U);
}

TEST_F(NetworkTransportTest, RequestsFailAfterShutdown)
{
    source_->Shutdown();
    EXPECT_FALSE(source_->OfferService(Specifier()).has_value());
}

}  // namespace
}  // namespace score::mw::com::gateway::network

namespace score::mw::com::gateway::network
{
namespace
{

TypeIdConfig IdsFor(std::string type_id, TypeIdPolicy policy = TypeIdPolicy::kWarn)
{
    TypeIdConfig config{};
    config.policy = policy;
    if (!type_id.empty())
    {
        config.type_ids[kInstance]["speed"] = std::move(type_id);
    }
    return config;
}

std::vector<score::mw::com::EventInfo> SpeedEvent()
{
    return {score::mw::com::impl::EventInfo{"speed", score::mw::com::impl::DataTypeMetaInfo{8U, 8U}}};
}

TEST_F(NetworkTransportTest, MatchingTypeIdsProvideTheService)
{
    Recreate(IdsFor("Speed@1"), IdsFor("Speed@1"));
    std::promise<void> called{};
    EXPECT_CALL(destination_core_, ProvideService(_, _)).WillOnce(Invoke([&called](auto, auto) {
        called.set_value();
        return score::Result<void>{};
    }));
    ASSERT_TRUE(source_->ProvideService(Specifier(), SpeedEvent()).has_value());
    EXPECT_EQ(called.get_future().wait_for(2s), std::future_status::ready);
    EXPECT_EQ(destination_->TypeIdRejections(), 0U);
}

TEST_F(NetworkTransportTest, MismatchedTypeIdRejectsTheService)
{
    Recreate(IdsFor("Speed@2"), IdsFor("Speed@1"));
    EXPECT_CALL(destination_core_, ProvideService(_, _)).Times(0);
    // The request itself is delivered (and acked); the destination does not provide the service.
    ASSERT_TRUE(source_->ProvideService(Specifier(), SpeedEvent()).has_value());
    SendOfferAndWait();
    EXPECT_EQ(destination_->TypeIdRejections(), 1U);
}

TEST_F(NetworkTransportTest, StrictPolicyRejectsAMissingTypeId)
{
    Recreate(IdsFor(""), IdsFor("Speed@1", TypeIdPolicy::kStrict));
    EXPECT_CALL(destination_core_, ProvideService(_, _)).Times(0);
    ASSERT_TRUE(source_->ProvideService(Specifier(), SpeedEvent()).has_value());
    SendOfferAndWait();
    EXPECT_EQ(destination_->TypeIdRejections(), 1U);
}

TEST_F(NetworkTransportTest, WarnPolicyAcceptsAMissingTypeId)
{
    Recreate(IdsFor("Speed@1"), IdsFor(""));
    std::promise<void> called{};
    EXPECT_CALL(destination_core_, ProvideService(_, _)).WillOnce(Invoke([&called](auto, auto) {
        called.set_value();
        return score::Result<void>{};
    }));
    ASSERT_TRUE(source_->ProvideService(Specifier(), SpeedEvent()).has_value());
    EXPECT_EQ(called.get_future().wait_for(2s), std::future_status::ready);
    EXPECT_EQ(destination_->TypeIdRejections(), 0U);
}

}  // namespace
}  // namespace score::mw::com::gateway::network
