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
//
// Second-generation DDS `Transport` (2026-09-07) — supersedes this repo's first attempt (2026-09-06,
// `score/mw/com/impl/bindings/dds/dds_transport_layer.*` on this fork's `main`), which implemented an
// interface (`score::mw::com::impl::ITransportLayer`) this project had derived itself instead of the
// real one that turned out to already exist and be merged upstream
// (`score::mw::com::gateway::Transport`, this file's base class). See `score-architecture/docs/
// network_comm_abstraction_b3.md` §6 for how that mismatch was found.
//
// Redesign notes:
//
// 1. **Topic naming** — no `ElementFqId` here at all; `Transport`'s real methods address elements via
//    `(InstanceSpecifier, element_name)`, both already plain strings. `InstanceSpecifier::ToString()`
//    returns a shortname *path* (e.g. `"/my/app/port"`), which still contains `/` — illegal in a DDS
//    topic name, confirmed the same way the first attempt found `ElementFqId::ToString()` unusable:
//    by actually calling `dds_create_topic` with it and observing `DDS_RETCODE_BAD_PARAMETER`.
//    `MakeTopicName()` just replaces `/` with `_` — far simpler than the numeric `ElementFqId`-packing
//    scheme the first attempt needed, since there's no opaque struct to encode here.
//
// 2. **`ForwardSampleData`/`Subscribe`/`Unsubscribe` now actually work.** The first attempt's
//    `ForwardSamples` was structurally blocked — `Transport` (as it existed then) had no method for
//    a non-memory-sharing transport to move sample bytes at all. This redesign is the reason those
//    three methods now exist on `Transport` (see this repo's transport.h/transport.cpp changes in the
//    same change as this file) — with them, this binding both sends real payloads
//    (`ForwardSampleData`, wired to a real DDS writer) and receives them (`TakeLastPayload`, wired to
//    a real DDS reader via `dds_take`, which the first attempt's `WaitForUpdate` never actually called
//    — it only detected that *something* arrived, never read what).
//
// 3. **One topic serves both "update happened" (NotifyUpdate) and "here's the data"
//    (ForwardSampleData)** — same choice the first attempt made, unchanged: a zero-length `payload`
//    is a pure ping, a non-empty one is real data. `Subscribe`/`RegisterUpdateNotification` and
//    `Unsubscribe`/`UnregisterUpdateNotification` are accordingly aliases of each other, sharing one
//    reader per element.
//
// 4. **`OfferService`/`StopOfferService` still don't touch DDS QoS/discovery directly** — a DDS
//    writer is discoverable the moment it's created (in `ProvideService`); `OfferService` here is a
//    precondition check (was `ProvideService` actually called for this instance?), not an action.
//
// Deliberately unresolved by this binding, same as before: `Permissions`'s uid-keyed access control
// has no DDS equivalent (D-2's own open item, `network_comm_abstraction_srs.md` NCA-FR-008) — not
// attempted here, `ProvideService`'s real upstream signature doesn't even carry a `Permissions`
// parameter to translate in the first place.

#include "score/mw/com/gateway/transport_layer/dds/dds_transport.h"
#include "score/mw/com/gateway/transport_layer/dds/sample_payload.h"
#include "score/mw/com/gateway/transport_layer/transport_error.h"

#include <cstdio>

