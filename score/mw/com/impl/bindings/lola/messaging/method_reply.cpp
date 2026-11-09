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

std::uint64_t ReplyGate::Begin() noexcept
{
    std::lock_guard<std::mutex> lock{mutex_};
    ++seq_;
    answered_ = false;
    deadline_.reset();
    return seq_;
}

bool ReplyGate::DeliverLocked(const score::Result<void>& result) noexcept
{
    answered_ = true;
    deadline_.reset();
    if (local_)
    {
        local_result_ = result;
        condition_.notify_all();
        return true;
    }
    if (connection_ == nullptr)
    {
        return false;
    }
    const auto message = SerializeMethodReply(result);
    return connection_->Reply(message).has_value();
}

bool ReplyGate::Deliver(const std::uint64_t seq,
                        const score::Result<void>& result,
                        score::cpp::callback<void()>* const write) noexcept
{
    std::lock_guard<std::mutex> lock{mutex_};
    if ((seq != seq_) || answered_ || ((!local_) && (connection_ == nullptr)))
    {
        return false;
    }
    if ((write != nullptr) && !write->empty())
    {
        (*write)();
    }
    return DeliverLocked(result);
}

bool ReplyGate::IsOpen(const std::uint64_t seq) noexcept
{
    std::lock_guard<std::mutex> lock{mutex_};
    return (seq == seq_) && (!answered_) && (local_ || (connection_ != nullptr));
}

void ReplyGate::ArmDeadline(const std::uint64_t seq, const Clock::time_point deadline) noexcept
{
    std::lock_guard<std::mutex> lock{mutex_};
    if ((seq == seq_) && (!answered_))
    {
        deadline_ = deadline;
    }
}

void ReplyGate::ExpireIfDue(const Clock::time_point now) noexcept
{
    std::lock_guard<std::mutex> lock{mutex_};
    if ((!answered_) && deadline_.has_value() && (now >= *deadline_))
    {
        score::mw::log::LogWarn("lola") << "Method call not answered in time; replying kReplyTimeout.";
        score::cpp::ignore = DeliverLocked(MakeUnexpected(MethodErrc::kReplyTimeout));
    }
}

score::Result<void> ReplyGate::WaitLocal(const std::chrono::milliseconds timeout) noexcept
{
    std::unique_lock<std::mutex> lock{mutex_};
    const auto done = [this]() {
        return local_result_.has_value();
    };
    if (timeout.count() == 0)
    {
        condition_.wait(lock, done);
    }
    else if (!condition_.wait_for(lock, timeout, done))
    {
        answered_ = true;
        return MakeUnexpected(MethodErrc::kReplyTimeout);
    }
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
        score::cpp::ignore = Send(MakeUnexpected(MethodErrc::kSkeletonAlreadyDestroyed));
    }
}

MethodReply& MethodReply::operator=(MethodReply&& other) noexcept
{
    if (this != &other)
    {
        if (IsPending())
        {
            score::cpp::ignore = Send(MakeUnexpected(MethodErrc::kSkeletonAlreadyDestroyed));
        }
        gate_ = std::move(other.gate_);
        seq_ = other.seq_;
    }
    return *this;
}

bool MethodReply::Send(const score::Result<void>& result) noexcept
{
    const auto gate = std::move(gate_);
    gate_.reset();
    return (gate != nullptr) && gate->Deliver(seq_, result, nullptr);
}

bool MethodReply::Complete(score::cpp::callback<void()>& write) noexcept
{
    const auto gate = std::move(gate_);
    gate_.reset();
    if (gate == nullptr)
    {
        return false;
    }
    const bool delivered = gate->Deliver(seq_, {}, &write);
    if (!delivered)
    {
        score::mw::log::LogWarn("lola") << "Method reply not sent: the call was already answered or its caller is gone.";
    }
    return delivered;
}

void MethodReply::ArmDeadline(const std::chrono::milliseconds timeout) noexcept
{
    if ((gate_ != nullptr) && (timeout.count() > 0))
    {
        gate_->ArmDeadline(seq_, ReplyGate::Clock::now() + timeout);
    }
}

bool MethodReply::IsOpen() const noexcept
{
    return (gate_ != nullptr) && gate_->IsOpen(seq_);
}

}  // namespace score::mw::com::impl::lola
