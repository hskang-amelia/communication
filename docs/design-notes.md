# `communication` fork — design notes

Decisions and findings for the work in this fork on top of `eclipse-score/communication`. This file is not part of
the upstream Sphinx documentation (`docs/sphinx/`).

---

## 1. Rust `Method<T>` / `Field<T>` for the LoLa runtime

### 1.1 Starting point

The Rust trait and macro design for methods and fields (`score_com_concept`, `score_com_macros`, the mock runtime and
the design documents under `score/mw/com/rust/design/`) comes from upstream PR #818, which is still open. PR #818 only
defines the API: the LoLa runtime bodies were `todo!()` placeholders and the FFI bridge had no method or field
functions. This fork implements that layer, scoped to upstream issue #782 (synchronous calls first).

### 1.2 Method FFI

- The FFI works at the level of the type-erased C++ bindings. `ProxyMethodBinding`/`SkeletonMethodBinding` already
  operate on byte spans plus a queue position, so unlike events no per-type `TypeOperations` is needed.
  `EXPORT_MW_COM_METHOD` registers a `MethodMemberOperation` that returns those bindings;
  `ProxyMethodBaseView::GetMethodBinding()` was added as the proxy-side counterpart of the existing skeleton accessor.
- Calls are synchronous and wrapped in an already-resolved `Future`. The C++ binding supports one call in flight
  (`kCallQueueSize == 1`), so every call uses queue position 0.
- **Argument layout.** C++ packs all arguments into one buffer with `type_erased_storage.h`
  (`CreateDataTypeSizeInfoFromTypes`/`SerializeArgs`): sequential fields, natural alignment, size rounded up to the
  largest alignment. A Rust tuple's layout is not guaranteed to match, so `MethodArgsCodec` (`score_com_concept`)
  serializes `Args` field by field with the same algorithm, and `LolaMethodInArgAllocator` hands out zero-copy slots
  at the same offsets. Both sides agree provided the buffer start is aligned, which the C++ allocation guarantees.
- The in-args buffer size is checked against `Args::layout()` before writing; a mismatch returns an error instead of
  overflowing the buffer.
- Void methods are detected with a `TypeId` check, since `Return` is generic in the concept traits.
- `LolaMethodCaller`/`LolaMethodHandler` are parameterized over the concrete `B: FFIBridge` rather than the runtime,
  because they need `LolaConsumerInfo<B>`/`LolaProviderInfo<B>` and the bridge itself.
- Known gap: `SkeletonMethodBinding` has no way to unregister a handler, so a registered Rust closure lives as long as
  the process.

### 1.3 Field FFI

The first version treated field get/set as plain methods named `{field}_get`/`{field}_set`. Checking the C++ side
showed that this cannot work:

- `GetMethodSignature<T>` is `score::Result<T>()` and `SetMethodSignature<T>` is `score::Result<T>(T)`
  (`field_getter_setter_signatures.h`). Reading or writing the return buffer as a plain `T` misreads its layout.
- `SkeletonField<T>` implements the field semantics on top of its event and methods: `Update()` before
  `OfferService` stores the initial value (`kFieldValueIsNotValid` otherwise); `RegisterSetHandler()` marks the setter
  as served (`kSetHandlerNotSet` otherwise) and publishes the accepted value; `PrepareOffer()` installs the get
  handler, which answers with the latest value and replaces anything registered earlier.
- A generated C++ Proxy/Skeleton has no `{field}_get`/`{field}_set` members to look up.

So fields go through the typed C++ API instead. `EXPORT_MW_COM_FIELD(type, member)` registers, under the field name:

1. the field's own `ProxyEvent<T>`/`SkeletonEvent<T>` with the event registry, so subscribe, receive, allocate and
   zero-copy send work on fields unchanged (`ProxyFieldBaseView::GetEventBase()` added for the proxy side);
2. a typed `FieldMemberOperation` calling `ProxyField<T>::Get()`/`Set()` and `SkeletonField<T>::Update()`/
   `RegisterSetHandler()`. Which operations a field supports is detected at compile time from the SFINAE-gated API,
   following its `WithGetter`/`WithSetter` tags.

Values cross the boundary as raw `T` memory, which is sound for `CommData` (`Reloc`) types. On the Rust side,
`LolaFieldGetCaller`/`LolaFieldSetCaller` call the field FFI (zero-copy set is not supported: `ProxyField<T>::Set`
copies its argument), and `LolaFieldPublisher` maps `update()` to `SkeletonField::Update()` and
`register_set_handler()` to `SkeletonField::RegisterSetHandler()`.

### 1.4 Field get handler is optional

The PR #818 type-state required `register_get_handler_{field}()` before `offer()`, while LoLa offers no custom get
handler. References:

- AUTOSAR ara::com (EXP_ARAComAPI R22-11, §5.4.8): registering a get handler is "fully optional"; without one, the
  binding answers Get with the latest value set via `Update`. A custom getter is described as an exotic use case whose
  value is not synchronized with what subscribers receive. The offer-time requirements are a set handler for every
  field with a setter and an initial value.
