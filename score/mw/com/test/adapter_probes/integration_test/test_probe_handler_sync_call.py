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

"""Adapter probe: synchronous proxy call from a skeleton method handler (OI-36, prints PROBE_RESULT lines)."""


def test_probe_handler_sync_call(target):
    with target.wrap_exec(
        "bin/adapter_probes", ["handler_sync_call"], cwd="/opt/adapter_probes", wait_on_exit=True, wait_timeout=60
    ):
        pass
