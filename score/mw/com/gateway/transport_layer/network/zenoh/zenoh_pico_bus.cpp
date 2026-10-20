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

#include "score/mw/log/logging.h"

#include <zenoh-pico.h>

#include <cstdint>
#include <string_view>
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

namespace
{

constexpr std::string_view kLogContext{"gwnt"};

/// \brief Builds the zenoh-pico session config. Returns the setting zenoh-pico rejected, empty on success.
std::string_view BuildConfig(const ZenohPicoBusConfig& config, z_owned_config_t& zconfig)
{
    if (z_config_default(&zconfig) != Z_OK)
    {
        return "default config";
    }
    std::string_view rejected{};
    if (zp_config_insert(z_loan_mut(zconfig), Z_CONFIG_MODE_KEY, config.mode.c_str()) != Z_OK)
    {
        rejected = "mode";
    }
    else if (!config.connect.empty() &&
             (zp_config_insert(z_loan_mut(zconfig), Z_CONFIG_CONNECT_KEY, config.connect.c_str()) != Z_OK))
    {
        rejected = "connect";
    }
    else if (!config.listen.empty() &&
             (zp_config_insert(z_loan_mut(zconfig), Z_CONFIG_LISTEN_KEY, config.listen.c_str()) != Z_OK))
    {
        rejected = "listen";
    }
    if (!rejected.empty())
    {
        z_drop(z_move(zconfig));
    }
    return rejected;
}

/// \brief z_open errors that a retry cannot fix, e.g. a malformed or unsupported locator: the _Z_ERR_CONFIG_* range,
/// which ends right above _Z_ERR_SCOUT_NO_RESULTS in zenoh-pico 1.x.
bool IsConfigError(std::int32_t result)
{
    return (result >= _Z_ERR_CONFIG_FAILED_INSERT) && (result < _Z_ERR_SCOUT_NO_RESULTS);
}

bool IsValidKeyExpr(const std::string& key)
{
    z_view_keyexpr_t view{};
    return z_view_keyexpr_from_str(&view, key.c_str()) == Z_OK;
}

/// \brief Checks what does not change between open attempts, so Open() only retries reaching the peer.
bool ValidateConfig(const ZenohPicoBusConfig& config)
{
    if (config.local_key.empty() || config.remote_key.empty() || (config.local_key == config.remote_key))
    {
        score::mw::log::LogError(kLogContext) << "ZenohPicoBus: local-key and remote-key must be set and differ";
        return false;
    }
    if (config.max_frame_size > ZenohPicoBus::LibraryMaxFrameSize())
    {
        score::mw::log::LogError(kLogContext)
            << "ZenohPicoBus: max-frame-size" << config.max_frame_size << "exceeds Z_FRAG_MAX_SIZE"
            << ZenohPicoBus::LibraryMaxFrameSize() << "of the linked zenoh-pico";
        return false;
    }
    if (!IsValidKeyExpr(config.local_key))
    {
        score::mw::log::LogError(kLogContext)
            << "ZenohPicoBus: invalid key expression in local-key:" << config.local_key;
        return false;
    }
    if (!IsValidKeyExpr(config.remote_key))
    {
        score::mw::log::LogError(kLogContext)
            << "ZenohPicoBus: invalid key expression in remote-key:" << config.remote_key;
        return false;
    }
    if ((config.mode != "peer") && (config.mode != "client"))
    {
        score::mw::log::LogError(kLogContext) << "ZenohPicoBus: mode must be peer or client, not" << config.mode;
        return false;
    }
    z_owned_config_t zconfig{};
    const std::string_view rejected = BuildConfig(config, zconfig);
    if (!rejected.empty())
    {
        score::mw::log::LogError(kLogContext) << "ZenohPicoBus: zenoh-pico rejected the" << rejected << "setting";
        return false;
    }
    z_drop(z_move(zconfig));
    return true;
}

}  // namespace

