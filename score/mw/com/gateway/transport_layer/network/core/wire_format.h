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
#ifndef SCORE_MW_COM_GATEWAY_TRANSPORT_LAYER_NETWORK_CORE_WIRE_FORMAT_H
#define SCORE_MW_COM_GATEWAY_TRANSPORT_LAYER_NETWORK_CORE_WIRE_FORMAT_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

/// \file
/// \brief Wire format of the network (copying) gateway transport.
/// \details Independent of mw::com and of the network middleware, so it can be shared by the Zenoh and DDS
/// transports and unit tested on its own. All integers are little endian. Strings are u16 length + UTF-8 bytes.
///
///   Header (16 bytes): magic u16 | version u8 | kind u8 | session u64 | sequence u32
///
/// Requests (ProvideService .. UnregisterNotification, RequestResult) are acknowledged by an Ack frame. Notifications
/// (UpdateNotification, EventSample) are fire and forget. ProvideService and OfferService are answered by a
/// RequestResult once the receiver has handled them.
namespace score::mw::com::gateway::network
{

constexpr std::uint16_t kWireMagic{0x4753U};  // "SG" when read little endian
/// Version 3 adds RequestResult. The protocol is pre-release: only frames of this version are decoded.
constexpr std::uint8_t kWireVersion{3U};
constexpr std::size_t kHeaderSize{16U};
constexpr std::size_t kMaxStringLength{0xFFFFU};
constexpr std::size_t kMaxEventsPerService{0xFFFFU};

enum class FrameKind : std::uint8_t
{
    kProvideService = 1U,
    kOfferService = 2U,
    kStopOfferService = 3U,
    kRegisterNotification = 4U,
    kUnregisterNotification = 5U,
    kUpdateNotification = 6U,
    kEventSample = 7U,
    kAck = 8U,
    kRequestResult = 9U,
};

/// \brief Service element kind on the wire. Values are part of the protocol and must not change.
enum class WireElementKind : std::uint8_t
{
    kEvent = 1U,
    kField = 2U,
    kMethod = 3U,
};

struct EventMeta
{
    std::string name;
    std::uint64_t size{0U};
    std::uint64_t alignment{0U};
    /// \brief Opaque identity of the event data type (see TypeIdConfig). Empty: not configured.
    std::string type_id{};
};

struct ProvideService
{
    std::string instance;
    std::vector<EventMeta> events;
};

struct OfferService
{
    std::string instance;
};

struct StopOfferService
{
    std::string instance;
};

struct ElementRef
{
    std::string instance;
    WireElementKind kind{WireElementKind::kEvent};
    std::string element;
};

struct RegisterNotification
{
    ElementRef ref;
};

struct UnregisterNotification
{
    ElementRef ref;
};

struct UpdateNotification
{
    ElementRef ref;
};

/// \brief A copied event sample. The payload is the binary representation of the sample data type, so both
/// gateways must agree on the data type layout (same type definition, endianness and alignment rules).
struct EventSample
{
    std::string instance;
    std::string element;
    std::vector<std::uint8_t> payload;
};

/// \brief Acknowledges the request (acked_session, acked_sequence). Sent on reception, before the request is
/// dispatched, which matches the semantics of the sample hypervisor transport.
struct Ack
{
    std::uint64_t acked_session{0U};
    std::uint32_t acked_sequence{0U};
};

/// \brief Outcome of handling a request at the receiver. Values are part of the protocol and must not change.
enum class RequestStatus : std::uint8_t
{
    kOk = 0U,
    /// Permanent: the receiver's configuration does not allow the request (e.g. type id check, unknown instance).
    kRejected = 1U,
    /// Possibly transient: the receiver's GatewayCore returned an error.
    kFailed = 2U,
};

/// \brief Result of the request (request_session, request_sequence), sent by the receiver after dispatching it.
/// \details Unlike the Ack it reports what the receiver did with the request. It is a request itself (acked, resent
/// and deduplicated), but handled on reception instead of being dispatched.
struct RequestResult
{
    std::uint64_t request_session{0U};
    std::uint32_t request_sequence{0U};
    RequestStatus status{RequestStatus::kOk};
};

using FrameBody = std::variant<ProvideService,
                               OfferService,
                               StopOfferService,
                               RegisterNotification,
                               UnregisterNotification,
                               UpdateNotification,
                               EventSample,
                               Ack,
                               RequestResult>;

struct FrameHeader
{
    FrameKind kind{FrameKind::kAck};
    std::uint64_t session{0U};
    std::uint32_t sequence{0U};
};

struct Frame
{
    FrameHeader header;
    FrameBody body;
};

enum class DecodeError : std::uint8_t
{
    kNone = 0U,
    kTruncated,
    kBadMagic,
    kUnsupportedVersion,
    kUnknownKind,
    kBadElementKind,
    kBadRequestStatus,
    kTrailingBytes,
};

enum class EncodeError : std::uint8_t
{
    kNone = 0U,
    kStringTooLong,
    kTooManyEvents,
    kPayloadTooLarge,
};

struct DecodeResult
{
    std::optional<Frame> frame;
    DecodeError error{DecodeError::kNone};
};

/// \brief Kind of the frame body (derived from the variant alternative).
FrameKind KindOf(const FrameBody& body) noexcept;

/// \brief True for frames that are acknowledged and retried by the sender.
bool RequiresAck(FrameKind kind) noexcept;

/// \brief Encodes a frame. The header kind is derived from the body, header.kind is ignored.
/// \return false (and leaves out empty) if a field exceeds the protocol limits.
bool Encode(const FrameHeader& header, const FrameBody& body, std::vector<std::uint8_t>& out, EncodeError& error);

/// \brief Encodes an EventSample frame directly from a sample buffer, without copying it into an EventSample first.
bool EncodeEventSample(std::uint64_t session,
                       std::uint32_t sequence,
                       const std::string& instance,
                       const std::string& element,
                       const std::uint8_t* data,
                       std::size_t size,
                       std::vector<std::uint8_t>& out,
                       EncodeError& error);

DecodeResult Decode(const std::uint8_t* data, std::size_t size);

const char* ToString(DecodeError error) noexcept;
const char* ToString(EncodeError error) noexcept;
const char* ToString(RequestStatus status) noexcept;

}  // namespace score::mw::com::gateway::network

#endif  // SCORE_MW_COM_GATEWAY_TRANSPORT_LAYER_NETWORK_CORE_WIRE_FORMAT_H
