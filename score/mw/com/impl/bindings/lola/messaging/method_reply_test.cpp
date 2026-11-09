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

#include <gtest/gtest.h>

#include <cstring>
#include <chrono>
#include <thread>
#include <vector>

namespace score::mw::com::impl::lola
{
namespace
{

class FakeConnection final : public score::message_passing::IServerConnection
{
  public:
    const score::message_passing::ClientIdentity& GetClientIdentity() const& noexcept override
    {
        return identity_;
    }
    score::message_passing::UserData& GetUserData() noexcept override
    {
        return user_data_;
    }
    score::cpp::expected_blank<score::os::Error> Reply(score::cpp::span<const std::uint8_t> message) noexcept override
    {
        replies_.emplace_back(message.begin(), message.end());
        return {};
    }
    score::cpp::expected_blank<score::os::Error> Notify(score::cpp::span<const std::uint8_t>) noexcept override
    {
        return {};
    }
    void RequestDisconnect() noexcept override {}

    std::vector<std::vector<std::uint8_t>> replies_{};

  private:
    score::message_passing::ClientIdentity identity_{};
    score::message_passing::UserData user_data_{};
};

MethodErrc ErrorOf(const std::vector<std::uint8_t>& reply)
{
    ErrorSerializer<MethodErrc>::SerializedErrorType serialized{};
    std::memcpy(&serialized, reply.data(), sizeof(serialized));
    const auto result = ErrorSerializer<MethodErrc>::Deserialize(serialized);
    return result.has_value() ? MethodErrc::kInvalid : static_cast<MethodErrc>(*result.error());
}

TEST(MethodReplyTest, SendRepliesOnceOverTheConnection)
{
    FakeConnection connection{};
    auto reply_gate = std::make_shared<ReplyGate>(connection);
    MethodReply reply{reply_gate, reply_gate->Begin()};
    ASSERT_TRUE(reply.IsPending());

    reply.Send({});
    reply.Send(MakeUnexpected(MethodErrc::kNotOffered));

    EXPECT_FALSE(reply.IsPending());
    ASSERT_EQ(connection.replies_.size(), 1U);
    EXPECT_EQ(connection.replies_[0], (std::vector<std::uint8_t>(sizeof(score::result::ErrorCode), 0U)));
}

TEST(MethodReplyTest, DroppedReplyAnswersSkeletonAlreadyDestroyed)
{
    FakeConnection connection{};
    {
        auto reply_gate = std::make_shared<ReplyGate>(connection);
    MethodReply reply{reply_gate, reply_gate->Begin()};
    }
    ASSERT_EQ(connection.replies_.size(), 1U);
    EXPECT_EQ(ErrorOf(connection.replies_[0]), MethodErrc::kSkeletonAlreadyDestroyed);
}

TEST(MethodReplyTest, MovedFromReplyDoesNotReply)
{
    FakeConnection connection{};
    auto first_gate = std::make_shared<ReplyGate>(connection);
    MethodReply first{first_gate, first_gate->Begin()};
    MethodReply second{std::move(first)};
    EXPECT_FALSE(first.IsPending());
    second.Send({});
    EXPECT_EQ(connection.replies_.size(), 1U);
}

TEST(MethodReplyTest, ReplyAfterCloseIsDropped)
{
    FakeConnection connection{};
    auto gate = std::make_shared<ReplyGate>(connection);
    MethodReply reply{gate, gate->Begin()};
    gate->Close();
    reply.Send({});
    EXPECT_TRUE(connection.replies_.empty());
    EXPECT_FALSE(reply.IsPending());
}

TEST(MethodReplyTest, LocalGateHandsResultToWaitingThread)
{
    auto gate = std::make_shared<ReplyGate>();
    MethodReply reply{gate, gate->Begin()};
    std::thread sender{[reply = std::move(reply)]() mutable {
        reply.Send(MakeUnexpected(MethodErrc::kNotOffered));
    }};
    const auto result = gate->WaitLocal(std::chrono::milliseconds{0});
    sender.join();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(static_cast<MethodErrc>(*result.error()), MethodErrc::kNotOffered);
}

TEST(MethodReplyTest, ReplyForAnOlderCallIsRefused)
{
    FakeConnection connection{};
    auto gate = std::make_shared<ReplyGate>(connection);
    MethodReply first{gate, gate->Begin()};
    gate->ExpireIfDue(ReplyGate::Clock::now());  // no deadline armed: nothing happens
    EXPECT_TRUE(connection.replies_.empty());
    first.ArmDeadline(std::chrono::milliseconds{1});
    gate->ExpireIfDue(ReplyGate::Clock::now() + std::chrono::milliseconds{5});
    ASSERT_EQ(connection.replies_.size(), 1U);
    EXPECT_EQ(ErrorOf(connection.replies_[0]), MethodErrc::kReplyTimeout);

    MethodReply second{gate, gate->Begin()};
    bool written{false};
    score::cpp::callback<void()> write{[&written]() {
        written = true;
    }};
    EXPECT_FALSE(first.Complete(write));  // late: answered by the timeout and replaced by the second call
    EXPECT_FALSE(written);
    EXPECT_TRUE(second.IsOpen());
    EXPECT_TRUE(second.Complete(write));
    EXPECT_TRUE(written);
    ASSERT_EQ(connection.replies_.size(), 2U);
    EXPECT_EQ(connection.replies_[1], (std::vector<std::uint8_t>(sizeof(score::result::ErrorCode), 0U)));
}

TEST(MethodReplyTest, CompleteAfterCloseDoesNotWrite)
{
    FakeConnection connection{};
    auto gate = std::make_shared<ReplyGate>(connection);
    MethodReply reply{gate, gate->Begin()};
    gate->Close();
    EXPECT_FALSE(reply.IsOpen());
    bool written{false};
    score::cpp::callback<void()> write{[&written]() {
        written = true;
    }};
    EXPECT_FALSE(reply.Complete(write));
    EXPECT_FALSE(written);
    EXPECT_TRUE(connection.replies_.empty());
}

TEST(MethodReplyTest, LocalWaitTimesOut)
{
    auto gate = std::make_shared<ReplyGate>();
    MethodReply reply{gate, gate->Begin()};
    const auto result = gate->WaitLocal(std::chrono::milliseconds{20});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(static_cast<MethodErrc>(*result.error()), MethodErrc::kReplyTimeout);
    EXPECT_FALSE(reply.Send({}));
}

}  // namespace
}  // namespace score::mw::com::impl::lola
