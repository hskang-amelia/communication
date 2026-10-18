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
#include "score/mw/com/gateway/transport_layer/network/zenoh/zenoh_pico_bus.h"

#include <zenoh-pico.h>

#include <thread>
#include <utility>

namespace score::mw::com::gateway::network
{

struct ZenohPicoBus::Handles
{
    z_owned_session_t session{};
    z_owned_publisher_t publisher{};
    z_owned_subscriber_t subscriber{};
    bool has_session{false};
    bool has_publisher{false};
    bool has_subscriber{false};
};

struct ZenohPicoBusAccess
{
    static void OnSample(z_loaned_sample_t* sample, void* context)
    {
        auto* const bus = static_cast<ZenohPicoBus*>(context);
        if ((bus == nullptr) || (sample == nullptr) || !bus->open_.load())
        {
            return;
        }
        z_owned_slice_t slice{};
        if (z_bytes_to_slice(z_sample_payload(sample), &slice) != Z_OK)
        {
            return;
        }
        bus->Deliver(z_slice_data(z_loan(slice)), z_slice_len(z_loan(slice)));
        z_drop(z_move(slice));
    }
};

ZenohPicoBus::ZenohPicoBus(ZenohPicoBusConfig config) : config_{std::move(config)} {}

ZenohPicoBus::~ZenohPicoBus()
{
    Close();
}

std::size_t ZenohPicoBus::LibraryMaxFrameSize() noexcept
{
    return static_cast<std::size_t>(Z_FRAG_MAX_SIZE);
}

void ZenohPicoBus::Deliver(const std::uint8_t* data, std::size_t size)
{
    std::lock_guard<std::mutex> lock{callback_mutex_};
    if (open_.load() && on_frame_)
    {
        on_frame_(data, size);
    }
}

BusStatus ZenohPicoBus::TryOpenOnce(Handles& handles)
{
    z_owned_config_t zconfig{};
    if (z_config_default(&zconfig) != Z_OK)
    {
        return BusStatus::kOpenFailed;
    }
    if (zp_config_insert(z_loan_mut(zconfig), Z_CONFIG_MODE_KEY, config_.mode.c_str()) != Z_OK)
    {
        z_drop(z_move(zconfig));
        return BusStatus::kOpenFailed;
    }
    if (!config_.connect.empty() &&
        (zp_config_insert(z_loan_mut(zconfig), Z_CONFIG_CONNECT_KEY, config_.connect.c_str()) != Z_OK))
    {
        z_drop(z_move(zconfig));
        return BusStatus::kOpenFailed;
    }
    if (!config_.listen.empty() &&
        (zp_config_insert(z_loan_mut(zconfig), Z_CONFIG_LISTEN_KEY, config_.listen.c_str()) != Z_OK))
    {
        z_drop(z_move(zconfig));
        return BusStatus::kOpenFailed;
    }
    // z_open consumes the config, also on failure.
    if (z_open(&handles.session, z_move(zconfig), nullptr) != Z_OK)
    {
        return BusStatus::kOpenFailed;
    }
    handles.has_session = true;

    z_view_keyexpr_t local_key{};
    z_view_keyexpr_t remote_key{};
    if ((z_view_keyexpr_from_str(&local_key, config_.local_key.c_str()) != Z_OK) ||
        (z_view_keyexpr_from_str(&remote_key, config_.remote_key.c_str()) != Z_OK))
    {
        return BusStatus::kOpenFailed;
    }

    z_publisher_options_t publisher_options{};
    z_publisher_options_default(&publisher_options);
    publisher_options.congestion_control = Z_CONGESTION_CONTROL_BLOCK;
    publisher_options.is_express = true;
    if (z_declare_publisher(z_loan(handles.session), &handles.publisher, z_loan(remote_key), &publisher_options) !=
        Z_OK)
    {
        return BusStatus::kOpenFailed;
    }
    handles.has_publisher = true;

    z_owned_closure_sample_t callback{};
    if (z_closure_sample(&callback, &ZenohPicoBusAccess::OnSample, nullptr, this) != Z_OK)
    {
        return BusStatus::kOpenFailed;
    }
    if (z_declare_subscriber(
            z_loan(handles.session), &handles.subscriber, z_loan(local_key), z_move(callback), nullptr) != Z_OK)
    {
        return BusStatus::kOpenFailed;
    }
    handles.has_subscriber = true;
    return BusStatus::kOk;
}

namespace
{

void Release(z_owned_session_t& session,
             z_owned_publisher_t& publisher,
             z_owned_subscriber_t& subscriber,
             bool& has_session,
             bool& has_publisher,
             bool& has_subscriber)
{
    if (has_subscriber)
    {
        z_drop(z_move(subscriber));
        has_subscriber = false;
    }
    if (has_publisher)
    {
        z_drop(z_move(publisher));
        has_publisher = false;
    }
    if (has_session)
    {
        z_drop(z_move(session));
        has_session = false;
    }
}

}  // namespace

BusStatus ZenohPicoBus::Open(FrameHandler on_frame)
{
    std::lock_guard<std::mutex> lifecycle_lock{lifecycle_mutex_};
    if (open_.load())
    {
        return BusStatus::kOk;
    }
    if (config_.local_key.empty() || config_.remote_key.empty() || (config_.local_key == config_.remote_key) ||
        (config_.max_frame_size > LibraryMaxFrameSize()))
    {
        return BusStatus::kOpenFailed;
    }
    {
        std::lock_guard<std::mutex> lock{callback_mutex_};
        on_frame_ = std::move(on_frame);
    }

    const auto deadline = std::chrono::steady_clock::now() + config_.open_timeout;
    while (true)
    {
        auto handles = std::make_unique<Handles>();
        // open_ must be true before the subscriber exists, otherwise early frames are discarded.
        open_.store(true);
        const auto status = TryOpenOnce(*handles);
        if (status == BusStatus::kOk)
        {
            std::lock_guard<std::mutex> lock{publish_mutex_};
            handles_ = std::move(handles);
            return BusStatus::kOk;
        }
        open_.store(false);
        {
            // Wait for a callback that may have slipped in before the failure.
            std::lock_guard<std::mutex> lock{callback_mutex_};
        }
        Release(handles->session,
                handles->publisher,
                handles->subscriber,
                handles->has_session,
                handles->has_publisher,
                handles->has_subscriber);
        if (std::chrono::steady_clock::now() >= deadline)
        {
            return BusStatus::kOpenFailed;
        }
        std::this_thread::sleep_for(config_.open_retry_interval);
    }
}

BusStatus ZenohPicoBus::Publish(const std::uint8_t* data, std::size_t size)
{
    if (size > config_.max_frame_size)
    {
        return BusStatus::kPublishFailed;
    }
    std::lock_guard<std::mutex> lock{publish_mutex_};
    if (!open_.load() || !handles_ || !handles_->has_publisher)
    {
        return BusStatus::kNotOpen;
    }
    z_owned_bytes_t payload{};
    if (z_bytes_copy_from_buf(&payload, data, size) != Z_OK)
    {
        return BusStatus::kPublishFailed;
    }
    // z_publisher_put consumes the payload, also on failure.
    return (z_publisher_put(z_loan(handles_->publisher), z_move(payload), nullptr) == Z_OK) ? BusStatus::kOk
                                                                                            : BusStatus::kPublishFailed;
}

void ZenohPicoBus::Close()
{
    std::lock_guard<std::mutex> lifecycle_lock{lifecycle_mutex_};
    if (!open_.exchange(false))
    {
        return;
    }
    std::unique_ptr<Handles> handles{};
    {
        std::lock_guard<std::mutex> lock{publish_mutex_};
        handles = std::move(handles_);
    }
    if (!handles)
    {
        return;
    }
    // Undeclare the subscriber first so no new callbacks start, then wait for one that is still running. A running
    // callback may publish (e.g. an Ack); Publish() sees open_ == false and returns without touching the handles.
    if (handles->has_subscriber)
    {
        z_drop(z_move(handles->subscriber));
        handles->has_subscriber = false;
    }
    {
        std::lock_guard<std::mutex> lock{callback_mutex_};
    }
    Release(handles->session,
            handles->publisher,
            handles->subscriber,
            handles->has_session,
            handles->has_publisher,
            handles->has_subscriber);
}

}  // namespace score::mw::com::gateway::network
