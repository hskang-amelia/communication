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
load("//bazel/tools:json_schema_validator.bzl", "validate_json_schema_test")

package(
    default_visibility = ["//score/mw/com/gateway:__subpackages__"],
)

filegroup(
    name = "mw_com_gateway_network_transport_config_schema",
    srcs = ["mw_com_gateway_network_transport_config_schema.json"],
)

validate_json_schema_test(
    name = "validate_zenoh_pico_transport_config_schema",
    json = "example/mw_com_gateway_zenoh_pico_transport_config.json",
    schema = ":mw_com_gateway_network_transport_config_schema",
    tags = ["lint"],
)

validate_json_schema_test(
    name = "validate_cyclone_dds_transport_config_schema",
    json = "example/mw_com_gateway_cyclone_dds_transport_config.json",
    schema = ":mw_com_gateway_network_transport_config_schema",
    tags = ["lint"],
)

cc_library(
    name = "network_transport_config_parser",
    srcs = ["network_transport_config_parser.cpp"],
    hdrs = ["network_transport_config_parser.h"],
    deps = [
        "//score/mw/com/gateway/transport_layer/network:network_core",
        "//score/mw/com/gateway/transport_layer/network/dds:cyclone_dds_bus_config",
        "//score/mw/com/gateway/transport_layer/network/zenoh:zenoh_pico_bus_config",
        "@score_baselibs//score/json",
        "@score_baselibs//score/language/futurecpp",
        "@score_baselibs//score/mw/log",
    ],
)
