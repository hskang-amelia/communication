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
#include "score/mw/com/gateway/transport_layer/transport_error.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <memory>
#include <mutex>
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
    config.result_timeout = 1s;
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
    void Recreate(TypeIdConfig source_ids, TypeIdConfig destination_ids, RpcChannelConfig config = FastConfig())
    {
        source_.reset();
        destination_.reset();
        auto buses = LoopbackBus::CreatePair();
        source_ =
            std::make_unique<NetworkTransport>(source_core_, std::move(buses.first), config, std::move(source_ids));
        destination_ = std::make_unique<NetworkTransport>(
            destination_core_, std::move(buses.second), config, std::move(destination_ids));
        ASSERT_TRUE(source_->Setup().has_value());
        ASSERT_TRUE(destination_->Setup().has_value());
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
    TypeIdConfig ids{};
    ids.type_ids[kInstance]["speed"] = "Speed@1";
    ids.type_ids[kInstance]["status"] = "Status@1";
    Recreate(ids, ids);
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

TEST_F(NetworkTransportTest, NotificationCallsRejectFieldsAndMethodsBeforeSending)
{
    EXPECT_CALL(destination_core_, RegisterUpdateNotification(_, _, _)).Times(0);
    EXPECT_CALL(destination_core_, UnregisterUpdateNotification(_, _, _)).Times(0);
    EXPECT_CALL(destination_core_, NotifyUpdate(_, _, _)).Times(0);

    for (const auto type : {impl::ServiceElementType::FIELD, impl::ServiceElementType::METHOD})
    {
        const auto registered = source_->RegisterUpdateNotification(Specifier(), type, "f");
        ASSERT_FALSE(registered.has_value());
        EXPECT_EQ(registered.error(), TransportErrorc::kNotSupported);

        const auto unregistered = source_->UnregisterUpdateNotification(Specifier(), type, "f");
        ASSERT_FALSE(unregistered.has_value());
        EXPECT_EQ(unregistered.error(), TransportErrorc::kNotSupported);

        const auto notified = source_->NotifyUpdate(Specifier(), type, "f");
        ASSERT_FALSE(notified.has_value());
        EXPECT_EQ(notified.error(), TransportErrorc::kNotSupported);
    }
    // Nothing went over the wire: the destination never dispatched a frame.
    EXPECT_EQ(destination_->Statistics().requests_dispatched, 0U);
    EXPECT_EQ(destination_->Statistics().notifications_dispatched, 0U);
}

/// A peer that does not apply the events-only rule (or a corrupted frame) must not reach the gateway core with a
/// field or method reference: GatewayApplication asserts on those and would take the whole gateway down.
TEST_F(NetworkTransportTest, InboundFieldOrMethodReferencesAreDroppedWithoutReachingTheCore)
{
    // A bare bus endpoint stands in for a misbehaving peer; only the destination has a transport.
    source_.reset();
    destination_.reset();
    auto buses = LoopbackBus::CreatePair();
    auto& peer = *buses.first;
    ASSERT_EQ(peer.Open([](const std::uint8_t*, std::size_t) {}), BusStatus::kOk);
    destination_ = std::make_unique<NetworkTransport>(destination_core_, std::move(buses.second), FastConfig());
    ASSERT_TRUE(destination_->Setup().has_value());

    EXPECT_CALL(destination_core_, RegisterUpdateNotification(_, _, _)).Times(0);
    EXPECT_CALL(destination_core_, UnregisterUpdateNotification(_, _, _)).Times(0);
    std::promise<void> event_update{};
    EXPECT_CALL(destination_core_, NotifyUpdate(_, impl::ServiceElementType::EVENT, std::string{"speed"}))
        .WillOnce(Invoke([&event_update](auto, auto, auto) {
            event_update.set_value();
            return score::Result<void>{};
        }));

    constexpr std::uint64_t kPeerSession{0xBADU};
    std::uint32_t sequence{1U};
    const auto publish = [&peer, &sequence](const FrameBody& body) {
        std::vector<std::uint8_t> buffer{};
        EncodeError error{};
        ASSERT_TRUE(Encode(FrameHeader{KindOf(body), kPeerSession, sequence++}, body, buffer, error));
        ASSERT_EQ(peer.Publish(buffer.data(), buffer.size()), BusStatus::kOk);
    };
    publish(UpdateNotification{ElementRef{std::string{kInstance}, WireElementKind::kField, "f"}});
    publish(RegisterNotification{ElementRef{std::string{kInstance}, WireElementKind::kMethod, "m"}});
    publish(UnregisterNotification{ElementRef{std::string{kInstance}, WireElementKind::kField, "f"}});
    // The transport is still alive and still forwards events after the bad frames.
    publish(UpdateNotification{ElementRef{std::string{kInstance}, WireElementKind::kEvent, "speed"}});

    EXPECT_EQ(event_update.get_future().wait_for(2s), std::future_status::ready);
    EXPECT_EQ(destination_->UnsupportedElementRejections(), 3U);
    peer.Close();
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

TEST_F(NetworkTransportTest, RemoteCoreErrorIsReportedAsTransientFailure)
{
    EXPECT_CALL(destination_core_, OfferService(_)).WillOnce(Invoke([](auto) {
        return score::Result<void>{score::MakeUnexpected(GatewayErrorc::kSkeletonOfferFailed)};
    }));
    EXPECT_CALL(destination_core_, ProvideService(_, _)).WillOnce(Invoke([](auto, auto) {
        return score::Result<void>{score::MakeUnexpected(GatewayErrorc::kSkeletonCreationFailed)};
    }));
    const auto offer = source_->OfferService(Specifier());
    ASSERT_FALSE(offer.has_value());
    EXPECT_EQ(offer.error(), TransportErrorc::kPeerRequestFailed);
    const auto provide = source_->ProvideService(Specifier(), {});
    ASSERT_FALSE(provide.has_value());
    EXPECT_EQ(provide.error(), TransportErrorc::kPeerRequestFailed);
}

TEST_F(NetworkTransportTest, NonWhitelistedServiceIsReportedAsRejection)
{
    EXPECT_CALL(destination_core_, ProvideService(_, _)).WillOnce(Invoke([](auto, auto) {
        return score::Result<void>{score::MakeUnexpected(GatewayErrorc::kNonWhitelistedService)};
    }));
    const auto result = source_->ProvideService(Specifier(), {});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), TransportErrorc::kRejectedByPeer);
}

