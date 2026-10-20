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
#include "score/mw/com/gateway/transport_layer/network/zenoh/zenoh_pico_bus_config.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace score::mw::com::gateway::network
{

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
    // Opens the session, the step Open() retries while the peer is not reachable. Returns the zenoh-pico result.
    std::int32_t OpenSession(Handles& handles);
    BusStatus DeclareEntities(Handles& handles);

    const ZenohPicoBusConfig config_;
    FrameHandler on_frame_;
    std::unique_ptr<Handles> handles_;
    // Serializes Open/Close.
    std::mutex lifecycle_mutex_;
    // Guards handles_ and publishes_in_flight_. Not held during a put, see Publish().
    std::mutex publish_mutex_;
    std::condition_variable publish_cv_;
    std::size_t publishes_in_flight_{0U};
    // Held while the frame handler runs, so Close() can wait for an in-flight callback.
    std::mutex callback_mutex_;
    std::atomic<bool> open_{false};
};

}  // namespace score::mw::com::gateway::network

#endif  // SCORE_MW_COM_GATEWAY_TRANSPORT_LAYER_NETWORK_ZENOH_ZENOH_PICO_BUS_H
