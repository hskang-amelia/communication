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
#ifndef SCORE_MW_COM_GATEWAY_TRANSPORT_LAYER_NETWORK_CONFIGURATION_NETWORK_TRANSPORT_CONFIG_PARSER_H
#define SCORE_MW_COM_GATEWAY_TRANSPORT_LAYER_NETWORK_CONFIGURATION_NETWORK_TRANSPORT_CONFIG_PARSER_H

#include "score/mw/com/gateway/transport_layer/network/core/rpc_channel.h"
#include "score/mw/com/gateway/transport_layer/network/dds/cyclone_dds_bus_config.h"
#include "score/mw/com/gateway/transport_layer/network/zenoh/zenoh_pico_bus_config.h"

#include "score/json/json_parser.h"

#include <string_view>

namespace score::mw::com::gateway::network
{

/// \brief Content of the transport-layer config file referenced by mw_com_gateway_config.json ("config-path").
/// \details Only the section of the selected transport id is required ("zenoh-pico" or "cyclone-dds"); "rpc" is
/// optional. See configuration/example/*.json and the schema next to this file.
struct NetworkTransportConfig
{
    RpcChannelConfig rpc{};
    ZenohPicoBusConfig zenoh_pico{};
    CycloneDdsBusConfig cyclone_dds{};
};

enum class NetworkBackend : std::uint8_t
{
    kZenohPico,
    kCycloneDds,
};

/// \brief Parses the config file. Terminates on a missing mandatory field, like the sample transport's parser.
NetworkTransportConfig ParseNetworkTransportConfig(std::string_view path, NetworkBackend backend) noexcept;
NetworkTransportConfig ParseNetworkTransportConfig(score::json::Any json, NetworkBackend backend) noexcept;

}  // namespace score::mw::com::gateway::network

#endif  // SCORE_MW_COM_GATEWAY_TRANSPORT_LAYER_NETWORK_CONFIGURATION_NETWORK_TRANSPORT_CONFIG_PARSER_H
