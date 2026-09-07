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
load("//quality/unit_testing:unit_testing.bzl", "cc_unit_test")
load("//score/mw:common_features.bzl", "COMPILER_WARNING_FEATURES")

package(
    default_visibility = ["//score/mw/com/gateway:__subpackages__"],
)

# sample_payload.h/.c are CHECKED-IN idlc output (idlc sample_payload.idl, CycloneDDS 0.10.4), not
# regenerated at build time — this repo has no @rules_cyclonedds-equivalent Bazel toolchain yet to
# run idlc as a genrule (same gap this repo's earlier dds attempt left open). Regenerate by hand
# (`idlc sample_payload.idl`) if the .idl changes.
cc_library(
    name = "sample_payload_idl",
    srcs = ["sample_payload.c"],
    hdrs = ["sample_payload.h"],
    features = COMPILER_WARNING_FEATURES,
    deps = ["@cyclonedds//:ddsc"],
)

cc_library(
    name = "dds_transport",
    srcs = ["dds_transport.cpp"],
    hdrs = ["dds_transport.h"],
    features = COMPILER_WARNING_FEATURES,
    visibility = [
        "//score/mw/com/gateway:__subpackages__",
    ],
    deps = [
        ":sample_payload_idl",
        "//score/mw/com/gateway/transport_layer:transport",
        "//score/mw/com/gateway/transport_layer:transport_error",
        "//score/mw/com:types",
        "//score/mw/com/impl:service_element_type",
        "@cyclonedds//:ddsc",
        "@score_baselibs//score/result",
    ],
)

# Runs against the real CycloneDDS runtime (no mocks) — a provider and consumer of one
# DdsTransport instance, in-process, proving actual bytes cross the transport. See
# dds_transport_test.cpp's class comment.
cc_unit_test(
    name = "dds_transport_test",
    srcs = ["dds_transport_test.cpp"],
    features = COMPILER_WARNING_FEATURES,
    deps = [
        ":dds_transport",
        "@score_baselibs//score/result",
    ],
)
