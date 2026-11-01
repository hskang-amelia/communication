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
# Runs every cross-process scenario of rust_method_lola_e2e and prints a summary.
#
#   run_test.sh <binary> <config template> [scenario ...]
#
# Without scenarios it runs all of them. "denied" needs passwordless sudo (it runs a consumer
# under another uid) and is skipped without it. Exit code 0 only if every scenario that ran passed.
set -u
BIN="$(realpath "$1")"
TEMPLATE="$(realpath "$2")"
shift 2
SCENARIOS=("$@")
[ ${#SCENARIOS[@]} -eq 0 ] && SCENARIOS=(basic notify seq concurrent overflow two_consumers early crash denied)

WORK="$(mktemp -d)"
ME="$(id -u)"
OTHER=65534
OPEN_CFG="$WORK/open.json"
STRICT_CFG="$WORK/strict.json"
sed 's|@ACCESS@||' "$TEMPLATE" > "$OPEN_CFG"
sed "s|@ACCESS@|\"permission-checks\": \"strict\", \"allowedProvider\": { \"QM\": [$ME] }, \"allowedConsumer\": { \"QM\": [$ME] },|" "$TEMPLATE" > "$STRICT_CFG"
chmod 0755 "$WORK"; chmod 0644 "$OPEN_CFG" "$STRICT_CFG"

PRODUCER=""
start_producer() {
    "$BIN" producer "$1" > "$WORK/producer.log" 2>&1 &
    PRODUCER=$!
    for _ in $(seq 1 100); do grep -q "PRODUCER offered" "$WORK/producer.log" 2>/dev/null && return 0; sleep 0.1; done
    echo "producer did not offer:"; cat "$WORK/producer.log"; return 1
}
stop_producer() {
    [ -n "$PRODUCER" ] && kill "$PRODUCER" 2>/dev/null && wait "$PRODUCER" 2>/dev/null
    PRODUCER=""
}
trap 'stop_producer; jobs -p | xargs -r kill 2>/dev/null; rm -rf "$WORK"' EXIT

consumer() { timeout 120 "$BIN" consumer "$@"; }

declare -A RESULT
run() {
    local name="$1"; shift
    echo; echo "=================== $name"
    "$@"; local rc=$?
    case $rc in 0) RESULT[$name]=PASS ;; 2) RESULT[$name]=SKIP ;; *) RESULT[$name]=FAIL ;; esac
    echo "--- producer log ($name)"; tail -n 20 "$WORK/producer.log" 2>/dev/null
    stop_producer
}

s_simple() { start_producer "$OPEN_CFG" && consumer "$OPEN_CFG" "$1"; }

s_concurrent() {
    start_producer "$OPEN_CFG" || return 1
    local pids=() ok=0
    for i in 1 2 3 4 5 6 7 8; do consumer "$OPEN_CFG" seq > "$WORK/c$i.log" 2>&1 & pids+=($!); done
    for i in "${!pids[@]}"; do wait "${pids[$i]}" && ok=$((ok+1)); done
    grep -h "FAIL\|HANG" "$WORK"/c*.log | head -5
    echo "concurrent consumers passed: $ok/8"
    [ "$ok" -eq 8 ]
}

s_overflow() {
    start_producer "$OPEN_CFG" || return 1
    local pids=() ok=0
    for i in $(seq 1 8); do consumer "$OPEN_CFG" slow1 > "$WORK/o$i.log" 2>&1 & pids+=($!); done
    for i in "${!pids[@]}"; do wait "${pids[$i]}" && ok=$((ok+1)); done
    local served refused
    served=$(grep -l "OK slow call" "$WORK"/o*.log | wc -l)
    refused=$(grep -l "DEFINED-ERROR" "$WORK"/o*.log | wc -l)
    grep -h "FAIL\|HANG\|DEFINED-ERROR" "$WORK"/o*.log | head -8
    echo "slow calls: $served served, $refused refused with an error, $ok/8 processes ended cleanly"
    [ "$ok" -eq 8 ] && [ "$served" -ge 1 ]
}

s_two_consumers() {
    start_producer "$OPEN_CFG" || return 1
    consumer "$OPEN_CFG" seq > "$WORK/t1.log" 2>&1 & local a=$!
    consumer "$OPEN_CFG" seq > "$WORK/t2.log" 2>&1 & local b=$!
    local r=0; wait $a || r=1; wait $b || r=1
    tail -n 3 "$WORK/t1.log" "$WORK/t2.log"; return $r
}

s_early() {
    consumer "$OPEN_CFG" early > "$WORK/early.log" 2>&1 & local c=$!
    sleep 3
    start_producer "$OPEN_CFG" || return 1
    local r=0; wait $c || r=1; cat "$WORK/early.log"; return $r
}

s_crash() {
    mkdir -p "$WORK/sync"; rm -f "$WORK/sync"/*
    start_producer "$OPEN_CFG" || return 1
    E2E_SYNC_DIR="$WORK/sync" consumer "$OPEN_CFG" crash > "$WORK/crash.log" 2>&1 & local c=$!
    for _ in $(seq 1 200); do [ -e "$WORK/sync/ready" ] && break; sleep 0.1; done
    kill -9 "$PRODUCER"; wait "$PRODUCER" 2>/dev/null; PRODUCER=""
    touch "$WORK/sync/killed"
    sleep 2
    start_producer "$OPEN_CFG" || return 1
    touch "$WORK/sync/restarted"
    local r=0; wait $c || r=1; cat "$WORK/crash.log"; return $r
}

s_denied() {
    if ! sudo -n true 2>/dev/null; then echo "SKIP: no passwordless sudo"; return 2; fi
    start_producer "$STRICT_CFG" || return 1
    echo "--- allowed consumer (uid $ME) under the strict config"
    consumer "$STRICT_CFG" basic > "$WORK/allowed.log" 2>&1; local allowed=$?
    tail -n 2 "$WORK/allowed.log"
    echo "--- consumer as uid $OTHER"
    # A copy the other uid can execute, wherever the build put the original.
    cp "$BIN" "$WORK/bin" && chmod 0755 "$WORK/bin"
    sudo -n setpriv --reuid=$OTHER --regid=$OTHER --clear-groups timeout 60 "$WORK/bin" consumer "$STRICT_CFG" denied
    local denied=$?
    [ $allowed -eq 0 ] && [ $denied -eq 0 ] && return 0; return 1
}

for s in "${SCENARIOS[@]}"; do
    case "$s" in
        basic|notify|seq) run "$s" s_simple "$s" ;;
        concurrent) run concurrent s_concurrent ;;
        overflow) run overflow s_overflow ;;
        two_consumers) run two_consumers s_two_consumers ;;
        early) run early s_early ;;
        crash) run crash s_crash ;;
        denied) run denied s_denied ;;
        *) echo "unknown scenario $s"; RESULT[$s]=FAIL ;;
    esac
done

echo; echo "=================== SUMMARY"
rc=0
for s in "${SCENARIOS[@]}"; do echo "$s: ${RESULT[$s]:-?}"; [ "${RESULT[$s]:-FAIL}" = FAIL ] && rc=1; done
exit $rc
