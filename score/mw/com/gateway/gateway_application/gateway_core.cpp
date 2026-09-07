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

#include "score/mw/com/gateway/gateway_application/gateway_core.h"

#include "score/mw/com/gateway/gateway_application/gateway_error.h"

namespace score::mw::com::gateway
{

score::Result<void> GatewayCore::ReceiveSampleData(score::mw::com::InstanceSpecifier /* service_instance_specifier */,
                                                   impl::ServiceElementType /* element_type */,
                                                   std::string /* element_name */,
                                                   score::cpp::span<const std::uint8_t> /* sample_data */)
{
    return score::MakeUnexpected(GatewayErrorc::kNotSupported);
}

}  // namespace score::mw::com::gateway
