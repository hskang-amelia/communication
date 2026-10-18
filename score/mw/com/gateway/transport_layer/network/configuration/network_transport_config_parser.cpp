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

#include "score/mw/com/gateway/transport_layer/network/core/wire_format.h"
#include "score/mw/log/logging.h"

#include <score/assert.hpp>

#include <chrono>
#include <exception>
#include <string>

namespace score::mw::com::gateway::network
{
namespace
{

using std::string_view_literals::operator""sv;

constexpr auto kRpcKey = "rpc"sv;
constexpr auto kAckTimeoutMsKey = "ack-timeout-ms"sv;
constexpr auto kMaxAttemptsKey = "max-attempts"sv;
constexpr auto kDedupeWindowKey = "dedupe-window"sv;
constexpr auto kMaxDispatchQueueKey = "max-dispatch-queue"sv;

constexpr auto kTypeCheckKey = "type-check"sv;
constexpr auto kPolicyKey = "policy"sv;
constexpr auto kTypeIdsKey = "type-ids"sv;

constexpr auto kZenohPicoKey = "zenoh-pico"sv;
constexpr auto kModeKey = "mode"sv;
constexpr auto kConnectKey = "connect"sv;
constexpr auto kListenKey = "listen"sv;
constexpr auto kLocalKeyKey = "local-key"sv;
constexpr auto kRemoteKeyKey = "remote-key"sv;
constexpr auto kOpenTimeoutMsKey = "open-timeout-ms"sv;
constexpr auto kMaxFrameSizeKey = "max-frame-size"sv;

constexpr auto kCycloneDdsKey = "cyclone-dds"sv;
constexpr auto kDomainIdKey = "domain-id"sv;
constexpr auto kLocalTopicKey = "local-topic"sv;
constexpr auto kRemoteTopicKey = "remote-topic"sv;
constexpr auto kHistoryDepthKey = "history-depth"sv;
constexpr auto kMaxBlockingTimeMsKey = "max-blocking-time-ms"sv;
constexpr auto kConfigXmlKey = "config-xml"sv;
constexpr auto kWaitForPeerMsKey = "wait-for-peer-ms"sv;

const score::json::Object& ObjectAt(const score::json::Object& parent, std::string_view key)
{
    const auto entry = parent.find(key);
    SCORE_LANGUAGE_FUTURECPP_ASSERT_PRD_MESSAGE(entry != parent.cend(), "mandatory section missing in network config");
    const auto object = entry->second.As<score::json::Object>();
    SCORE_LANGUAGE_FUTURECPP_ASSERT_PRD_MESSAGE(object.has_value(), "section of network config is not an object");
    return object.value().get();
}

std::string MandatoryString(const score::json::Object& object, std::string_view key)
{
    const auto entry = object.find(key);
    SCORE_LANGUAGE_FUTURECPP_ASSERT_PRD_MESSAGE(entry != object.cend(), "mandatory key missing in network config");
    const auto value = entry->second.As<std::string>();
    SCORE_LANGUAGE_FUTURECPP_ASSERT_PRD_MESSAGE(value.has_value(), "network config value is not a string");
    return value.value();
}

template <typename T>
void Optional(const score::json::Object& object, std::string_view key, T& target)
{
    const auto entry = object.find(key);
    if (entry != object.cend())
    {
        const auto value = entry->second.As<T>();
        SCORE_LANGUAGE_FUTURECPP_ASSERT_PRD_MESSAGE(value.has_value(), "network config value has the wrong type");
        target = value.value();
    }
}

void OptionalString(const score::json::Object& object, std::string_view key, std::string& target)
{
    const auto entry = object.find(key);
    if (entry != object.cend())
    {
        const auto value = entry->second.As<std::string>();
        SCORE_LANGUAGE_FUTURECPP_ASSERT_PRD_MESSAGE(value.has_value(), "network config value is not a string");
        target = value.value();
    }
}

void OptionalMs(const score::json::Object& object, std::string_view key, std::chrono::milliseconds& target)
{
    std::uint32_t ms{static_cast<std::uint32_t>(target.count())};
    Optional(object, key, ms);
    target = std::chrono::milliseconds{ms};
}

void ParseRpc(const score::json::Object& top, RpcChannelConfig& rpc)
{
    if (top.find(kRpcKey) == top.cend())
    {
        return;
    }
    const auto& object = ObjectAt(top, kRpcKey);
    OptionalMs(object, kAckTimeoutMsKey, rpc.ack_timeout);
    Optional(object, kMaxAttemptsKey, rpc.max_attempts);
    std::uint32_t dedupe{static_cast<std::uint32_t>(rpc.dedupe_window)};
    Optional(object, kDedupeWindowKey, dedupe);
    rpc.dedupe_window = dedupe;
    std::uint32_t queue{static_cast<std::uint32_t>(rpc.max_dispatch_queue)};
    Optional(object, kMaxDispatchQueueKey, queue);
    rpc.max_dispatch_queue = queue;
    SCORE_LANGUAGE_FUTURECPP_ASSERT_PRD_MESSAGE(rpc.ack_timeout.count() > 0, "rpc.ack-timeout-ms must be > 0");
    SCORE_LANGUAGE_FUTURECPP_ASSERT_PRD_MESSAGE(rpc.max_attempts > 0U, "rpc.max-attempts must be > 0");
    SCORE_LANGUAGE_FUTURECPP_ASSERT_PRD_MESSAGE(rpc.dedupe_window > 0U, "rpc.dedupe-window must be > 0");
    SCORE_LANGUAGE_FUTURECPP_ASSERT_PRD_MESSAGE(rpc.max_dispatch_queue > 0U, "rpc.max-dispatch-queue must be > 0");
}

void ParseTypeCheck(const score::json::Object& top, TypeIdConfig& config)
{
    if (top.find(kTypeCheckKey) == top.cend())
    {
        return;
    }
    const auto& object = ObjectAt(top, kTypeCheckKey);
    std::string policy{"warn"};
    OptionalString(object, kPolicyKey, policy);
    SCORE_LANGUAGE_FUTURECPP_ASSERT_PRD_MESSAGE((policy == "warn") || (policy == "strict"),
                                                "type-check.policy must be \"warn\" or \"strict\"");
    config.policy = (policy == "strict") ? TypeIdPolicy::kStrict : TypeIdPolicy::kWarn;
    if (object.find(kTypeIdsKey) == object.cend())
    {
        return;
    }
    for (const auto& [instance_key, instance_value] : ObjectAt(object, kTypeIdsKey))
    {
        const auto events = instance_value.As<score::json::Object>();
        SCORE_LANGUAGE_FUTURECPP_ASSERT_PRD_MESSAGE(events.has_value(),
                                                    "type-check.type-ids entries must map event names to type ids");
        auto& target = config.type_ids[std::string{instance_key.GetAsStringView()}];
        for (const auto& [event_key, event_value] : events.value().get())
        {
            const auto type_id = event_value.As<std::string>();
            SCORE_LANGUAGE_FUTURECPP_ASSERT_PRD_MESSAGE(type_id.has_value() && !type_id.value().get().empty(),
                                                        "type-check.type-ids values must be non-empty strings");
            SCORE_LANGUAGE_FUTURECPP_ASSERT_PRD_MESSAGE(type_id.value().get().size() <= kMaxStringLength,
                                                        "type-check.type-ids values must not exceed 65535 bytes");
            target[std::string{event_key.GetAsStringView()}] = type_id.value().get();
        }
    }
}

void ParseZenohPico(const score::json::Object& top, ZenohPicoBusConfig& config)
{
    const auto& object = ObjectAt(top, kZenohPicoKey);
    config.local_key = MandatoryString(object, kLocalKeyKey);
    config.remote_key = MandatoryString(object, kRemoteKeyKey);
    OptionalString(object, kModeKey, config.mode);
    OptionalString(object, kConnectKey, config.connect);
    OptionalString(object, kListenKey, config.listen);
    OptionalMs(object, kOpenTimeoutMsKey, config.open_timeout);
    std::uint32_t max_frame{static_cast<std::uint32_t>(config.max_frame_size)};
    Optional(object, kMaxFrameSizeKey, max_frame);
    config.max_frame_size = max_frame;
    SCORE_LANGUAGE_FUTURECPP_ASSERT_PRD_MESSAGE((config.mode == "peer") || (config.mode == "client"),
                                                "zenoh-pico.mode must be \"peer\" or \"client\"");
    SCORE_LANGUAGE_FUTURECPP_ASSERT_PRD_MESSAGE(!config.connect.empty() || !config.listen.empty(),
                                                "zenoh-pico needs a connect or listen endpoint");
    SCORE_LANGUAGE_FUTURECPP_ASSERT_PRD_MESSAGE(config.local_key != config.remote_key,
                                                "zenoh-pico.local-key and remote-key must differ");
    SCORE_LANGUAGE_FUTURECPP_ASSERT_PRD_MESSAGE(config.max_frame_size >= 64U,
                                                "zenoh-pico.max-frame-size must be >= 64");
}

void ParseCycloneDds(const score::json::Object& top, CycloneDdsBusConfig& config)
{
    const auto& object = ObjectAt(top, kCycloneDdsKey);
    config.local_topic = MandatoryString(object, kLocalTopicKey);
    config.remote_topic = MandatoryString(object, kRemoteTopicKey);
    Optional(object, kDomainIdKey, config.domain_id);
    Optional(object, kHistoryDepthKey, config.history_depth);
    OptionalMs(object, kMaxBlockingTimeMsKey, config.max_blocking_time);
    OptionalString(object, kConfigXmlKey, config.cyclone_config_xml);
    OptionalMs(object, kWaitForPeerMsKey, config.wait_for_peer);
    SCORE_LANGUAGE_FUTURECPP_ASSERT_PRD_MESSAGE(config.local_topic != config.remote_topic,
                                                "cyclone-dds.local-topic and remote-topic must differ");
    SCORE_LANGUAGE_FUTURECPP_ASSERT_PRD_MESSAGE(config.domain_id <= 232U, "cyclone-dds.domain-id must be <= 232");
    SCORE_LANGUAGE_FUTURECPP_ASSERT_PRD_MESSAGE(config.history_depth > 0, "cyclone-dds.history-depth must be > 0");
}

}  // namespace

NetworkTransportConfig ParseNetworkTransportConfig(std::string_view path, NetworkBackend backend) noexcept
{
    const score::json::JsonParser json_parser_obj;
    // NOLINTNEXTLINE(score-banned-function): AoU of score::json::JsonParser — caller must guarantee path integrity.
    auto json_result = json_parser_obj.FromFile(path);
    if (!json_result.has_value())
    {
        ::score::mw::log::LogFatal("lola")
            << "Parsing network transport config file" << path << "failed with error:" << json_result.error().Message()
            << ": " << json_result.error().UserMessage() << " . Terminating.";
        std::terminate();
    }
    return ParseNetworkTransportConfig(std::move(json_result).value(), backend);
}

NetworkTransportConfig ParseNetworkTransportConfig(score::json::Any json, NetworkBackend backend) noexcept
{
    auto top_level_object = json.As<score::json::Object>();
    if (!top_level_object.has_value())
    {
        ::score::mw::log::LogFatal("lola")
            << "Parsing network transport configuration failed: Expected top-level JSON object. Terminating.";
        std::terminate();
    }
    const auto& top = top_level_object.value().get();

    NetworkTransportConfig config{};
    ParseRpc(top, config.rpc);
    ParseTypeCheck(top, config.type_check);
    if (backend == NetworkBackend::kZenohPico)
    {
        ParseZenohPico(top, config.zenoh_pico);
    }
    else
    {
        ParseCycloneDds(top, config.cyclone_dds);
    }
    return config;
}

}  // namespace score::mw::com::gateway::network
