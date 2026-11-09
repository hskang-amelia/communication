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
#ifndef SCORE_MW_COM_IMPL_BINDINGS_LOLA_MESSAGING_METHOD_REPLY_H
#define SCORE_MW_COM_IMPL_BINDINGS_LOLA_MESSAGING_METHOD_REPLY_H

#include "score/message_passing/i_server_connection.h"
#include "score/result/result.h"

#include <score/callback.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>

namespace score::mw::com::impl::lola
{

/// \brief The reply message for a method call result (the error code, or success).
std::array<std::uint8_t, sizeof(score::result::ErrorCode)> SerializeMethodReply(const score::Result<void>& result) noexcept;

/// \brief Where the result of a method call goes: back over a message passing connection, or to a caller waiting in
/// the same process.
///
/// message_passing allows IServerConnection::Reply() from any thread, but not after the connection's disconnect
/// callback. Close() is called from that callback; a reply that is in progress finishes first, later ones are dropped.
///
/// A connection carries one method call at a time. Each call gets the next sequence number from Begin(); a reply for
/// an older call, or for a call that was already answered (for instance by a timeout), is refused.
class ReplyGate final
{
  public:
    using Clock = std::chrono::steady_clock;

    /// \brief For a call from another process.
    explicit ReplyGate(score::message_passing::IServerConnection& connection) noexcept : connection_{&connection} {}
    /// \brief For a call from this process; the caller blocks in WaitLocal().
    ReplyGate() noexcept : connection_{nullptr}, local_{true} {}

    /// \brief Starts the next call on this connection.
    std::uint64_t Begin() noexcept;

    /// \brief Answers call `seq` with `result`; `write` (may be null) runs first, under the same lock, so that a call
    /// that has been answered or replaced cannot have its return value written any more.
    /// \return false if the call was already answered or replaced, the connection is gone, or the reply failed.
    bool Deliver(std::uint64_t seq, const score::Result<void>& result, score::cpp::callback<void()>* write) noexcept;

    /// \brief Whether call `seq` still waits for its answer.
    bool IsOpen(std::uint64_t seq) noexcept;

    /// \brief Call `seq` is answered with kReplyTimeout if it is still open at `deadline`.
    void ArmDeadline(std::uint64_t seq, Clock::time_point deadline) noexcept;

    /// \brief Sends the timeout answer if the armed deadline has passed.
    void ExpireIfDue(Clock::time_point now) noexcept;

    void Close() noexcept;

    /// \brief Waits for a same-process call; `timeout` zero waits without limit.
    score::Result<void> WaitLocal(std::chrono::milliseconds timeout) noexcept;

  private:
    bool DeliverLocked(const score::Result<void>& result) noexcept;

    std::mutex mutex_{};
    std::condition_variable condition_{};
    score::message_passing::IServerConnection* connection_;
    bool local_{false};
    std::uint64_t seq_{0U};
    bool answered_{true};
    std::optional<Clock::time_point> deadline_{};
    std::optional<score::Result<void>> local_result_{};
};

/// \brief The reply to one method call, which can be sent after the method call handler has returned.
///
/// Move-only. If it is destroyed without Send(), it replies kSkeletonAlreadyDestroyed, so the caller never waits for a
/// reply that will not come.
class MethodReply final
{
  public:
    MethodReply() noexcept = default;
    MethodReply(std::shared_ptr<ReplyGate> gate, std::uint64_t seq) noexcept : gate_{std::move(gate)}, seq_{seq} {}
    ~MethodReply() noexcept;

    MethodReply(const MethodReply&) = delete;
    MethodReply& operator=(const MethodReply&) = delete;
    MethodReply(MethodReply&& other) noexcept = default;
    MethodReply& operator=(MethodReply&& other) noexcept;

    /// \brief Sends the result of the method call. Only the first call has an effect.
    /// \return false if the call was no longer open (answered by a timeout, caller gone).
    bool Send(const score::Result<void>& result) noexcept;

    /// \brief Writes the return value with `write` and answers success, only while the call is still open.
    /// \return false, without calling `write`, if the call was no longer open.
    bool Complete(score::cpp::callback<void()>& write) noexcept;

    /// \brief Answers kReplyTimeout if not answered within `timeout` (zero: no limit).
    void ArmDeadline(std::chrono::milliseconds timeout) noexcept;

    /// \brief Whether the caller still waits for this reply.
    bool IsOpen() const noexcept;

    /// \brief Whether this object still owns the reply.
    bool IsPending() const noexcept
    {
        return gate_ != nullptr;
    }

  private:
    std::shared_ptr<ReplyGate> gate_{};
    std::uint64_t seq_{0U};
};

}  // namespace score::mw::com::impl::lola

#endif  // SCORE_MW_COM_IMPL_BINDINGS_LOLA_MESSAGING_METHOD_REPLY_H