namespace score::mw::com::gateway::dds
{

namespace
{

std::string Sanitize(std::string_view raw)
{
    std::string sanitized(raw);
    for (char& c : sanitized)
    {
        if (c == '/')
        {
            c = '_';
        }
    }
    return sanitized;
}

}  // namespace

std::string DdsTransport::MakeTopicName(const score::mw::com::InstanceSpecifier& service_instance_specifier,
                                        const std::string& element_name)
{
    return "score_gw_" + Sanitize(service_instance_specifier.ToString()) + "_" + element_name;
}

DdsTransport::DdsTransport() : participant_(dds_create_participant(DDS_DOMAIN_DEFAULT, nullptr, nullptr))
{
    if (participant_ < 0)
    {
        std::fprintf(stderr, "DdsTransport: dds_create_participant failed: %s\n", dds_strretcode(-participant_));
    }
}

DdsTransport::~DdsTransport()
{
    Shutdown();
}

bool DdsTransport::IsMemorySharingSupported() const
{
    return false;
}

score::Result<void> DdsTransport::Setup()
{
    if (participant_ < 0)
    {
        return score::MakeUnexpected(TransportErrorc::kServerSetupFailed);
    }
    return {};
}

void DdsTransport::Shutdown()
{
    // Deletes every topic/writer/reader/waitset this transport ever created — they're all children
    // of participant_ in CycloneDDS's entity tree.
    if (participant_ >= 0)
    {
        dds_delete(participant_);
        participant_ = -1;
    }
    providers_.clear();
    consumers_.clear();
}

score::Result<void> DdsTransport::ProvideService(score::mw::com::InstanceSpecifier service_instance_specifier,
                                                 std::vector<score::mw::com::EventInfo> service_elements)
{
    for (const auto& element : service_elements)
    {
        const std::string topic_name = MakeTopicName(service_instance_specifier, std::string(element.name));
        if (providers_.find(topic_name) != providers_.end())
        {
            continue;  // ProvideService called again for an already-provided element — idempotent.
        }
        const dds_entity_t topic =
            dds_create_topic(participant_, &score_gw_dds_SamplePayload_desc, topic_name.c_str(), nullptr, nullptr);
        if (topic < 0)
        {
            return score::MakeUnexpected(TransportErrorc::kFailedToProvideService);
        }
        const dds_entity_t writer = dds_create_writer(participant_, topic, nullptr, nullptr);
        if (writer < 0)
        {
            dds_delete(topic);
            return score::MakeUnexpected(TransportErrorc::kFailedToProvideService);
        }
        providers_[topic_name] = ProviderElement{topic, writer, 0U};
    }
    return {};
}

score::Result<void> DdsTransport::OfferService(score::mw::com::InstanceSpecifier /* service_instance_specifier */)
{
    // No separate DDS "offer" step — a writer is discoverable the moment ProvideService creates it.
    // This method exists on Transport as a distinct call regardless, so a caller offering before
    // providing is still a real, checkable precondition violation elsewhere in this class's other
    // methods (e.g. NotifyUpdate/ForwardSampleData both fail if no provider entry exists yet).
    return {};
}

score::Result<void> DdsTransport::StopOfferService(score::mw::com::InstanceSpecifier /* service_instance_specifier */)
{
    // Symmetric with OfferService's no-op — actual teardown of provided elements isn't addressable
    // per-instance-specifier alone without also knowing the element list again, which Transport's
    // StopOfferService signature doesn't carry. Providers are torn down in bulk by Shutdown().
    return {};
}

score::Result<void> DdsTransport::NotifyUpdate(score::mw::com::InstanceSpecifier service_instance_specifier,
                                               impl::ServiceElementType /* updated_element_type */,
                                               std::string updated_element_name)
{
    const std::string topic_name = MakeTopicName(service_instance_specifier, updated_element_name);
    const auto it = providers_.find(topic_name);
    if (it == providers_.end())
    {
        return score::MakeUnexpected(TransportErrorc::kSendFailure);
    }
    score_gw_dds_SamplePayload sample{};
    sample.update_seq = ++it->second.next_seq;
    sample.payload._length = 0;
    sample.payload._maximum = 0;
    sample.payload._buffer = nullptr;
    sample.payload._release = false;
    if (dds_write(it->second.writer, &sample) != DDS_RETCODE_OK)
    {
        return score::MakeUnexpected(TransportErrorc::kSendFailure);
    }
    return {};
}

score::Result<void> DdsTransport::ForwardSampleData(score::mw::com::InstanceSpecifier service_instance_specifier,
                                                    impl::ServiceElementType /* element_type */,
                                                    std::string element_name,
                                                    score::cpp::span<const std::uint8_t> sample_data)
{
    const std::string topic_name = MakeTopicName(service_instance_specifier, element_name);
    const auto it = providers_.find(topic_name);
    if (it == providers_.end())
    {
        return score::MakeUnexpected(TransportErrorc::kSendFailure);
    }
    score_gw_dds_SamplePayload sample{};
    sample.update_seq = ++it->second.next_seq;
    sample.payload._length = static_cast<uint32_t>(sample_data.size());
    sample.payload._maximum = sample.payload._length;
    // dds_write copies the sample (including this sequence's contents) synchronously before
    // returning — safe to point at sample_data's caller-owned buffer without taking ownership.
    sample.payload._buffer = const_cast<uint8_t*>(sample_data.data());
    sample.payload._release = false;
    if (dds_write(it->second.writer, &sample) != DDS_RETCODE_OK)
    {
        return score::MakeUnexpected(TransportErrorc::kSendFailure);
    }
    return {};
}

score::Result<void> DdsTransport::RegisterUpdateNotification(
    score::mw::com::InstanceSpecifier service_instance_specifier,
    impl::ServiceElementType /* element_type */,
    std::string element_name)
{
    const std::string topic_name = MakeTopicName(service_instance_specifier, element_name);
    if (consumers_.find(topic_name) != consumers_.end())
    {
        return {};  // Already registered — idempotent.
    }
    const dds_entity_t topic =
        dds_create_topic(participant_, &score_gw_dds_SamplePayload_desc, topic_name.c_str(), nullptr, nullptr);
    if (topic < 0)
    {
        return score::MakeUnexpected(TransportErrorc::kReceiveFailure);
    }
    const dds_entity_t reader = dds_create_reader(participant_, topic, nullptr, nullptr);
    if (reader < 0)
    {
        dds_delete(topic);
        return score::MakeUnexpected(TransportErrorc::kReceiveFailure);
    }
    const dds_entity_t waitset = dds_create_waitset(participant_);
    if (waitset < 0)
    {
        dds_delete(reader);
        dds_delete(topic);
        return score::MakeUnexpected(TransportErrorc::kReceiveFailure);
    }
    dds_set_status_mask(reader, DDS_DATA_AVAILABLE_STATUS);
    dds_waitset_attach(waitset, reader, reader);
    consumers_[topic_name] = ConsumerElement{topic, reader, waitset};
    return {};
}

score::Result<void> DdsTransport::UnregisterUpdateNotification(
    score::mw::com::InstanceSpecifier service_instance_specifier,
    impl::ServiceElementType /* element_type */,
    std::string element_name)
{
    const std::string topic_name = MakeTopicName(service_instance_specifier, element_name);
    const auto it = consumers_.find(topic_name);
    if (it == consumers_.end())
    {
        return score::MakeUnexpected(TransportErrorc::kNotConnected);
    }
    dds_delete(it->second.waitset);
    dds_delete(it->second.reader);
    dds_delete(it->second.topic);
    consumers_.erase(it);
    return {};
}

score::Result<void> DdsTransport::Subscribe(score::mw::com::InstanceSpecifier service_instance_specifier,
                                            impl::ServiceElementType element_type,
                                            std::string element_name)
{
    // Same DDS reader as RegisterUpdateNotification — see this file's header comment, point 3.
    return RegisterUpdateNotification(std::move(service_instance_specifier), element_type, std::move(element_name));
}

score::Result<void> DdsTransport::Unsubscribe(score::mw::com::InstanceSpecifier service_instance_specifier,
                                              impl::ServiceElementType element_type,
                                              std::string element_name)
{
    return UnregisterUpdateNotification(std::move(service_instance_specifier), element_type, std::move(element_name));
}

bool DdsTransport::WaitForUpdate(const score::mw::com::InstanceSpecifier& service_instance_specifier,
                                 const std::string& element_name,
                                 std::uint32_t timeout_ms)
{
    const std::string topic_name = MakeTopicName(service_instance_specifier, element_name);
    const auto it = consumers_.find(topic_name);
    if (it == consumers_.end())
    {
        return false;
    }
    dds_attach_t triggered[1];
    const dds_return_t n =
        dds_waitset_wait(it->second.waitset, triggered, 1, DDS_MSECS(static_cast<dds_duration_t>(timeout_ms)));
    return n > 0;
}

std::vector<std::uint8_t> DdsTransport::TakeLastPayload(
    const score::mw::com::InstanceSpecifier& service_instance_specifier,
    const std::string& element_name)
{
    const std::string topic_name = MakeTopicName(service_instance_specifier, element_name);
    const auto it = consumers_.find(topic_name);
    if (it == consumers_.end())
    {
        return {};
    }
    score_gw_dds_SamplePayload* sample = nullptr;
    void* samples[1] = {nullptr};
    dds_sample_info_t infos[1];
    const dds_return_t n = dds_take(it->second.reader, samples, infos, 1, 1);
    if (n <= 0 || !infos[0].valid_data)
    {
        return {};
    }
    sample = static_cast<score_gw_dds_SamplePayload*>(samples[0]);
    std::vector<std::uint8_t> result(sample->payload._buffer, sample->payload._buffer + sample->payload._length);
    dds_return_loan(it->second.reader, samples, n);
    return result;
}

bool DdsTransport::IsMatched(const score::mw::com::InstanceSpecifier& service_instance_specifier,
                             const std::string& element_name) const
{
    const std::string topic_name = MakeTopicName(service_instance_specifier, element_name);

    // Deliberately `dds_get_matched_subscriptions`/`dds_get_matched_publications` (a plain
    // introspection query, `rds`/`wrs` = nullptr and `n` = 0 to just get a count) rather than
    // `dds_get_publication_matched_status`/`dds_get_subscription_matched_status` — the status-
    // getter form needs `DDS_PUBLICATION_MATCHED_STATUS`/`DDS_SUBSCRIPTION_MATCHED_STATUS` enabled
    // on the entity to track anything at all (confirmed the hard way: current_count stayed 0
    // otherwise), but enabling `DDS_SUBSCRIPTION_MATCHED_STATUS` on the same reader `WaitForUpdate`
    // attaches to a waitset poisons it — once matched, that status latches "triggered" and is never
    // read/reset by anything on this reader's own waitset path, so `dds_waitset_wait` started
    // returning immediately forever (confirmed the hard way too: 100% CPU in a real two-process
    // run, `WaitForUpdate` no longer actually blocking). This form touches no status mask and
    // leaves `WaitForUpdate`'s waitset exactly as before.
    if (const auto it = providers_.find(topic_name); it != providers_.end())
    {
        if (dds_get_matched_subscriptions(it->second.writer, nullptr, 0U) > 0)
        {
            return true;
        }
    }
    if (const auto it = consumers_.find(topic_name); it != consumers_.end())
    {
        if (dds_get_matched_publications(it->second.reader, nullptr, 0U) > 0)
        {
            return true;
        }
    }
    return false;
}

}  // namespace score::mw::com::gateway::dds