TEST_F(NetworkTransportTest, NoResultInTimeIsATransientFailure)
{
    RpcChannelConfig config = FastConfig();
    config.result_timeout = 300ms;
    Recreate({}, {}, config);
    // The destination handles the request for longer than the source waits.
    std::promise<void> release{};
    auto released = release.get_future().share();
    EXPECT_CALL(destination_core_, OfferService(_)).WillOnce(Invoke([released](auto) {
        static_cast<void>(released.wait_for(5s));
        return score::Result<void>{};
    }));

    const auto start = std::chrono::steady_clock::now();
    const auto result = source_->OfferService(Specifier());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), TransportErrorc::kPeerRequestFailed);
    EXPECT_GE(std::chrono::steady_clock::now() - start, config.result_timeout);

    release.set_value();
    // The late result is dropped by the source.
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while ((source_->Statistics().stale_results == 0U) && (std::chrono::steady_clock::now() < deadline))
    {
        std::this_thread::sleep_for(10ms);
    }
    EXPECT_EQ(source_->Statistics().stale_results, 1U);
}

TEST_F(NetworkTransportTest, GatewaysProvidingToEachOtherConcurrentlyDoNotDeadlock)
{
    // Both gateways forward a service to the other at the same time. Each GatewayCore handles ProvideService under
    // its application lock (which the caller does not hold while waiting) and registers a notification back, like
    // GatewayApplication re-registering active subscriptions.
    std::mutex source_app_mutex{};
    std::mutex destination_app_mutex{};
    const auto handler = [](std::mutex& app_mutex, std::unique_ptr<NetworkTransport>& self) {
        return [&app_mutex, &self](auto, auto) {
            std::lock_guard<std::mutex> lock{app_mutex};
            return self->RegisterUpdateNotification(Specifier(), impl::ServiceElementType::EVENT, "speed");
        };
    };
    constexpr int kRounds{10};
    EXPECT_CALL(source_core_, ProvideService(_, _))
        .Times(kRounds)
        .WillRepeatedly(Invoke(handler(source_app_mutex, source_)));
    EXPECT_CALL(destination_core_, ProvideService(_, _))
        .Times(kRounds)
        .WillRepeatedly(Invoke(handler(destination_app_mutex, destination_)));
    ON_CALL(source_core_, RegisterUpdateNotification(_, _, _))
        .WillByDefault(Invoke([&source_app_mutex](auto, auto, auto) {
            std::lock_guard<std::mutex> lock{source_app_mutex};
            return score::Result<void>{};
        }));
    ON_CALL(destination_core_, RegisterUpdateNotification(_, _, _))
        .WillByDefault(Invoke([&destination_app_mutex](auto, auto, auto) {
            std::lock_guard<std::mutex> lock{destination_app_mutex};
            return score::Result<void>{};
        }));

    const auto provide_many = [](NetworkTransport& transport) {
        int ok{0};
        for (int i = 0; i < kRounds; ++i)
        {
            ok += transport.ProvideService(Specifier(), {}).has_value() ? 1 : 0;
        }
        return ok;
    };
    auto from_source = std::async(std::launch::async, [&]() {
        return provide_many(*source_);
    });
    auto from_destination = std::async(std::launch::async, [&]() {
        return provide_many(*destination_);
    });
    // A deadlock fails here instead of hanging the test.
    ASSERT_EQ(from_source.wait_for(10s), std::future_status::ready);
    ASSERT_EQ(from_destination.wait_for(10s), std::future_status::ready);
    EXPECT_EQ(from_source.get(), kRounds);
    EXPECT_EQ(from_destination.get(), kRounds);
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
    // The destination does not provide the service and reports the rejection to the source.
    const auto result = source_->ProvideService(Specifier(), SpeedEvent());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), TransportErrorc::kRejectedByPeer);
    EXPECT_EQ(destination_->TypeIdRejections(), 1U);
}

TEST_F(NetworkTransportTest, StrictPolicyRejectsAMissingTypeId)
{
    Recreate(IdsFor(""), IdsFor("Speed@1", TypeIdPolicy::kStrict));
    EXPECT_CALL(destination_core_, ProvideService(_, _)).Times(0);
    const auto result = source_->ProvideService(Specifier(), SpeedEvent());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), TransportErrorc::kRejectedByPeer);
    EXPECT_EQ(destination_->TypeIdRejections(), 1U);
}

TEST_F(NetworkTransportTest, DefaultConfigurationRejectsEventsWithoutTypeIds)
{
    EXPECT_CALL(destination_core_, ProvideService(_, _)).Times(0);
    const auto result = source_->ProvideService(Specifier(), SpeedEvent());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), TransportErrorc::kRejectedByPeer);
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
