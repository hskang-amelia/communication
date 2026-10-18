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
#include "score/mw/com/gateway/transport_layer/dds/dds_transport.h"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

namespace score::mw::com::gateway::dds
{
namespace
{

// End-to-end against the real Eclipse CycloneDDS runtime (not a mock): a provider and consumer
// side of one DdsTransport instance, publishing and subscribing to the same in-process
// participant. Proves actual bytes cross the transport, not just that the calls return success.
class DdsTransportTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        ASSERT_TRUE(transport_.Setup().has_value());

        auto specifier_result = score::mw::com::InstanceSpecifier::Create("/my/app/port");
        ASSERT_TRUE(specifier_result.has_value());
        specifier_ = specifier_result.value();

        score::mw::com::EventInfo event_info{};
        event_info.name = kElementName;
        std::vector<score::mw::com::EventInfo> elements{event_info};

        ASSERT_TRUE(transport_.ProvideService(specifier_, elements).has_value());
        ASSERT_TRUE(transport_.OfferService(specifier_).has_value());
        ASSERT_TRUE(transport_.Subscribe(specifier_, score::mw::com::impl::ServiceElementType::EVENT, kElementName)
                        .has_value());

        // Give DDS discovery a moment to match writer/reader within the same participant.
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    static constexpr char kElementName[] = "my_event";
    static constexpr std::uint32_t kWaitTimeoutMs = 2000U;

    DdsTransport transport_;
    score::mw::com::InstanceSpecifier specifier_{score::mw::com::InstanceSpecifier::Create("/x").value()};
};

TEST_F(DdsTransportTest, NotifyUpdateDeliversZeroLengthPing)
{
    ASSERT_TRUE(
        transport_.NotifyUpdate(specifier_, score::mw::com::impl::ServiceElementType::EVENT, kElementName).has_value());

    ASSERT_TRUE(transport_.WaitForUpdate(specifier_, kElementName, kWaitTimeoutMs));
    EXPECT_TRUE(transport_.TakeLastPayload(specifier_, kElementName).empty());
}

TEST_F(DdsTransportTest, ForwardSampleDataDeliversExactBytes)
{
    const std::vector<std::uint8_t> sent{1, 2, 3, 4, 5};
    ASSERT_TRUE(transport_
                    .ForwardSampleData(specifier_,
                                       score::mw::com::impl::ServiceElementType::EVENT,
                                       kElementName,
                                       score::cpp::span<const std::uint8_t>(sent.data(), sent.size()))
                    .has_value());

    ASSERT_TRUE(transport_.WaitForUpdate(specifier_, kElementName, kWaitTimeoutMs));
    EXPECT_EQ(transport_.TakeLastPayload(specifier_, kElementName), sent);
}

TEST_F(DdsTransportTest, UnregisterAfterUnsubscribeFailsWithNotConnected)
{
    ASSERT_TRUE(
        transport_.Unsubscribe(specifier_, score::mw::com::impl::ServiceElementType::EVENT, kElementName).has_value());

    const auto result = transport_.UnregisterUpdateNotification(
        specifier_, score::mw::com::impl::ServiceElementType::EVENT, kElementName);
    EXPECT_FALSE(result.has_value());
}

TEST(DdsTransportPropertiesTest, IsMemorySharingSupportedIsFalse)
{
    DdsTransport transport;
    EXPECT_FALSE(transport.IsMemorySharingSupported());
}

}  // namespace
}  // namespace score::mw::com::gateway::dds
