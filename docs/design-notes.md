# `communication` (`hskang-amelia` fork) — design notes

Append-only journal for this account's own work on top of `eclipse-score/communication`. Entries
are dated and never rewritten — later entries correct or supersede earlier ones explicitly rather
than editing them away. This file is **not** part of upstream's own Sphinx documentation build
(`docs/sphinx/`) — it's a separate, plain-Markdown log for this fork's own contribution work.

---

## 1. `mw::com` `Method<T>` (RPC) — porting PR #818's Rust design into this fork (2026-09-08)

### 1.1 Background — why this, why now

Rust `mw::com` has no RPC support today, which was identified as a high-leverage gap to close for
this account's broader Rust `mw::com` work.

Before touching any code, a research pass (background agent, this session) established the actual
state, correcting an earlier, second-hand assumption:

- **`hskang-amelia/communication_rust` is not the Rust binding at all** — it's a 2-file personal
  analysis log from a prior session. The real Rust binding lives inside **this** repo
  (`communication`), at `score/mw/com/rust/` (concept crate + macros) and
  `score/mw/com/impl/rust/com-api/` (FFI + per-backend runtime crates).
- **The C++ backend already fully implements Method** — `score/mw/com/impl/methods/`
  (`ProxyMethod`/`SkeletonMethod` and their specializations) is complete, tested (not a stub), with
  a real design doc (`score/mw/com/design/methods/README.md`). It's synchronous-only by design for
  now (`kCallQueueSize = 1`, `proxy_method_base.h`) — the design doc itself names async as future
  work.
- **Only the Rust binding is missing**, and not silently — `score_com_concept::interface_macros.rs`
  on this fork's `main` (before this entry) explicitly rejected `Method<T>`/`Field<T>` via
  `compile_error!` at macro-expansion time, pending exactly the design this entry ports in.
- Upstream PR **eclipse-score/communication#818** ("Rust::com Field and Method Rust Design and
  Example of APIs usage", author `bharatGoswami8`, open, non-draft as of this check) has the actual
  trait-level design. Upstream issue **#782** ("Improvement: Runtime implementation for Rust Method
  APIs") tracks the remaining runtime work — confirmed real via `WebFetch` (GitHub API access to
  `eclipse-score/communication` itself isn't available in this session's scope, only to this
  account's own forks).
- On the sync/async bridge question this account raised as a comment on #782, the maintainer
  answered directly: milestone 1 can wrap the existing synchronous C++ call in an already-resolved
  `Future` — real async dispatch needs C++-side issue **#767** (a real, unassigned, undiscussed
  reentrancy bug: a method call from within another method's callback handler fails) fixed first,
  and that is *not* a milestone-1 blocker. E2E (safety) protection for Rust Methods/Fields is
  explicitly out of scope for now (not even covered for the existing Rust Event impl), maintainer
  asked for a separate tracking ticket.

### 1.2 What PR #818 actually contains — verified by fetching its real branch

GitHub's PR-diff view isn't reachable from this session for `eclipse-score/communication` (out of
this session's repo scope), so the actual diff was fetched directly with plain `git` (anonymous
clone/fetch of the public repo, same technique used earlier this session for `score-crates#73` and
`lifecycle#489`): `git fetch origin pull/818/head:pr-818` against a throwaway clone of
`eclipse-score/communication`.

Confirmed by reading the real diff (`git diff --name-status main pr-818 -- 'score/mw/com/rust/*'
'score/mw/com/impl/rust/*'`, 37 files):

- **Trait/macro design — genuinely complete.** `score_com_concept`: `method_concept.rs` (368
  lines, new — `MethodCaller`/`MethodHandler`/`MethodInArgAllocator`/`MethodInArgMaybeUninit`/
  `MethodInArgPtr`/`MethodReturnSample`/`ZeroCopyArgs`), `field_concept.rs` (new),
  `interface_consumer_macros.rs`/`interface_producer_macros.rs`/`method_arities_macros.rs` (new),
  `interface_macros.rs` (rewritten, +953 lines — `Method<T>`/`Field<T>` now accepted instead of
  `compile_error!`'d). `score_com_macros::type_state_validator.rs` (new, 516 lines). Design docs
  (`design_document_method.md`/`design_document_field.md`) and diagrams included.
- **LoLa runtime — real files exist, bodies are `todo!()`.** Three files in
  `com-api-runtime-lola/`: `method.rs` (new, 246 lines — `LolaMethodCaller`/`LolaMethodHandler`/
  `LolaMethodInArgAllocator`/`LolaFieldGetCaller`/`LolaFieldSetCaller`, every method body
  `todo!("...")`, each with a `// TODO: https://github.com/eclipse-score/communication/issues/782`
  marker), `field_consumer.rs` (118 lines, field get/set caller bodies `todo!()`),
  `field_producer.rs` (113 lines, same). Field *subscription* itself (as opposed to get/set calls)
  reuses the existing Event `Subscriber`/`Subscription` trait machinery as-is — only the call-based
  get/set half is new and stubbed.
- **The FFI bridge layer is untouched by #818 — confirmed, this is the real remaining gap.**
  `com-api-ffi-lola/bridge_ffi_lola.rs` and `score_com_cpp_bridge/register_interface.cpp/.h` are
  *not* in the diff at all. Grepping this fork's `main` (before this entry) for `method`/`Method` in
  either file: zero hits. Every existing `extern "C"` function is Event-shaped
  (`mw_com_proxy_event_subscribe`, `mw_com_skeleton_send_event`, `mw_com_create_proxy`/
  `_skeleton`, ...). **So implementing #782 for real needs a new FFI export layer on both sides
  (new C++ functions calling into the already-complete `ProxyMethod`/`SkeletonMethod`, new Rust
  `extern "C"` declarations) *before* `method.rs`'s `todo!()` bodies can be filled in against
  anything real — this is more than "just fill in the todos", and PR #818 itself doesn't attempt
  it.** Reusable patterns spotted for that FFI layer: the callback-adapter pattern already used for
  event receive handlers (`FatPtr`/`mw_com_impl_call_dyn_fnmut` in `bridge_ffi_lola.rs`) is a
  template for `MethodHandler::register_handler`'s C++→Rust callback.
- PR #818's branch is ~9 days behind current upstream `main` at the time of this check (last
  commit 2026-08-30 vs. `main` HEAD 2026-09-08) — some rebase/conflict resolution should be
  expected if/when adopting further changes from it, though the specific files ported here applied
  cleanly against this fork's `main`.

### 1.3 What this entry actually did

