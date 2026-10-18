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
| `zenoh_pico` | [zenoh-pico](https://github.com/eclipse-zenoh/zenoh-pico) 1.x, peer or client mode | `zenoh/ZenohPicoBus` |
| `cyclone_dds` | [Eclipse Cyclone DDS](https://github.com/eclipse-cyclonedds/cyclonedds) 0.10 (C API) | `dds/CycloneDdsBus` |

## How it fits together

```
 source domain (has the real skeleton)                    destination domain (local consumers)
 GatewayApplication ──Transport──> NetworkTransport        NetworkTransport ──GatewayCore──> GatewayApplication
   GenericProxy                     RpcChannel  ── frames ──> RpcChannel                       GenericSkeleton
                                    IMessageBus (Zenoh / DDS)  IMessageBus
```

* **`core/`** — middleware- and mw::com-independent:
  * `wire_format` — versioned little-endian frames: ProvideService (instance + event name/size/alignment/type id),
    OfferService, StopOfferService, (Un)RegisterNotification, UpdateNotification, EventSample, Ack. Version 2 added
    the type id; version 1 frames are still decoded.
  * `type_id_check` — compares the type id an event was provided with against the local configuration.
  * `rpc_channel` — requests are acked on reception and resent until acked; the receiver drops resends per peer
    session; inbound frames are dispatched in order on a dedicated thread. Same semantics as the sample hypervisor
    transport: an ack confirms delivery, not the result of the remote `GatewayCore` call.
  * `message_bus` — the only thing a middleware has to provide: point-to-point frames between two inboxes.
* **`network_transport`** — maps `Transport` calls to frames and inbound frames to `GatewayCore` calls, including
  `ForwardSampleData` → `GatewayCore::ReceiveSampleData`. No middleware dependency.
* **`zenoh/`, `dds/`** — the two `IMessageBus` implementations.

The gateway side of the copying path lives in `GatewayApplication`:

* source: the proxy event's receive handler reads the new samples (`GenericProxyEvent::GetNewSamples`) and calls
  `Transport::ForwardSampleData` instead of `NotifyUpdate` when the transport does not share memory;
* destination: `GatewayApplication::ReceiveSampleData` checks the size against the skeleton event's data type,
  `Allocate()`s, copies and `Send()`s (which notifies local consumers).

## Constraints

* **Binary sample layout.** Samples are copied as the binary representation of the C++ data type (LoLa's in-memory
  format). Both gateways must use the same type definition, endianness and alignment rules. Proxy events with
  `HasSerializedFormat() == true` are not forwarded.
* **Type identity.** The destination checks the sample size against the declared size, which does not catch a
  same-size layout change (renamed or reordered members). Configure a type id per event in `type-check.type-ids`
  (an opaque string, e.g. `"DriveMode@1.2"` or a hash of the generated type definition) on both gateways. A mismatch
  always keeps the destination from providing the service and is logged there (`TypeIdRejections()`); a missing id is
  logged and accepted with `"policy": "warn"` (default) and rejected with `"strict"`. The source only receives the
  delivery Ack, so it is not told about a rejection.
* **Events only.** Fields and methods are rejected (`kNotSupported`), like the rest of the gateway today.
* **zenoh-pico frame size.** zenoh-pico's default build reassembles messages up to `FRAG_MAX_SIZE = 4096` bytes. Build
  it with a larger `FRAG_MAX_SIZE` (verified here with 4 MiB) and set `max-frame-size` accordingly; `Publish()` rejects
  larger frames. On an MCU keep both small.
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
(it retries until `open-timeout-ms`).

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
| Wire format incl. v1 compatibility and type ids, type id check, RpcChannel (33 tests: lost request/ack, duplicates, late peer, nested request while caller waits, full dispatch queue, peer restart, stop from the dispatcher and restart, 8 concurrent senders) | `test/standalone` CMake harness, `-Werror -Wconversion -Wsign-conversion -Wshadow` | pass; ThreadSanitizer 20× repeat clean; ASan+UBSan clean |
| ZenohPicoBus, CycloneDdsBus against the real libraries, two processes, both start orders: ProvideService with type ids → nested RegisterNotification → 2000 × 256 B samples → 1 MiB sample → 200 request round trips → StopOffer | `test/standalone/run_two_process.py` | pass on both; 0 lost, 0 reordered, 0 corrupt; also clean under ASan+UBSan |
| `NetworkTransport`, config parser, `GatewayApplication` changes, `TransportFactory` with both flags, `network_transport_test.cpp`, new `gateway_application_test.cpp` cases | `g++ -fsyntax-only` against the real communication + baselibs headers, strict warnings | compile clean. **Not executed:** linking the mw::com runtime needs the Bazel build |

Request round trip on localhost (2 vCPU container, 200 probes): p50 ≈ 45–70 µs, p99 ≈ 90–300 µs, max ≈ 2.4–3.6 ms.
This is loopback only, not an in-vehicle network figure.

Standalone run:

```
cmake -S test/standalone -B build -DGTEST_SOURCE_DIR=<googletest> -DCMAKE_PREFIX_PATH="<zenoh-pico>;<cyclonedds>"
cmake --build build && ctest --test-dir build --output-on-failure
```
