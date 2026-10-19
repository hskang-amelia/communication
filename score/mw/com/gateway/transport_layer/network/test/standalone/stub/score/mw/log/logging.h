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
// Minimal stand-in for baselibs score/mw/log in the standalone CMake harness: writes to stderr, one line per stream,
// items separated by a space like mw::log.
#ifndef SCORE_MW_COM_GATEWAY_TRANSPORT_LAYER_NETWORK_TEST_STANDALONE_STUB_SCORE_MW_LOG_LOGGING_H
#define SCORE_MW_COM_GATEWAY_TRANSPORT_LAYER_NETWORK_TEST_STANDALONE_STUB_SCORE_MW_LOG_LOGGING_H

#include <iostream>
#include <sstream>
#include <string_view>

namespace score::mw::log
{

class LogStream
{
  public:
    explicit LogStream(std::string_view level) : level_{level} {}
    LogStream(const LogStream&) = delete;
    LogStream& operator=(const LogStream&) = delete;
    LogStream(LogStream&&) = delete;
    LogStream& operator=(LogStream&&) = delete;
    ~LogStream()
    {
        std::cerr << level_ << ':' << buffer_.str() << '\n';
    }

    template <typename T>
    LogStream& operator<<(const T& value)
    {
        buffer_ << ' ' << value;
        return *this;
    }

  private:
    std::string_view level_;
    std::ostringstream buffer_;
};

inline LogStream LogFatal(std::string_view)
{
    return LogStream{"fatal"};
}
inline LogStream LogError(std::string_view)
{
    return LogStream{"error"};
}
inline LogStream LogWarn(std::string_view)
{
    return LogStream{"warn"};
}
inline LogStream LogInfo(std::string_view)
{
    return LogStream{"info"};
}
inline LogStream LogDebug(std::string_view)
{
    return LogStream{"debug"};
}

}  // namespace score::mw::log

#endif  // SCORE_MW_COM_GATEWAY_TRANSPORT_LAYER_NETWORK_TEST_STANDALONE_STUB_SCORE_MW_LOG_LOGGING_H
