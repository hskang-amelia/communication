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
#ifndef SCORE_MW_COM_GATEWAY_TRANSPORT_LAYER_TRANSPORT_H
#define SCORE_MW_COM_GATEWAY_TRANSPORT_LAYER_TRANSPORT_H

#include "score/mw/com/gateway/transport_layer/transport_error.h"
#include "score/mw/com/impl/service_element_type.h"
#include "score/mw/com/types.h"
#include "score/result/result.h"

#include <score/span.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace score::mw::com::gateway
{

/// \brief Binding-agnostic view onto a single event/field sample update, used to explicitly forward payload bytes
/// between gateway instances when the transport layer does not support memory sharing (see
/// IsMemorySharingSupported()).
/// \details Deliberately holds only a raw, non-owning view onto the already-serialized sample bytes plus the
/// identifying information needed to copy them into the correct destination slot. It carries no assumption about how
/// the transport layer implementation gets these bytes across the wire.
struct SamplePayload
{
    /// \brief name of the service element (event/field) this sample belongs to.
    std::string element_name;
    /// \brief type of the service element (event, field, method). Currently only EVENT is supported.
    impl::ServiceElementType element_type;
    /// \brief non-owning view onto the sample bytes to forward. Only valid for the duration of the call it is
    /// passed to (e.g. ForwardSamples()).
    score::cpp::span<const std::byte> data;
};

/// \brief Abstract base class for gateway transport layer implementations.
class Transport
{
  public:
    Transport() = default;
    virtual ~Transport() = default;

    /// \brief Returns whether this transport implementation supports memory sharing between source and destination.
    /// \return true if shared memory transport is supported, false otherwise.
    virtual bool IsMemorySharingSupported() const = 0;

    /// \brief Sets up the transport layer (e.g. establishes connections or allocates resources).
    /// \return result indicating success or failure.
    virtual score::Result<void> Setup() = 0;

    /// \brief Shuts down the transport layer and releases all resources.
    virtual void Shutdown() = 0;

    /// \brief ProvideService API signature to be provided when IsMemorySharingSupported() == true.
    /// \details Transport layer implementation shall "forward" this call to the destination gateway, which is
    /// expected to create a (generic) skeleton for the given service instance and service elements. The (generic)
    /// skeleton created by the destination gateway in this case expects to work on existing shared memory, which is
    /// shared between the source and destination gateway. The transport layer implementation must ensure, that the
    /// shared memory is visible/accessible on the destination gateway side, when the (generic) skeleton gets created.
    /// \param service_instance_specifier instance specifier of the service to provide. It is expected, that this
    /// specifier is configured/existent in the mw_com_config.json at the destination gateway side.
    /// \param service_elements configuration of the service elements (events, fields, methods) of the service to
    /// provide. This information is needed to create the (generic) skeleton on the destination gateway side.
    /// \return result indicating success or failure.
    virtual score::Result<void> ProvideService(score::mw::com::InstanceSpecifier service_instance_specifier,
                                               std::vector<score::mw::com::EventInfo> service_elements) = 0;

    /// \brief OfferService API to trigger service-instance offering at the destination gateway side.
    /// \details Transport layer implementation shall "forward" this call to the destination gateway and trigger the
    /// offering of the service instance at the destination gateway side. The service instance is expected to be
    /// already created at the destination gateway side, e.g. by a previous call to ProvideService().
    /// \param service_instance_specifier instance specifier of the service instance to offer.
    /// \return result indicating success or failure.
    virtual score::Result<void> OfferService(score::mw::com::InstanceSpecifier service_instance_specifier) = 0;

    /// \brief StopOfferService API to trigger service-instance stop-offer at the destination gateway side.
    /// \details Transport layer implementation shall "forward" this call to the destination gateway and trigger the
    /// stop-offer of the service instance at the destination gateway side. The service instance is expected to be
    /// already created at the destination gateway side, e.g. by a previous call to ProvideService().
    /// \param service_instance_specifier instance specifier of the service instance to stop-offer.
    /// \return result indicating success or failure.
    virtual score::Result<void> StopOfferService(score::mw::com::InstanceSpecifier service_instance_specifier) = 0;

    /// \brief NotifyUpdate API to inform the destination gateway about updates of service elements of a service
    /// instance on the source gateway side.
    /// \details The transport layer implementation shall "forward" this call to the destination gateway, which is
    /// expected to trigger update notifications at the (generic) skeleton of the related service instance on the
    /// destination gateway side for the given service element. API will only be called by the source gateway, if
    /// previously the destination gateway registered for update notifications for the given service instance and
    /// service element by calling the RegisterUpdateNotification API.
    /// \param service_instance_specifier instance specifier of the service instance owning the service element, for
    /// which an update shall be notified.
    /// \param updated_element_type type of the updated service element (event, field, method). Currently only EVENT
    /// is supported.
    /// \param updated_element_name name of the updated service element.
    /// \return result indicating success or failure.
    virtual score::Result<void> NotifyUpdate(score::mw::com::InstanceSpecifier service_instance_specifier,
                                             impl::ServiceElementType updated_element_type,
                                             std::string updated_element_name) = 0;

    /// \brief API to register for update notifications for service elements of a service instance at the source
    /// gateway side.
    /// \details The transport layer implementation shall "forward" this call to the source gateway, which is expected
    /// to register for update notifications at the (generic) proxy of the related service instance on the source
    /// gateway side for the given service element.
    /// \param service_instance_specifier instance specifier of the service instance owning the service element, for
    /// which an update-notification shall be registered.
    /// \param element_type type of the service element (event, field, method). Currently only EVENT is supported.
    /// \param element_name name of the service element.
    /// \return result indicating success or failure.
    virtual score::Result<void> RegisterUpdateNotification(score::mw::com::InstanceSpecifier service_instance_specifier,
                                                           impl::ServiceElementType element_type,
                                                           std::string element_name) = 0;

    /// \brief API to unregister update notifications for service elements of a service instance at the source gateway
    /// side.
    /// \details See RegisterUpdateNotification API for the corresponding registration semantics.
    /// \param service_instance_specifier instance specifier of the service instance owning the service element, for
    /// which the update-notification shall be unregistered.
    /// \param element_type type of the service element (event, field, method). Currently only EVENT is supported.
    /// \param element_name name of the service element.
    /// \return result indicating success or failure.
    virtual score::Result<void> UnregisterUpdateNotification(
        score::mw::com::InstanceSpecifier service_instance_specifier,
        impl::ServiceElementType element_type,
        std::string element_name) = 0;

    // ---------------------------------------------------------------------------------------------------------
    // Copying-gateway APIs (IsMemorySharingSupported() == false). Not pure virtual: the existing
    // memory-sharing-only implementation (transport_layer/sample) never overrides these and keeps compiling
    // unchanged. Every method here returns TransportErrorc::kNotSupported by default (see transport.cpp) — a
    // transport that returns false from IsMemorySharingSupported() MUST override all three, or a forwarded
    // service using it will silently never actually deliver data. See this repo's README.md "Copying Gateway"
    // row, which already named this gateway type; until now nothing in this class actually supported it.
    // ---------------------------------------------------------------------------------------------------------

    /// \brief Forwards the actual sample payload for an event/field to the destination gateway, when there is no
    /// shared memory to make it visible in.
    /// \details Mirrors NotifyUpdate's addressing (same three identifying parameters) — NotifyUpdate alone is
    /// enough for a memory-sharing transport (the destination reads the update from shared memory itself), but a
    /// copying transport has no shared memory for the destination to read from, so the payload has to travel
    /// through this call instead.
    /// \param service_instance_specifier instance specifier of the service instance owning the service element.
    /// \param element_type type of the service element (event, field, method). Currently only EVENT is supported.
    /// \param element_name name of the service element whose sample is being forwarded.
    /// \param sample_data the serialized sample payload, exactly as the source-side proxy received it.
    /// \return result indicating success or failure.
    virtual score::Result<void> ForwardSampleData(score::mw::com::InstanceSpecifier service_instance_specifier,
                                                  impl::ServiceElementType element_type,
                                                  std::string element_name,
                                                  score::cpp::span<const std::uint8_t> sample_data);

    /// \brief Forwards a batch of event/field sample updates to the destination gateway, an alternative to
    /// ForwardSampleData() for a transport that wants to bulk multiple elements' updates (potentially across
    /// different service instances) into a single round-trip.
    /// \details EXPLORATORY (no concrete Transport implementation overrides this yet, this repo's DDS binding
    /// included — it still forwards one sample at a time via ForwardSampleData()). Ported from
    /// generalized_gateway/transport_layer/README.md's allocate-then-copy model (see GatewayCore::
    /// AllocateSamples()/SendOrUpdateSamples()), which that doc's own maintainer flagged as possibly out of sync
    /// with the real implementation — kept here as a starting point, not a finished design.
    /// \param service_instance_specifier instance specifier of the service instance owning the updated elements.
    /// \param samples collection of sample payloads to forward.
    /// \return result indicating success or failure.
    /// \note Default implementation returns TransportErrorc::kNotSupported. Transport implementations for which
    /// IsMemorySharingSupported() always returns true do not need to override this API.
    virtual score::Result<void> ForwardSamples(score::mw::com::InstanceSpecifier service_instance_specifier,
                                               std::vector<SamplePayload> samples)
    {
        static_cast<void>(service_instance_specifier);
        static_cast<void>(samples);
        return score::MakeUnexpected(TransportErrorc::kNotSupported);
    }

    /// \brief Subscribes for actual sample data (not just update notifications) for a service element, on a
    /// transport that doesn't support memory sharing.
    /// \details A memory-sharing transport's destination side already has direct access to the data once
    /// RegisterUpdateNotification fires; a copying transport needs an explicit subscribe/unsubscribe pair so the
    /// source side knows whether it's worth calling ForwardSampleData at all.
    /// \param service_instance_specifier instance specifier of the service instance owning the service element.
    /// \param element_type type of the service element (event, field, method). Currently only EVENT is supported.
    /// \param element_name name of the service element to subscribe to.
    /// \return result indicating success or failure.
    virtual score::Result<void> Subscribe(score::mw::com::InstanceSpecifier service_instance_specifier,
                                          impl::ServiceElementType element_type,
                                          std::string element_name);

    /// \brief Reverse of Subscribe — see its doc comment for the corresponding semantics.
    virtual score::Result<void> Unsubscribe(score::mw::com::InstanceSpecifier service_instance_specifier,
                                            impl::ServiceElementType element_type,
                                            std::string element_name);
};

}  // namespace score::mw::com::gateway

#endif  // SCORE_MW_COM_GATEWAY_TRANSPORT_LAYER_TRANSPORT_H
