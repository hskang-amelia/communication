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
#include "score/mw/com/gateway/transport_layer/network/core/type_id_check.h"

namespace score::mw::com::gateway::network
{

const std::string& TypeIdConfig::Find(const std::string& instance, const std::string& element) const noexcept
{
    static const std::string kNone{};
    const auto instance_it = type_ids.find(instance);
    if (instance_it == type_ids.cend())
    {
        return kNone;
    }
    const auto element_it = instance_it->second.find(element);
    if (element_it == instance_it->second.cend())
    {
        return kNone;
    }
    return element_it->second;
}

TypeIdVerdict CompareTypeId(const std::string& local, const std::string& remote) noexcept
{
    if (local.empty() && remote.empty())
    {
        return TypeIdVerdict::kMissingBoth;
    }
    if (local.empty())
    {
        return TypeIdVerdict::kMissingLocal;
    }
    if (remote.empty())
    {
        return TypeIdVerdict::kMissingRemote;
    }
    return (local == remote) ? TypeIdVerdict::kMatch : TypeIdVerdict::kMismatch;
}

bool IsAccepted(TypeIdVerdict verdict, TypeIdPolicy policy) noexcept
{
    switch (verdict)
    {
        case TypeIdVerdict::kMatch:
            return true;
        case TypeIdVerdict::kMismatch:
            return false;
        case TypeIdVerdict::kMissingLocal:
        case TypeIdVerdict::kMissingRemote:
        case TypeIdVerdict::kMissingBoth:
            return policy == TypeIdPolicy::kWarn;
        default:
            return false;
    }
}

const char* ToString(TypeIdVerdict verdict) noexcept
{
    switch (verdict)
    {
        case TypeIdVerdict::kMatch:
            return "type id match";
        case TypeIdVerdict::kMismatch:
            return "type id mismatch";
        case TypeIdVerdict::kMissingLocal:
            return "type id not configured locally";
        case TypeIdVerdict::kMissingRemote:
            return "type id not sent by the peer";
        case TypeIdVerdict::kMissingBoth:
            return "type id configured on neither side";
        default:
            return "unknown type id verdict";
    }
}

}  // namespace score::mw::com::gateway::network
