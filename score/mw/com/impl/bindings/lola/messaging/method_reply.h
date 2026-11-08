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

#include <array>
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
class ReplyGate final
{
  public:
    /// \brief For a call from another process.
    explicit ReplyGate(score::message_passing::IServerConnection& connection) noexcept : connection_{&connection} {}
    /// \brief For a call from this process; the caller blocks in WaitLocal().
    ReplyGate() noexcept : connection_{nullptr}, local_{true} {}

    /// \return false if the connection is gone or the reply could not be sent.
    bool Deliver(const score::Result<void>& result) noexcept;
    void Close() noexcept;
    score::Result<void> WaitLocal() noexcept;

  private:
    std::mutex mutex_{};
    std::condition_variable condition_{};
    score::message_passing::IServerConnection* connection_;
    bool local_{false};
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
    explicit MethodReply(std::shared_ptr<ReplyGate> gate) noexcept : gate_{std::move(gate)} {}
    ~MethodReply() noexcept;

    MethodReply(const MethodReply&) = delete;
    MethodReply& operator=(const MethodReply&) = delete;
    MethodReply(MethodReply&& other) noexcept = default;
    MethodReply& operator=(MethodReply&& other) noexcept;

    /// \brief Sends the result of the method call. Only the first call has an effect.
    void Send(const score::Result<void>& result) noexcept;

    /// \brief Whether a reply is still owed.
    bool IsPending() const noexcept
    {
        return gate_ != nullptr;
    }

  private:
    std::shared_ptr<ReplyGate> gate_{};
};

}  // namespace score::mw::com::impl::lola

#endif  // SCORE_MW_COM_IMPL_BINDINGS_LOLA_MESSAGING_METHOD_REPLY_H