- C++ LoLa (upstream `main`): no user get handler; offer checks only the set handler and the initial value.
- PR #818 review: the maintainers agreed the Rust side should not enforce a get handler if C++ does not.

`TypeStateValidator` therefore no longer tracks get handlers: `register_get_handler_{field}()` is still generated for
getter fields but leaves the type state unchanged. `offer()` requires every initial value, every set handler and
every method handler. The LoLa runtime logs and ignores a custom get handler. This should be replaced by upstream's
version once PR #818 lands.

### 1.5 Verification status

- `score_com_macros` builds with Cargo, and a stand-in crate using `TypeStateValidator` confirmed: `offer()` compiles
  with and without get handlers, and does not compile without a set handler or an initial value.
- The `FieldMemberOperationImpl` template logic was compiled (`g++ -std=c++17 -Wall -Wextra -Werror`) and run against
  stand-in field types.
- Rust unit tests with a mocked `FFIBridge` cover the method copy path, field get/set callers, field update and set
  handler, and field subscriptions.
- Not yet done: a full Bazel build and test run, and an integration test against a real C++ proxy/skeleton. No
  interface in this repository uses `EXPORT_MW_COM_FIELD` yet.

---

## 2. `message_passing`: nested calls from a callback (upstream #767)

### 2.1 Problem

`ClientConnection::SendWaitReply()` fails with `EAGAIN` when called from a callback running on the engine thread.
Upstream #767 describes the case: App1 calls App2, whose handler calls back into App1. The maintainers consider the
guard deliberate deadlock prevention, advise against cyclic synchronous coupling between applications, and are
designing a fix internally.

### 2.2 Prototype and findings

A prototype let a nested `SendWaitReply()` keep pumping `UnixDomainEngine`'s dispatch loop until its reply arrived,
with a 1-byte correlation id and auxiliary reply slots so that a nested call on an already busy connection did not
deadlock. Review of the prototype found:

- Conflicts with ASIL-B requirements in `score/message_passing/dependability`: `ServerCallbacksAreSequential` (a
  second callback of the same server starts before the first returns) and `ServerProcessesSinglePendingRequest` (the
  auxiliary slots send a second request on a connection before the first is answered). It also lifts the `EAGAIN`
  guard that mitigates the failure mode `IpcApiMisuseOrLifecycleViolation`.
- It existed only on `UnixDomainEngine`, while QNX is the safety target (`SafetyCertifiedTransportMechanism`), so host
  tests would pass scenarios that fail on target.
- Pumping the whole engine reenters arbitrary state, the same class of bug Project Zero reported for Mojo's
  synchronous calls (issue 42451509): a disconnect processed during the nested pump deletes a `ServerConnection` whose
  handler is still on the stack; a nested `poll()` overwrites the outer loop's `revents`, so the outer loop reads an
  already drained blocking socket and hangs; and nested receives overwrite the engine's single receive buffer.

### 2.3 Decision

The prototype was removed. `score/message_passing` matches upstream except for one independent fix:
`UnixDomainEngine::SendProtocolMessage()` cast the payload size to the 16-bit frame length without a check, so a
payload over 65535 bytes wrapped and desynchronized the stream. It now fails with `EMSGSIZE`.

#767 is handled by application design (no cyclic synchronous calls between applications) until upstream's fix lands.
If nested calls are ever needed in production, that is a requirements change: revise the two requirements above, add
the reentrancy failure modes to the safety analysis, implement it on QNX as well, and limit the nested pump to
REQUEST/REPLY with a preallocated, bounded receive-buffer pool.

---

## 3. E2E protection for Rust methods and fields (upstream #1062)

### 3.1 Upstream direction

- Nothing in `score/mw/com` implements E2E protection today, on the C++ or the Rust side.
- The maintainers stated that S-CORE cannot provide E2E functionality based directly on the AUTOSAR specifications,
  because of licensing restrictions.
- They are designing an E2E protection/verification API for C++. The Rust API is to be derived from it: the API
  mechanics can be designed freely, but the error classes are limited to what C++ provides.
- Events come first, methods after.

### 3.2 Decision

No E2E code in this fork. An earlier standalone prototype based on an AUTOSAR profile was removed. The Rust work waits
for the C++ API and starts with events.

Points to carry into that work:

- The method in-args and return buffers are allocated by C++ to the exact size of `Args`/`Return`. Any header added by
  the framework needs the size computation to grow on both sides at once (`CreateDataTypeSizeInfoFromTypes` and
  `MethodArgsCodec::layout()`).
- Methods need more protection metadata than events (request/response type, result, caller identity), so a shared
  mechanism should not assume an identical header.
- A lost or delayed method response is not detectable by payload protection; that needs a timeout.
- `MethodHandlerCall::call` returns `Return`, not a `Result`, so a handler cannot refuse a request whose check failed
  (an upstream concept change).
- Rust-side protection alone covers only Rust-to-Rust traffic.

If a product needs E2E before the upstream API exists, it belongs in the product layer at the payload level, using an
implementation the product is licensed to use, so that it can be replaced by the upstream API later.
