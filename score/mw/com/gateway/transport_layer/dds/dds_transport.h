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
#ifndef SCORE_MW_COM_GATEWAY_TRANSPORT_LAYER_DDS_DDS_TRANSPORT_H
#define SCORE_MW_COM_GATEWAY_TRANSPORT_LAYER_DDS_DDS_TRANSPORT_H

#include "score/mw/com/gateway/transport_layer/transport.h"

#include <dds/dds.h>

#include <cstdint>
#include <map>
#include <string>

namespace score::mw::com::gateway::dds
{

/// \brief A real Eclipse CycloneDDS-backed `Transport` implementation.
/// \details `IsMemorySharingSupported()` returns `false` — DDS is a network protocol between
/// domains that, by construction, cannot see into each other's shared memory. This is exactly the
/// "Copying Gateway" case `Transport::ForwardSampleData`/`Subscribe`/`Unsubscribe` were added for
/// (see this repo's design-notes.md and `score-architecture/docs/network_comm_abstraction_b3.md`
/// §8) — before that extension, a DDS `Transport` could not be built at all: the base class had no
/// way to move actual sample bytes across a non-memory-sharing boundary.
///
/// One DDS topic per `(InstanceSpecifier, element_name)` pair, named by
/// `MakeTopicName()` — see that function's doc comment for the naming scheme and why it replaces
/// this repo's earlier (abandoned) `ElementFqId`-based attempt.
class DdsTransport final : public Transport
{
  public:
    DdsTransport();
    ~DdsTransport() override;

    DdsTransport(const DdsTransport&) = delete;
    DdsTransport& operator=(const DdsTransport&) = delete;
    DdsTransport(DdsTransport&&) = delete;
    DdsTransport& operator=(DdsTransport&&) = delete;

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

    // ---- Copying-gateway APIs (this transport's whole reason for existing) ----

    score::Result<void> ForwardSampleData(score::mw::com::InstanceSpecifier service_instance_specifier,
                                          impl::ServiceElementType element_type,
                                          std::string element_name,
                                          score::cpp::span<const std::uint8_t> sample_data) override;
    score::Result<void> Subscribe(score::mw::com::InstanceSpecifier service_instance_specifier,
                                  impl::ServiceElementType element_type,
                                  std::string element_name) override;
    score::Result<void> Unsubscribe(score::mw::com::InstanceSpecifier service_instance_specifier,
                                    impl::ServiceElementType element_type,
                                    std::string element_name) override;

    /// \brief Binding-specific extension, not part of `Transport`: blocks (up to `timeout_ms`) until a
    /// sample or update-only ping actually arrives on the given element's reader.
    /// \details `Transport`'s abstract interface has no method for a consumer to actually observe an
    /// incoming notification/sample — `RegisterUpdateNotification`/`Subscribe` only set up the
    /// registration, they don't block waiting for data. Every real binding has to solve this
    /// somehow; this is CycloneDDS's answer (a waitset), analogous to this repo's earlier `impl::
    /// ITransportLayer` prototype's `WaitForUpdate` extension for the same reason.
    /// \return `true` if a sample/ping was observed before the timeout, `false` on timeout.
    bool WaitForUpdate(const score::mw::com::InstanceSpecifier& service_instance_specifier,
                       const std::string& element_name,
                       std::uint32_t timeout_ms);

    /// \brief Binding-specific extension: reads the last actual payload received for a subscribed
    /// element (empty if the last sample was a zero-length `NotifyUpdate` ping, or nothing has
    /// arrived yet).
    std::vector<std::uint8_t> TakeLastPayload(const score::mw::com::InstanceSpecifier& service_instance_specifier,
                                              const std::string& element_name);

  private:
    struct ProviderElement
    {
        dds_entity_t topic{0};
        dds_entity_t writer{0};
        std::uint64_t next_seq{0};
    };
    struct ConsumerElement
    {
        dds_entity_t topic{0};
        dds_entity_t reader{0};
        dds_entity_t waitset{0};
    };

    /// \brief Builds this element's DDS topic name from its `(InstanceSpecifier, element_name)`
    /// identity.
    /// \details `InstanceSpecifier::ToString()` returns a shortname *path* (e.g.
    /// `"/my/app/port"`), which contains `/` — illegal in a DDS topic name (confirmed the same way
    /// this repo's earlier `ElementFqId::ToString()` attempt was: by actually calling
    /// `dds_create_topic` with it and observing `DDS_RETCODE_BAD_PARAMETER`). Replaces every `/`
    /// with `_` and concatenates the element name, which — unlike the old `ElementFqId`-packing
    /// scheme this repo's earlier attempt used — is already a plain identifier-safe string, so no
    /// further encoding is needed for it.
    static std::string MakeTopicName(const score::mw::com::InstanceSpecifier& service_instance_specifier,
                                     const std::string& element_name);

    dds_entity_t participant_{0};
    std::map<std::string, ProviderElement> providers_;
    std::map<std::string, ConsumerElement> consumers_;
};

}  // namespace score::mw::com::gateway::dds

#endif  // SCORE_MW_COM_GATEWAY_TRANSPORT_LAYER_DDS_DDS_TRANSPORT_H
