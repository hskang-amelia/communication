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

load("@rules_rust//rust:defs.bzl", "rust_binary")
load("@rules_shell//shell:sh_test.bzl", "sh_test")

# Cross-process check of the Rust Method/Field support on the LoLa runtime: a producer and a
# consumer process, the consumer calls the method and the field's getter and setter.
rust_binary(
    name = "rust_method_lola_e2e",
    srcs = ["main.rs"],
    features = ["link_std_cpp_lib"],
    deps = [
        "//score/mw/com/example/rust-method-lola-e2e/gen",
        "//score/mw/com/rust:score_com",
        "@score_communication_crate_index//:futures",
    ],
)

sh_test(
    name = "rust_method_lola_e2e_test",
    srcs = ["run_test.sh"],
    args = [
        "$(rootpath :rust_method_lola_e2e)",
        "$(rootpath etc/mw_com_config.json)",
    ],
    data = [
        "etc/mw_com_config.json",
        ":rust_method_lola_e2e",
    ],
    target_compatible_with = ["@platforms//os:linux"],
)
