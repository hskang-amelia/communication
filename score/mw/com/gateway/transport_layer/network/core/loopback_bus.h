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
#ifndef SCORE_MW_COM_GATEWAY_TRANSPORT_LAYER_NETWORK_CORE_LOOPBACK_BUS_H
#define SCORE_MW_COM_GATEWAY_TRANSPORT_LAYER_NETWORK_CORE_LOOPBACK_BUS_H

#include "score/mw/com/gateway/transport_layer/network/core/message_bus.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace score::mw::com::gateway::network
{

/// \brief In-memory IMessageBus pair for tests. Frames are delivered asynchronously on a per-endpoint receive
/// thread (like a real middleware). A fault hook can drop or duplicate frames to exercise retries.
class LoopbackBus final : public IMessageBus
{
  public:
    enum class Fault : std::uint8_t
    {
        kDeliver,
        kDrop,
        kDuplicate,
    };
    /// \brief Decides the fate of each frame published by this endpoint.
    using FaultHook = std::function<Fault(const std::uint8_t* data, std::size_t size)>;

    /// \brief Creates two connected endpoints.
    static std::pair<std::unique_ptr<LoopbackBus>, std::unique_ptr<LoopbackBus>> CreatePair();

    ~LoopbackBus() override;

    BusStatus Open(FrameHandler on_frame) override;
    BusStatus Publish(const std::uint8_t* data, std::size_t size) override;
    void Close() override;

    void SetFaultHook(FaultHook hook);

  private:
    LoopbackBus() = default;
    void Enqueue(std::vector<std::uint8_t> frame);
    void ReceiveLoop();

    LoopbackBus* peer_{nullptr};
    FrameHandler on_frame_;
    std::mutex hook_mutex_;
    FaultHook fault_hook_;

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::vector<std::uint8_t>> inbox_;
    bool open_{false};
    bool stop_{false};
    std::thread receive_thread_;
};

}  // namespace score::mw::com::gateway::network

#endif  // SCORE_MW_COM_GATEWAY_TRANSPORT_LAYER_NETWORK_CORE_LOOPBACK_BUS_H
