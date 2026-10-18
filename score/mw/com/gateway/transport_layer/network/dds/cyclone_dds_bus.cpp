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
#include "score/mw/com/gateway/transport_layer/network/dds/cyclone_dds_bus.h"

#include "gateway_frame.h"

#include <dds/dds.h>

#include <array>
#include <limits>
#include <thread>
#include <utility>

namespace score::mw::com::gateway::network
{

struct CycloneDdsBusAccess
{
    static void OnDataAvailable(dds_entity_t reader, void* context)
    {
        auto* const bus = static_cast<CycloneDdsBus*>(context);
        if (bus != nullptr)
        {
            bus->TakeAll(reader);
        }
    }
};

namespace
{

constexpr std::size_t kTakeBatch{16U};

dds_qos_t* CreateQos(const CycloneDdsBusConfig& config)
{
    dds_qos_t* const qos = dds_create_qos();
    dds_qset_reliability(qos, DDS_RELIABILITY_RELIABLE, DDS_MSECS(config.max_blocking_time.count()));
    dds_qset_durability(qos, DDS_DURABILITY_VOLATILE);
    dds_qset_history(qos, DDS_HISTORY_KEEP_LAST, config.history_depth);
    return qos;
}

bool IsMatched(dds_entity_t reader, dds_entity_t writer)
{
    dds_publication_matched_status_t publication{};
    dds_subscription_matched_status_t subscription{};
    if ((dds_get_publication_matched_status(writer, &publication) != DDS_RETCODE_OK) ||
        (dds_get_subscription_matched_status(reader, &subscription) != DDS_RETCODE_OK))
    {
        return false;
    }
    return (publication.current_count > 0U) && (subscription.current_count > 0U);
}

}  // namespace

CycloneDdsBus::CycloneDdsBus(CycloneDdsBusConfig config) : config_{std::move(config)} {}

CycloneDdsBus::~CycloneDdsBus()
{
    Close();
}

void CycloneDdsBus::TakeAll(std::int32_t reader)
{
    std::lock_guard<std::mutex> lock{callback_mutex_};
    if (!open_.load())
    {
        return;
    }
    while (true)
    {
        std::array<void*, kTakeBatch> samples{};
        std::array<dds_sample_info_t, kTakeBatch> infos{};
        // samples[0] == nullptr: Cyclone loans the sample memory, returned below.
        const dds_return_t taken = dds_take(reader, samples.data(), infos.data(), kTakeBatch, kTakeBatch);
        if (taken <= 0)
        {
            return;
        }
        const auto count = static_cast<std::size_t>(taken);
        for (std::size_t i = 0U; i < count; ++i)
        {
            if (infos[i].valid_data && on_frame_)
            {
                const auto* const frame = static_cast<const score_gateway_Frame*>(samples[i]);
                on_frame_(frame->data._buffer, frame->data._length);
            }
        }
        static_cast<void>(dds_return_loan(reader, samples.data(), taken));
        if (count < kTakeBatch)
        {
            return;
        }
    }
}

BusStatus CycloneDdsBus::Open(FrameHandler on_frame)
{
    std::lock_guard<std::mutex> lifecycle_lock{lifecycle_mutex_};
    if (open_.load())
    {
        return BusStatus::kOk;
    }
    if (config_.local_topic.empty() || config_.remote_topic.empty() || (config_.local_topic == config_.remote_topic) ||
        (config_.history_depth <= 0))
    {
        return BusStatus::kOpenFailed;
    }
    {
        std::lock_guard<std::mutex> lock{callback_mutex_};
        on_frame_ = std::move(on_frame);
    }

    const auto domain_id = static_cast<dds_domainid_t>(config_.domain_id);
    if (!config_.cyclone_config_xml.empty())
    {
        domain_ = dds_create_domain(domain_id, config_.cyclone_config_xml.c_str());
        if (domain_ < 0)
        {
            domain_ = 0;
            return BusStatus::kOpenFailed;
        }
    }

    participant_ = dds_create_participant(domain_id, nullptr, nullptr);
    if (participant_ < 0)
    {
        participant_ = 0;
        ReleaseEntities();
        return BusStatus::kOpenFailed;
    }

    const dds_entity_t local_topic =
        dds_create_topic(participant_, &score_gateway_Frame_desc, config_.local_topic.c_str(), nullptr, nullptr);
    const dds_entity_t remote_topic =
        dds_create_topic(participant_, &score_gateway_Frame_desc, config_.remote_topic.c_str(), nullptr, nullptr);
    if ((local_topic < 0) || (remote_topic < 0))
    {
        ReleaseEntities();
        return BusStatus::kOpenFailed;
    }

    dds_qos_t* const qos = CreateQos(config_);
    dds_listener_t* const listener = dds_create_listener(this);
    dds_lset_data_available(listener, &CycloneDdsBusAccess::OnDataAvailable);

    writer_ = dds_create_writer(participant_, remote_topic, qos, nullptr);
    // open_ must be true before the reader exists, otherwise early frames are discarded.
    open_.store(true);
    reader_ = dds_create_reader(participant_, local_topic, qos, listener);
    dds_delete_listener(listener);
    dds_delete_qos(qos);

    if ((writer_ < 0) || (reader_ < 0))
    {
        open_.store(false);
        writer_ = (writer_ < 0) ? 0 : writer_;
        reader_ = (reader_ < 0) ? 0 : reader_;
        ReleaseEntities();
        return BusStatus::kOpenFailed;
    }

    if (config_.wait_for_peer.count() > 0)
    {
        const auto deadline = std::chrono::steady_clock::now() + config_.wait_for_peer;
        while (!IsMatched(reader_, writer_) && (std::chrono::steady_clock::now() < deadline))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
    }
    return BusStatus::kOk;
}

BusStatus CycloneDdsBus::Publish(const std::uint8_t* data, std::size_t size)
{
    if (size > std::numeric_limits<std::uint32_t>::max())
    {
        return BusStatus::kPublishFailed;
    }
    std::lock_guard<std::mutex> lock{publish_mutex_};
    if (!open_.load() || (writer_ <= 0))
    {
        return BusStatus::kNotOpen;
    }
    score_gateway_Frame frame{};
    // The writer serializes synchronously, so the caller's buffer is borrowed, not copied or released.
    frame.data._buffer = const_cast<std::uint8_t*>(data);
    frame.data._length = static_cast<std::uint32_t>(size);
    frame.data._maximum = static_cast<std::uint32_t>(size);
    frame.data._release = false;
    return (dds_write(writer_, &frame) == DDS_RETCODE_OK) ? BusStatus::kOk : BusStatus::kPublishFailed;
}

void CycloneDdsBus::Close()
{
    std::lock_guard<std::mutex> lifecycle_lock{lifecycle_mutex_};
    if (!open_.exchange(false))
    {
        return;
    }
    // Deleting the reader waits for a running listener. That listener may publish an Ack; Publish() sees
    // open_ == false and returns without touching the writer.
    if (reader_ > 0)
    {
        static_cast<void>(dds_delete(reader_));
        reader_ = 0;
    }
    {
        std::lock_guard<std::mutex> lock{callback_mutex_};
    }
    std::lock_guard<std::mutex> lock{publish_mutex_};
    ReleaseEntities();
}

void CycloneDdsBus::ReleaseEntities() noexcept
{
    // Deleting the participant (or domain) deletes all its topics, readers and writers.
    if (participant_ > 0)
    {
        static_cast<void>(dds_delete(participant_));
    }
    if (domain_ > 0)
    {
        static_cast<void>(dds_delete(domain_));
    }
    participant_ = 0;
    domain_ = 0;
    reader_ = 0;
    writer_ = 0;
}

}  // namespace score::mw::com::gateway::network