Ported all 37 files from PR #818's current head verbatim into this fork's designated branch
(`claude/s-core-kyron-review-qp9ss0`) via `git show pr-818:<path>` per file (not a patch/merge —
each file either didn't exist yet here or is fully replaced), rather than attempting to
merge/rebase the whole PR branch (which would have pulled in unrelated churn — the PR's `main` had
independently diverged elsewhere, e.g. an unrelated `all_service_elements` test-directory
restructuring not part of this feature).

Commit: `560721e` "Port PR #818's Method<T>/Field<T> Rust design
(score_com_concept/score_com_macros)", pushed to this fork's `claude/s-core-kyron-review-qp9ss0`.
**Not pushed upstream, not commented on #818/#782** — user explicitly asked to scope this to the
fork only for now and think about the upstream side separately later.

**Verification status — genuinely none.** Other repos touched this session have real Cargo
workspaces this sandbox could `cargo build`/`test` against even without Bazel. **This repo has
zero `Cargo.toml` files anywhere**
(confirmed by `find`) — its Rust code is Bazel-only, and this sandbox has no Bazel (confirmed
earlier this session, `bazelisk`'s own download of the real `bazel` binary is blocked by org egress
policy). `score_com_concept`'s own dependencies (`containers::fixed_capacity`, `score_log`) are
themselves other in-repo-but-Bazel-external crates with no local path and no crates.io equivalent,
so even a hand-rolled standalone Cargo harness (the trick used for `kyron`'s `TimerWorker`) isn't
practical here. **This port is unverified — not "verified via a different, honest path" like this
session's other unverifiable-by-Bazel work, but genuinely unchecked by any compiler.** Treat it as
"faithfully copied from a real, existing PR branch" rather than "known to build."

### 1.4 FFI export layer + `LolaMethodCaller`/`LolaMethodHandler` implementation (2026-09-08)

Following up on §1.3's file port, this entry designs and implements the new Method FFI bridge and
fills in `method.rs`'s `LolaMethodCaller`/`LolaMethodHandler`/`LolaMethodInArgAllocator` bodies (the
`LolaFieldGetCaller`/`LolaFieldSetCaller` `todo!()`s are left as-is — see §1.5).

**Research first.** Read, in full, the C++ APIs the FFI layer needs to bridge:
`proxy_method_base.h`, all four `ProxyMethod<Signature>` specializations (concretely
`proxy_method_with_in_args_and_return.h`), `skeleton_method_base.h`/`skeleton_method.h`,
`proxy_method_binding.h`/`skeleton_method_binding.h` (the type-erased binding interfaces —
`GetInArgsBuffer`/`GetReturnValueBuffer`/`DoCall` on the proxy side, `RegisterHandler` with a fully
type-erased `void(QualityType, optional<span<byte>>, optional<span<byte>>)` callback on the skeleton
side), `method_signature_element_ptr.h`, and — the one piece not yet read as of §1.4's earlier
draft — `proxy_method_binding_factory.h`/`_impl.h`, `skeleton_method_binding_factory.h`/`_impl.h`
(confirms bindings are constructed via `ProxyMethodBindingFactory<Signature>::Create(handle,
parent_binding, method_name, method_type)` / `SkeletonMethodBindingFactory::Create(instance_id,
parent_binding, method_name, method_type)`, called from inside `ProxyMethod`/`SkeletonMethod`'s own
constructors — confirming the binding pointer only becomes reachable once a concrete
`ProxyMethod<Signature>`/`SkeletonMethod<Signature>` member already exists on the generated
Proxy/Skeleton, exactly like events). Also read the existing Event FFI bridge in full
(`registry_bridge_macro.h`/`.cpp`, `bridge_ffi_lola.rs`) as the template.

**Design decision — FFI boundary sits at the already-type-erased binding level.** Unlike events
(`ProxyEvent<T>`/`SkeletonEvent<T>` need a per-type `TypeOperations` vtable, since sample data itself
is generically typed), `ProxyMethodBinding`/`SkeletonMethodBinding` are *already* fully type-erased on
the C++ side (`score::cpp::span<std::byte>` + a queue position; the skeleton handler callback is
already `void(QualityType, optional<span<byte>>, optional<span<byte>>)`). So the new
`MethodMemberOperation`/`MethodMemberOperationImpl` registry classes
(`registry_bridge_macro.h`) need **no** `TypeOperations`-equivalent and no `Signature` template
parameter at all — confirming the determination made before this entry's predecessor was drafted.

**C++ side changes:**
- `proxy_method_base.h`: added `ProxyMethodBaseView::GetMethodBinding()`, mirroring
  `SkeletonMethodBaseView::GetMethodBinding()` (which already existed) — `ProxyMethodBase` had no
  equivalent accessor before this.
- `registry_bridge_macro.h`: new `MethodMemberOperation`/`MethodMemberOperationImpl` classes, a second
  registry map on `InterfaceOperations` (`RegisterMethodOperation`/`GetMethodOperation`, parallel to
  the existing event map — kept separate since Rust already knows statically, from the `interface!`
  macro expansion, whether a member is a Method or an Event/Field), matching
  `GlobalRegistryMapping::RegisterMethodOperation`/`FindMethodOperation`, a new
  `EXPORT_MW_COM_METHOD(method_member)` macro (no type parameter needed — see design decision above),
  and a new `mw_com_impl_call_method_handler` callback-adapter declaration (the skeleton-handler
  counterpart of the existing `mw_com_impl_call_dyn_fnmut`).
- `registry_bridge_macro.cpp`: six new `extern "C"` functions —
  `mw_com_get_method_from_proxy`/`_from_skeleton`, `mw_com_proxy_method_get_in_args_buffer`,
  `mw_com_proxy_method_get_return_value_buffer`, `mw_com_proxy_method_do_call`,
  `mw_com_skeleton_method_register_handler` (the last one adapts
  `SkeletonMethodBinding::TypeErasedHandler`'s two `optional<span<byte>>` parameters into flattened
  `(ptr, len, present)` triples for the FFI boundary, calling into `mw_com_impl_call_method_handler`).
  **Known gap, flagged explicitly rather than fixed**: `SkeletonMethodBinding` exposes no
  unregister/dispose hook, so the boxed Rust closure `mw_com_skeleton_method_register_handler`
  registers currently leaks for the process's lifetime once registered — there is no C++-side
  callback to free it through, unlike the event receive-handler path
  (`mw_com_proxy_clear_event_receive_handler` + `mw_com_impl_delete_boxed_fnmut`).

**Rust side changes:**
- `bridge_ffi.rs` (the `FFIBridge` trait crate): new opaque `ProxyMethodBinding`/`SkeletonMethodBinding`
  structs, six new trait methods on `FFIBridge` (`get_method_from_proxy`/`_from_skeleton`,
  `proxy_method_get_in_args_buffer`/`_get_return_value_buffer` returning `Option<(*mut u8, usize)>`,
  `proxy_method_do_call`, `skeleton_method_register_handler`).
- `bridge_ffi_mock.rs`: matching `mock! { ... }` declarations and `SharedMockBridge` forwarding impls
  for all six (this crate hand-writes its mockall block rather than using `#[automock]`, so both had
  to be updated in lockstep with the trait).
- `bridge_ffi_lola.rs`: the six extern `"C"` declarations, `LolaFFIBridge`'s implementations of them,
  and the new `mw_com_impl_call_method_handler` Rust-side definition (transmute + `catch_unwind` +
  `process::abort` on panic, same pattern as the existing event adapters).
- `consumer.rs`/`producer.rs`: added `pub(crate)` accessors (`LolaConsumerInfo::interface_id()`/
  `bridge()`, `NativeProxyBase::as_ptr()`, `LolaProviderInfo::interface_id()`/`bridge()`/
  `skeleton_ptr()`) — needed by `method.rs` (a sibling module) to reach otherwise-private fields;
  deliberately narrow (raw-pointer/string/bridge-clone getters only, no new mutable access).
- `runtime.rs`: `LolaMethodCaller`/`LolaMethodHandler`'s third generic parameter changed from a fully
  generic `R: Runtime` to a concrete `B: FFIBridge` (see below for why), so
  `type MethodCaller<...> = LolaMethodCaller<Args, Return, Self>` became
  `LolaMethodCaller<Args, Return, B>` (and the same for `MethodHandler`).
- `score_com_concept/error.rs`: added `MethodFailedReason` + `Error::MethodError` (no Method-shaped
  error variant existed before — the concept crate's error enum was written before any Method
  implementation existed to report a failure from).

**Why `LolaMethodCaller`/`LolaMethodHandler` needed a generic-parameter change.** The ported (from PR
#818) stub declared `LolaMethodCaller<Args: MethodArgs, Return: CommData, R: Runtime>` implementing
`MethodCaller<Args, Return, R>` fully generically over `R`. That's fine for a type that only ever
touches `Args`/`Return` and never needs to *do* anything runtime-specific — but a real implementation
needs concrete access to `R::ConsumerInfo` (to get a bridge + interface id + handle) and to make actual
FFI calls, and Rust can't project "the concrete `B: FFIBridge` behind this generic `R: Runtime`" back
out of an opaque `R::ConsumerInfo` associated-type value without already naming `B` as its own
parameter. So `LolaMethodCaller`/`LolaMethodHandler` now take `B: FFIBridge` directly (their 3rd
parameter) and implement `MethodCaller<Args, Return, LolaRuntimeImpl<B>>` concretely — exactly the
same shape every other Lola runtime type (`LolaSubscribableImpl<T, B>`, `LolaPublisher<T, B>`, …)
already uses, so this brings Method in line with the rest of the crate rather than introducing a new
pattern.

**The buffer-layout problem, and how it's solved.** The C++ binding packs a method's *entire*
argument list into one shared type-erased buffer (`ProxyMethodBinding::GetInArgsBuffer`), using
`type_erased_storage.h`'s `CreateDataTypeSizeInfoFromTypes`/`SerializeArgs`/`DeserializeArgs`: each
argument placed sequentially at the next offset that's a multiple of its own `alignof`, exactly
matching what a C compiler would do for `struct { Arg1 a; Arg2 b; ...; }`. Rust's `Args` is a tuple
(e.g. `(T1, T2)`), and — critically — **Rust's own tuple layout is *not* guaranteed to match this**:
`#[repr(Rust)]` lets rustc reorder tuple fields, so treating the whole tuple as one
`size_of::<Args>()`-sized blob and `memcpy`-ing it as a unit would have been a real (silent,
non-compile-time-detectable) memory-corruption bug for any method with 2+ arguments — even though
each individual argument type is itself safely byte-copyable (`CommData: Reloc`). This wasn't a risk
for events (`CommData` for a single sample type `T`, no cross-field ordering question) or for
`Return` (also always a single type, never a tuple).

Fixed by writing a small LoLa-local trait, `LolaCopyCodec` (`method.rs`, arity 0–8, generated by a
`macro_rules!` mirroring `score_com_concept::method_arities_macros`'s own `T1..T8`/`impl_all_arities!`
naming without depending on it), which serializes/deserializes `Args` **field by field**, using the
identical sequential-natural-alignment offset algorithm as the C++ side, rather than as one blob. The
zero-copy path (`allocate`/`invoke_zero_copy`) doesn't need this trait at all: `LolaMethodInArgAllocator`
reproduces the same algorithm one field at a time via a `Cell<usize>` running offset, correct only
because `MethodArgsAllocate::alloc_uninit`'s macro-generated body calls `allocator.allocate::<T>()`
once per field in strict left-to-right order (Rust guarantees tuple-literal evaluation order) and a
fresh allocator is constructed per in-flight call (sound given `kCallQueueSize == 1`, i.e. only one
call ever in flight at a time in this milestone).

**`void`-returning methods.** `Return: CommData` is fully generic in the upstream-ported trait
signature (unchanged here), so there's no compile-time way to special-case `Return == ()`. Detected
instead via `TypeId::of::<Return>() == TypeId::of::<()>()` (sound: `CommData: Reloc: 'static`, and
`TypeId` equality between two `'static` types is a genuine proof of same-type-ness — the same
principle `Any::downcast_ref` relies on) to decide whether to call `GetReturnValueBuffer`/read a
value back at all, versus producing `()` via `transmute_copy` (safe here specifically because the
`TypeId` check already proves `Return` *is* `()`, so this is a same-type, zero-sized copy, not a real
reinterpretation).

**Verification status — unchanged from §1.3, if anything higher-stakes here.** Still zero Cargo.toml,
still no Bazel in this sandbox — none of this C++ or Rust code has been compiled. The buffer-layout
algorithm above is the single highest-risk correctness assumption in this whole entry: getting it
wrong would be a silent memory-safety bug, not a compile error, and there is no way in this sandbox to
even write a unit test that would catch a mistake in it (there's no C++ counterpart to run against).
Flagged in the FFI functions' own doc comments (`registry_bridge_macro.cpp`, `method.rs`'s
`LolaCopyCodec`) rather than only here, so it's visible to whoever eventually reviews or tries to build
this with real Bazel.

**Also not addressed in this entry:** `field_consumer.rs`/`field_producer.rs` (`LolaFieldGetCaller`/
`LolaFieldSetCaller` in `method.rs` are untouched, still `todo!()` — they route through this same
Method machinery underneath, and are the natural next step, but weren't done here); the handler-leak
gap noted above; real async dispatch (blocked on upstream C++ issue #767, out of scope per the
maintainer's own guidance, see §1.1).

### 1.6 Field<T> — `field_consumer.rs`/`field_producer.rs` + `LolaFieldGetCaller`/`LolaFieldSetCaller` (2026-09-08)

Follows up on §1.4's "not addressed" list: implements Field support end to end (get/set/subscribe),
using the FFI layer from §1.4 for get/set and adding two small new FFI calls for subscription-only
queries.

**Research first — how Field actually maps onto Method/Event, confirmed via the real C++ headers**
(`score/mw/com/impl/proxy_field.h`, `plumbing/proxy_field_binding_factory_impl.h`,
`skeleton_field.h`): `ProxyFieldImpl`/`SkeletonFieldImpl` are pure *composition*, not new binding
machinery. A field with all three capability tags holds, concretely:
- a real `ProxyEvent<T>`/`SkeletonEvent<T>` (built through the ordinary
  `ProxyEventBindingFactory`/`SkeletonEventBindingFactory`, tagged `ServiceElementType::FIELD`, but
  otherwise identical to a plain event), registered under the field's own bare member name — this is
  `WithNotifier`;
- a `ProxyMethod<GetMethodSignature<T>>`/`SkeletonMethod<GetMethodSignature<T>>` built through the
  *same* `ProxyMethodBindingFactory`/`SkeletonMethodBindingFactory` §1.4 already wired up, registered
  under `{name}_get` — this is `WithGetter`;
- likewise a `..SetMethodSignature<T>>` instance under `{name}_set` — this is `WithSetter`.

Confirmed on the Rust side too, in `score_com_concept`'s already-ported `field_concept.rs` and
`interface_consumer_macros.rs`/`interface_producer_macros.rs` (read in full this entry): the
`interface!` macro generates `{name}_get: R::FieldGetCaller<T>`/`{name}_set: R::FieldSetCaller<T>` as
separate consumer-struct fields, constructing them via `<R::FieldGetCaller<T> as
MethodCaller<(), T, R>>::new(concat!(stringify!($name), "_get"), ...)` — i.e., **field get/set are
just `MethodCaller` instances under a name-suffix convention**, calling into exactly the same
`ProxyMethodBinding` FFI §1.4 already built. `FieldSubscriber<T,R>` is likewise defined as a marker
supertrait of `Subscriber<T,R>` (constraining only its `Subscription` associated type to also
implement `FieldSubscription<T,R>`), reusing `Subscriber`/`Subscription` wholesale.

**What this meant for the implementation — almost no new FFI needed:**
- `LolaFieldGetCaller<T, B>`/`LolaFieldSetCaller<T, B>` (`method.rs`) are now thin wrapper structs
  delegating every `MethodCaller` method to an internal `LolaMethodCaller<(), T, B>`/
  `LolaMethodCaller<(T,), T, B>` — no new FFI, no new logic, just composition. (They exist as their
  own named types, rather than being plain type aliases for `LolaMethodCaller`, only because
  `Runtime::FieldGetCaller`/`FieldSetCaller` are declared as distinct associated types in
  `score_com_concept`, presumably so a future runtime could route them differently — LoLa doesn't need
  to.)
- `LolaFieldSubscriber<T, B>`/`LolaFieldSubscription<T, B>` (`field_consumer.rs`) are thin wrappers
  around `consumer.rs`'s already-fully-implemented `LolaSubscribableImpl<T, B>`/`LolaSubscriberImpl<T,
  B>` — again, no new FFI for the base `Subscriber`/`Subscription` methods.
- `LolaFieldPublisher<T, B>` (`field_producer.rs`) composes a `LolaPublisher<T, B>` (`producer.rs`'s
  event publisher — `update()`/`allocate()`/`FieldSampleMut::update()` are literally `self.notifier
  .send(value)`/`.allocate()`/the wrapped `EventSampleMut::send()`, since `Publisher::send()` is
  already a *default* trait method built on `allocate`+`write`+`send`) plus its own two
  `SkeletonMethodBinding`s for get/set, reusing §1.4's `register_method_handler_raw`/
  `mw_com_skeleton_method_register_handler`/`mw_com_impl_call_method_handler` machinery directly.
- `LolaFieldSampleMut`/`LolaFieldSampleMaybeUninit` (`field_producer.rs`) are pure newtype wrappers
  around `producer.rs`'s `LolaSampleMut<'a,T,B>`/`LolaSampleMaybeUninit<'a,T,B>`.

**The one place `LolaMethodHandler`/`MethodHandlerCall` genuinely couldn't be reused.**
`FieldPublisher::register_set_handler`/`register_get_handler` take the field's value **by value**
(`Fn(T) -> T` / `Fn() -> T` — confirmed against `field_concept.rs`'s real trait, not
`design_document_field.md`'s prose, which says `Fn(T)` with no return value in one place and disagrees
with the actual code — the code is ground truth, so `Fn(T) -> T` is what's implemented here), whereas
`MethodHandlerCall::call(&self, args: &Args) -> Return` always hands the argument **by reference**.
Adapting the by-value callback through `MethodHandlerCall` would need to clone `T` out of a `&T`,
imposing an unwanted `T: Clone` bound `CommData` doesn't otherwise require. Solved by extracting
`method.rs`'s FFI-registration dance itself (`Box::into_raw` + transmute-to-`FatPtr` + register + free
on failure) into a shared `pub(crate) fn register_method_handler_raw`, and having
`register_set_handler`/`register_get_handler` build their own closures directly against
`LolaCopyCodec::read_from` (which already deserializes a whole `Args` tuple **by value**, via
`ptr::read` — an owned move, no cloning) instead of going through `MethodHandlerCall` at all.
`LolaCopyCodec` was made `pub(crate)` (from a `method.rs`-private trait) for `field_producer.rs` to
reach it.

**Genuinely new FFI — subscription-only queries.** `FieldSubscription::get_free_sample_count()`/
`get_num_new_samples_available()` have no equivalent on the plain `Subscription` trait, and no FFI
existed for them (checked `bridge_ffi.rs` — only `subscribe_to_event`/`unsubscribe_to_event`/
`get_samples_from_event`/the receive-handler pair existed). Added
`mw_com_proxy_event_get_free_sample_count`/`mw_com_proxy_event_get_num_new_samples_available`
(`registry_bridge_macro.cpp`, wrapping `ProxyEventBase::GetFreeSampleCount()`/
`GetNumNewSamplesAvailable()` directly — no registry lookup needed, they take an already-obtained
`ProxyEventBase*`) plus the matching `FFIBridge` trait methods/mock declarations/`LolaFFIBridge` impl,
and exposed them as two new `pub(crate)` methods directly on `LolaSubscriberImpl` (`consumer.rs`) —
reusing its existing `ProxyEventManager` guard rather than adding a second, guard-bypassing access
path for what's a much less frequently called query.

**A field lacking a capability tag.** `LolaFieldPublisher::new()` has no compile-time way to know
which of `WithGetter`/`WithSetter`/`WithNotifier` apply (that's tracked only by the macro-generated
`Producer` struct's type-state validator, one layer up) — so a missing tag just means the
corresponding C++-side member was never registered, and the FFI lookup returns null, stored as `None`
rather than a hard construction error. `update()`/`register_set_handler()`/`register_get_handler()`
log an error (or return one) if called against a `None` piece, rather than panicking — should never
actually happen against well-formed generated code, since the type-state validator only ever calls
the methods a field's own tags support.

**`LolaMethodCaller<Args, Return, B>`'s `MethodCaller` impl bound (`Args: LolaCopyCodec`) already
covered `()`/`(T,)`** — no changes needed there for Field to compile against it; `runtime.rs`'s
`FieldGetCaller`/`FieldSetCaller` associated types needed the same `Self` → `B` generic-parameter fix
§1.4 already made for `MethodCaller`/`MethodHandler` (`FieldPublisher`/`FieldSubscriber` were already
declared over `B` directly in the ported stub, so those two needed no change).

**Verification status — unchanged.** Still zero Cargo.toml, still no Bazel in this sandbox. Everything
in this entry is reasoned through against the real C++ headers and the real, already-ported
`score_com_concept` trait/macro code, not compiled or tested.

### 1.5 Deliberately not done in this entry

1. Any upstream action — no comment on #818/#782/#767, no PR against `eclipse-score/communication`.
   User's explicit instruction: fork only for now, think about upstream separately.
2. Filling in any of the three `todo!()` files' actual bodies — that's the in-progress next phase
   (§1.4), not this entry.
3. Rebasing PR #818's own branch against current upstream `main` — out of scope; this entry only
   copied the specific files needed, not adopted the branch wholesale.

---

## 2. `message_passing` reentrancy fix experiment for communication#767 (2026-09-09)

### 2.1 Background

Upstream issue **eclipse-score/communication#767** ("Method call within a method callback handler
not possible"): `ClientConnection::SendWaitReply()` (`score/message_passing/client_connection.cpp`)
unconditionally rejects with `EAGAIN` any call made from within a callback already running on the
engine's own dispatch thread — the scenario in the issue is App1 calling App2's `BarMethod`, whose
handler calls back into App1's own `FooMethod` synchronously. Maintainer `kitsnet` confirmed the
guard is a real deadlock-prevention measure, not an oversight; maintainer `LittleHuba` said a fix
was being designed internally ("nailing down details"), declined to share specifics yet, and
separately advised against cyclic bidirectional synchronous coupling between apps in general.

This entry is a **fork-only experiment**, branch `claude/fix-767-nested-method-reentrancy` — not
proposed upstream, no comment on #767 itself, per this account's standing instruction to keep this
kind of exploration scoped to the fork.

Confirmed this session (`bazel build`/`bazel test` actually work here against this repo's C++
targets, unlike the Rust/Cargo situation in §1 — `bazel build //score/message_passing:client_connection_test
//score/message_passing:unix_domain_test` compiles and runs cleanly), so this entry's fixes are
genuinely compiled and test-verified, not just reasoned through.

### 2.2 First attempt — nested pumping (commit `0969cc3e`)

**Root cause, precisely**: for `UnixDomainEngine`, one single background thread (`thread_`,
`RunOnThread()`'s `poll()` loop) does *both* jobs — invoking server-side request handlers *and*
processing the reply that would unblock a client's own `SendWaitReply()`. Confirmed by tracing the
actual call path: `score/mw/com/impl/bindings/lola/proxy_method.cpp` → `LolaMessaging::CallMethod()`
→ `MessagePassingServiceInstance::CallMethod()` (comment there names `SendWaitReply` directly) — so
`mw::com`'s C++ `ProxyMethod`/`SkeletonMethod` genuinely goes through this exact guard, not just
`message_passing`'s own direct users. `QnxDispatchEngine` was checked too: it already uses *two*
separate `DispatchThreadRunner`s (`client_runner_`/`server_runner_`), so this specific single-thread
bottleneck may not even apply there — left untouched, see §2.5.

**Fix**: extracted `UnixDomainEngine::RunOnThread()`'s per-iteration body (due timers, one `poll()`
pass, dispatch ready endpoints) into a new `PumpNestedIteration()`, exposed on `ISharedResourceEngine`
behind a new `SupportsNestedPump()` capability flag (defaults to `false` — every other engine keeps
today's `EAGAIN` behavior unless it opts in; only `UnixDomainEngine` does). `SendWaitReply()`, when
called from within a callback and the engine supports it, no longer parks on the condition variable —
it keeps calling `PumpNestedIteration()` (the same dispatch step recursively) until its own reply
arrives, the same technique single-threaded reactors (COM message pumping, Qt/GLib nested event
loops) use for reentrant calls. Bounded by a `thread_local` depth counter, `ELOOP` past 8 levels, so a
genuinely unbounded chain fails fast instead of growing the C++ stack forever.

Verified with a new end-to-end regression test (`unix_domain_server_to_client_test.cpp`,
`BarMethodCallingFooMethodFromItsOwnCallbackSucceeds`) reproducing the issue's own diagram with real
sockets and threads (two separate `UnixDomainEngine`s, one per simulated app): confirmed it fails with
`EAGAIN` (reply payload is 1 byte, the error marker) against the pre-fix code and resolves cleanly
with it, by temporarily `git stash`-ing just the fix files and re-running.

### 2.3 Adversarial check found a second, worse bug — same-connection reentry hangs (commit `2a2b3b8e`)

Asked directly by the user afterward to verify this was actually right rather than taking the passing
integration test at face value. `--config=tsan` doesn't work in this sandbox (the registered
`llvm_toolchain` needs a newer glibc than this box has — confirmed the same failure independently
blocks the *default* toolchain's own `-fsanitize=thread` runtime too, and even blocks a plain
`bazel build //score/message_passing/...`/`:all` for an unrelated reason, a `rules_rust` build tool
needing the same newer glibc; none of this is fixable from inside the sandbox), so verification here
is test-based and manual code tracing, not sanitizer-based.

Tracing `SendWaitReply()` again with an adversarial eye: if a call is already in flight on a
connection (`waiting_for_reply_.has_value()`), a *new* call gets queued (`TryQueueMessage`) rather
than sent — fine for two independent external callers, but if the new call is itself the *nested* one,
the only thing that can ever drain that queue is the in-flight call's own reply, which depends on the
very callback the nested call is running inside of. Circular wait, and — unlike the original
`EAGAIN` guard — nested-pumping cannot rescue it, since pumping the engine's *other* I/O can't produce
a reply that depends on this call stack returning first.

**Confirmed empirically**, not just reasoned through: added
`SelfRecursiveCallOnSameConnectionDoesNotHangOrCrash` — a server whose handler calls back into a
*second* `ClientConnection` that targets its own server identifier (so the reentrant call revisits a
connection already busy) — run on a background `std::thread` with a 3-second bound so a real hang
can be detected without hanging the whole test binary (and, critically, *not* attempting any normal
cleanup/`Stop()` on the timeout path, since those would also hang — deliberately leaking via raw
`new` with no matching `delete` in that branch, rather than through the fixture's usual stack-scoped
RAII objects). It hung for the full 3 seconds against the §2.2-only code, confirming the second bug
for real.

**Fix**: when a nested call finds its own connection already busy, fail immediately with `EDEADLK`
instead of queuing — this specific shape can be proven undecidable-in-general/unsafe rather than
merely "probably fine", so it's the one case still worth rejecting outright. Re-ran the same test:
resolves in ~2ms with `EDEADLK` instead of hanging 3s. Re-ran the full existing suite plus both new
regression tests together afterward — all pass, including the original #767 reproduction (different
connections, still fixed) and the mocked `SendWaitReplySucceedsWhenCalledInCallbackAndEngineSupportsNestedPump`
unit test (single call, no queuing involved, unaffected by this second fix).

**Important, and worth being honest about**: this second bug is *not specific to the nested-pump
approach* — an alternative "dispatch server callbacks on a separate thread" design (considered and
rejected earlier for being a much larger, more invasive change touching `UnixDomainServer`'s dispatch
model rather than just `ClientConnection`) would hit the *identical* wall, since `waiting_for_reply_`
is per-connection state, not per-thread state. It's inherent to `ClientConnection` only ever
supporting one in-flight call per connection at a time — matching exactly what `LittleHuba` warned
about generically re-reading the issue thread ("tightly coupl[ing] two applications bi-directionally
... can easily lead to a deadlock").

### 2.4 What a real fix looks like — not just a sketch, this is already on the roadmap

Re-reading `score/mw/com/design/methods/README.md`'s own "Call queue handling" section confirms this
isn't invented for this entry: *"Currently, we are restricting the call-queue size for a method in a
proxy instance to a single entry. Since we currently only support synchronous method calls this is a
sensible restriction. Nevertheless, the `impl::ProxyMethod` class template provides the semantic
functionality to handle larger call-queues, which might be used in future for asynchronous method
calls."* — i.e. the **`ProxyMethod`/`MethodInArgPtr`/`MethodReturnPtr` layer already assumes multiple
concurrent in-flight calls per proxy method are coming eventually**; it's specifically the
`message_passing` layer underneath (`ClientConnection`'s single `waiting_for_reply_` slot) that hasn't
caught up. §2.3's `EDEADLK` fail-fast is a safety net for *today's* single-slot reality, not a
substitute for this.

User asked, after §2.3, to actually prototype this rather than just describe it — done in §2.5.

### 2.5 Multi-slot, correlation-ID prototype (branch `claude/fix-767-nested-method-reentrancy`, follow-up commits)

**Scope, decided up front.** `ClientConnection`'s `waiting_for_reply_` (a single `optional<ReplyCallback>`)
replaced with `waiting_for_reply_` (kept, now called "the primary slot") *plus* a small fixed-size
`pending_nested_calls_` table (`kMaxNestedPendingCalls = 4`, `client_connection.h`) of *additional*
slots a nested call can use instead of queuing behind an already-busy connection. Every REQUEST/REPLY
payload gets a 1-byte correlation id prefix (`kPrimaryCorrelationId = 0` for the primary slot, 1..4 for
the auxiliary ones), added/stripped transparently — `IClientConnection`/`IServerConnection`'s public
API (`SendWaitReply`, `Reply`, `MessageCallback`, ...) is completely unchanged. Deliberately **not**
attempted: making the slot count a `client_config_`/`server_config_` knob (hardcoded instead), touching
`fully_ordered` ordering guarantees for the auxiliary slots (only the primary slot's ordering semantics
are preserved as before), folding the id into `SendProtocolMessage`'s own wire framing instead of the
message payload (would need touching both engine backends' transport code, not just `ClientConnection`/
`UnixDomainServer`), accounting for the extra byte in `max_send_size_`/`max_reply_size_` validation, or
touching `QnxDispatchEngine` at all (same rationale as §2.2 — untestable here, and its two-runner-thread
design may not even need this). This is a sketch proving the mechanism, not a finished feature.

**What changed, concretely:**
- `client_server_communication.h`: new `CorrelationId` (`uint8_t`) and `kPrimaryCorrelationId` (`0`).
- `client_connection.h`/`.cpp`: `pending_nested_calls_` table, `FindFreeNestedSlotUnderLock()`,
  `BuildCorrelatedMessage()` (prepends the id byte into a small owned buffer — an actual allocation per
  send in this prototype, unlike the rest of the class's careful allocation-free design; a real version
  would preallocate scratch buffers the way `send_storage_` already does for queued sends),
  `ResolvePendingCallUnderLock()` (routes an incoming REPLY's id to the primary slot or the matching
  auxiliary one). `SendWaitReply()`'s nested branch: on finding the primary slot busy, tries
  `FindFreeNestedSlotUnderLock()` before falling back to §2.3's `EDEADLK` — only when *every* slot
  (primary + all 4 auxiliary) is genuinely busy does it still fail fast, exactly as before, just with a
  higher ceiling. `ProcessInputEvent`'s `REPLY` case strips the id byte and calls
  `ResolvePendingCallUnderLock()` instead of assuming there's only ever one thing to resolve.
  `ProcessSendQueueUnderLock()`/`SendWithCallback()`'s direct-send paths also prefix `kPrimaryCorrelationId`
  now, since anything reaching the wire through the primary slot needs to be self-consistent with what
  `ProcessInputEvent` now expects on the way back.
- `unix_domain_server.h`/`.cpp`: `ServerConnection` gained `current_request_id_`, saved/restored around
  each `sent_with_reply_callback_`/`OnMessageSentWithReply` invocation in `ProcessInput()` (a plain
  local-variable save/restore is enough, not an explicit stack, because dispatch is single-threaded and
  call-stack-reentrant, not genuinely concurrent — the same property §2.2 leaned on for the pump itself).
  `Reply()` echoes back whichever id was active when it's called, automatically — so a handler invoked
  reentrant (nested, before an earlier invocation on the same connection replied) just calls `Reply()`
  exactly as it always did, with no id-awareness of its own.

**Verified — including confirming the fix that necessitated this**: the exact reply-framing mismatch
this refactor could plausibly introduce (a REPLY missing its new id prefix silently never resolving its
future, hanging in the pump loop forever) is *not hypothetical* — it's precisely what happened when the
existing mocked unit test `SendWaitReplySucceedsWhenCalledInCallbackAndEngineSupportsNestedPump` and the
integration test `SendWaitReplyFailsWhenReceiveTooLong` were first run against this change, both still
using pre-§2.5 raw (unprefixed) reply payloads in their mocks — both hung instead of failing loudly,
`bazel test` output showing nothing beyond "Terminated" until narrowed down with individually-filtered,
short-timeout runs. Fixed by updating both mocks to include the id prefix (documented inline in each
test). Whole point of writing this down: **the fix for the fix's own test breakage is itself evidence
this kind of framing mismatch fails silently (a hang, not a clean error) rather than loudly** — worth
remembering if this prototype is ever taken further.

Once those two were fixed, the full existing suite (`client_connection_test`, `unix_domain_test`,
`non_allocating_future_test`) passes, *and* §2.3's own regression test
(renamed `SelfRecursiveCallProceedsUntilSlotsExhaustThenFailsFast`, since its old name/assertion no
longer matched what happens) now demonstrates the actual improvement: the self-recursive same-connection
call proceeds to **depth 5** (1 primary + 4 auxiliary slots, each genuinely sent and independently
correlated over the real socket) before hitting `EDEADLK` — versus depth 1 (immediate failure) before
this entry. Confirmed by temporarily reverting just this entry's fix files (`git stash`) and observing
the test correctly fail (`result.value()[1]` was `1`, not `5`) against the §2.3-only code.

**What this does and doesn't prove.** It proves the core idea — decoupling reply correlation from "one
call in flight per connection" via an explicit id — genuinely works over a real transport, not just in
theory, and that it's additive/non-breaking to the existing single-slot path (every pre-existing test
needed only the id-prefix fix, no logic changes). It does **not** prove a production design: a real
version needs the slot count to scale with actual need (not a hardcoded 4), needs to not allocate a
buffer per send, needs the id in the wire header rather than the payload so `max_send_size_`/
`max_reply_size_` stay meaningful, needs `QnxDispatchEngine` parity, and needs the *server* side to
expose this to more than one hardcoded internal field if it's ever to support truly concurrent request
handling (right now `current_request_id_` still assumes strictly nested, never actually parallel,
dispatch — true for every engine here, single-threaded, but worth stating outright since it's a real
constraint on the design, not an incidental implementation detail).

---

## 3. E2E protection design research for communication#1062 (2026-09-10)

### 3.0 Sourcing note

How this entry draws on the AUTOSAR specifications, written down so the next one doesn't have to
settle it again:

- Findings are in our own words. Nothing is quoted.
- Tables, figures and worked examples stay in the documents. The CRC prototype validates against the
  public "CRC-32/AUTOSAR" catalog check value instead (§3.4), which pins the same parameters and
  needs no document to check against.
- The specification PDFs stay out of the tree — `.gitignore` covers them.
- Requirement identifiers aren't cited; findings are attributed to the document that carries them
  (`FO_RS_E2E`, `FO_PRS_E2EProtocol`). The rest of the codebase cites `[SWS_CM_*]` freely, so this is
  a choice for these notes rather than a repo rule — §3.8.4 E covers reintroducing traceability
  properly when it's actually needed.
- Protocol facts — field order and widths, the CRC polynomial — are used as facts. They're what any
  implementation of this profile embodies, and this repository implements the Adaptive AUTOSAR
  Communication Management specification by its own README's declaration.
- The AUTOSAR name appears descriptively. Nothing here claims conformance or certification, and no
  type or API is named after it.

### 3.1 Background

Upstream **eclipse-score/communication#1062** ("Improvement: E2E protection for Rust Method/Field
APIs") is this account's own tracking issue, opened per maintainer `bharatGoswami8`'s request on #782
(§1.1 above) once E2E was confirmed out of scope for #782's own milestone. As of this entry, the issue
has no design (`How: Not designed yet`) and one comment from `bharatGoswami8` asking for E2E to be
handled at the Rust API payload level, independent of whether the communication type is an Event,
Field, Method, or anything else.

Before drafting a reply or any code, the AUTOSAR specification documents themselves were read locally
(`FO_RS_E2E`, `FO_PRS_E2EProtocol`, `CP_SWS_E2ETransformer`, `CP_SWS_BSWGeneral` — all freely
downloadable from autosar.org) rather than relying on secondhand summaries — see §3.0 above for how
they're drawn on below.

### 3.2 What this repo has today — nothing

Exhaustive search (filenames + content grep for "e2e", CRC, checksum, sequence counter, AUTOSAR
data-protection terms) across `score/mw/com/` (C++ and Rust both) found zero matches. No `design/e2e/`
doc either. This isn't a partial-support gap to extend — it's genuinely greenfield for this codebase,
on both the C++ and Rust sides, for every communication type (confirming `bharatGoswami8`'s own
"not even covered for the existing Rust Event impl" from #782).

### 3.3 What the actual AUTOSAR specs say

- **`FO_RS_E2E`** (Foundation-cluster requirements, read in full): it records the adaptive-platform
  E2E use cases as still being worked out — i.e. AUTOSAR's own spec hasn't settled AP use cases
  either; this is genuinely open design space, not a "go port CP's answer" task. Three of its
  requirements matter here. One requires method/client-server support for both AP and CP, and
  extends to the server deciding whether to apply a method call based on the E2E check result.
  Another requires variable-length data support, again for both platforms. A third covers the E2E
  Transformer, invoked via RTE — and that one is **CP-only**; AP has no standardized equivalent
  auto-wiring mechanism. Explicit caveat: E2E cannot itself solve a lost/delayed *method response*
  timeout — that needs a separate mechanism at the application or communication-management layer (a
  related but distinct concern from #767's own reentrancy work above).
- **`FO_PRS_E2EProtocol`** (the actual protocol spec, 314pp, targeted read): defines both regular
  profiles (1,2,4,5,6,7,8,11,22,44,76 — for events/signals) and a separate **method-specific "Xm"
  family (4m, 7m, 8m, 44m)**, added specifically because request/response and multi-client addressing
  aren't meaningful concepts for plain signal data. Xm profiles add, beyond the regular
  Counter/DataID/Length/CRC fields: **Message Type** (request/response), **Message Result**
  (OK/error), and **Source ID** (which client called). Concrete layouts: Profile 4m = 16-byte header
  (32-bit CRC, ≤4KB payload), 7m = 24-byte header (64-bit CRC, ≤4MB), 8m = 32-bit CRC variant of 4m/7m.
  **This is a direct, concrete complication for `bharatGoswami8`'s "independent of communication
  type" framing**: the wrap/unwrap *mechanism* can be shared, but Method needs strictly more fields
  than a plain Event/Field would — worth raising explicitly rather than assuming "type-agnostic" means
  "identical wire format" for all three.
- **`CP_SWS_E2ETransformer`**: confirms the architectural pattern worth mirroring — E2E is applied
  transparently between the RTE and the network binding, configured declaratively (not by application
  code), with dedicated sequence diagrams (§9.3) for E2E-protected *method* calls specifically. The
  natural equivalent boundary in this codebase's architecture is the Rust↔C++ FFI bridge (the same
  layer §1's Method<T> port and the #767 branch's correlation-id work both live at) — application-level
  method handlers would stay just as unaware of E2E as a CP SW-C is.
- **`CP_SWS_BSWGeneral`**: not E2E-specific — the cross-cutting rulebook every CP Basic Software module
  follows (MISRA-C, Init/DeInit/GetVersionInfo shape, error classification, config variants).
  Referenced directly by E2ETransformer's own spec (§5.1) as a dependency, which is why grabbing it
  alongside the E2E docs was reasonable. Also directly answers the user's own question of "what even
  is CP" — AUTOSAR Classic Platform, RTE-code-generated, structurally unrelated to the Adaptive
  Platform `ara::com`-style library this codebase actually implements; E2E itself originated in CP
  before being generalized to the AP+CP-shared Foundation cluster (hence living in `FO_*`, not `CP_*`
  or `AP_*`, documents).

### 3.4 Standalone CRC prototype (not wired into the crate)

Checked whether a Rust prototype could actually be wired into `com-api-runtime-lola/method.rs`'s real
FFI call sites (`invoke_with_copy`, `do_call_and_read_return`, `register_handler`) — it cannot, cleanly,
as a small addition: the in-args/return-value buffers there are **fixed-size, pre-allocated by the C++
side to exactly `Args`/`Return`'s own layout** (`CreateDataTypeSizeInfoFromTypes`), read/written via a
raw pointer, not a free-form byte stream. Adding an E2E header would need the buffer-size computation
on *both* the C++ and Rust sides to grow by the header size together — a real, coordinated design
change, not a Rust-only sketch (unlike #767's message_passing wire format, which was already a plain
byte stream both sides already treated as opaque).

Also confirmed (again) this sandbox's Rust/Bazel situation is unchanged from §1.3: `bazel build
//score/mw/com/rust/score_com_concept:score_com_concept` still fails — the `ferrocene` Rust toolchain
needs a newer glibc than this box has (the exact same failure blocking `--config=tsan` on the
`claude/fix-767-nested-method-reentrancy` branch). **But a plain, non-Bazel `rustc`/`cargo` on this
box works fine** — confirmed by actually compiling and running one. So while a prototype wired into
the real crate isn't buildable here, the core protect/check *algorithm* is independently
verifiable.

`docs/prototypes/e2e_profile4m_crc.rs`: a standalone (not a Bazel/Cargo target) Profile 4m-shaped
CRC-32 protect/check implementation, run with a bare `rustc`. `FO_PRS_E2EProtocol` states only the
polynomial and defers the remaining CRC parameters (init/refin/refout/xorout) to a separate CRC
library document; the prototype therefore uses the publicly documented "CRC-32/AUTOSAR" parameters
(init=refin=refout=xorout — all-ones/true) and validates them against that variant's own **public
catalog check value** (`0x1697d06a` for ASCII `"123456789"`), which is the standard
cross-implementation sanity check for it, and one anybody can rerun without the documents at hand.
Verified end to end (all via
`rustc -O e2e_profile4m_crc.rs -o e2e_profile4m_crc && ./e2e_profile4m_crc`): a normal
protect→check round trip recovers the original payload; flipping a payload bit is caught as
`WrongCrc`; replaying the same frame without advancing the counter is caught as `WrongSequence` — the
same "prove it actually catches the failure, not just that the happy path works" standard the #767
branch's regression tests were held to.

### 3.5 Deliberately not done in this entry

1. No reply posted to #1062 yet, no design proposal committed to as final — this is research to
   inform that reply, not a substitute for actually discussing scope with the maintainer first.
2. No attempt to wire this into `method.rs`'s real FFI call sites — §3.4 explains why that's a real,
   coordinated C++/Rust design change, not a quick addition.
3. No Event/Field CRC prototype — only Method's own "Xm" profile shape was prototyped, since that's
   what #782/#1062's own history is actually about; Event/Field would use the plain (non-m) profile
   family instead, one field-set narrower.

### 3.6 §3.5 item 2 revisited — the FFI wiring turned out to be possible after all

The user asked whether `reference_integration`'s devcontainer image could actually build/test Rust
here with a matching glibc. It can, and by a wide margin: `ghcr.io/eclipse-score/devcontainer:v1.11.0`
(already cached locally, `docker images`) runs Ubuntu 24.04 / glibc 2.39 — confirmed via
`docker run ... ldd --version` — which is exactly what the `ferrocene` Rust toolchain's own error
messages named as missing in this session's bare sandbox. Ran `bazel build
//score/mw/com/rust/score_com_concept:score_com_concept` and `bazel test
//score/mw/com/rust/score_com_concept:score_com_concept-test` inside a throwaway container (bind-mount
+ `cp` to a writable `/tmp` copy, since the mount itself is read-only) — both genuinely compile and the
existing test suite passes. **So this fork's Rust code was never actually unverifiable — only this
particular sandbox lacked a new enough glibc for the toolchain it needs; the real project environment
(same image the actual devcontainer/CI would use) has always been able to build it.**

With that confirmed, revisited §3.5 item 2. A prototype wired into the *actual* `interface!`-declared
service manifest (a brand new test interface, service discovery config, etc.) is still out of scope —
that's real, non-trivial plumbing unrelated to E2E itself. But `method.rs` already has a lighter-weight
precedent for exactly this: `test_method_call_round_trip_copy_path`, a unit test that drives the real
`LolaMethodCaller::invoke_with_copy` (real `MethodArgsCodec` serialize/deserialize, real call
sequencing) against a *mocked* `FFIBridge` (`bridge_ffi_mock`) standing in for the C++ side — no real
process, socket, or shared memory needed. Extended that same pattern instead:

- `ProtectedArg { header: [u8; 16], value: i32 }` — a `CommData`/`Reloc` type carrying the Profile 4m
  header as one of its *own* fields, exactly matching §3.4's finding that a real interface would need
  to declare room for the header up front (this is standing in for what `interface!` would generate
  once a user opts a method into E2E, not a way of avoiding that requirement).
- `test_method_call_with_e2e_protection_round_trip`: `invoke_with_copy` protects a request (via the
  same CRC-32/AUTOSAR logic as `docs/prototypes/e2e_profile4m_crc.rs`, now inlined as
  `e2e_protect_header`/`e2e_check_header` in `method.rs`'s own test module) — the mocked
  `proxy_method_do_call` plays the skeleton side (`check()`s the request *before* touching the
  application value, computes the result, `protect()`s the response) — the caller then `check()`s the
  response. Every step through the real FFI call sequence, not a hand-rolled simulation of it.
- `test_method_call_with_e2e_protection_catches_corruption`: same wiring, but the mocked "transport"
  flips a payload bit between the client's protect and the handler's read — asserts the handler's own
  `check()` rejects it (`WrongCrc`) rather than silently processing corrupted data, and that
  `invoke_with_copy` surfaces the failure to the caller.

**Two real compile errors found and fixed while getting this to build** (via the same devcontainer):
the bare name `Result` in this file already resolves to `score_com_concept`'s own 1-parameter alias
(fixed to `score_com_concept::Error`) from this file's top-of-file `use` — `e2e_check_header`'s
`Result<u16, E2ECheckError>` needed `core::result::Result` spelled out explicitly instead. And
`sample.value` (where `sample: LolaMethodReturnSample<ProtectedArg>`) resolved to
`LolaMethodReturnSample`'s *own* private `value: ProtectedArg` field (visible to this child test
module) rather than deref-ing into `ProtectedArg`'s own `value: i32` — needed `(*sample).value`
instead, matching how the pre-existing test already did this (`*sample`, not `sample.value`).

**Verified, not just written**: `bazel test
//score/mw/com/impl/rust/com-api/com-api-runtime-lola:com-api-runtime-lola-tests` inside the
devcontainer — `11 passed; 0 failed` (9 pre-existing + these 2 new), ~336s including a cold dependency
fetch.

**Still an honest limitation, not fully closed**: `protect()`/`check()` are called *manually* in this
test's code (both the "client" and the mocked "skeleton" side), not automatically injected by
`LolaMethodCaller`/`LolaMethodHandler` themselves the way CP's E2E Transformer is genuinely transparent
to application code (§3.3). Making it automatic would mean `LolaMethodCaller`/`LolaMethodHandler`
gaining actual knowledge of "this Args/Return type wants E2E" (some marker trait or wrapper type
recognized generically) — a real design decision, not a mechanical follow-up, and out of scope for
this entry too.

### 3.7 Closing §3.6's remaining gap — a generic, transparent wrapper

User asked for that "real design decision" explained, then to prototype it. The design: a generic
envelope `E2E<T> { header: [u8; 16], payload: T }` (unlike §3.6's `ProtectedArg`, which only ever
worked for one hand-written struct) plus two wrapper types — `ProtectedMethodCaller<Args, Return, B>`
(wraps a `LolaMethodCaller<(E2E<Args>,), E2E<Return>, B>`) and `ProtectedMethodHandler<Args, Return,
B>` (wraps a `LolaMethodHandler<(E2E<Args>,), E2E<Return>, B>`) — so that application code calling
`caller.invoke(args)` or registering a handler `Fn(&Args) -> Return` never names `E2E<_>` at all; the
wrapper's own `invoke`/`register_handler` do the `protect()`/`check()` internally. This is what an
`interface!`-generated Consumer/Producer would plug in for a method marked E2E-protected, in place of
the plain `LolaMethodCaller`/`LolaMethodHandler` — the macro-level choice is just "which concrete type
to instantiate," no runtime dispatch needed.

Three new tests (`test_protected_method_caller_generic_round_trip`,
`test_protected_method_caller_generic_catches_corruption`, and — see the correction below —
`test_protected_method_handler_generic_round_trip`) prove this is actually generic, not just another
one-off: the "application" value being sent is a plain `MethodTestArg` (already defined earlier in
this test module for the *original*, unprotected test) — nothing at the call site names `E2E<_>`, and
the same `E2E<T>`/`ProtectedMethodCaller` code is reused verbatim from §3.6's `ProtectedArg`-specific
tests without modification.

**A real compile error surfaced immediately, and it's informative**: the first draft used a bare `i32`
as the "plain application type," which doesn't compile — `score_com_concept::CommData` has no blanket
impl for primitive types (every wire type needs its own `const ID`), so `i32` alone can never be a
Method's `Args`/`Return` element in this design, protected or not. Fixed by reusing `MethodTestArg`
(already `CommData`) instead — a reminder that "generic over `T`" here specifically means "generic
over any `CommData` type," a real, checked constraint, not just any Rust type that happens to fit in a
buffer.

**Correction to this entry's first draft**: it initially marked `ProtectedMethodHandler`
`#[allow(dead_code)]` and left it untested, on the stated basis that "this crate doesn't test
handler-side registration anyway." That was wrong — a check of the *whole* crate (not just
`method.rs`) turned up `field_producer.rs`'s `test_field_get_set_round_trip`, which already mocks
`skeleton_method_register_handler`, captures the boxed handler closure, and invokes it through a
`call_and_free_handler` helper exactly as the real C++ `mw_com_impl_call_method_handler` would. So a
third test, `test_protected_method_handler_generic_round_trip`, was added following that same pattern:
registers a plain `Fn(&MethodTestArg) -> MethodTestArg` closure through `ProtectedMethodHandler`
(which never names `E2E<_>`), feeds the captured handler a *protected* request buffer, reads back the
response, and asserts the response's own E2E header checks out. `#[allow(dead_code)]` removed;
`call_and_free_handler` copied into `method.rs`'s test module (it's crate-private to
`field_producer.rs`'s).

Verified the same way as §3.6: `bazel test
//score/mw/com/impl/rust/com-api/com-api-runtime-lola:com-api-runtime-lola-tests` inside the
devcontainer — `14 passed; 0 failed` (11 from §3.6 + 3 new: two caller-side, one handler-side).

**Two remaining honest gaps, both worth stating outright rather than glossing over:**
1. `E2E<T>::CommData::ID` is hardcoded to the literal `"E2E"` regardless of `T` — fine for these
   single-interface mocked tests, wrong for a real system with more than one E2E-protected method
   (`ID` is meant to identify a wire type uniquely). A real implementation needs some way to compose
   a distinct ID per `T` (e.g. from `T::ID`).
2. A rejected/corrupted request has nowhere to go on the handler side: `MethodHandlerCall::call`
   returns `Return` directly, no `Result`, so `ProtectedMethodHandler::register_handler`'s `Err(_)` arm
   can only fabricate a response (`core::mem::zeroed()`) whose own bad header will fail the caller's
   check — which happens to work for the plain-old-data `CommData` types this codebase uses so far,
   but isn't a sound general answer to "how does a handler cleanly refuse a corrupted call."

### 3.8 Consolidated status — four questions (what's needed / done / not done / how prototyped around, and what production would need)

This section ties §§3.1–3.7 together into a single reference. Nothing new was built for it.

#### 3.8.1 What a real implementation of #1062 has to do

Make AUTOSAR E2E protection apply to Rust `Method<T>`/`Field<T>` calls *transparently* — application
code stays E2E-unaware while the framework does, at minimum:

| Concern | Requirement |
| --- | --- |
| Protocol | Build/validate an AUTOSAR E2E Profile 4m header (16 B: Length, Counter, DataID, CRC, MsgType/MsgResult/SourceID). CRC-32/AUTOSAR, polynomial `0xF4ACFB13`. |
| Caller (proxy) side | `protect()` the request just before serialization; `check()` the reply. |
| Handler (skeleton) side | `check()` the request and be able to *refuse* it before the application handler body runs; `protect()` the response. |
| Buffer-size agreement | The +16 B has to be accounted for on **both** the C++ (`CreateDataTypeSizeInfoFromTypes<ArgTypes...>()`, `proxy_method_with_in_args_and_return.h`) and Rust (`MethodArgsCodec::layout()`) sides *at the same time* — they must stay equal, since C++ allocates the buffer and Rust writes into it. |
| Configuration | An `interface!`-level opt-in ("this method is E2E-protected"); the generated Consumer/Producer then instantiates the protected type. Profile / DataID / offset from manifest config (AUTOSAR: `E2EDataTransformationSet`). |
| E2E state machine | Counter-delta tolerance, consecutive-failure counting, channel-status determination — `FO_RS_E2E` extends this to the server deciding whether to apply a method call based on the E2E check result. |

#### 3.8.2 What was actually built

- **Standalone algorithm** (`docs/prototypes/e2e_profile4m_crc.rs`, §3.4): CRC-32/AUTOSAR verified
  against the public catalog check value (`0x1697d06a`); Profile 4m header protect/check round trip,
  corruption caught as `WrongCrc`, replay caught as `WrongSequence`. Runs under a bare `rustc`.
- **Wired into the real call path** (`method.rs` test module, §3.6–3.7):
  - `E2E<T> { header: [u8; 16], payload: T }` — a generic envelope implementing `CommData`/`Reloc`.
  - `ProtectedMethodCaller<Args, Return, B>` wraps a `LolaMethodCaller<(E2E<Args>,), E2E<Return>, B>`;
    `invoke(args)` takes plain `Args`, does `protect`/`check` internally.
  - `ProtectedMethodHandler<Args, Return, B>` — the symmetric provider-side wrapper; `register_handler`
    takes a plain `Fn(&Args) -> Return`.
  - Five tests, all green in the devcontainer (`14 passed; 0 failed` for the whole
    `com-api-runtime-lola-tests` target): two exercise `invoke_with_copy` + the real `MethodArgsCodec`
    serialize/deserialize path with a hand-written `ProtectedArg`; two exercise the *generic*
    `ProtectedMethodCaller` with a plain `MethodTestArg` and nothing E2E-named at the call site; one
    exercises `ProtectedMethodHandler` through the real `skeleton_method_register_handler` FFI
    registration.
- **Build-environment finding** (§3.6): "Rust unbuildable in this sandbox" was purely a local glibc
  mismatch; `ghcr.io/eclipse-score/devcontainer:v1.11.0` (glibc 2.39) builds and tests the crate
  normally.

#### 3.8.3 What was not built, and where the prototype took a different path

| Item | The real way | The prototype's shortcut | Why |
| --- | --- | --- | --- |
| Framework-added header | proxy/skeleton binding prepends 16 B independently of `Args`/`Return`; both C++ and Rust buffer math grow | header is a **field of the user's type** (`E2E<T>`), so to C++ it is just "a 16-B-bigger type" | C++ `CreateDataTypeSizeInfoFromTypes` and Rust `MethodArgsCodec::layout()` must agree; a framework-added prefix means editing both, an `E2E<T>` field agrees automatically |
| Going through the real C++ backend | real proxy/skeleton over socket / shared memory | **everything is mocked FFI**; C++ is never executed | the mocked test setup has no real binding — so "C++/Rust buffer-size agreement" is **not actually verified**, only Rust-side self-consistency is |
| Handler refusing a request | `MethodHandlerCall::call` returns a `Result` | `call` returns `Return` directly; the `Err` arm fabricates a `mem::zeroed()` response whose bad header fails the *caller's* check | the trait signature is upstream `score_com_concept`'s; the prototype can't change it |
| DataID management | one DataID per method, from config | `E2E_TEST_DATA_ID = 0x0a0b0c0d` hardcoded, passed via a `ProtectedMethodCaller::data_id` field | single-interface tests |
| `E2E<T>::CommData::ID` | distinct per `T` (composed from `T::ID`) | every `E2E<T>` shares `"E2E"` | wrong for a system with more than one E2E method |
| Reading payload bytes for CRC | padding-safe (`bytemuck`/`zerocopy`, or a `CommData`-level no-padding guarantee) | raw `slice::from_raw_parts` cast (`e2e_as_bytes`) | plain structs happen to have no padding; a padded type would feed uninitialized bytes to the CRC |
| SourceID | identifies the calling client (28 bits) | hardcoded `0x1234567` | multi-client addressing not implemented |
| E2E state machine | counter-delta window, consecutive-failure count, channel-status verdict | none — only CRC + DataID checked; counter is stored, not validated | the server-side decision `FO_RS_E2E` requires is not implemented |
| Field / Event | same mechanism, plain (non-`m`) profile family (one field narrower) | Method only | #782/#1062 history is Method-centric |
| Async response timeout | separate mechanism at the comm-management layer (E2E can't do it) | none | milestone-1 scope, related to #767 |
| `interface!` macro integration | macro sees the E2E opt-in and generates `ProtectedMethodCaller` | none — tests assemble it by hand | macro work is its own large scope |

#### 3.8.4 What production-quality code would need on top of this

**A. C++ side (the largest remaining piece)**
- `CreateDataTypeSizeInfoFromTypes` (or a new serialization step) must account for the E2E profile
  header size; `ProxyMethodBinding::GetInArgsBuffer`/`GetReturnValueBuffer` must return buffers that
  include header space.
- A real proxy↔skeleton integration test (not mocked) to verify the C++/Rust buffer-size agreement
  that §3.8.3 leaves unverified.

**B. Rust side cleanup**
- Compose `E2E<T>::CommData::ID` from `T::ID` (e.g. a `const fn` building `"E2E<…>"`).
- Make `e2e_as_bytes` sound: a `CommData`-level "no padding / all bytes initialized" guarantee, a
  `bytemuck::Pod` bound, or field-by-field serialization (as `MethodArgsCodec` already does).
- Add a `Result` return to `MethodHandlerCall::call` (an upstream `score_com_concept` change) so a
  handler can refuse a corrupted request explicitly, removing the `mem::zeroed()` workaround.
- `ProtectedMethodCaller::next_counter`: `Cell<u16>` → `AtomicU16` for concurrent/reentrant use.

**C. E2E protocol completeness**
- Implement the E2E state machine: counter-delta validation (so `WrongSequence` distinguishes "lost N
  messages"), a `MaxDeltaCounter` config, consecutive-failure counting, an
  `E2EStatus::{Ok, Error, Repeated, WrongSequence, NoNewData}`-style verdict.
- Config-driven profile / DataID / offset selection (Profile 4m uses explicit DataID).
- Wire SourceID to a real client identity.
- Table-driven CRC (the current bitwise loop is safe but slow), or a vetted crate.

**D. Configuration & integration**
- `interface!` attribute parsing (`#[e2e(profile = "4m", data_id = 0x…)]` or similar) → generated code
  instantiates `ProtectedMethodCaller`/`ProtectedMethodHandler`.
- Read E2E parameters from the manifest / config.

**E. Safety & documentation**
- State explicitly that Rust-side E2E protects only the **Rust↔Rust path** — the C++ side has no E2E
  today, so Rust↔C++ traffic would be unprotected — or coordinate with C++-side E2E work.
- Safety case: E2E is the ISO 26262 mechanism for carrying ASIL data over a non-safety channel; real
  certification needs requirement tracing back to `FO_RS_E2E` and safety analysis. These notes cite
  no requirement identifiers (§3.0), so how that tracing is expressed in-tree is still an open
  question to settle with the maintainers rather than something to infer from here.
