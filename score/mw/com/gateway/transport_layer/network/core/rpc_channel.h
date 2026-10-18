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
#ifndef SCORE_MW_COM_GATEWAY_TRANSPORT_LAYER_NETWORK_CORE_RPC_CHANNEL_H
#define SCORE_MW_COM_GATEWAY_TRANSPORT_LAYER_NETWORK_CORE_RPC_CHANNEL_H

#include "score/mw/com/gateway/transport_layer/network/core/message_bus.h"
#include "score/mw/com/gateway/transport_layer/network/core/wire_format.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace score::mw::com::gateway::network
{

enum class ChannelStatus : std::uint8_t
{
    kOk = 0U,
    kNotStarted,
    kOpenFailed,
    kEncodeFailed,
    kPublishFailed,
    kTimeout,
};

const char* ToString(ChannelStatus status) noexcept;

struct RpcChannelConfig
{
    /// \brief Time to wait for an Ack before resending a request.
    std::chrono::milliseconds ack_timeout{200};
    /// \brief Total number of sends of one request (first send + retries).
    std::uint32_t max_attempts{5U};
    /// \brief Number of recently dispatched request sequences remembered per peer session to drop resends.
    std::size_t dedupe_window{256U};
    /// \brief Inbound frames waiting for dispatch. Further notifications are dropped, requests are not acked.
    std::size_t max_dispatch_queue{1024U};
};

struct RpcChannelStatistics
{
    std::uint64_t frames_received{0U};
    std::uint64_t decode_failures{0U};
    std::uint64_t requests_dispatched{0U};
    std::uint64_t duplicates_dropped{0U};
    std::uint64_t notifications_dispatched{0U};
    std::uint64_t queue_overflows{0U};
    std::uint64_t retries{0U};
    std::uint64_t stale_acks{0U};
};

/// \brief Reliable request delivery and in-order dispatch on top of an IMessageBus.
/// \details
/// - Requests (ProvideService, OfferService, StopOfferService, (Un)RegisterNotification) are acked by the receiver
///   on reception and resent until acked (max_attempts). The receiver drops resends it already dispatched.
/// - Notifications (UpdateNotification, EventSample) are sent once.
/// - Inbound frames are dispatched on a dedicated thread, in arrival order. A handler may call SendRequest()
///   (e.g. the destination gateway re-registering notifications while handling ProvideService) without blocking
///   Ack reception, because Acks are processed on the middleware receive thread.
/// - The ack only confirms delivery, not the result of the remote GatewayCore call. This matches the sample
///   hypervisor transport; remote failures are logged on the remote side.
class RpcChannel
{
  public:
    using FrameDispatcher = std::function<void(const Frame& frame)>;

    RpcChannel(IMessageBus& bus, RpcChannelConfig config, std::uint64_t session_id);
    ~RpcChannel();
    RpcChannel(const RpcChannel&) = delete;
    RpcChannel& operator=(const RpcChannel&) = delete;
    RpcChannel(RpcChannel&&) = delete;
    RpcChannel& operator=(RpcChannel&&) = delete;

    /// \brief Opens the bus and starts the dispatch thread. dispatcher receives every non-Ack frame.
    ChannelStatus Start(FrameDispatcher dispatcher);

    /// \brief Wakes up blocked senders (they return kNotStarted), closes the bus and joins the dispatch thread.
    /// \details May be called from the dispatcher; the dispatch thread is then joined by the next Start() or by the
    /// destructor. Precondition: the channel is not destroyed from within its own dispatcher.
    void Stop();

    /// \brief Sends a request and blocks until it is acked or all attempts timed out.
    ChannelStatus SendRequest(const FrameBody& body);

    /// \brief Sends a notification once, without waiting.
    ChannelStatus SendNotification(const FrameBody& body);

    /// \brief Sends an EventSample notification straight from a sample buffer (no intermediate copy).
    ChannelStatus SendEventSample(const std::string& instance,
                                  const std::string& element,
                                  const std::uint8_t* data,
                                  std::size_t size);

    std::uint64_t SessionId() const noexcept
    {
        return session_id_;
    }

    RpcChannelStatistics Statistics() const;

    /// \brief Returns a random, non-zero session id. A restarted gateway gets a new one, which resets the peer's
    /// duplicate detection.
    static std::uint64_t GenerateSessionId();

  private:
    void OnFrame(const std::uint8_t* data, std::size_t size);
    void HandleAck(const Ack& ack);
    bool IsDuplicate(std::uint64_t session, std::uint32_t sequence);
    void SendAckFor(const FrameHeader& header);
    ChannelStatus PublishBuffer(const std::vector<std::uint8_t>& buffer);
    std::uint32_t NextSequence() noexcept;
    void DispatchLoop();

    IMessageBus& bus_;
    const RpcChannelConfig config_;
    const std::uint64_t session_id_;
    FrameDispatcher dispatcher_;

    std::atomic<bool> running_{false};
    std::atomic<std::uint32_t> next_sequence_{1U};
    std::mutex publish_mutex_;

    // Pending requests waiting for their Ack, keyed by sequence.
    std::mutex pending_mutex_;
    std::condition_variable pending_cv_;
    std::unordered_set<std::uint32_t> pending_;
    std::unordered_set<std::uint32_t> acked_;

    // Duplicate detection for inbound requests (remote session + recent sequences).
    std::mutex dedupe_mutex_;
    std::uint64_t remote_session_{0U};
    std::unordered_set<std::uint32_t> seen_;
    std::deque<std::uint32_t> seen_order_;

    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<Frame> queue_;
    bool stop_dispatch_{false};
    std::thread dispatch_thread_;

    mutable std::mutex stats_mutex_;
    RpcChannelStatistics stats_{};
};

}  // namespace score::mw::com::gateway::network

#endif  // SCORE_MW_COM_GATEWAY_TRANSPORT_LAYER_NETWORK_CORE_RPC_CHANNEL_H
