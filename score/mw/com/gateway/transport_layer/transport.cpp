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
#include "score/mw/com/gateway/transport_layer/transport.h"

#include "score/mw/com/gateway/transport_layer/transport_error.h"

namespace score::mw::com::gateway
{

// Default (non-pure) bodies for the copying-gateway APIs — see transport.h's doc comment on this block.
// A memory-sharing-only transport (IsMemorySharingSupported() == true) never overrides these and correctly
// reports every one of them as unsupported.

score::Result<void> Transport::ForwardSampleData(score::mw::com::InstanceSpecifier /* service_instance_specifier */,
                                                 impl::ServiceElementType /* element_type */,
                                                 std::string /* element_name */,
                                                 score::cpp::span<const std::uint8_t> /* sample_data */)
{
    return score::MakeUnexpected(TransportErrorc::kNotSupported);
}

score::Result<void> Transport::Subscribe(score::mw::com::InstanceSpecifier /* service_instance_specifier */,
                                         impl::ServiceElementType /* element_type */,
                                         std::string /* element_name */)
{
    return score::MakeUnexpected(TransportErrorc::kNotSupported);
}

score::Result<void> Transport::Unsubscribe(score::mw::com::InstanceSpecifier /* service_instance_specifier */,
                                           impl::ServiceElementType /* element_type */,
                                           std::string /* element_name */)
{
    return score::MakeUnexpected(TransportErrorc::kNotSupported);
}

bool Transport::WaitForUpdate(const score::mw::com::InstanceSpecifier& /* service_instance_specifier */,
                              const std::string& /* element_name */,
                              std::uint32_t /* timeout_ms */)
{
    return false;
}

std::vector<std::uint8_t> Transport::TakeLastPayload(
    const score::mw::com::InstanceSpecifier& /* service_instance_specifier */,
    const std::string& /* element_name */)
{
    return {};
}

}  // namespace score::mw::com::gateway
