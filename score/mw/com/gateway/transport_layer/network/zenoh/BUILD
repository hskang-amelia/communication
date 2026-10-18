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
    name = "zenoh_pico_bus_config",
    hdrs = ["zenoh_pico_bus_config.h"],
)

# `@zenoh_pico` is not registered in MODULE.bazel yet (see ../README.md "Bazel integration"), hence `manual`: the
# target is only built when requested explicitly or through --//score/mw/com/gateway/transport_layer/network:with_zenoh_pico.
# zenoh-pico must be built with FRAG_MAX_SIZE >= the configured max-frame-size (default build: 4096 bytes).
cc_library(
    name = "zenoh_pico_bus",
    srcs = ["zenoh_pico_bus.cpp"],
    hdrs = ["zenoh_pico_bus.h"],
    tags = ["manual"],
    deps = [
        ":zenoh_pico_bus_config",
        "//score/mw/com/gateway/transport_layer/network:network_core",
        "@zenoh_pico//:zenohpico",
    ],
)
