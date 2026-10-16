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
#ifndef SCORE_MW_COM_GATEWAY_TRANSPORT_LAYER_NETWORK_ZENOH_ZENOH_PICO_BUS_H
#define SCORE_MW_COM_GATEWAY_TRANSPORT_LAYER_NETWORK_ZENOH_ZENOH_PICO_BUS_H

#include "score/mw/com/gateway/transport_layer/network/core/message_bus.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>

namespace score::mw::com::gateway::network
{

struct ZenohPicoBusConfig
{
    /// \brief "peer" (default, no router needed) or "client" (connects to a zenoh router).
    std::string mode{"peer"};
    /// \brief Endpoint to connect to, e.g. "tcp/10.0.0.2:7447". Empty: none.
    std::string connect;
    /// \brief Endpoint to listen on, e.g. "tcp/0.0.0.0:7447". Empty: none. In peer mode one side listens and the
    /// other connects.
    std::string listen;
    /// \brief Key expression of this gateway's inbox, e.g. "score/gateway/hpc/inbox".
    std::string local_key;
    /// \brief Key expression of the peer gateway's inbox.
    std::string remote_key;
    /// \brief How long Open() keeps retrying to open the session (the listening peer may start later).
    std::chrono::milliseconds open_timeout{30000};
    std::chrono::milliseconds open_retry_interval{200};
    /// \brief Largest frame Publish() accepts. Must not exceed the Z_FRAG_MAX_SIZE zenoh-pico was built with
    /// (default build: 4096 bytes, which is too small for most event samples).
    std::size_t max_frame_size{4096U};
};

/// \brief IMessageBus on zenoh-pico: one subscriber on local_key, one publisher on remote_key.
/// \details Publishes with congestion control BLOCK so requests and acks are not dropped under load. Incoming
/// payloads are made contiguous (z_bytes_to_slice) before they are handed to the frame handler.
class ZenohPicoBus final : public IMessageBus
{
  public:
    explicit ZenohPicoBus(ZenohPicoBusConfig config);
    ~ZenohPicoBus() override;

    BusStatus Open(FrameHandler on_frame) override;
    BusStatus Publish(const std::uint8_t* data, std::size_t size) override;
    void Close() override;

    /// \brief Z_FRAG_MAX_SIZE of the linked zenoh-pico build.
    static std::size_t LibraryMaxFrameSize() noexcept;

  private:
    struct Handles;
    friend struct ZenohPicoBusAccess;

    void Deliver(const std::uint8_t* data, std::size_t size);
    BusStatus TryOpenOnce(Handles& handles);

    const ZenohPicoBusConfig config_;
    FrameHandler on_frame_;
    std::unique_ptr<Handles> handles_;
    // Serializes Open/Close.
    std::mutex lifecycle_mutex_;
    // Guards the publisher handle against Close().
    std::mutex publish_mutex_;
    // Held while the frame handler runs, so Close() can wait for an in-flight callback.
    std::mutex callback_mutex_;
    std::atomic<bool> open_{false};
};

}  // namespace score::mw::com::gateway::network

#endif  // SCORE_MW_COM_GATEWAY_TRANSPORT_LAYER_NETWORK_ZENOH_ZENOH_PICO_BUS_H
