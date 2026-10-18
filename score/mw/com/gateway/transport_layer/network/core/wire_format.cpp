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

#include <limits>
#include <type_traits>

namespace score::mw::com::gateway::network
{
namespace
{

constexpr std::size_t kMaxPayloadSize{std::numeric_limits<std::uint32_t>::max()};

class Writer
{
  public:
    explicit Writer(std::vector<std::uint8_t>& out) noexcept : out_{out} {}

    void U8(std::uint8_t value) { out_.push_back(value); }

    void U16(std::uint16_t value) { PutLittleEndian(value); }

    void U32(std::uint32_t value) { PutLittleEndian(value); }

    void U64(std::uint64_t value) { PutLittleEndian(value); }

    bool Str(const std::string& value)
    {
        if (value.size() > kMaxStringLength)
        {
            return false;
        }
        U16(static_cast<std::uint16_t>(value.size()));
        out_.insert(out_.end(), value.begin(), value.end());
        return true;
    }

    bool Bytes(const std::uint8_t* data, std::size_t size)
    {
        if (size > kMaxPayloadSize)
        {
            return false;
        }
        U32(static_cast<std::uint32_t>(size));
        if (size > 0U)
        {
            out_.insert(out_.end(), data, data + size);
        }
        return true;
    }

  private:
    template <typename T>
    void PutLittleEndian(T value)
    {
        static_assert(std::is_unsigned<T>::value, "unsigned integers only");
        // Widen first: shifting a promoted u8/u16 would mix int and unsigned operands.
        const auto wide = static_cast<std::uint64_t>(value);
        for (std::size_t i = 0U; i < sizeof(T); ++i)
        {
            out_.push_back(static_cast<std::uint8_t>((wide >> (8U * i)) & 0xFFU));
        }
    }

    std::vector<std::uint8_t>& out_;
};

class Reader
{
  public:
    Reader(const std::uint8_t* data, std::size_t size) noexcept : data_{data}, size_{size} {}

    bool U8(std::uint8_t& value) { return GetLittleEndian(value); }

    bool U16(std::uint16_t& value) { return GetLittleEndian(value); }

    bool U32(std::uint32_t& value) { return GetLittleEndian(value); }

    bool U64(std::uint64_t& value) { return GetLittleEndian(value); }

    bool Str(std::string& value)
    {
        std::uint16_t length{0U};
        if (!U16(length) || Remaining() < length)
        {
            return false;
        }
        value.assign(reinterpret_cast<const char*>(data_ + offset_), length);
        offset_ += length;
        return true;
    }

    bool Bytes(std::vector<std::uint8_t>& value)
    {
        std::uint32_t length{0U};
        if (!U32(length) || Remaining() < length)
        {
            return false;
        }
        value.assign(data_ + offset_, data_ + offset_ + length);
        offset_ += length;
        return true;
    }

    std::size_t Remaining() const noexcept { return size_ - offset_; }

  private:
    template <typename T>
    bool GetLittleEndian(T& value)
    {
        static_assert(std::is_unsigned<T>::value, "unsigned integers only");
        if (Remaining() < sizeof(T))
        {
            return false;
        }
        T result{0U};
        for (std::size_t i = 0U; i < sizeof(T); ++i)
        {
            result = static_cast<T>(result | static_cast<T>(static_cast<T>(data_[offset_ + i]) << (8U * i)));
        }
        offset_ += sizeof(T);
        value = result;
        return true;
    }

    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t offset_{0U};
};

void WriteHeader(Writer& writer, FrameKind kind, std::uint64_t session, std::uint32_t sequence)
{
    writer.U16(kWireMagic);
    writer.U8(kWireVersion);
    writer.U8(static_cast<std::uint8_t>(kind));
    writer.U64(session);
    writer.U32(sequence);
}

bool IsValidElementKind(std::uint8_t value) noexcept
{
    return (value >= static_cast<std::uint8_t>(WireElementKind::kEvent)) &&
           (value <= static_cast<std::uint8_t>(WireElementKind::kMethod));
}

bool WriteElementRef(Writer& writer, const ElementRef& ref)
{
    if (!writer.Str(ref.instance))
    {
        return false;
    }
    writer.U8(static_cast<std::uint8_t>(ref.kind));
    return writer.Str(ref.element);
}

DecodeError ReadElementRef(Reader& reader, ElementRef& ref)
{
    std::uint8_t kind{0U};
    if (!reader.Str(ref.instance) || !reader.U8(kind) || !reader.Str(ref.element))
    {
        return DecodeError::kTruncated;
    }
    if (!IsValidElementKind(kind))
    {
        return DecodeError::kBadElementKind;
    }
    ref.kind = static_cast<WireElementKind>(kind);
    return DecodeError::kNone;
}

struct BodyWriter
{
    Writer& writer;
    EncodeError& error;

