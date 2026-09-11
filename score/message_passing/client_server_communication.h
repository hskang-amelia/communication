/********************************************************************************
 * Copyright (c) 2025 Contributors to the Eclipse Foundation
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
#ifndef SCORE_LIB_MESSAGE_PASSING_CLIENT_SERVER_COMMUNICATION_H
#define SCORE_LIB_MESSAGE_PASSING_CLIENT_SERVER_COMMUNICATION_H

#include <cstdint>

namespace score::message_passing::detail
{

enum class ClientToServer : std::uint8_t
{
    SEND,
    REQUEST
};

enum class ServerToClient : std::uint8_t
{
    REPLY,
    NOTIFY
};

/// \brief Correlation id prototype for communication#767 (see docs/design-notes.md §2.5, this fork only).
/// \details Every REQUEST/REPLY payload (never SEND/NOTIFY, which have no reply to correlate) is prefixed with one
/// of these. `kPrimaryCorrelationId` names whatever call currently occupies a connection's one traditional
/// "primary" slot (`ClientConnection::waiting_for_reply_`); higher values name one of a small number of additional
/// slots (`ClientConnection::pending_nested_calls_`) that a nested call can use instead of queuing behind an
/// already-busy connection. A real implementation would fold this into the wire framing (`SendProtocolMessage`'s
/// own header) instead of the message payload, and account for it in `max_send_size_`/`max_reply_size_`; this
/// prototype keeps it at the payload level to avoid touching the engine-specific transport code in both backends.
using CorrelationId = std::uint8_t;
constexpr CorrelationId kPrimaryCorrelationId = 0;

}  // namespace score::message_passing::detail

#endif  // SCORE_LIB_MESSAGE_PASSING_CLIENT_SERVER_COMMUNICATION_H
