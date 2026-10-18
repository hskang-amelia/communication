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
#include "score/mw/com/gateway/transport_layer/network/configuration/network_transport_config_parser.h"

#include "score/json/json_parser.h"

#include <gtest/gtest.h>

#include <fstream>
#include <string>

namespace score::mw::com::gateway::network
{
namespace
{

using score::json::operator""_json;

class NetworkTransportConfigParserFixture : public ::testing::Test
{
  public:
    const std::string get_path(const std::string& file_name)
    {
        const std::string default_path =
            "score/mw/com/gateway/transport_layer/network/configuration/example/" + file_name;

        std::ifstream file(default_path);
        if (file.is_open())
        {
            file.close();
            return default_path;
        }
        else
        {
            return "external/score_communication/" + default_path;
        }
    }
};

TEST_F(NetworkTransportConfigParserFixture, ParseExampleZenohPicoJson)
{
    const auto config = ParseNetworkTransportConfig(get_path("mw_com_gateway_zenoh_pico_transport_config.json"),
                                                    NetworkBackend::kZenohPico);

    EXPECT_EQ(std::chrono::milliseconds{200}, config.rpc.ack_timeout);
    EXPECT_EQ(5U, config.rpc.max_attempts);
    EXPECT_EQ(TypeIdPolicy::kWarn, config.type_check.policy);
    EXPECT_EQ("DriveMode@1.2", config.type_check.Find("/vehicle/drive_mode", "mode"));
    EXPECT_EQ("peer", config.zenoh_pico.mode);
    EXPECT_EQ("tcp/0.0.0.0:7447", config.zenoh_pico.listen);
    EXPECT_EQ("score/gateway/hpc/inbox", config.zenoh_pico.local_key);
    EXPECT_EQ("score/gateway/zonal_front/inbox", config.zenoh_pico.remote_key);
    EXPECT_EQ(65536U, config.zenoh_pico.max_frame_size);
}

TEST_F(NetworkTransportConfigParserFixture, ParseExampleCycloneDdsJson)
{
    const auto config = ParseNetworkTransportConfig(get_path("mw_com_gateway_cyclone_dds_transport_config.json"),
                                                    NetworkBackend::kCycloneDds);

    EXPECT_EQ(10U, config.rpc.max_attempts);
    EXPECT_EQ(0U, config.cyclone_dds.domain_id);
    EXPECT_EQ("score_gateway_hpc", config.cyclone_dds.local_topic);
    EXPECT_EQ("score_gateway_zonal_front", config.cyclone_dds.remote_topic);
    EXPECT_EQ(256, config.cyclone_dds.history_depth);
    EXPECT_EQ(std::chrono::milliseconds{2000}, config.cyclone_dds.wait_for_peer);
}

TEST_F(NetworkTransportConfigParserFixture, ParseMinimalZenohPicoAppliesDefaults)
{
    // Given a config with only the mandatory zenoh-pico keys
    auto j = R"(
{
  "zenoh-pico": {
    "connect": "tcp/10.0.0.2:7447",
    "local-key": "a/inbox",
    "remote-key": "b/inbox"
  }
}
)"_json;

    // When the config is parsed
    const auto config = ParseNetworkTransportConfig(std::move(j), NetworkBackend::kZenohPico);

    // Then the defaults of the schema must be applied
    EXPECT_EQ(std::chrono::milliseconds{200}, config.rpc.ack_timeout);
    EXPECT_EQ(5U, config.rpc.max_attempts);
    EXPECT_EQ(256U, config.rpc.dedupe_window);
    EXPECT_EQ(1024U, config.rpc.max_dispatch_queue);
    EXPECT_EQ(TypeIdPolicy::kWarn, config.type_check.policy);
    EXPECT_TRUE(config.type_check.type_ids.empty());
    EXPECT_EQ("peer", config.zenoh_pico.mode);
    EXPECT_EQ(std::chrono::milliseconds{30000}, config.zenoh_pico.open_timeout);
    EXPECT_EQ(4096U, config.zenoh_pico.max_frame_size);
}

TEST_F(NetworkTransportConfigParserFixture, ParseMinimalCycloneDdsAppliesDefaults)
{
    // Given a config with only the mandatory cyclone-dds keys
    auto j = R"(
{
  "cyclone-dds": {
    "local-topic": "a_inbox",
    "remote-topic": "b_inbox"
  }
}
)"_json;