    bool operator()(const ProvideService& body) const
    {
        if (!writer.Str(body.instance))
        {
            error = EncodeError::kStringTooLong;
            return false;
        }
        if (body.events.size() > kMaxEventsPerService)
        {
            error = EncodeError::kTooManyEvents;
            return false;
        }
        writer.U16(static_cast<std::uint16_t>(body.events.size()));
        for (const auto& event : body.events)
        {
            if (!writer.Str(event.name))
            {
                error = EncodeError::kStringTooLong;
                return false;
            }
            writer.U64(event.size);
            writer.U64(event.alignment);
            if (!writer.Str(event.type_id))
            {
                error = EncodeError::kStringTooLong;
                return false;
            }
        }
        return true;
    }

    bool operator()(const OfferService& body) const { return Instance(body.instance); }

    bool operator()(const StopOfferService& body) const { return Instance(body.instance); }

    bool operator()(const RegisterNotification& body) const { return Ref(body.ref); }

    bool operator()(const UnregisterNotification& body) const { return Ref(body.ref); }

    bool operator()(const UpdateNotification& body) const { return Ref(body.ref); }

    bool operator()(const EventSample& body) const
    {
        if (!writer.Str(body.instance) || !writer.Str(body.element))
        {
            error = EncodeError::kStringTooLong;
            return false;
        }
        if (!writer.Bytes(body.payload.data(), body.payload.size()))
        {
            error = EncodeError::kPayloadTooLarge;
            return false;
        }
        return true;
    }

    bool operator()(const Ack& body) const
    {
        writer.U64(body.acked_session);
        writer.U32(body.acked_sequence);
        return true;
    }

  private:
    bool Instance(const std::string& instance) const
    {
        if (!writer.Str(instance))
        {
            error = EncodeError::kStringTooLong;
            return false;
        }
        return true;
    }