std::int32_t ZenohPicoBus::OpenSession(Handles& handles)
{
    z_owned_config_t zconfig{};
    if (!BuildConfig(config_, zconfig).empty())
    {
        return _Z_ERR_CONFIG_FAILED_INSERT;
    }
    // z_open consumes the config, also on failure.
    const z_result_t result = z_open(&handles.session, z_move(zconfig), nullptr);
    if (result != Z_OK)
    {
        return result;
    }
    handles.has_session = true;
#if (ZENOH_PICO_MAJOR == 1) && (ZENOH_PICO_MINOR < 7) && (Z_FEATURE_MULTI_THREAD == 1)
    // Before zenoh-pico 1.7, z_open does not start the read and lease tasks. z_drop of the session stops them.
    if ((zp_start_read_task(z_loan_mut(handles.session), nullptr) != Z_OK) ||
        (zp_start_lease_task(z_loan_mut(handles.session), nullptr) != Z_OK))
    {
        score::mw::log::LogError(kLogContext) << "ZenohPicoBus: could not start the zenoh-pico read and lease tasks";
        return _Z_ERR_SYSTEM_TASK_FAILED;
    }
#endif
    return Z_OK;
}

BusStatus ZenohPicoBus::DeclareEntities(Handles& handles)
{
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
        score::mw::log::LogError(kLogContext)
            << "ZenohPicoBus: could not declare the publisher on" << config_.remote_key;
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
        score::mw::log::LogError(kLogContext)
            << "ZenohPicoBus: could not declare the subscriber on" << config_.local_key;
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
    if (!ValidateConfig(config_))
    {
        return BusStatus::kOpenFailed;
    }
    {
        std::lock_guard<std::mutex> lock{callback_mutex_};
        on_frame_ = std::move(on_frame);
    }

    const auto deadline = std::chrono::steady_clock::now() + config_.open_timeout;
    auto handles = std::make_unique<Handles>();
    while (true)
    {
        const std::int32_t result = OpenSession(*handles);
        if (result == Z_OK)
        {
            break;
        }
        Release(handles->session,
                handles->publisher,
                handles->subscriber,
                handles->has_session,
                handles->has_publisher,
                handles->has_subscriber);
        if (IsConfigError(result))
        {
            score::mw::log::LogError(kLogContext)
                << "ZenohPicoBus: zenoh-pico rejected the session config (mode, connect, listen), error" << result;
            return BusStatus::kOpenFailed;
        }
        if (std::chrono::steady_clock::now() >= deadline)
        {
            score::mw::log::LogError(kLogContext)
                << "ZenohPicoBus: could not open a zenoh-pico session within" << config_.open_timeout.count() << "ms";
            return BusStatus::kOpenFailed;
        }
        std::this_thread::sleep_for(config_.open_retry_interval);
    }

    // open_ must be true before the subscriber exists, otherwise early frames are discarded.
    open_.store(true);
    if (DeclareEntities(*handles) == BusStatus::kOk)
    {
        std::lock_guard<std::mutex> lock{publish_mutex_};
        handles_ = std::move(handles);
        return BusStatus::kOk;
    }
    open_.store(false);
    // Undeclare the subscriber first, then wait for a callback that may have slipped in before the failure.
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
    return BusStatus::kOpenFailed;
}

BusStatus ZenohPicoBus::Publish(const std::uint8_t* data, std::size_t size)
{
    if (size > config_.max_frame_size)
    {
        return BusStatus::kPublishFailed;
    }
    const z_loaned_publisher_t* publisher{nullptr};
    {
        std::lock_guard<std::mutex> lock{publish_mutex_};
        if (!open_.load() || !handles_ || !handles_->has_publisher)
        {
            return BusStatus::kNotOpen;
        }
        publisher = z_loan(handles_->publisher);
        ++publishes_in_flight_;
    }
    // Not under publish_mutex_: zenoh-pico holds its own lock while it runs the subscriber callback, which publishes
    // Acks, so holding publish_mutex_ across the put would invert the lock order. Close() waits for this put.
    BusStatus status{BusStatus::kPublishFailed};
    z_owned_bytes_t payload{};
    if (z_bytes_copy_from_buf(&payload, data, size) == Z_OK)
    {
        // z_publisher_put consumes the payload, also on failure.
        status =
            (z_publisher_put(publisher, z_move(payload), nullptr) == Z_OK) ? BusStatus::kOk : BusStatus::kPublishFailed;
    }
    {
        std::lock_guard<std::mutex> lock{publish_mutex_};
        if (--publishes_in_flight_ == 0U)
        {
            publish_cv_.notify_all();
        }
    }
    return status;
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
        // Publish() sees open_ == false from here on; wait for puts that started before.
        std::unique_lock<std::mutex> lock{publish_mutex_};
        publish_cv_.wait(lock, [this]() {
            return publishes_in_flight_ == 0U;
        });
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
