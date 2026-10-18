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
#include "score/mw/com/gateway/transport_layer/network/core/wire_format.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace score::mw::com::gateway::network
{
namespace
{

std::vector<std::uint8_t> EncodeOrFail(const FrameBody& body, std::uint64_t session = 7U, std::uint32_t seq = 42U)
{
    std::vector<std::uint8_t> out{};
    EncodeError error{EncodeError::kNone};
    EXPECT_TRUE(Encode(FrameHeader{FrameKind::kAck, session, seq}, body, out, error));
    EXPECT_EQ(error, EncodeError::kNone);
    return out;
}

Frame DecodeOrFail(const std::vector<std::uint8_t>& bytes)
{
    auto result = Decode(bytes.data(), bytes.size());
    EXPECT_EQ(result.error, DecodeError::kNone) << ToString(result.error);
    EXPECT_TRUE(result.frame.has_value());
    return result.frame.value_or(Frame{});
}

TEST(WireFormatTest, HeaderIsLittleEndianAndKindComesFromBody)
{
    const auto bytes = EncodeOrFail(OfferService{"a"}, 0x0102030405060708U, 0x0A0B0C0DU);
    ASSERT_GE(bytes.size(), kHeaderSize);
    EXPECT_EQ(bytes[0], 0x53U);
    EXPECT_EQ(bytes[1], 0x47U);
    EXPECT_EQ(bytes[2], kWireVersion);
    EXPECT_EQ(bytes[3], static_cast<std::uint8_t>(FrameKind::kOfferService));
    EXPECT_EQ(bytes[4], 0x08U);
    EXPECT_EQ(bytes[11], 0x01U);
    EXPECT_EQ(bytes[12], 0x0DU);
    EXPECT_EQ(bytes[15], 0x0AU);
}

TEST(WireFormatTest, ProvideServiceRoundTrip)
{
    ProvideService in{"/vehicle/speed", {{"speed", 8U, 8U}, {"status", 1U, 1U}}};
    const auto frame = DecodeOrFail(EncodeOrFail(in, 9U, 3U));
    EXPECT_EQ(frame.header.kind, FrameKind::kProvideService);
    EXPECT_EQ(frame.header.session, 9U);
    EXPECT_EQ(frame.header.sequence, 3U);
    const auto& out = std::get<ProvideService>(frame.body);
    EXPECT_EQ(out.instance, in.instance);
    ASSERT_EQ(out.events.size(), 2U);
    EXPECT_EQ(out.events[0].name, "speed");
    EXPECT_EQ(out.events[0].size, 8U);
    EXPECT_EQ(out.events[1].alignment, 1U);
}

TEST(WireFormatTest, ProvideServiceCarriesTypeIds)
{
    ProvideService in{"/vehicle/drive", {{"mode", 4U, 4U, "DriveMode@1.2"}, {"raw", 16U, 8U, ""}}};
    const auto frame = DecodeOrFail(EncodeOrFail(in));
    const auto& out = std::get<ProvideService>(frame.body);
    ASSERT_EQ(out.events.size(), 2U);
    EXPECT_EQ(out.events[0].type_id, "DriveMode@1.2");
    EXPECT_EQ(out.events[1].type_id, "");
    EXPECT_EQ(out.events[1].size, 16U);
}

void PutU16(std::vector<std::uint8_t>& out, std::uint16_t value)
{
    out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
}

void PutU64(std::vector<std::uint8_t>& out, std::uint64_t value)
{
    for (std::uint32_t shift = 0U; shift < 64U; shift += 8U)
    {
        out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
    }
}

void PutStr(std::vector<std::uint8_t>& out, const std::string& value)
{
    PutU16(out, static_cast<std::uint16_t>(value.size()));
    out.insert(out.end(), value.begin(), value.end());
}

TEST(WireFormatTest, DecodesVersion1ProvideServiceWithoutTypeIds)
{
    // A version 1 peer: header + instance + count + (name, size, alignment) per event, no type id.
    std::vector<std::uint8_t> bytes{0x53U, 0x47U, 1U, static_cast<std::uint8_t>(FrameKind::kProvideService)};
    PutU64(bytes, 11U);
    bytes.insert(bytes.end(), {5U, 0U, 0U, 0U});
    PutStr(bytes, "/vehicle/speed");
    PutU16(bytes, 1U);
    PutStr(bytes, "speed");
    PutU64(bytes, 8U);
    PutU64(bytes, 8U);

    const auto frame = DecodeOrFail(bytes);
    EXPECT_EQ(frame.header.session, 11U);
    EXPECT_EQ(frame.header.sequence, 5U);
    const auto& out = std::get<ProvideService>(frame.body);
    ASSERT_EQ(out.events.size(), 1U);
    EXPECT_EQ(out.events[0].name, "speed");
    EXPECT_EQ(out.events[0].size, 8U);
    EXPECT_EQ(out.events[0].type_id, "");
}

TEST(WireFormatTest, Version1OfferServiceStillDecodes)
{
    auto bytes = EncodeOrFail(OfferService{"/vehicle/speed"});
    bytes[2] = 1U;
    const auto frame = DecodeOrFail(bytes);
    EXPECT_EQ(std::get<OfferService>(frame.body).instance, "/vehicle/speed");
}

TEST(WireFormatTest, AllBodiesRoundTrip)
{
    const std::vector<FrameBody> bodies{
        ProvideService{"i", {}},
        OfferService{"i"},
        StopOfferService{"i"},
        RegisterNotification{{"i", WireElementKind::kEvent, "e"}},
        UnregisterNotification{{"i", WireElementKind::kField, "f"}},
        UpdateNotification{{"i", WireElementKind::kEvent, "e"}},
        EventSample{"i", "e", {1U, 2U, 3U}},
        Ack{11U, 12U},
    };
    for (const auto& body : bodies)
    {
        const auto frame = DecodeOrFail(EncodeOrFail(body));
        EXPECT_EQ(frame.header.kind, KindOf(body));
        EXPECT_EQ(frame.body.index(), body.index());
    }
    EXPECT_EQ(std::get<UnregisterNotification>(DecodeOrFail(EncodeOrFail(bodies[4])).body).ref.kind,
              WireElementKind::kField);
    EXPECT_EQ(std::get<Ack>(DecodeOrFail(EncodeOrFail(bodies[7])).body).acked_sequence, 12U);
}

TEST(WireFormatTest, EncodeEventSampleMatchesGenericEncoding)
{
    const std::vector<std::uint8_t> payload{0xDEU, 0xADU, 0xBEU, 0xEFU};
    std::vector<std::uint8_t> direct{};
    EncodeError error{EncodeError::kNone};
    ASSERT_TRUE(EncodeEventSample(5U, 6U, "inst", "evt", payload.data(), payload.size(), direct, error));
    std::vector<std::uint8_t> generic{};
    ASSERT_TRUE(Encode(FrameHeader{FrameKind::kEventSample, 5U, 6U}, EventSample{"inst", "evt", payload}, generic,
                       error));
    EXPECT_EQ(direct, generic);
}

TEST(WireFormatTest, EmptyAndLargePayloads)
{
    std::vector<std::uint8_t> big(1U << 20U);
    for (std::size_t i = 0U; i < big.size(); ++i)
    {
        big[i] = static_cast<std::uint8_t>(i * 31U);
    }
    for (const auto& payload : {big, std::vector<std::uint8_t>{}})
    {
        const EventSample in{"i", "e", payload};
        const auto out = std::get<EventSample>(DecodeOrFail(EncodeOrFail(in)).body);
        EXPECT_EQ(out.payload, in.payload);
    }
}

TEST(WireFormatTest, RejectsTooLongString)
{
    std::vector<std::uint8_t> out{0U};
    EncodeError error{EncodeError::kNone};
    EXPECT_FALSE(Encode(FrameHeader{}, OfferService{std::string(kMaxStringLength + 1U, 'x')}, out, error));
    EXPECT_EQ(error, EncodeError::kStringTooLong);
    EXPECT_TRUE(out.empty());
    EXPECT_TRUE(Encode(FrameHeader{}, OfferService{std::string(kMaxStringLength, 'x')}, out, error));
}

TEST(WireFormatTest, RejectsMalformedFrames)
{
    auto bytes = EncodeOrFail(RegisterNotification{{"inst", WireElementKind::kEvent, "evt"}});

    EXPECT_EQ(Decode(nullptr, 0U).error, DecodeError::kTruncated);
    EXPECT_EQ(Decode(bytes.data(), kHeaderSize - 1U).error, DecodeError::kTruncated);

    // Every strict prefix of a valid frame is rejected, never read out of bounds.
    for (std::size_t length = kHeaderSize; length < bytes.size(); ++length)
    {
        EXPECT_EQ(Decode(bytes.data(), length).error, DecodeError::kTruncated) << "length " << length;
    }

    auto trailing = bytes;
    trailing.push_back(0U);
    EXPECT_EQ(Decode(trailing.data(), trailing.size()).error, DecodeError::kTrailingBytes);

    auto bad_magic = bytes;
    bad_magic[0] = 0U;
    EXPECT_EQ(Decode(bad_magic.data(), bad_magic.size()).error, DecodeError::kBadMagic);

    auto bad_version = bytes;
    bad_version[2] = static_cast<std::uint8_t>(kWireVersion + 1U);
    EXPECT_EQ(Decode(bad_version.data(), bad_version.size()).error, DecodeError::kUnsupportedVersion);
    bad_version[2] = 0U;
    EXPECT_EQ(Decode(bad_version.data(), bad_version.size()).error, DecodeError::kUnsupportedVersion);

    auto bad_kind = bytes;
    bad_kind[3] = 0xFFU;
    EXPECT_EQ(Decode(bad_kind.data(), bad_kind.size()).error, DecodeError::kUnknownKind);

    auto bad_element = bytes;
    // header(16) + u16 len(2) + "inst"(4) -> element kind byte
    bad_element[kHeaderSize + 2U + 4U] = 0x09U;
    EXPECT_EQ(Decode(bad_element.data(), bad_element.size()).error, DecodeError::kBadElementKind);
}

TEST(WireFormatTest, HugeDeclaredLengthIsTruncatedNotAllocated)
{
    auto bytes = EncodeOrFail(EventSample{"i", "e", {}});
    // Overwrite the u32 payload length (last 4 bytes) with 0xFFFFFFFF.
    for (std::size_t i = bytes.size() - 4U; i < bytes.size(); ++i)
    {
        bytes[i] = 0xFFU;
    }
    EXPECT_EQ(Decode(bytes.data(), bytes.size()).error, DecodeError::kTruncated);
}

TEST(WireFormatTest, HugeEventCountIsTruncatedNotAllocated)
{
    auto bytes = EncodeOrFail(ProvideService{"i", {}});
    // Overwrite the u16 event count (last 2 bytes) with 0xFFFF; no event data follows.
    bytes[bytes.size() - 2U] = 0xFFU;
    bytes[bytes.size() - 1U] = 0xFFU;
    EXPECT_EQ(Decode(bytes.data(), bytes.size()).error, DecodeError::kTruncated);

    // An event with empty strings is the smallest valid encoding and still decodes.
    const auto minimal = DecodeOrFail(EncodeOrFail(ProvideService{"i", {{"", 0U, 0U, ""}}}));
    EXPECT_EQ(std::get<ProvideService>(minimal.body).events.size(), 1U);
}

TEST(WireFormatTest, AckRequirement)
{
    EXPECT_TRUE(RequiresAck(FrameKind::kProvideService));
    EXPECT_TRUE(RequiresAck(FrameKind::kOfferService));
    EXPECT_TRUE(RequiresAck(FrameKind::kStopOfferService));
    EXPECT_TRUE(RequiresAck(FrameKind::kRegisterNotification));
    EXPECT_TRUE(RequiresAck(FrameKind::kUnregisterNotification));
    EXPECT_FALSE(RequiresAck(FrameKind::kUpdateNotification));
    EXPECT_FALSE(RequiresAck(FrameKind::kEventSample));
    EXPECT_FALSE(RequiresAck(FrameKind::kAck));
}

}  // namespace
}  // namespace score::mw::com::gateway::network