    bool Ref(const ElementRef& ref) const
    {
        if (!WriteElementRef(writer, ref))
        {
            error = EncodeError::kStringTooLong;
            return false;
        }
        return true;
    }
};

template <typename T>
DecodeError ReadRefBody(Reader& reader, FrameBody& body)
{
    T value{};
    const auto error = ReadElementRef(reader, value.ref);
    if (error == DecodeError::kNone)
    {
        body = std::move(value);
    }
    return error;
}

template <typename T>
DecodeError ReadInstanceBody(Reader& reader, FrameBody& body)
{
    T value{};
    if (!reader.Str(value.instance))
    {
        return DecodeError::kTruncated;
    }
    body = std::move(value);
    return DecodeError::kNone;
}

DecodeError ReadBody(FrameKind kind, std::uint8_t version, Reader& reader, FrameBody& body)
{
    switch (kind)
    {
        case FrameKind::kProvideService:
        {
            ProvideService value{};
            std::uint16_t count{0U};
            if (!reader.Str(value.instance) || !reader.U16(count))
            {
                return DecodeError::kTruncated;
            }
            value.events.reserve(count);
            for (std::uint16_t i = 0U; i < count; ++i)
            {
                EventMeta event{};
                if (!reader.Str(event.name) || !reader.U64(event.size) || !reader.U64(event.alignment))
                {
                    return DecodeError::kTruncated;
                }
                if ((version >= 2U) && !reader.Str(event.type_id))
                {
                    return DecodeError::kTruncated;
                }
                value.events.push_back(std::move(event));
            }
            body = std::move(value);
            return DecodeError::kNone;
        }
        case FrameKind::kOfferService:
            return ReadInstanceBody<OfferService>(reader, body);
        case FrameKind::kStopOfferService:
            return ReadInstanceBody<StopOfferService>(reader, body);
        case FrameKind::kRegisterNotification:
            return ReadRefBody<RegisterNotification>(reader, body);
        case FrameKind::kUnregisterNotification:
            return ReadRefBody<UnregisterNotification>(reader, body);
        case FrameKind::kUpdateNotification:
            return ReadRefBody<UpdateNotification>(reader, body);
        case FrameKind::kEventSample:
        {
            EventSample value{};
            if (!reader.Str(value.instance) || !reader.Str(value.element) || !reader.Bytes(value.payload))
            {
                return DecodeError::kTruncated;
            }
            body = std::move(value);
            return DecodeError::kNone;
        }
        case FrameKind::kAck:
        {
            Ack value{};
            if (!reader.U64(value.acked_session) || !reader.U32(value.acked_sequence))
            {
                return DecodeError::kTruncated;
            }
            body = value;
            return DecodeError::kNone;
        }
        default:
            return DecodeError::kUnknownKind;
    }
}

bool IsKnownKind(std::uint8_t value) noexcept
{
    return (value >= static_cast<std::uint8_t>(FrameKind::kProvideService)) &&
           (value <= static_cast<std::uint8_t>(FrameKind::kAck));
}

}  // namespace

FrameKind KindOf(const FrameBody& body) noexcept
{
    // Variant alternatives are declared in FrameKind order, starting at 1.
    return static_cast<FrameKind>(static_cast<std::uint8_t>(body.index() + 1U));
}

bool RequiresAck(FrameKind kind) noexcept
{
    switch (kind)
    {
        case FrameKind::kProvideService:
        case FrameKind::kOfferService:
        case FrameKind::kStopOfferService:
        case FrameKind::kRegisterNotification:
        case FrameKind::kUnregisterNotification:
            return true;
        case FrameKind::kUpdateNotification:
        case FrameKind::kEventSample:
        case FrameKind::kAck:
        default:
            return false;
    }
}

bool Encode(const FrameHeader& header, const FrameBody& body, std::vector<std::uint8_t>& out, EncodeError& error)
{
    out.clear();
    error = EncodeError::kNone;
    Writer writer{out};
    WriteHeader(writer, KindOf(body), header.session, header.sequence);
    if (!std::visit(BodyWriter{writer, error}, body))
    {
        out.clear();
        return false;
    }
    return true;
}

bool EncodeEventSample(std::uint64_t session,
                       std::uint32_t sequence,
                       const std::string& instance,
                       const std::string& element,
                       const std::uint8_t* data,
                       std::size_t size,
                       std::vector<std::uint8_t>& out,
                       EncodeError& error)
{
    out.clear();
    error = EncodeError::kNone;
    out.reserve(kHeaderSize + 2U + instance.size() + 2U + element.size() + 4U + size);
    Writer writer{out};
    WriteHeader(writer, FrameKind::kEventSample, session, sequence);
    if (!writer.Str(instance) || !writer.Str(element))
    {
        error = EncodeError::kStringTooLong;
        out.clear();
        return false;
    }
    if (!writer.Bytes(data, size))
    {
        error = EncodeError::kPayloadTooLarge;
        out.clear();
        return false;
    }
    return true;
}

DecodeResult Decode(const std::uint8_t* data, std::size_t size)
{
    DecodeResult result{};
    if ((data == nullptr) || (size < kHeaderSize))
    {
        result.error = DecodeError::kTruncated;
        return result;
    }

    Reader reader{data, size};
    std::uint16_t magic{0U};
    std::uint8_t version{0U};
    std::uint8_t kind{0U};
    Frame frame{};
    // Header size was checked above, so these reads cannot fail.
    static_cast<void>(reader.U16(magic));
    static_cast<void>(reader.U8(version));
    static_cast<void>(reader.U8(kind));
    static_cast<void>(reader.U64(frame.header.session));
    static_cast<void>(reader.U32(frame.header.sequence));

    if (magic != kWireMagic)
    {
        result.error = DecodeError::kBadMagic;
        return result;
    }
    if ((version < kMinWireVersion) || (version > kWireVersion))
    {
        result.error = DecodeError::kUnsupportedVersion;
        return result;
    }
    if (!IsKnownKind(kind))
    {
        result.error = DecodeError::kUnknownKind;
        return result;
    }
    frame.header.kind = static_cast<FrameKind>(kind);

    const auto body_error = ReadBody(frame.header.kind, version, reader, frame.body);
    if (body_error != DecodeError::kNone)
    {
        result.error = body_error;
        return result;
    }
    if (reader.Remaining() != 0U)
    {
        result.error = DecodeError::kTrailingBytes;
        return result;
    }
    result.frame = std::move(frame);
    return result;
}

const char* ToString(DecodeError error) noexcept
{
    switch (error)
    {
        case DecodeError::kNone:
            return "none";
        case DecodeError::kTruncated:
            return "truncated frame";
        case DecodeError::kBadMagic:
            return "bad magic";
        case DecodeError::kUnsupportedVersion:
            return "unsupported wire version";
        case DecodeError::kUnknownKind:
            return "unknown frame kind";
        case DecodeError::kBadElementKind:
            return "bad service element kind";
        case DecodeError::kTrailingBytes:
            return "trailing bytes after frame body";
        default:
            return "unknown decode error";
    }
}

const char* ToString(EncodeError error) noexcept
{
    switch (error)
    {
        case EncodeError::kNone:
            return "none";
        case EncodeError::kStringTooLong:
            return "string exceeds 65535 bytes";
        case EncodeError::kTooManyEvents:
            return "more than 65535 events";
        case EncodeError::kPayloadTooLarge:
            return "payload exceeds 4 GiB";
        default:
            return "unknown encode error";
    }
}

}  // namespace score::mw::com::gateway::network
