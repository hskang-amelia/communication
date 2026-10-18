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
 *******************************************************************************/
#include "score/mw/com/gateway/transport_layer/transport_factory.h"

#include "score/memory/shared/shared_memory_factory.h"
#include "score/mw/com/gateway/transport_layer/qemu/configuration/qemu_transport_config_parser.h"
#include "score/mw/com/gateway/transport_layer/qemu/ivshmem/ivshmem_bar_discovery.h"
#include "score/mw/com/gateway/transport_layer/qemu/ivshmem/ivshmem_typed_memory_provider.h"
#include "score/mw/com/gateway/transport_layer/qemu/qemu_hypervisor_transport.h"
#include "score/mw/com/gateway/transport_layer/sample/bidirectional_transport.h"
#include "score/mw/com/gateway/transport_layer/sample/configuration/sample_transport_config_parser.h"
#include "score/mw/com/gateway/transport_layer/sample/sample_hypervisor_transport.h"

#include <cstdint>
// Network (copying) transports are optional: the middleware libraries are only linked when enabled with
// --//score/mw/com/gateway/transport_layer/network:with_zenoh_pico / :with_cyclone_dds.
#if defined(SCORE_GATEWAY_WITH_ZENOH_PICO) || defined(SCORE_GATEWAY_WITH_CYCLONE_DDS)
#include "score/mw/com/gateway/transport_layer/network/configuration/network_transport_config_parser.h"
#include "score/mw/com/gateway/transport_layer/network/network_transport.h"
#endif
#if defined(SCORE_GATEWAY_WITH_ZENOH_PICO)
#include "score/mw/com/gateway/transport_layer/network/zenoh/zenoh_pico_bus.h"
#endif
#if defined(SCORE_GATEWAY_WITH_CYCLONE_DDS)
#include "score/mw/com/gateway/transport_layer/network/dds/cyclone_dds_bus.h"
#endif

#include <cstdlib>
#include <string>
#include <string_view>

namespace score::mw::com::gateway
{
namespace
{

constexpr std::string_view kSampleHypervisorTransportId{"sample_hypervisor"};
constexpr std::string_view kQemuHypervisorTransportId{"qemu_hypervisor"};

std::unique_ptr<IBidirectionalTransport> CreateBidirectionalTransport(const std::string& transport_config_path)
{
    return std::make_unique<BidirectionalTransport>(ParseSampleTransportConfig(transport_config_path));
}

std::shared_ptr<qemu::ivshmem::IvshmemTypedMemoryProvider> CreateIvshmemProvider(std::uint32_t preferred_bar_num)
{
    std::uint64_t paddr = 0U;
    std::uint64_t size = 0U;
    if (!qemu::ivshmem::DiscoverIvshmemBar(paddr, size, preferred_bar_num))
    {
        std::terminate();
    }

    return std::make_shared<qemu::ivshmem::IvshmemTypedMemoryProvider>(paddr, size);
}

}  // namespace

std::unique_ptr<Transport> TransportFactory::Create(GatewayCore& gateway_core,
                                                    const std::string& transport_layer_id,
                                                    const std::string& transport_config_path)
{
    if (transport_layer_id == kSampleHypervisorTransportId)
    {
        return std::make_unique<SampleHyperVisorTransport>(gateway_core,
                                                           CreateBidirectionalTransport(transport_config_path));
    }

    if (transport_layer_id == kQemuHypervisorTransportId)
    {
        const auto qemu_config = qemu::ParseQemuTransportConfig(transport_config_path);
        auto ivshmem_provider = CreateIvshmemProvider(qemu_config.GetPreferredIvshmemBarNum());
        score::memory::shared::SharedMemoryFactory::SetInterVMMemoryProvider(ivshmem_provider);

        return std::make_unique<QemuHypervisorTransport>(
            gateway_core, CreateBidirectionalTransport(transport_config_path), std::move(ivshmem_provider));
    }

#if defined(SCORE_GATEWAY_WITH_ZENOH_PICO)
    if (transport_layer_id == "zenoh_pico")
    {
        auto config = network::ParseNetworkTransportConfig(transport_config_path, network::NetworkBackend::kZenohPico);
        return std::make_unique<network::NetworkTransport>(
            gateway_core, std::make_unique<network::ZenohPicoBus>(config.zenoh_pico), config.rpc, config.type_check);
    }
#endif
#if defined(SCORE_GATEWAY_WITH_CYCLONE_DDS)
    if (transport_layer_id == "cyclone_dds")
    {
        auto config =
            network::ParseNetworkTransportConfig(transport_config_path, network::NetworkBackend::kCycloneDds);
        return std::make_unique<network::NetworkTransport>(
            gateway_core, std::make_unique<network::CycloneDdsBus>(config.cyclone_dds), config.rpc, config.type_check);
    }
#endif

    // Unknown transport layer id — no implementation registered.
    std::terminate();
}

}  // namespace score::mw::com::gateway
