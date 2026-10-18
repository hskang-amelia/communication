# *******************************************************************************
# Copyright (c) 2026 Contributors to the Eclipse Foundation
#
# See the NOTICE file(s) distributed with this work for additional
# information regarding copyright ownership.
#
# This program and the accompanying materials are made available under the
# terms of the Apache License Version 2.0 which is available at
# https://www.apache.org/licenses/LICENSE-2.0
#
# SPDX-License-Identifier: Apache-2.0
# *******************************************************************************

load("@bazel_skylib//rules:common_settings.bzl", "bool_flag")
load("@rules_cc//cc:defs.bzl", "cc_library")
load("//quality/unit_testing:unit_testing.bzl", "cc_unit_test")

package(
    default_visibility = ["//score/mw/com/gateway:__subpackages__"],
)

# Opt-in switches for the middleware backends. Off by default so `bazel build //...` does not need zenoh-pico or
# Cyclone DDS (neither is registered in MODULE.bazel yet, see README.md "Bazel integration").
#   bazel build --//score/mw/com/gateway/transport_layer/network:with_zenoh_pico //score/mw/com/gateway/...
bool_flag(
    name = "with_zenoh_pico",
    build_setting_default = False,
)

bool_flag(
    name = "with_cyclone_dds",
    build_setting_default = False,
)

config_setting(
    name = "config_with_zenoh_pico",
    flag_values = {":with_zenoh_pico": "True"},
)

config_setting(
    name = "config_with_cyclone_dds",
    flag_values = {":with_cyclone_dds": "True"},
)

# Middleware- and mw::com-independent protocol: wire format + reliable request delivery.
cc_library(
    name = "network_core",
    srcs = [
        "core/rpc_channel.cpp",
        "core/type_id_check.cpp",
        "core/wire_format.cpp",
    ],
    hdrs = [
        "core/message_bus.h",
        "core/rpc_channel.h",
        "core/type_id_check.h",
        "core/wire_format.h",
    ],
)

cc_library(
    name = "loopback_bus",
    testonly = True,
    srcs = ["core/loopback_bus.cpp"],
    hdrs = ["core/loopback_bus.h"],
    deps = [":network_core"],
)

cc_unit_test(
    name = "network_core_test",
    srcs = [
        "core/rpc_channel_test.cpp",
        "core/type_id_check_test.cpp",
        "core/wire_format_test.cpp",
    ],
    deps = [
        ":loopback_bus",
        ":network_core",
    ],
)

# The copying-gateway Transport. Does not depend on a middleware: the IMessageBus is injected by TransportFactory.
cc_library(
    name = "network_transport",
    srcs = ["network_transport.cpp"],
    hdrs = ["network_transport.h"],
    deps = [
        ":network_core",
        "//score/mw/com:types",
        "//score/mw/com/gateway/gateway_application:gateway_core",
        "//score/mw/com/gateway/transport_layer:transport",
        "//score/mw/com/gateway/transport_layer:transport_error",
        "//score/mw/com/impl:service_element_type",
        "@score_baselibs//score/language/futurecpp",
        "@score_baselibs//score/mw/log",
        "@score_baselibs//score/result",
    ],
)

cc_unit_test(
    name = "network_transport_test",
    srcs = ["network_transport_test.cpp"],
    deps = [
        ":loopback_bus",
        ":network_core",
        ":network_transport",
        "//score/mw/com/gateway/gateway_application:gateway_core_mock",
        "//score/mw/com/gateway/gateway_application:gateway_error",
    ],
)
