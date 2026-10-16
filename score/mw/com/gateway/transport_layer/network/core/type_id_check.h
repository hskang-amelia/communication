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
#ifndef SCORE_MW_COM_GATEWAY_TRANSPORT_LAYER_NETWORK_CORE_TYPE_ID_CHECK_H
#define SCORE_MW_COM_GATEWAY_TRANSPORT_LAYER_NETWORK_CORE_TYPE_ID_CHECK_H

#include <cstdint>
#include <map>
#include <string>

namespace score::mw::com::gateway::network
{

/// \brief What the destination gateway does when an event's type id cannot be confirmed.
enum class TypeIdPolicy : std::uint8_t
{
    /// A missing type id (on either side) is logged and the event is accepted. A mismatch is always rejected.
    kWarn = 0U,
    /// Both sides must configure the same type id for every event, otherwise the service is rejected.
    kStrict = 1U,
};

enum class TypeIdVerdict : std::uint8_t
{
    kMatch = 0U,
    kMismatch,
    kMissingLocal,
    kMissingRemote,
    kMissingBoth,
};

/// \brief Type identities of the forwarded event data types.
/// \details Samples are copied in their binary C++ layout, and the destination only checks the size. A type id lets
/// both gateways confirm that they use the same type definition, so a same-size layout change (renamed or reordered
/// members) is caught when the service is provided instead of producing wrong values. The id is an opaque string
/// chosen by the integrator, e.g. "DriveMode@1.2" or a hash of the generated type definition; it is compared for
/// equality only.
struct TypeIdConfig
{
    TypeIdPolicy policy{TypeIdPolicy::kWarn};
    /// instance specifier -> event name -> type id
    std::map<std::string, std::map<std::string, std::string>> type_ids{};

    /// \brief The configured type id of an event, or an empty string.
    const std::string& Find(const std::string& instance, const std::string& element) const noexcept;
};

/// \brief Compares the local and the received type id of one event. An empty id means "not configured".
TypeIdVerdict CompareTypeId(const std::string& local, const std::string& remote) noexcept;

/// \brief True if a service may be provided with this verdict for one of its events.
bool IsAccepted(TypeIdVerdict verdict, TypeIdPolicy policy) noexcept;

const char* ToString(TypeIdVerdict verdict) noexcept;

}  // namespace score::mw::com::gateway::network

#endif  // SCORE_MW_COM_GATEWAY_TRANSPORT_LAYER_NETWORK_CORE_TYPE_ID_CHECK_H
