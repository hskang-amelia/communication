<!--
*******************************************************************************
Copyright (c) 2026 Contributors to the Eclipse Foundation

See the NOTICE file(s) distributed with this work for additional
information regarding copyright ownership.

This program and the accompanying materials are made available under the
terms of the Apache License Version 2.0 which is available at
https://www.apache.org/licenses/LICENSE-2.0

SPDX-License-Identifier: Apache-2.0
*******************************************************************************
-->

# Network (copying) gateway transport — zenoh-pico and Cyclone DDS

A `Transport` for the mw::com LoLa gateway between two **hosts** (HPC ↔ zonal ECU, ECU ↔ ECU), where the two LoLa
domains cannot share memory. It implements the *Copying Gateway* of [../../README.md](../../README.md)
(`IsMemorySharingSupported() == false`): event samples travel over the network and the destination's Forwarding
Skeleton writes them into its own shared memory.

| Transport id (`mw_com_gateway_config.json`) | Middleware | Bus class |
|---|---|---|
| `zenoh_pico` | [zenoh-pico](https://github.com/eclipse-zenoh/zenoh-pico) 1.x (tested: 1.10.1, 1.6.2), peer or client mode | `zenoh/ZenohPicoBus` |
| `cyclone_dds` | [Eclipse Cyclone DDS](https://github.com/eclipse-cyclonedds/cyclonedds) 0.10 (tested: 0.10.5), C API | `dds/CycloneDdsBus` |

## How it fits together

```
 source domain (has the real skeleton)                    destination domain (local consumers)
 GatewayApplication ──Transport──> NetworkTransport        NetworkTransport ──GatewayCore──> GatewayApplication
   GenericProxy                     RpcChannel  ── frames ──> RpcChannel                       GenericSkeleton
                                    IMessageBus (Zenoh / DDS)  IMessageBus
```

* **`core/`** — middleware- and mw::com-independent:
  * `wire_format` — versioned little-endian frames: ProvideService (instance + event name/size/alignment/type id),
    OfferService, StopOfferService, (Un)RegisterNotification, UpdateNotification, EventSample, Ack, RequestResult
    (request session + sequence + status ok/rejected/failed). The protocol is pre-release: only the current version
    (3) is decoded.
  * `type_id_check` — compares the type id an event was provided with against the local configuration.
  * `rpc_channel` — requests are acked on reception and resent until acked; the receiver drops resends per peer
    session; inbound frames are dispatched in order on a dedicated thread. An ack confirms delivery only (it is sent
    before dispatch). The outcome is reported by a separate RequestResult frame, see below.
  * `message_bus` — the only thing a middleware has to provide: point-to-point frames between two inboxes.
* **`network_transport`** — maps `Transport` calls to frames and inbound frames to `GatewayCore` calls, including
  `ForwardSampleData` → `GatewayCore::ReceiveSampleData`. No middleware dependency.
* **`zenoh/`, `dds/`** — the two `IMessageBus` implementations.

The gateway side of the copying path lives in `GatewayApplication`:

* source: the proxy event's receive handler reads the new samples (`GenericProxyEvent::GetNewSamples`) and calls
  `Transport::ForwardSampleData` instead of `NotifyUpdate` when the transport does not share memory;
* destination: `GatewayApplication::ReceiveSampleData` checks the size against the skeleton event's data type,
  `Allocate()`s, copies and `Send()`s (which notifies local consumers).

## Request results

`ProvideService` and `OfferService` return what the destination did with the request, not only that it arrived:

* The destination sends a RequestResult after handling the request: `ok`; `rejected` (permanent: type id check,
  invalid or non-whitelisted instance specifier); `failed` (its `GatewayCore` returned an error). The other requests
  keep ack-only semantics.
* The source waits for it up to `rpc.result-timeout-ms` after the ack (default 5000 ms, at least twice
  `ack-timeout-ms * max-attempts`) and returns `TransportErrorc::kRejectedByPeer` for `rejected` and
  `kPeerRequestFailed` for `failed` or a missing result. `GatewayApplication::PropagateService` logs a rejection and
  keeps the proxy (no retry until the service is found again); a transient failure erases the proxy as before, so the
  next discovery retries.
* The RequestResult is acked, resent and deduplicated like a request, but completed on the receive thread like an
  Ack, never through the dispatch queue. A waiting caller therefore never depends on its own dispatch thread, and
  `GatewayApplication` does not hold its mutex while waiting, so two gateways providing services to each other at
  the same time cannot block each other. A result that arrives too late is counted (`stale_results`) and dropped.

## Constraints

* **Binary sample layout.** Samples are copied as the binary representation of the C++ data type (LoLa's in-memory
  format). Both gateways must use the same type definition, endianness and alignment rules. Proxy events with
  `HasSerializedFormat() == true` are not forwarded.
* **Type identity.** The destination checks the sample size against the declared size, which does not catch a
  same-size layout change (renamed or reordered members). Configure a type id per event in `type-check.type-ids`
  (an opaque string, e.g. `"DriveMode@1.2"` or a hash of the generated type definition) on both gateways. A mismatch
  always keeps the destination from providing the service and is logged there (`TypeIdRejections()`); a missing id is
  rejected with `"policy": "strict"` (default) and only logged and accepted with `"warn"`, which is meant for
  development: a size change then aborts the destination's typed consumers and a same-size change goes unnoticed. The
  transport logs a warning at setup when `warn` is active. Rejections are reported to the source.
* **Events only.** Fields and methods are rejected (`kNotSupported`), like the rest of the gateway today.
* **zenoh-pico frame size.** zenoh-pico's default build reassembles messages up to `FRAG_MAX_SIZE = 4096` bytes. Build
  it with a larger `FRAG_MAX_SIZE` (verified here with 4 MiB) and set `max-frame-size` accordingly; `Open()` fails
  and logs both values if `max-frame-size` exceeds the library's `Z_FRAG_MAX_SIZE`, `Publish()` rejects larger frames.
  On an MCU keep both small.
* **zenoh-pico before 1.7.** `z_open` starts the read and lease tasks itself only from 1.7 on; for older 1.x builds
  with `Z_FEATURE_MULTI_THREAD` the bus starts them after `z_open`.
* **Ordering across restarts.** A restarted peer gets a new session id, which resets duplicate detection. Re-providing
  services after a peer restart is the job of `GatewayApplication` service discovery, as for the other transports.

## Configuration

`mw_com_gateway_config.json` selects the transport, e.g.
`"transport-layer": {"id": "zenoh_pico", "config-path": "/etc/gateway/zenoh_pico_transport.json"}`.
The transport config is validated by
[configuration/mw_com_gateway_network_transport_config_schema.json](configuration/mw_com_gateway_network_transport_config_schema.json);
examples: [zenoh-pico](configuration/example/mw_com_gateway_zenoh_pico_transport_config.json),
[Cyclone DDS](configuration/example/mw_com_gateway_cyclone_dds_transport_config.json). Each side uses the peer's
inbox as its `remote-key` / `remote-topic`. With zenoh-pico in peer mode one side `listen`s and the other `connect`s
(it retries until `open-timeout-ms`). Only reaching the peer is retried: an invalid key expression or mode, a
`max-frame-size` above the library limit, or a locator zenoh-pico rejects fails `Open()` at once with a log.

## Bazel integration

The middleware backends are opt-in, so `bazel build //...` stays independent of zenoh-pico and Cyclone DDS:

```
bazel build --//score/mw/com/gateway/transport_layer/network:with_zenoh_pico  //score/mw/com/gateway/...
bazel build --//score/mw/com/gateway/transport_layer/network:with_cyclone_dds //score/mw/com/gateway/...
```

The flags add the bus libraries and register the ids in `TransportFactory`. `network_core`, `network_transport`, the
config parser and their tests build without the flags. **Not done yet:** `@zenoh_pico` and `@cyclonedds` are not
registered in `MODULE.bazel`. The bus targets are tagged `manual` and expect
`@zenoh_pico//:zenohpico` and `@cyclonedds//:ddsc`. Without access to the Bazel Central Registry, this change does
not guess module versions.

## Verification

| What | How | Result |
|---|---|---|
| Wire format incl. RequestResult and version check, type id check, RpcChannel (44 tests: lost request/ack/result, duplicates, late peer, nested request while caller waits, result while the local dispatcher is blocked, both sides waiting for results, result timeout, full dispatch queue, peer restart, stop from the dispatcher and restart, 8 concurrent senders) | `test/standalone` CMake harness, `-Werror -Wconversion -Wsign-conversion -Wshadow` | pass, 200 repeats; ThreadSanitizer 20 repeats clean |
| `network_transport_test` (18 tests incl. rejected/failed/timed-out results and two transports providing to each other concurrently), `transport_error_test` | linked against the real mw::com `InstanceSpecifier` and baselibs `result`, with a logging stub | pass; ThreadSanitizer 20× repeat clean |
| ZenohPicoBus (zenoh-pico 1.10.1 and 1.6.2), CycloneDdsBus (Cyclone DDS 0.10.5), two processes on loopback, both start orders: ProvideService with type ids → nested RegisterNotification → 2000 × 256 B samples → 1 MiB sample → 200 request round trips → StopOffer | `test/standalone/run_two_process.py` via ctest | pass on both, 10 consecutive ctest runs; 0 lost, 0 reordered, 0 corrupt; ThreadSanitizer 5 runs clean (races inside the uninstrumented middleware libraries suppressed, lock-order checks active); ASan+UBSan 2 runs clean |
| ZenohPicoBus config errors (invalid key expression, mode, locator, `max-frame-size` above `Z_FRAG_MAX_SIZE`) | small driver against the real library | `Open()` fails within 1 ms with a specific log; an unreachable peer is retried until `open-timeout-ms` |
| `dds/generated/gateway_frame.{c,h}` | `idlc -l c idl/gateway_frame.idl` (Cyclone DDS 0.10.5), clang-format 19 | identical to the checked-in files except the `Source:` line |
| Config parser (27 tests) | linked against baselibs `json` | pass |
| `GatewayApplication` changes, `TransportFactory` with both flags, `gateway_application_test.cpp` | `g++ -fsyntax-only` against the real communication + baselibs headers, strict warnings | compile clean. **Not executed:** linking the mw::com runtime needs the Bazel build |

Request round trip on localhost (4 vCPU container, 200 probes): p50 ≈ 60–75 µs, p99 ≈ 200–300 µs, max ≈ 4–17 ms.
This is loopback only, not an in-vehicle network figure.

Standalone run (GCC 13, CMake 3.28). The middleware is built from the release tags:

```
git clone -b 1.10.1 https://github.com/eclipse-zenoh/zenoh-pico
cmake -S zenoh-pico -B zp-build -DCMAKE_BUILD_TYPE=Release -DBUILD_EXAMPLES=OFF -DBUILD_TESTING=OFF \
  -DFRAG_MAX_SIZE=4194304 -DCMAKE_INSTALL_PREFIX=<zenoh-pico>
cmake --build zp-build && cmake --install zp-build

git clone -b 0.10.5 https://github.com/eclipse-cyclonedds/cyclonedds
cmake -S cyclonedds -B cdds-build -DCMAKE_BUILD_TYPE=Release -DBUILD_IDLC=ON -DBUILD_EXAMPLES=OFF -DBUILD_TESTING=OFF \
  -DENABLE_SSL=OFF -DENABLE_SECURITY=OFF -DENABLE_SHM=OFF -DCMAKE_INSTALL_PREFIX=<cyclonedds>
cmake --build cdds-build && cmake --install cdds-build

cmake -S test/standalone -B build -DGTEST_SOURCE_DIR=<googletest> -DCMAKE_PREFIX_PATH="<zenoh-pico>;<cyclonedds>" \
  -DREQUIRE_ZENOH_PICO=ON -DREQUIRE_CYCLONE_DDS=ON
cmake --build build && ctest --test-dir build --output-on-failure
```

A backend whose library is not found is skipped with a warning; `REQUIRE_ZENOH_PICO=ON` / `REQUIRE_CYCLONE_DDS=ON` make
that a configure error. `-DSANITIZE_THREAD=ON` builds with ThreadSanitizer; with uninstrumented middleware libraries
run it with `TSAN_OPTIONS=suppressions=<file>` containing `race:libddsc.so` and `race:libzenohpico.so`. The
two-process test uses zenoh ports 17447/17448 and DDS domains 42/43 on loopback (Cyclone DDS restricted to `lo` with
unicast discovery unless `CYCLONEDDS_URI` is set), so do not run two instances of it at the same time.
