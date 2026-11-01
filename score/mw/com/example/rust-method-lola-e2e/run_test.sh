#!/usr/bin/env bash
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
# Starts the producer and the consumer as two processes; passes if the consumer does.
set -u
BIN="$1"
CONFIG="$2"
"$BIN" producer "$CONFIG" &
PRODUCER=$!
trap 'kill $PRODUCER 2>/dev/null; wait $PRODUCER 2>/dev/null' EXIT
timeout 60 "$BIN" consumer "$CONFIG"
