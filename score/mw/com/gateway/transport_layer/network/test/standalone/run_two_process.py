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
"""Runs bus_peer as source and destination in two processes, in both start orders.

usage: run_two_process.py <bus_peer> <zenoh|dds> [extra bus_peer args...]

Everything runs on loopback. Each start order uses its own fixed zenoh port (17447, 17448) and DDS domain (42, 43),
so a peer left over from one run cannot join the next. Cyclone DDS is restricted to the loopback interface with
unicast discovery unless CYCLONEDDS_URI is already set.
"""

import json
import os
import subprocess
import sys
import time

CYCLONE_LOOPBACK = (
    '<CycloneDDS><Domain id="any"><General><Interfaces><NetworkInterface name="lo"/></Interfaces>'
    "<AllowMulticast>false</AllowMulticast></General><Discovery><ParticipantIndex>auto</ParticipantIndex>"
    '<Peers><Peer address="127.0.0.1"/></Peers></Discovery></Domain></CycloneDDS>'
)


def run(binary, backend, first, delay_s, index, extra):
    common = ["--port", str(17447 + index), "--domain", str(42 + index)] + extra
    env = dict(os.environ)
    env.setdefault("CYCLONEDDS_URI", CYCLONE_LOOPBACK)
    roles = ["destination", "source"] if first == "destination" else ["source", "destination"]
    procs = {}
    procs[roles[0]] = subprocess.Popen([binary, backend, roles[0]] + common, stdout=subprocess.PIPE, text=True, env=env)
    time.sleep(delay_s)
    procs[roles[1]] = subprocess.Popen([binary, backend, roles[1]] + common, stdout=subprocess.PIPE, text=True, env=env)

    results, ok = {}, True
    for role, proc in procs.items():
        try:
            out, _ = proc.communicate(timeout=90)
        except subprocess.TimeoutExpired:
            proc.kill()
            out, _ = proc.communicate()
            ok = False
        line = next((l for l in out.splitlines() if l.startswith("RESULT ")), None)
        results[role] = json.loads(line[len("RESULT ") :]) if line else {"ok": False, "error": "no RESULT line"}
        ok = ok and proc.returncode == 0 and results[role].get("ok", False)
    return ok, results


def main():
    binary, backend, extra = sys.argv[1], sys.argv[2], sys.argv[3:]
    all_ok = True
    for index, (first, delay) in enumerate((("destination", 0.5), ("source", 2.0))):
        ok, results = run(binary, backend, first, delay, index, extra)
        print(f"[{backend}] {first} first, peer after {delay}s: {'PASS' if ok else 'FAIL'}")
        for role in ("source", "destination"):
            print(f"  {role}: {json.dumps(results[role])}")
        all_ok = all_ok and ok
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
