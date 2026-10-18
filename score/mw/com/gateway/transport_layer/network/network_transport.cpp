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

#include "score/mw/com/gateway/gateway_application/gateway_error.h"
#include "score/mw/com/gateway/transport_layer/transport_error.h"
#include "score/mw/log/logging.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace score::mw::com::gateway::network
{
namespace
{

constexpr std::string_view kLogContext{"gwnt"};

score::Result<void> ToResult(ChannelStatus status)
{
    switch (status)
    {
        case ChannelStatus::kOk:
            return {};
        case ChannelStatus::kNotStarted:
            return score::MakeUnexpected(TransportErrorc::kNotConnected);
        case ChannelStatus::kOpenFailed:
            return score::MakeUnexpected(TransportErrorc::kClientSetupFailed);
        case ChannelStatus::kEncodeFailed:
            return score::MakeUnexpected(TransportErrorc::kInvalidMessage);
        case ChannelStatus::kPublishFailed:
            return score::MakeUnexpected(TransportErrorc::kSendFailure);
        case ChannelStatus::kTimeout:
            return score::MakeUnexpected(TransportErrorc::kTimeout);
        case ChannelStatus::kNoResult:
            return score::MakeUnexpected(TransportErrorc::kPeerRequestFailed, "no result from the peer");
        default:
            return score::MakeUnexpected(TransportErrorc::kSendFailure);
    }
}

/// Result of a request that the peer answers with a RequestResult.
score::Result<void> ToResult(ChannelStatus channel_status, RequestStatus request_status)
{
    if (channel_status != ChannelStatus::kOk)
    {
        return ToResult(channel_status);
    }
    switch (request_status)
    {
        case RequestStatus::kOk:
            return {};
        case RequestStatus::kRejected:
            return score::MakeUnexpected(TransportErrorc::kRejectedByPeer);
        case RequestStatus::kFailed:
        default:
            return score::MakeUnexpected(TransportErrorc::kPeerRequestFailed);
    }
}

bool ToWireKind(impl::ServiceElementType type, WireElementKind& kind) noexcept
{
    switch (type)
    {
        case impl::ServiceElementType::EVENT:
            kind = WireElementKind::kEvent;
            return true;
        case impl::ServiceElementType::FIELD:
            kind = WireElementKind::kField;
            return true;
        case impl::ServiceElementType::METHOD:
            kind = WireElementKind::kMethod;
            return true;
        case impl::ServiceElementType::INVALID:
        default:
            return false;
    }
}

impl::ServiceElementType FromWireKind(WireElementKind kind) noexcept
{
    switch (kind)
    {
        case WireElementKind::kEvent:
            return impl::ServiceElementType::EVENT;
        case WireElementKind::kField:
            return impl::ServiceElementType::FIELD;
        case WireElementKind::kMethod:
            return impl::ServiceElementType::METHOD;
        default:
            return impl::ServiceElementType::INVALID;
    }
}

std::string ToWireInstance(const score::mw::com::InstanceSpecifier& specifier)
{
    return std::string{specifier.ToString()};
}

score::Result<ElementRef> MakeRef(const score::mw::com::InstanceSpecifier& specifier,
                                  impl::ServiceElementType type,
                                  std::string element)
{
    ElementRef ref{};
    if (!ToWireKind(type, ref.kind))
    {
        return score::MakeUnexpected(TransportErrorc::kNotSupported);
    }
    ref.instance = ToWireInstance(specifier);
    ref.element = std::move(element);
    return ref;
}

/// Calls f(specifier) for the InstanceSpecifier of an inbound frame; logs and drops the frame if it is invalid.
template <typename F>
void WithInstance(const std::string& instance, std::string_view what, F&& f)
{
    auto result = score::mw::com::InstanceSpecifier::Create(std::string{instance});
    if (!result.has_value())
    {
        score::mw::log::LogError(kLogContext) << "NetworkTransport: invalid instance specifier in" << what;
        return;
    }
    std::forward<F>(f)(std::move(result).value());
}

void LogIfFailed(const score::Result<void>& result, std::string_view what)
{
    if (!result.has_value())
    {
        score::mw::log::LogError(kLogContext)
            << "NetworkTransport: GatewayCore" << what << "failed:" << result.error().Message();
    }
}

/// Maps the GatewayCore result of a request to the status reported to the peer.
RequestStatus ToRequestStatus(const score::Result<void>& result, std::string_view what)
{
    LogIfFailed(result, what);
    if (result.has_value())
    {
        return RequestStatus::kOk;
    }
    // Not listed under expected-received-services: retrying cannot help.
    return (result.error() == GatewayErrorc::kNonWhitelistedService) ? RequestStatus::kRejected
                                                                     : RequestStatus::kFailed;
}

}  // namespace

NetworkTransport::NetworkTransport(GatewayCore& gateway_core,
                                   std::unique_ptr<IMessageBus> bus,
                                   RpcChannelConfig rpc_config,
                                   TypeIdConfig type_ids)
    : gateway_core_{gateway_core},
      type_ids_{std::move(type_ids)},
      bus_{std::move(bus)},
      channel_{*bus_, rpc_config, RpcChannel::GenerateSessionId()}
{
}

NetworkTransport::~NetworkTransport()
{
    Shutdown();
}

bool NetworkTransport::IsMemorySharingSupported() const
{
    return false;
}

score::Result<void> NetworkTransport::Setup()
{
    const auto status = channel_.Start([this](const Frame& frame) {
        Dispatch(frame);
    });
    if (status != ChannelStatus::kOk)
    {
        score::mw::log::LogError(kLogContext)
            << "NetworkTransport: setup failed:" << std::string_view{ToString(status)};
        return score::MakeUnexpected(TransportErrorc::kServerSetupFailed);
    }
    if (type_ids_.policy == TypeIdPolicy::kWarn)
    {
        score::mw::log::LogWarn(kLogContext)
            << "NetworkTransport: type-check policy \"warn\": events without a type id are accepted. Use for "
               "development only; a layout change of such an event is not detected.";
    }
    return {};
}

void NetworkTransport::Shutdown()
{
    channel_.Stop();
}

score::Result<void> NetworkTransport::ProvideService(score::mw::com::InstanceSpecifier service_instance_specifier,
                                                     std::vector<score::mw::com::EventInfo> service_elements)
{
    network::ProvideService request{};
    request.instance = ToWireInstance(service_instance_specifier);
    request.events.reserve(service_elements.size());
    for (const auto& element : service_elements)
    {
        std::string name{element.name};
        std::string type_id{type_ids_.Find(request.instance, name)};
        request.events.push_back(EventMeta{std::move(name),
                                           static_cast<std::uint64_t>(element.data_type_meta_info.size),
                                           static_cast<std::uint64_t>(element.data_type_meta_info.alignment),
                                           std::move(type_id)});
    }
    RequestStatus status{RequestStatus::kOk};
    const auto channel_status = channel_.SendRequestForResult(request, status);
    return ToResult(channel_status, status);
}

score::Result<void> NetworkTransport::OfferService(score::mw::com::InstanceSpecifier service_instance_specifier)
{
    RequestStatus status{RequestStatus::kOk};
    const auto channel_status =
        channel_.SendRequestForResult(network::OfferService{ToWireInstance(service_instance_specifier)}, status);
    return ToResult(channel_status, status);
}

score::Result<void> NetworkTransport::StopOfferService(score::mw::com::InstanceSpecifier service_instance_specifier)
{
    return ToResult(channel_.SendRequest(network::StopOfferService{ToWireInstance(service_instance_specifier)}));
}

score::Result<void> NetworkTransport::NotifyUpdate(score::mw::com::InstanceSpecifier service_instance_specifier,
                                                   impl::ServiceElementType updated_element_type,
                                                   std::string updated_element_name)
{
    auto ref = MakeRef(service_instance_specifier, updated_element_type, std::move(updated_element_name));
    if (!ref.has_value())
    {
        return score::MakeUnexpected(TransportErrorc::kNotSupported);
    }
    return ToResult(channel_.SendNotification(UpdateNotification{std::move(ref).value()}));
}

score::Result<void> NetworkTransport::RegisterUpdateNotification(
    score::mw::com::InstanceSpecifier service_instance_specifier,
    impl::ServiceElementType element_type,
    std::string element_name)
{
    auto ref = MakeRef(service_instance_specifier, element_type, std::move(element_name));
    if (!ref.has_value())
    {
        return score::MakeUnexpected(TransportErrorc::kNotSupported);
    }
    return ToResult(channel_.SendRequest(RegisterNotification{std::move(ref).value()}));
}

score::Result<void> NetworkTransport::UnregisterUpdateNotification(
    score::mw::com::InstanceSpecifier service_instance_specifier,
    impl::ServiceElementType element_type,
    std::string element_name)
{
    auto ref = MakeRef(service_instance_specifier, element_type, std::move(element_name));
    if (!ref.has_value())
    {
        return score::MakeUnexpected(TransportErrorc::kNotSupported);
    }
    return ToResult(channel_.SendRequest(UnregisterNotification{std::move(ref).value()}));
}

score::Result<void> NetworkTransport::ForwardSampleData(score::mw::com::InstanceSpecifier service_instance_specifier,
                                                        impl::ServiceElementType element_type,
                                                        std::string element_name,
                                                        score::cpp::span<const std::uint8_t> sample_data)
{
    if (element_type != impl::ServiceElementType::EVENT)
    {
        return score::MakeUnexpected(TransportErrorc::kNotSupported);
    }
    return ToResult(channel_.SendEventSample(
        ToWireInstance(service_instance_specifier), element_name, sample_data.data(), sample_data.size()));
}

RpcChannelStatistics NetworkTransport::Statistics() const
{
    return channel_.Statistics();
}

std::uint64_t NetworkTransport::TypeIdRejections() const noexcept
{
    return type_id_rejections_.load();
}

bool NetworkTransport::CheckTypeIds(const network::ProvideService& request)
{
    bool accepted{true};
    for (const auto& event : request.events)
    {
        const auto& local = type_ids_.Find(request.instance, event.name);
        const auto verdict = CompareTypeId(local, event.type_id);
        if (verdict == TypeIdVerdict::kMatch)
        {
            continue;
        }
        const bool event_accepted = IsAccepted(verdict, type_ids_.policy);
        auto stream = event_accepted ? score::mw::log::LogWarn(kLogContext) : score::mw::log::LogError(kLogContext);
        stream << "NetworkTransport: ProvideService" << std::string_view{request.instance} << "event"
               << std::string_view{event.name} << ":" << std::string_view{ToString(verdict)} << "(local"
               << std::string_view{local} << ", peer" << std::string_view{event.type_id} << ")"
               << (event_accepted ? std::string_view{"- accepted"} : std::string_view{"- service not provided"});
        accepted = accepted && event_accepted;
    }
    return accepted;
}

RequestStatus NetworkTransport::HandleProvideService(const network::ProvideService& request)
{
    if (!CheckTypeIds(request))
    {
        type_id_rejections_.fetch_add(1U);
        return RequestStatus::kRejected;
    }
    // Stays kRejected if the instance specifier is invalid.
    RequestStatus status{RequestStatus::kRejected};
    WithInstance(request.instance, "ProvideService", [this, &request, &status](score::mw::com::InstanceSpecifier spec) {
        // EventInfo::name is a string_view into the frame, which outlives this call.
        std::vector<score::mw::com::EventInfo> elements{};
        elements.reserve(request.events.size());
        for (const auto& event : request.events)
        {
            elements.push_back(score::mw::com::impl::EventInfo{
                std::string_view{event.name},
                score::mw::com::impl::DataTypeMetaInfo{static_cast<std::size_t>(event.size),
                                                       static_cast<std::size_t>(event.alignment)}});
        }
        status = ToRequestStatus(gateway_core_.ProvideService(std::move(spec), std::move(elements)), "ProvideService");
    });
    return status;
}

RequestStatus NetworkTransport::HandleOfferService(const network::OfferService& request)
{
    RequestStatus status{RequestStatus::kRejected};
    WithInstance(request.instance, "OfferService", [this, &status](score::mw::com::InstanceSpecifier spec) {
        status = ToRequestStatus(gateway_core_.OfferService(std::move(spec)), "OfferService");
    });
    return status;
}

void NetworkTransport::ReportResult(const FrameHeader& request, RequestStatus status, std::string_view what)
{
    const auto channel_status = channel_.SendResult(request, status);
    if (channel_status != ChannelStatus::kOk)
    {
        // The source's wait for this result ends with a timeout.
        score::mw::log::LogWarn(kLogContext) << "NetworkTransport: could not report the result of" << what
                                             << "to the peer:" << std::string_view{ToString(channel_status)};
    }
}

void NetworkTransport::Dispatch(const Frame& frame)
{
    if (const auto* provide = std::get_if<network::ProvideService>(&frame.body))
    {
        ReportResult(frame.header, HandleProvideService(*provide), "ProvideService");
    }
    else if (const auto* offer = std::get_if<network::OfferService>(&frame.body))
    {
        ReportResult(frame.header, HandleOfferService(*offer), "OfferService");
    }
    else if (const auto* stop = std::get_if<network::StopOfferService>(&frame.body))
    {
        WithInstance(stop->instance, "StopOfferService", [this](score::mw::com::InstanceSpecifier spec) {
            gateway_core_.StopOfferService(std::move(spec));
        });
    }
    else if (const auto* reg = std::get_if<RegisterNotification>(&frame.body))
    {
        WithInstance(reg->ref.instance, "RegisterNotification", [this, reg](score::mw::com::InstanceSpecifier spec) {
            LogIfFailed(gateway_core_.RegisterUpdateNotification(
                            std::move(spec), FromWireKind(reg->ref.kind), std::string{reg->ref.element}),
                        "RegisterUpdateNotification");
        });
    }
    else if (const auto* unreg = std::get_if<UnregisterNotification>(&frame.body))
    {
        WithInstance(
            unreg->ref.instance, "UnregisterNotification", [this, unreg](score::mw::com::InstanceSpecifier spec) {
                LogIfFailed(gateway_core_.UnregisterUpdateNotification(
                                std::move(spec), FromWireKind(unreg->ref.kind), std::string{unreg->ref.element}),
                            "UnregisterUpdateNotification");
            });
    }
    else if (const auto* update = std::get_if<UpdateNotification>(&frame.body))
    {
        WithInstance(
            update->ref.instance, "UpdateNotification", [this, update](score::mw::com::InstanceSpecifier spec) {
                LogIfFailed(gateway_core_.NotifyUpdate(
                                std::move(spec), FromWireKind(update->ref.kind), std::string{update->ref.element}),
                            "NotifyUpdate");
            });
    }
    else if (const auto* sample = std::get_if<EventSample>(&frame.body))
    {
        WithInstance(sample->instance, "EventSample", [this, sample](score::mw::com::InstanceSpecifier spec) {
            LogIfFailed(gateway_core_.ReceiveSampleData(
                            std::move(spec),
                            impl::ServiceElementType::EVENT,
                            std::string{sample->element},
                            score::cpp::span<const std::uint8_t>{sample->payload.data(), sample->payload.size()}),
                        "ReceiveSampleData");
        });
    }
}

}  // namespace score::mw::com::gateway::network
