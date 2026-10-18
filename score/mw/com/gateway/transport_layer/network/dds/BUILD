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

load("@rules_cc//cc:defs.bzl", "cc_library")

package(
    default_visibility = ["//score/mw/com/gateway:__subpackages__"],
)

cc_library(
    name = "cyclone_dds_bus_config",
    hdrs = ["cyclone_dds_bus_config.h"],
)

# generated/gateway_frame.{c,h} are checked-in idlc output (Cyclone DDS 0.10.5): `idlc -l c idl/gateway_frame.idl`.
# Same `@cyclonedds//:ddsc` label and `manual` tag as ../../dds (DdsTransport); see ../README.md "Bazel integration".
cc_library(
    name = "gateway_frame_idl",
    srcs = ["generated/gateway_frame.c"],
    hdrs = ["generated/gateway_frame.h"],
    strip_include_prefix = "generated",
    tags = ["manual"],
    deps = ["@cyclonedds//:ddsc"],
)

cc_library(
    name = "cyclone_dds_bus",
    srcs = ["cyclone_dds_bus.cpp"],
    hdrs = ["cyclone_dds_bus.h"],
    tags = ["manual"],
    deps = [
        ":cyclone_dds_bus_config",
        ":gateway_frame_idl",
        "//score/mw/com/gateway/transport_layer/network:network_core",
        "@cyclonedds//:ddsc",
    ],
)
