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
#include "score/mw/com/impl/bindings/lola/messaging/method_reply.h"

#include "score/mw/com/impl/bindings/lola/methods/method_error.h"
#include "score/mw/com/impl/error_serializer.h"
#include "score/mw/log/logging.h"

#include <array>
#include <cstring>

namespace score::mw::com::impl::lola
{

std::array<std::uint8_t, sizeof(score::result::ErrorCode)> SerializeMethodReply(
    const score::Result<void>& result) noexcept
{
    using Payload = ErrorSerializer<MethodErrc>::SerializedErrorType;
    static_assert(sizeof(Payload) == sizeof(score::result::ErrorCode));
    const Payload serialized = result.has_value()
                                   ? ErrorSerializer<MethodErrc>::SerializeSuccess()
                                   : ErrorSerializer<MethodErrc>::SerializeError(static_cast<MethodErrc>(*result.error()));
    std::array<std::uint8_t, sizeof(Payload)> out{};
    // NOLINTNEXTLINE(score-banned-function) trivially copyable integer into an array of its size
    score::cpp::ignore = std::memcpy(out.data(), &serialized, sizeof(Payload));
    return out;
}

bool ReplyGate::Deliver(const score::Result<void>& result) noexcept
{
    if (local_)
    {
        {
            std::lock_guard<std::mutex> lock{mutex_};
            local_result_ = result;
        }
        condition_.notify_all();
        return true;
    }
    const auto message = SerializeMethodReply(result);
    std::lock_guard<std::mutex> lock{mutex_};
    if (connection_ == nullptr)
    {
        return false;
    }
    return connection_->Reply(message).has_value();
}

score::Result<void> ReplyGate::WaitLocal() noexcept
{
    std::unique_lock<std::mutex> lock{mutex_};
    condition_.wait(lock, [this]() {
        return local_result_.has_value();
    });
    return *local_result_;
}

void ReplyGate::Close() noexcept
{
    std::lock_guard<std::mutex> lock{mutex_};
    connection_ = nullptr;
}

MethodReply::~MethodReply() noexcept
{
    if (IsPending())
    {
        Send(MakeUnexpected(MethodErrc::kSkeletonAlreadyDestroyed));
    }
}

MethodReply& MethodReply::operator=(MethodReply&& other) noexcept
{
    if (this != &other)
    {
        if (IsPending())
        {
            Send(MakeUnexpected(MethodErrc::kSkeletonAlreadyDestroyed));
        }
        gate_ = std::move(other.gate_);
    }
    return *this;
}

void MethodReply::Send(const score::Result<void>& result) noexcept
{
    const auto gate = std::move(gate_);
    gate_.reset();
    if (gate == nullptr)
    {
        return;
    }
    if (!gate->Deliver(result))
    {
        score::mw::log::LogWarn("lola") << "Method reply not sent: the caller's connection is gone.";
    }
}

}  // namespace score::mw::com::impl::lola
