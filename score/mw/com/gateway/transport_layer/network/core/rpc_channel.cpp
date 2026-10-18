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
#include "score/mw/com/gateway/transport_layer/network/core/rpc_channel.h"

#include <random>
#include <utility>

namespace score::mw::com::gateway::network
{

const char* ToString(BusStatus status) noexcept
{
    switch (status)
    {
        case BusStatus::kOk:
            return "ok";
        case BusStatus::kOpenFailed:
            return "bus open failed";
        case BusStatus::kNotOpen:
            return "bus not open";
        case BusStatus::kPublishFailed:
            return "bus publish failed";
        default:
            return "unknown bus status";
    }
}

const char* ToString(ChannelStatus status) noexcept
{
    switch (status)
    {
        case ChannelStatus::kOk:
            return "ok";
        case ChannelStatus::kNotStarted:
            return "channel not started";
        case ChannelStatus::kOpenFailed:
            return "bus open failed";
        case ChannelStatus::kEncodeFailed:
            return "frame encoding failed";
        case ChannelStatus::kPublishFailed:
            return "bus publish failed";
        case ChannelStatus::kTimeout:
            return "no ack received";
        default:
            return "unknown channel status";
    }
}

RpcChannel::RpcChannel(IMessageBus& bus, RpcChannelConfig config, std::uint64_t session_id)
    : bus_{bus}, config_{config}, session_id_{session_id}
{
}

RpcChannel::~RpcChannel()
{
    Stop();
}

std::uint64_t RpcChannel::GenerateSessionId()
{
    std::random_device device{};
    std::mt19937_64 engine{(static_cast<std::uint64_t>(device()) << 32U) ^ static_cast<std::uint64_t>(device())};
    std::uint64_t id{0U};
    while (id == 0U)
    {
        id = engine();
    }
    return id;
}

ChannelStatus RpcChannel::Start(FrameDispatcher dispatcher)
{
    if (running_.load())
    {
        return ChannelStatus::kOk;
    }
    if (dispatch_thread_.joinable())
    {
        // Left over from a Stop() that was called on the dispatch thread itself; that thread has exited by now.
        dispatch_thread_.join();
    }
    dispatcher_ = std::move(dispatcher);
    {
        std::lock_guard<std::mutex> lock{queue_mutex_};
        stop_dispatch_ = false;
        queue_.clear();
    }
    dispatch_thread_ = std::thread{[this]() {
        DispatchLoop();
    }};

    running_.store(true);
    const auto open_status = bus_.Open([this](const std::uint8_t* data, std::size_t size) {
        OnFrame(data, size);
    });
    if (open_status != BusStatus::kOk)
    {
        Stop();
        return ChannelStatus::kOpenFailed;
    }
    return ChannelStatus::kOk;
}

void RpcChannel::Stop()
{
    const bool was_running = running_.exchange(false);
    {
        std::lock_guard<std::mutex> lock{pending_mutex_};
        pending_cv_.notify_all();
    }
    if (was_running)
    {
        bus_.Close();
    }
    {
        std::lock_guard<std::mutex> lock{queue_mutex_};
        stop_dispatch_ = true;
    }
    queue_cv_.notify_all();
    // Called from inside the dispatcher (a handler shutting the transport down), the thread cannot join itself. It
    // returns from DispatchLoop() right after the handler; Start() or the destructor takes care of it.
    if (dispatch_thread_.joinable() && (dispatch_thread_.get_id() != std::this_thread::get_id()))
    {
        dispatch_thread_.join();
    }
}

std::uint32_t RpcChannel::NextSequence() noexcept
{
    std::uint32_t sequence = next_sequence_.fetch_add(1U);
    if (sequence == 0U)
    {
        sequence = next_sequence_.fetch_add(1U);
    }
    return sequence;
}

ChannelStatus RpcChannel::PublishBuffer(const std::vector<std::uint8_t>& buffer)
{
    std::lock_guard<std::mutex> lock{publish_mutex_};
    return (bus_.Publish(buffer.data(), buffer.size()) == BusStatus::kOk) ? ChannelStatus::kOk
                                                                           : ChannelStatus::kPublishFailed;
}

ChannelStatus RpcChannel::SendRequest(const FrameBody& body)
{
    if (!running_.load())
    {
        return ChannelStatus::kNotStarted;
    }
    const std::uint32_t sequence = NextSequence();
    std::vector<std::uint8_t> buffer{};
    EncodeError encode_error{EncodeError::kNone};
    if (!Encode(FrameHeader{KindOf(body), session_id_, sequence}, body, buffer, encode_error))
    {
        return ChannelStatus::kEncodeFailed;
    }

    {
        std::lock_guard<std::mutex> lock{pending_mutex_};
        pending_.insert(sequence);
    }

    ChannelStatus result{ChannelStatus::kTimeout};
    for (std::uint32_t attempt = 0U; attempt < config_.max_attempts; ++attempt)
    {
        if (attempt > 0U)
        {
            std::lock_guard<std::mutex> lock{stats_mutex_};
            ++stats_.retries;
        }
        // A publish error can be transient (e.g. a reconnecting session): keep retrying until attempts run out and
        // report what happened in the last attempt.
        const bool published = (PublishBuffer(buffer) == ChannelStatus::kOk);

        std::unique_lock<std::mutex> lock{pending_mutex_};
        static_cast<void>(pending_cv_.wait_for(lock, config_.ack_timeout, [this, sequence]() {
            return (acked_.count(sequence) != 0U) || !running_.load();
        }));
        if (acked_.count(sequence) != 0U)
        {
            result = ChannelStatus::kOk;
            break;
        }
        if (!running_.load())
        {
            result = ChannelStatus::kNotStarted;
            break;
        }
        result = published ? ChannelStatus::kTimeout : ChannelStatus::kPublishFailed;
    }

    std::lock_guard<std::mutex> lock{pending_mutex_};
    pending_.erase(sequence);
    acked_.erase(sequence);
    return result;
}

ChannelStatus RpcChannel::SendNotification(const FrameBody& body)
{
    if (!running_.load())
    {
        return ChannelStatus::kNotStarted;
    }
    std::vector<std::uint8_t> buffer{};
    EncodeError encode_error{EncodeError::kNone};
    if (!Encode(FrameHeader{KindOf(body), session_id_, NextSequence()}, body, buffer, encode_error))
    {
        return ChannelStatus::kEncodeFailed;
    }
    return PublishBuffer(buffer);
}

ChannelStatus RpcChannel::SendEventSample(const std::string& instance,
                                          const std::string& element,
                                          const std::uint8_t* data,
                                          std::size_t size)
{
    if (!running_.load())
    {
        return ChannelStatus::kNotStarted;
    }
    std::vector<std::uint8_t> buffer{};
    EncodeError encode_error{EncodeError::kNone};
    if (!EncodeEventSample(session_id_, NextSequence(), instance, element, data, size, buffer, encode_error))
    {
        return ChannelStatus::kEncodeFailed;
    }
    return PublishBuffer(buffer);
}

void RpcChannel::OnFrame(const std::uint8_t* data, std::size_t size)
{
    {
        std::lock_guard<std::mutex> lock{stats_mutex_};
        ++stats_.frames_received;
    }
    if (!running_.load())
    {
        return;
    }

    auto decoded = Decode(data, size);
    if (!decoded.frame.has_value())
    {
        std::lock_guard<std::mutex> lock{stats_mutex_};
        ++stats_.decode_failures;
        return;
    }
    Frame& frame = decoded.frame.value();

    // Frames we sent ourselves never reach us on a point-to-point inbox, but guard against loops (e.g. a
    // misconfiguration where local and remote inbox are the same).
    if (frame.header.session == session_id_)
    {
        return;
    }

    if (const auto* ack = std::get_if<Ack>(&frame.body))
    {
        HandleAck(*ack);
        return;
    }

    const bool needs_ack = RequiresAck(frame.header.kind);
    std::lock_guard<std::mutex> dedupe_lock{dedupe_mutex_};
    if (needs_ack && IsDuplicate(frame.header.session, frame.header.sequence))
    {
        // Our earlier Ack got lost: ack again, but do not dispatch twice.
        SendAckFor(frame.header);
        std::lock_guard<std::mutex> lock{stats_mutex_};
        ++stats_.duplicates_dropped;
        return;
    }

    const FrameHeader header = frame.header;
    {
        std::lock_guard<std::mutex> lock{queue_mutex_};
        if (queue_.size() >= config_.max_dispatch_queue)
        {
            // No Ack: the sender resends the request later. Notifications are lost.
            std::lock_guard<std::mutex> stats_lock{stats_mutex_};
            ++stats_.queue_overflows;
            return;
        }
    }

    if (needs_ack)
    {
        if (remote_session_ != header.session)
        {
            remote_session_ = header.session;
            seen_.clear();
            seen_order_.clear();
        }
        seen_.insert(header.sequence);
        seen_order_.push_back(header.sequence);
        while (seen_order_.size() > config_.dedupe_window)
        {
            seen_.erase(seen_order_.front());
            seen_order_.pop_front();
        }
        // Ack before the frame becomes visible to the dispatcher: a handler may stop the channel (and close the
        // bus) right away, which would otherwise swallow this Ack.
        SendAckFor(header);
    }

    {
        // Only this receive path grows the queue, so the capacity check above still holds (dispatch only shrinks it).
        std::lock_guard<std::mutex> lock{queue_mutex_};
        queue_.push_back(std::move(frame));
    }
    queue_cv_.notify_one();
}

bool RpcChannel::IsDuplicate(std::uint64_t session, std::uint32_t sequence)
{
    return (session == remote_session_) && (seen_.count(sequence) != 0U);
}

void RpcChannel::SendAckFor(const FrameHeader& header)
{
    std::vector<std::uint8_t> buffer{};
    EncodeError encode_error{EncodeError::kNone};
    const FrameBody ack{Ack{header.session, header.sequence}};
    if (Encode(FrameHeader{FrameKind::kAck, session_id_, 0U}, ack, buffer, encode_error))
    {
        static_cast<void>(PublishBuffer(buffer));
    }
}

void RpcChannel::HandleAck(const Ack& ack)
{
    if (ack.acked_session != session_id_)
    {
        std::lock_guard<std::mutex> lock{stats_mutex_};
        ++stats_.stale_acks;
        return;
    }
    {
        std::lock_guard<std::mutex> lock{pending_mutex_};
        if (pending_.count(ack.acked_sequence) == 0U)
        {
            std::lock_guard<std::mutex> stats_lock{stats_mutex_};
            ++stats_.stale_acks;
            return;
        }
        acked_.insert(ack.acked_sequence);
    }
    pending_cv_.notify_all();
}

void RpcChannel::DispatchLoop()
{
    while (true)
    {
        Frame frame{};
        {
            std::unique_lock<std::mutex> lock{queue_mutex_};
            queue_cv_.wait(lock, [this]() {
                return stop_dispatch_ || !queue_.empty();
            });
            if (stop_dispatch_)
            {
                return;
            }
            frame = std::move(queue_.front());
            queue_.pop_front();
        }
        {
            std::lock_guard<std::mutex> lock{stats_mutex_};
            if (RequiresAck(frame.header.kind))
            {
                ++stats_.requests_dispatched;
            }
            else
            {
                ++stats_.notifications_dispatched;
            }
        }
        if (dispatcher_)
        {
            dispatcher_(frame);
        }
    }
}

RpcChannelStatistics RpcChannel::Statistics() const
{
    std::lock_guard<std::mutex> lock{stats_mutex_};
    return stats_;
}

}  // namespace score::mw::com::gateway::network