    // When the config is parsed
    const auto config = ParseNetworkTransportConfig(std::move(j), NetworkBackend::kCycloneDds);

    // Then the defaults of the schema must be applied
    EXPECT_EQ(0U, config.cyclone_dds.domain_id);
    EXPECT_EQ(256, config.cyclone_dds.history_depth);
    EXPECT_EQ(std::chrono::milliseconds{1000}, config.cyclone_dds.max_blocking_time);
    EXPECT_TRUE(config.cyclone_dds.cyclone_config_xml.empty());
    EXPECT_EQ(std::chrono::milliseconds{0}, config.cyclone_dds.wait_for_peer);
}

TEST_F(NetworkTransportConfigParserFixture, ParseSchemaBoundariesIsAccepted)
{
    // Given a config with every bounded value at its schema limit
    auto j = R"(
{
  "rpc": {"ack-timeout-ms": 1, "max-attempts": 1, "dedupe-window": 1, "max-dispatch-queue": 1},
  "cyclone-dds": {"domain-id": 232, "history-depth": 1, "local-topic": "a", "remote-topic": "b"}
}
)"_json;

    // When the config is parsed
    const auto config = ParseNetworkTransportConfig(std::move(j), NetworkBackend::kCycloneDds);

    // Then the values must be taken over unchanged
    EXPECT_EQ(std::chrono::milliseconds{1}, config.rpc.ack_timeout);
    EXPECT_EQ(1U, config.rpc.dedupe_window);
    EXPECT_EQ(1U, config.rpc.max_dispatch_queue);
    EXPECT_EQ(232U, config.cyclone_dds.domain_id);
    EXPECT_EQ(1, config.cyclone_dds.history_depth);
}

TEST_F(NetworkTransportConfigParserFixture, ParseFromInvalidPathDies)
{
    EXPECT_DEATH(ParseNetworkTransportConfig("/nonexistent/transport_config.json", NetworkBackend::kZenohPico), ".*");
}

TEST_F(NetworkTransportConfigParserFixture, ParseNonObjectTopLevelDies)
{
    auto j = R"([1, 2, 3])"_json;
    EXPECT_DEATH(ParseNetworkTransportConfig(std::move(j), NetworkBackend::kZenohPico), ".*");
}

TEST_F(NetworkTransportConfigParserFixture, ParseMissingBackendSectionDies)
{
    auto j = R"({"zenoh-pico": {"listen": "tcp/0.0.0.0:7447", "local-key": "a", "remote-key": "b"}})"_json;
    EXPECT_DEATH(ParseNetworkTransportConfig(std::move(j), NetworkBackend::kCycloneDds), ".*");
}

TEST_F(NetworkTransportConfigParserFixture, ParseMissingLocalKeyDies)
{
    auto j = R"({"zenoh-pico": {"listen": "tcp/0.0.0.0:7447", "remote-key": "b"}})"_json;
    EXPECT_DEATH(ParseNetworkTransportConfig(std::move(j), NetworkBackend::kZenohPico), ".*");
}

TEST_F(NetworkTransportConfigParserFixture, ParseZeroAckTimeoutDies)
{
    auto j = R"({"rpc": {"ack-timeout-ms": 0}, "cyclone-dds": {"local-topic": "a", "remote-topic": "b"}})"_json;
    EXPECT_DEATH(ParseNetworkTransportConfig(std::move(j), NetworkBackend::kCycloneDds), ".*");
}

TEST_F(NetworkTransportConfigParserFixture, ParseZeroMaxAttemptsDies)
{
    auto j = R"({"rpc": {"max-attempts": 0}, "cyclone-dds": {"local-topic": "a", "remote-topic": "b"}})"_json;
    EXPECT_DEATH(ParseNetworkTransportConfig(std::move(j), NetworkBackend::kCycloneDds), ".*");
}

TEST_F(NetworkTransportConfigParserFixture, ParseZeroDedupeWindowDies)
{
    auto j = R"({"rpc": {"dedupe-window": 0}, "cyclone-dds": {"local-topic": "a", "remote-topic": "b"}})"_json;
    EXPECT_DEATH(ParseNetworkTransportConfig(std::move(j), NetworkBackend::kCycloneDds), ".*");
}

