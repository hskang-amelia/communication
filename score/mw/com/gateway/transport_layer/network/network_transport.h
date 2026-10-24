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
#ifndef SCORE_MW_COM_GATEWAY_TRANSPORT_LAYER_NETWORK_NETWORK_TRANSPORT_H
#define SCORE_MW_COM_GATEWAY_TRANSPORT_LAYER_NETWORK_NETWORK_TRANSPORT_H

#include "score/mw/com/gateway/gateway_application/gateway_core.h"
#include "score/mw/com/gateway/transport_layer/network/core/message_bus.h"
#include "score/mw/com/gateway/transport_layer/network/core/rpc_channel.h"
#include "score/mw/com/gateway/transport_layer/network/core/type_id_check.h"
#include "score/mw/com/gateway/transport_layer/transport.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string_view>

namespace score::mw::com::gateway::network
{

/// \brief Copying-gateway Transport (IsMemorySharingSupported() == false) over a network middleware.
/// \details Carries the complete gateway protocol, not only sample data: ProvideService, OfferService,
/// StopOfferService and (Un)RegisterUpdateNotification are delivered to the peer's GatewayCore as acked requests;
/// UpdateNotification and ForwardSampleData as notifications. Inbound frames are dispatched to the local GatewayCore
/// in arrival order on RpcChannel's dispatch thread. The middleware is chosen by the IMessageBus passed in
/// (ZenohPicoBus, CycloneDdsBus), see TransportFactory ids "zenoh_pico" and "cyclone_dds".
///
/// ProvideService and OfferService wait for the destination's RequestResult (RpcChannelConfig::result_timeout) after
/// the delivery Ack and return:
/// - TransportErrorc::kRejectedByPeer: permanent, the destination's configuration does not allow the service (type id
///   check, invalid or non-whitelisted instance specifier);
/// - TransportErrorc::kPeerRequestFailed: the destination's GatewayCore failed, or no result arrived in time.
/// The caller must not hold a lock that the local dispatch thread (GatewayCore calls for inbound frames) may need:
/// the peer may be waiting the same way for a request that this gateway dispatches.
///
/// Type ids (TypeIdConfig): ProvideService carries the configured type id of each event. The destination compares
/// them with its own configuration and rejects the service if one event fails the check (TypeIdRejections() counts
/// these; the reason is logged at the destination).
///
/// Received samples are pushed into GatewayCore::ReceiveSampleData.
class NetworkTransport final : public Transport
{
  public:
    NetworkTransport(GatewayCore& gateway_core,
                     std::unique_ptr<IMessageBus> bus,
                     RpcChannelConfig rpc_config,
                     TypeIdConfig type_ids = {});
    ~NetworkTransport() override;

    NetworkTransport(const NetworkTransport&) = delete;
    NetworkTransport& operator=(const NetworkTransport&) = delete;
    NetworkTransport(NetworkTransport&&) = delete;
    NetworkTransport& operator=(NetworkTransport&&) = delete;

    bool IsMemorySharingSupported() const override;
    score::Result<void> Setup() override;
    void Shutdown() override;

    score::Result<void> ProvideService(score::mw::com::InstanceSpecifier service_instance_specifier,
                                       std::vector<score::mw::com::EventInfo> service_elements) override;
    score::Result<void> OfferService(score::mw::com::InstanceSpecifier service_instance_specifier) override;
    score::Result<void> StopOfferService(score::mw::com::InstanceSpecifier service_instance_specifier) override;
    score::Result<void> NotifyUpdate(score::mw::com::InstanceSpecifier service_instance_specifier,
                                     impl::ServiceElementType updated_element_type,
                                     std::string updated_element_name) override;
    score::Result<void> RegisterUpdateNotification(score::mw::com::InstanceSpecifier service_instance_specifier,
                                                   impl::ServiceElementType element_type,
                                                   std::string element_name) override;
    score::Result<void> UnregisterUpdateNotification(score::mw::com::InstanceSpecifier service_instance_specifier,
                                                     impl::ServiceElementType element_type,
                                                     std::string element_name) override;

    score::Result<void> ForwardSampleData(score::mw::com::InstanceSpecifier service_instance_specifier,
                                          impl::ServiceElementType element_type,
                                          std::string element_name,
                                          score::cpp::span<const std::uint8_t> sample_data) override;

    RpcChannelStatistics Statistics() const;

    /// \brief Number of ProvideService requests rejected because of the type id check.
    std::uint64_t TypeIdRejections() const noexcept;

    /// \brief Number of inbound frames dropped because they referenced a field or a method (events only).
    std::uint64_t UnsupportedElementRejections() const noexcept;

  private:
    void Dispatch(const Frame& frame);
    bool AcceptsElement(const ElementRef& ref, std::string_view what);
    bool CheckTypeIds(const network::ProvideService& request);
    RequestStatus HandleProvideService(const network::ProvideService& request);
    RequestStatus HandleOfferService(const network::OfferService& request);
    void ReportResult(const FrameHeader& request, RequestStatus status, std::string_view what);

    GatewayCore& gateway_core_;
    const TypeIdConfig type_ids_;
    std::atomic<std::uint64_t> type_id_rejections_{0U};
    std::atomic<std::uint64_t> unsupported_element_rejections_{0U};
    // Declared before channel_: the channel references the bus and must be destroyed first.
    std::unique_ptr<IMessageBus> bus_;
    RpcChannel channel_;
};

}  // namespace score::mw::com::gateway::network

#endif  // SCORE_MW_COM_GATEWAY_TRANSPORT_LAYER_NETWORK_NETWORK_TRANSPORT_H
