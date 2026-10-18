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
#ifndef SCORE_MW_COM_GATEWAY_TRANSPORT_LAYER_NETWORK_CORE_MESSAGE_BUS_H
#define SCORE_MW_COM_GATEWAY_TRANSPORT_LAYER_NETWORK_CORE_MESSAGE_BUS_H

#include <cstddef>
#include <cstdint>
#include <functional>

namespace score::mw::com::gateway::network
{

enum class BusStatus : std::uint8_t
{
    kOk = 0U,
    kOpenFailed,
    kNotOpen,
    kPublishFailed,
};

const char* ToString(BusStatus status) noexcept;

/// \brief Point-to-point frame channel between two gateways on top of a pub/sub middleware.
/// \details Each gateway reads from its own inbox (local key / topic) and writes to the inbox of its peer (remote
/// key / topic). The bus delivers opaque frames; ordering, acknowledgement and retries are handled by RpcChannel.
/// Implementations: ZenohPicoBus, CycloneDdsBus, and an in-memory LoopbackBus for tests.
class IMessageBus
{
  public:
    /// \brief Called from the middleware's receive thread. The buffer is only valid during the call.
    using FrameHandler = std::function<void(const std::uint8_t* data, std::size_t size)>;

    IMessageBus() = default;
    virtual ~IMessageBus() = default;
    IMessageBus(const IMessageBus&) = delete;
    IMessageBus& operator=(const IMessageBus&) = delete;
    IMessageBus(IMessageBus&&) = delete;
    IMessageBus& operator=(IMessageBus&&) = delete;

    /// \brief Connects to the middleware and starts delivering frames from the local inbox to on_frame.
    virtual BusStatus Open(FrameHandler on_frame) = 0;

    /// \brief Writes one frame to the peer's inbox. May be called from any thread, including from on_frame.
    virtual BusStatus Publish(const std::uint8_t* data, std::size_t size) = 0;

    /// \brief Stops delivery and releases middleware resources. on_frame is not called after Close() returns.
    virtual void Close() = 0;
};

}  // namespace score::mw::com::gateway::network

#endif  // SCORE_MW_COM_GATEWAY_TRANSPORT_LAYER_NETWORK_CORE_MESSAGE_BUS_H