TEST_F(NetworkTransportConfigParserFixture, ParseZeroMaxDispatchQueueDies)
{
    auto j = R"({"rpc": {"max-dispatch-queue": 0}, "cyclone-dds": {"local-topic": "a", "remote-topic": "b"}})"_json;
    EXPECT_DEATH(ParseNetworkTransportConfig(std::move(j), NetworkBackend::kCycloneDds), ".*");
}

TEST_F(NetworkTransportConfigParserFixture, ParseUnknownTypeCheckPolicyDies)
{
    auto j = R"({"type-check": {"policy": "off"}, "cyclone-dds": {"local-topic": "a", "remote-topic": "b"}})"_json;
    EXPECT_DEATH(ParseNetworkTransportConfig(std::move(j), NetworkBackend::kCycloneDds), ".*");
}

TEST_F(NetworkTransportConfigParserFixture, ParseEmptyTypeIdDies)
{
    auto j = R"(
{
  "type-check": {"type-ids": {"/vehicle/drive_mode": {"mode": ""}}},
  "cyclone-dds": {"local-topic": "a", "remote-topic": "b"}
}
)"_json;
    EXPECT_DEATH(ParseNetworkTransportConfig(std::move(j), NetworkBackend::kCycloneDds), ".*");
}

TEST_F(NetworkTransportConfigParserFixture, ParseUnknownZenohModeDies)
{
    auto j =
        R"({"zenoh-pico": {"mode": "router", "listen": "tcp/0.0.0.0:7447", "local-key": "a", "remote-key": "b"}})"_json;
    EXPECT_DEATH(ParseNetworkTransportConfig(std::move(j), NetworkBackend::kZenohPico), ".*");
}

TEST_F(NetworkTransportConfigParserFixture, ParseZenohWithoutEndpointDies)
{
    auto j = R"({"zenoh-pico": {"local-key": "a", "remote-key": "b"}})"_json;
    EXPECT_DEATH(ParseNetworkTransportConfig(std::move(j), NetworkBackend::kZenohPico), ".*");
}

TEST_F(NetworkTransportConfigParserFixture, ParseEqualZenohKeysDies)
{
    auto j = R"({"zenoh-pico": {"listen": "tcp/0.0.0.0:7447", "local-key": "a", "remote-key": "a"}})"_json;
    EXPECT_DEATH(ParseNetworkTransportConfig(std::move(j), NetworkBackend::kZenohPico), ".*");
}

TEST_F(NetworkTransportConfigParserFixture, ParseTooSmallMaxFrameSizeDies)
{
    auto j = R"(
{
  "zenoh-pico": {"listen": "tcp/0.0.0.0:7447", "local-key": "a", "remote-key": "b", "max-frame-size": 63}
}
)"_json;
    EXPECT_DEATH(ParseNetworkTransportConfig(std::move(j), NetworkBackend::kZenohPico), ".*");
}

TEST_F(NetworkTransportConfigParserFixture, ParseEqualDdsTopicsDies)
{
    auto j = R"({"cyclone-dds": {"local-topic": "a", "remote-topic": "a"}})"_json;
    EXPECT_DEATH(ParseNetworkTransportConfig(std::move(j), NetworkBackend::kCycloneDds), ".*");
}

TEST_F(NetworkTransportConfigParserFixture, ParseTooLargeDomainIdDies)
{
    auto j = R"({"cyclone-dds": {"domain-id": 233, "local-topic": "a", "remote-topic": "b"}})"_json;
    EXPECT_DEATH(ParseNetworkTransportConfig(std::move(j), NetworkBackend::kCycloneDds), ".*");
}

TEST_F(NetworkTransportConfigParserFixture, ParseZeroHistoryDepthDies)
{
    auto j = R"({"cyclone-dds": {"history-depth": 0, "local-topic": "a", "remote-topic": "b"}})"_json;
    EXPECT_DEATH(ParseNetworkTransportConfig(std::move(j), NetworkBackend::kCycloneDds), ".*");
}

TEST_F(NetworkTransportConfigParserFixture, ParseNegativeHistoryDepthDies)
{
    auto j = R"({"cyclone-dds": {"history-depth": -1, "local-topic": "a", "remote-topic": "b"}})"_json;
    EXPECT_DEATH(ParseNetworkTransportConfig(std::move(j), NetworkBackend::kCycloneDds), ".*");
}

}  // namespace
}  // namespace score::mw::com::gateway::network
