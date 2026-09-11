/********************************************************************************
 * Copyright (c) 2026 Contributors to the Eclipse Foundation
 *
 * See the NOTICE file(s) distributed with this work for additional
 * information regarding copyright ownership.
 *
 * This program and the accompanying materials are made available under the
 * terms of the Apache License Version 2.0 which is available at
 * https://www.apache.org/licenses/LICENSE-2.0
 *
 * SPDX-License-Identifier: Apache-2.0
 ********************************************************************************/

// TODO: https://github.com/eclipse-score/communication/issues/782
//
// `LolaMethodCaller`/`LolaMethodHandler`/`LolaMethodInArgAllocator` below implement the real FFI
// wiring for Rust `Method<T>` calls against the LoLa binding (this fork, 2026-09-08), following up on
// the `score_com_concept`/`score_com_macros` trait-level design ported from upstream PR #818. That
// port left these three (plus `field_consumer.rs`/`field_producer.rs`, now also implemented — see
// their own module doc comments) as pure placeholders, because — as documented in this fork's
// `docs/design-notes.md` §1.2 — PR #818 itself
// doesn't touch the FFI bridge layer at all (`com-api-ffi-lola/bridge_ffi_lola.rs`,
// `registry_bridge_macro.h`/`.cpp`), which had zero Method-shaped `extern "C"` functions before this.
// That FFI layer (`mw_com_get_method_from_proxy`/`_skeleton`, `mw_com_proxy_method_get_in_args_buffer`,
// `mw_com_proxy_method_get_return_value_buffer`, `mw_com_proxy_method_do_call`,
// `mw_com_skeleton_method_register_handler`) is what this file now calls into.
//
// Scope of this implementation (milestone 1, per the upstream maintainer's own guidance on #782,
// recorded in docs/design-notes.md §1.1): synchronous method calls wrapped in an already-resolved
// `Future` — not real async dispatch. Real async needs the C++-side reentrancy bug tracked by
// upstream issue #767 fixed first (a method call from within another method's callback handler
// currently fails), which is unrelated to this fork's own work and out of scope here. This also
// matches the only shape the C++ binding itself currently implements: `kCallQueueSize == 1U`
// (`score/mw/com/impl/methods/proxy_method_base.h`) — there is only ever one call in flight at a time,
// so every FFI call below hardcodes queue position 0.
//
// Field get/set (`LolaFieldGetCaller`/`LolaFieldSetCaller` below) are now implemented too (this fork,
// 2026-09-08, same pass as `field_consumer.rs`/`field_producer.rs`'s own FFI implementation): both are
// thin wrappers delegating to `LolaMethodCaller<(), T, B>` / `LolaMethodCaller<(T,), T, B>`
// respectively — a field's get/set are, on the C++ side, ordinary `ProxyMethod`/`SkeletonMethod`
// instances (`MethodType::kGet`/`kSet`) registered under the field's `{name}_get`/`{name}_set` member
// names (confirmed via `proxy_field.h`/`proxy_field_binding_factory_impl.h`: `ProxyFieldImpl` composes
// a real `ProxyMethod<GetMethodSignature<T>>`/`ProxyMethod<SetMethodSignature<T>>` built through the
// same `ProxyMethodBindingFactory` this file already uses for plain methods), so no new FFI was needed
// for this half of Field support at all.
//
// Verification status: like the rest of this fork's Method<T> work, this is **completely unverified
// by any compiler** — this repository has no Cargo.toml anywhere (Bazel-only Rust) and this sandbox
// has no Bazel available. Treat every unsafe block and buffer-layout assumption below as reasoned
// through by hand against the real C++ headers (quoted/cited in the doc comments), not as
// "known to build" — see docs/design-notes.md §1.3/§1.4 for the fuller caveat.
use core::any::TypeId;
use core::cell::Cell;
use core::fmt::Debug;
use core::future::Future;
use core::marker::PhantomData;
use core::ops::Deref;
use core::ptr::NonNull;

use bridge_ffi_rs::{FFIBridge, FatPtr, ProxyMethodBinding, SkeletonMethodBinding};
use score_com_concept::{
    CommData, Error, MethodArgs, MethodArgsAllocate, MethodArgsPtrTuple, MethodCaller, MethodFailedReason,
    MethodHandler, MethodHandlerCall, MethodInArgAllocator, MethodInArgMaybeUninit, MethodInArgPtr,
    MethodReturnSample, Result, ZeroCopyArgs,
};
use score_log as log;

use crate::consumer::{LolaConsumerInfo, NativeProxyBase};
use crate::producer::LolaProviderInfo;
use crate::LolaRuntimeImpl;

/// Return sample for a method call result on the consumer side. Wraps the return value and provides
/// `Deref<Target = T>` access, mirroring how `LolaSample<T>` works for event data.
///
/// Note: unlike `LolaSample<T>` (events), this does *not* reference shared memory owned by the C++
/// binding after construction — `do_call_and_read_return` below reads the value out of the C++-owned
/// return-value buffer once, via `ptr::read`, into this struct's own `value` field. A future
/// zero-copy-on-the-return-path optimisation (referencing the C++ buffer directly instead of copying
/// out of it) is possible but is not what "zero-copy" refers to in this milestone — here it only
/// covers the *argument* side (`invoke_zero_copy`, via `LolaMethodInArgAllocator`).
pub struct LolaMethodReturnSample<T> {
    value: T,
}

impl<T> Deref for LolaMethodReturnSample<T> {
    type Target = T;
    fn deref(&self) -> &T {
        &self.value
    }
}

impl<T> MethodReturnSample<T> for LolaMethodReturnSample<T> {}

// Per-`Args`-tuple byte-layout codec, used by the *copy* call/handler path (`invoke_with_copy` /
// `LolaMethodHandler::register_handler`'s callback) to pack/unpack a whole `Args` tuple into/out of
// a single type-erased buffer, matching field-for-field what
// `score/mw/com/impl/util/type_erased_storage.h`'s `CreateDataTypeSizeInfoFromTypes`/
// `SerializeArgs`/`DeserializeArgs` do on the C++ side (sequential natural-alignment placement, as if
// the compiler laid out `struct { T1 a; T2 b; ...; }`) — needed because Rust's own tuple layout
// isn't guaranteed to match that (see `score_com_concept::MethodArgsCodec`'s doc comment).
//
// [2026-09-09] Originally defined locally here (`LolaCopyCodec`, LoLa-only, per this trait's
// original doc comment) — moved up into `score_com_concept` as `MethodArgsCodec` because
// `Runtime::MethodCaller`/`MethodHandler`'s associated-type declarations can't be satisfied for a
// `LolaMethodCaller`/`LolaMethodHandler` impl that's only conditionally `MethodCaller`/`MethodHandler`
// (Rust rejects impls stricter than what the trait declares, `E0276`); the trait itself needs to
// declare the extra bound (on `invoke_with_copy`/`register_handler` specifically, not as a blanket
// impl requirement) for every implementor, LoLa or not, to agree on. The bound itself is satisfied via
// that trait declaration, not a `use` here: `Args: MethodArgsCodec` is already in scope wherever the
// `MethodCaller`/`MethodHandler` trait methods below need it, so this file never names the trait
// directly (removed the now-unused `use score_com_concept::MethodArgsCodec;` that used to sit here).

/// Flattened FFI-callback signature for a boxed skeleton-method handler: quality type, then optional
/// `(ptr, len)` pairs for the in-args and return-value buffers. See
/// `mw_com_skeleton_method_register_handler`'s C++ doc comment (`registry_bridge_macro.h`) for why the
/// `Option`s are represented this way across the FFI boundary. Also used by `field_producer.rs`'s
/// get/set handlers, which share this exact callback shape.
pub(crate) type BoxedMethodHandlerFn =
    Box<dyn FnMut(u8, Option<(*mut u8, usize)>, Option<(*mut u8, usize)>) + Send + 'static>;

/// Shared low-level FFI dance for registering a boxed, flattened-signature Rust closure as a
/// `SkeletonMethodBinding`'s handler.
///
/// Factored out (this fork, 2026-09-08, alongside Field get/set support in `field_producer.rs`) so
/// that both `LolaMethodHandler::register_handler` (below) and `LolaFieldPublisher::
/// register_get_handler`/`register_set_handler` (`field_producer.rs`) share the exact same
/// Box::into_raw + transmute-to-FatPtr + register + (on failure) reclaim dance, differing only in
/// what closure they build beforehand — `LolaFieldPublisher` can't reuse
/// `LolaMethodHandler::register_handler` itself because `FieldPublisher`'s callback signatures
/// (`Fn(T) -> T` / `Fn() -> T`, values by *value*) don't match `MethodHandlerCall::call(&self, args:
/// &Args) -> Return` (args by *reference* — see `field_producer.rs`'s module doc comment for why that
/// mismatch can't be bridged without an unwanted `T: Clone` bound).
///
/// # Safety
/// `binding` must be a valid, non-null `SkeletonMethodBinding*` for the duration of the FFI call this
/// makes. `handler` is consumed: on success (return value `true`) C++ takes ownership of it (see the
/// known handler-disposal gap noted on `mw_com_skeleton_method_register_handler`'s C++ doc comment —
/// it is never freed again, for the lifetime of the process); on failure (`false`) it is freed here
/// before returning.
pub(crate) fn register_method_handler_raw<B: FFIBridge>(
    bridge: &B,
    binding: NonNull<SkeletonMethodBinding>,
    handler: BoxedMethodHandlerFn,
) -> bool {
    let raw = Box::into_raw(handler);
    // SAFETY: FatPtr is a binary-compatible reinterpretation of a `dyn Trait` fat pointer — same
    // technique bridge_ffi_lola.rs's own event-handler adapters already rely on.
    let fat_ptr: FatPtr = unsafe { core::mem::transmute(raw) };
    // SAFETY: binding is valid per this function's own SAFETY contract; fat_ptr references the
    // just-boxed closure above.
    let registered = unsafe { bridge.skeleton_method_register_handler(binding.as_ptr(), &fat_ptr) };
    if !registered {
        // Registration failed synchronously: the C++ side never took ownership of raw, so it's safe
        // and correct to free it here instead of leaking it.
        drop(unsafe { Box::from_raw(raw) });
    }
    registered
}

/// Whether `Return` is exactly `()` (a `void`-returning method), checked at runtime via `TypeId`
/// since `Return: CommData` is otherwise fully generic here and the trait signature
/// (`score_com_concept::MethodCaller`/`MethodHandler`) can't be changed to special-case it statically
/// without touching the upstream-ported concept crate. Sound because `CommData: Reloc: 'static`
/// (`score_com_concept::reloc::Reloc`), so `TypeId::of::<Return>()` is always available, and
/// `TypeId` equality between two `'static` types is a genuine proof they are the same type — the same
/// principle `core::any::Any::downcast_ref` relies on.
fn is_void<Return: 'static>() -> bool {
    TypeId::of::<Return>() == TypeId::of::<()>()
}

/// Owns the `ProxyBase` this method caller created for itself (see `LolaMethodCaller::new`'s doc
/// comment for why each caller gets its own proxy instance, mirroring how each event `Subscriber`
/// already does the same thing in `consumer.rs`) plus the resolved `ProxyMethodBinding*`.
pub struct LolaMethodCaller<Args: MethodArgs, Return: CommData, B: FFIBridge> {
    // Order matters for Drop: `binding` must never be used after `_proxy` is dropped (the binding is
    // owned by the proxy on the C++ side), so `_proxy` must drop after `binding` stops being used —
    // Rust drops fields in declaration order, so `binding` (a plain pointer, no Drop impl of its own)
    // must be declared *before* `_proxy` here.
    binding: NonNull<ProxyMethodBinding>,
    _proxy: NativeProxyBase<B>,
    bridge: B,
    _phantom: PhantomData<(Args, Return)>,
}

// SAFETY: mirrors NativeProxyBase<B>'s own unsafe Send/Sync impls in consumer.rs — `binding` is a
// pointer into memory owned by the C++-side ProxyMethod<Signature>, which itself is only ever
// destroyed together with `_proxy` (see field-order comment above), and provides no interior
// mutability visible from Rust beyond what `FFIBridge`'s methods already treat as thread-safe.
unsafe impl<Args: MethodArgs, Return: CommData, B: FFIBridge> Send for LolaMethodCaller<Args, Return, B> {}

impl<Args: MethodArgs, Return: CommData, B: FFIBridge> LolaMethodCaller<Args, Return, B> {
    /// Performs the actual method call (queue position 0) and reads back the return value, shared by
    /// both `invoke_with_copy` (after it has serialized `Args` itself) and `invoke_zero_copy` (whose
    /// arguments were already written directly into the C++ in-args buffer by
    /// `LolaMethodInArgMaybeUninit::write`, so there's nothing left to serialize here).
    fn do_call_and_read_return(&self) -> Result<Return> {
        let return_ptr = if is_void::<Return>() {
            None
        } else {
            // SAFETY: self.binding is a valid ProxyMethodBinding* for the lifetime of self (kept
            // alive by _proxy). Must be called before do_call below, matching
            // ProxyMethod<Signature>::operator()'s own ordering (proxy_method_with_in_args_and_return.h).
            let (ptr, _len) = unsafe {
                self.bridge
                    .proxy_method_get_return_value_buffer(self.binding.as_ptr(), 0)
            }
            .ok_or(Error::MethodError(MethodFailedReason::ReturnValueBufferUnavailable))?;
            Some(ptr)
        };

        // SAFETY: self.binding is valid as above; any needed in-args buffer has already been filled
        // by the caller (invoke_with_copy) or by LolaMethodInArgMaybeUninit::write (invoke_zero_copy)
        // before this function is called.
        let call_ok = unsafe { self.bridge.proxy_method_do_call(self.binding.as_ptr(), 0) };
        if !call_ok {
            return Err(Error::MethodError(MethodFailedReason::CallFailed));
        }

        let value = if let Some(ptr) = return_ptr {
            // SAFETY: ptr was just populated by DoCall above (successful DoCall guarantees the
            // return-value buffer now holds a live Return value, per ProxyMethodBinding::DoCall's own
            // contract), sized/aligned for Return by the C++ side's
            // CreateDataTypeSizeInfoFromTypes<ReturnType>() — a single type, so no field-ordering
            // ambiguity (see MethodArgsCodec's doc comment for why that only matters for tuples).
            unsafe { (ptr as *const Return).read() }
        } else {
            // is_void::<Return>() is true here, i.e. Return and () are the exact same type (proven by
            // the TypeId check, not by the compiler) — so transmute_copy from a real () value is a
            // same-type, zero-sized copy, not a real reinterpretation of bytes.
            let unit = ();
            unsafe { core::mem::transmute_copy::<(), Return>(&unit) }
        };
        Ok(value)
    }
}

impl<Args: MethodArgs, Return: CommData, B: FFIBridge> MethodCaller<Args, Return, LolaRuntimeImpl<B>>
    for LolaMethodCaller<Args, Return, B>
{
    fn new(method_name: &str, instance_info: LolaConsumerInfo<B>) -> Result<Self>
    where
        Self: Sized,
    {
        // Each LolaMethodCaller creates its own ProxyBase instance from the shared HandleType,
        // exactly mirroring how each LolaSubscribableImpl::new() (consumer.rs) already creates its
        // own proxy per event subscriber rather than sharing one across a Consumer's members. This
        // may look wasteful (one native proxy per method/event rather than one per Consumer) but it
        // is this crate's existing, already-relied-upon pattern on the consumer side — not something
        // introduced here — see NativeProxyBase::new's call sites in consumer.rs for the precedent.
        let handle = instance_info
            .get_handle()
            .ok_or(Error::ConsumerError(score_com_concept::ConsumerFailedReason::ServiceHandleNotFound))?;
        let proxy = NativeProxyBase::new(instance_info.bridge(), instance_info.interface_id(), handle)?;

        // SAFETY: proxy.as_ptr() is a valid, just-created ProxyBase*; interface_id()/method_name are
        // valid UTF-8 strings borrowed for the duration of this FFI call only.
        let raw_binding = unsafe {
            instance_info
                .bridge()
                .get_method_from_proxy(proxy.as_ptr(), instance_info.interface_id(), method_name)
        };
        let binding = NonNull::new(raw_binding)
            .ok_or(Error::MethodError(MethodFailedReason::MethodCallerCreationFailed))?;

        Ok(Self {
            binding,
            bridge: instance_info.bridge().clone(),
            _proxy: proxy,
            _phantom: PhantomData,
        })
    }

    #[allow(clippy::manual_async_fn)]
    fn invoke_with_copy<'a>(&'a self, args: Args) -> impl Future<Output = Result<LolaMethodReturnSample<Return>>> + 'a {
        async move {
            let (size, _align) = Args::layout();
            if size > 0 {
                // SAFETY: self.binding is valid for the lifetime of self. Must only be called when
                // this method actually has in-arguments (size > 0), matching
                // ProxyMethodBinding::GetInArgsBuffer's own "must not be called otherwise" contract.
                let (buf_ptr, buf_len) = unsafe {
                    self.bridge
                        .proxy_method_get_in_args_buffer(self.binding.as_ptr(), 0)
                }
                .ok_or(Error::MethodError(MethodFailedReason::InArgsBufferUnavailable))?;
                debug_assert!(
                    buf_len >= size,
                    "C++ in-args buffer ({buf_len} bytes) smaller than Args::layout() ({size} bytes) expects \
                     — this would indicate the C++ CreateDataTypeSizeInfoFromTypes<ArgTypes...>() and this \
                     MethodArgsCodec impl disagree about this method's argument layout"
                );
                // SAFETY: buf_ptr is valid for buf_len >= size bytes per the check above (in a
                // release build without the debug_assert, a real mismatch here would be a
                // memory-safety bug — see MethodArgsCodec's doc comment on why this is the highest-risk
                // assumption in this whole FFI layer), and aligned per the C++ side's own contract.
                unsafe { args.write_into(buf_ptr) };
            }
            let value = self.do_call_and_read_return()?;
            Ok(LolaMethodReturnSample { value })
        }
    }

    fn allocate(&self) -> Result<<Args as MethodArgsAllocate<LolaMethodInArgAllocator>>::UninitTuple>
    where
        Args: MethodArgsAllocate<LolaMethodInArgAllocator>,
    {
        let (size, _align) = Args::layout();
        let buffer = if size > 0 {
            // SAFETY: same contract as in invoke_with_copy above.
            let (ptr, _len) = unsafe {
                self.bridge
                    .proxy_method_get_in_args_buffer(self.binding.as_ptr(), 0)
            }
            .ok_or(Error::MethodError(MethodFailedReason::InArgsBufferUnavailable))?;
            ptr
        } else {
            // Args == () (arity 0): MethodArgsAllocate<A>::alloc_uninit for () never calls
            // allocator.allocate::<T>() at all (see score_com_concept::method_concept's impl), so this
            // null pointer is never dereferenced.
            core::ptr::null_mut()
        };
        let allocator = LolaMethodInArgAllocator::new(buffer);
        Ok(Args::alloc_uninit(&allocator))
    }

    #[allow(clippy::manual_async_fn)]
    fn invoke_zero_copy<'a>(
        &'a self,
        _ptrs: <Args as MethodArgsPtrTuple<LolaRuntimeImpl<B>>>::PtrTuple,
    ) -> impl Future<Output = Result<LolaMethodReturnSample<Return>>> + 'a
    where
        Args: MethodArgsPtrTuple<LolaRuntimeImpl<B>>,
    {
        async move {
            // The pointers in _ptrs already point directly into the C++ in-args buffer (written by
            // LolaMethodInArgMaybeUninit::write when the caller populated them via allocate()), so —
            // unlike invoke_with_copy — there is nothing left to serialize here.
            let value = self.do_call_and_read_return()?;
            Ok(LolaMethodReturnSample { value })
        }
    }
}

/// Runtime-specific concrete type for a fully-initialised Lola method argument pointer: a raw pointer
/// directly into the C++-owned in-args buffer, at the offset `LolaMethodInArgAllocator::allocate`
/// computed for this argument.
///
/// `ptr` is never read back in this milestone: `invoke_zero_copy` (see its doc comment) has nothing
/// left to do with a fully-initialised argument pointer once `write`/`assume_init` (below) have
/// populated the C++-owned buffer directly, since the call itself only needs the buffer's *address*
/// (already known to C++), not this handle. Kept (rather than dropped from the type) because
/// `score_com_concept::MethodInArgPtr<T>` requires a concrete pointer-carrying type to exist per its
/// trait contract; a future consumer of the pointer (e.g. read-back for a diagnostics/tracing hook)
/// would read it through this same field.
#[allow(dead_code)]
pub struct LolaMethodInArgPtr<T> {
    ptr: *mut T,
    _phantom: PhantomData<T>,
}

impl<T> MethodInArgPtr<T> for LolaMethodInArgPtr<T> {}

/// Lola placeholder for a single pre-allocated method argument slot: same raw pointer as
/// `LolaMethodInArgPtr<T>`, just not yet considered "initialised" (no encoded distinction on the Rust
/// side beyond that — writing happens in `write`/`assume_init` below).
pub struct LolaMethodInArgMaybeUninit<T> {
    ptr: *mut T,
    _phantom: PhantomData<T>,
}

impl<T> MethodInArgMaybeUninit<T> for LolaMethodInArgMaybeUninit<T> {
    type Ptr = LolaMethodInArgPtr<T>;

    fn write(self, val: T) -> ZeroCopyArgs<LolaMethodInArgPtr<T>> {
        // SAFETY: self.ptr was produced by LolaMethodInArgAllocator::allocate::<T>() (see that impl),
        // which only ever hands out pointers within the live in-args buffer for the in-flight call, at
        // an offset reserved exclusively for this T (kCallQueueSize == 1, so there is no concurrent
        // caller who could also be writing into this buffer right now).
        unsafe {
            self.ptr.write(val);
        }
        ZeroCopyArgs(LolaMethodInArgPtr {
            ptr: self.ptr,
            _phantom: PhantomData,
        })
    }

    unsafe fn assume_init(self) -> ZeroCopyArgs<LolaMethodInArgPtr<T>> {
        ZeroCopyArgs(LolaMethodInArgPtr {
            ptr: self.ptr,
            _phantom: PhantomData,
        })
    }
}

/// Runtime-specific method argument allocator: hands out one `LolaMethodInArgMaybeUninit<T>` per
/// `allocate::<T>()` call, each pointing at the next offset within a single shared in-args buffer —
/// reproducing, one field at a time, the exact same sequential natural-alignment packing algorithm
/// `MethodArgsCodec` uses for the copy path (see that trait's doc comment), via a running `Cell<usize>`
/// offset that each `allocate` call advances.
///
/// This only produces a correct layout because `MethodArgsAllocate::alloc_uninit`'s macro-generated
/// body (`score_com_concept::method_arities_macros`) calls `allocator.allocate::<T>()` once per `Args`
/// field, strictly in left-to-right tuple-literal-construction order — Rust guarantees tuple literal
/// elements are evaluated in the order they're written — and because a fresh `LolaMethodInArgAllocator`
/// is constructed per in-flight call (`LolaMethodCaller::allocate`), so there's no risk of a stale
/// offset carrying over between calls even though `kCallQueueSize == 1` means calls never overlap
/// anyway.
pub struct LolaMethodInArgAllocator {
    buffer: *mut u8,
    offset: Cell<usize>,
}

impl LolaMethodInArgAllocator {
    fn new(buffer: *mut u8) -> Self {
        Self {
            buffer,
            offset: Cell::new(0),
        }
    }
}

impl MethodInArgAllocator for LolaMethodInArgAllocator {
    type MethodInArgPtr<T: CommData> = LolaMethodInArgPtr<T>;
    type MethodInArgMaybeUninit<T: CommData> = LolaMethodInArgMaybeUninit<T>;

    fn allocate<T: CommData>(&self) -> LolaMethodInArgMaybeUninit<T> {
        let current = self.offset.get();
        let field_align = core::mem::align_of::<T>();
        let padding = if current.is_multiple_of(field_align) {
            0
        } else {
            field_align - (current % field_align)
        };
        let field_offset = current + padding;
        self.offset.set(field_offset + core::mem::size_of::<T>());
        // SAFETY: self.buffer is valid for Args::layout().0 bytes (the caller — LolaMethodCaller::allocate
        // — only constructs this allocator with a real buffer when that size is > 0), and field_offset +
        // size_of::<T>() never exceeds that as long as `allocate` is called once per Args field in
        // declaration order, which is exactly what alloc_uninit's generated body does (see this
        // struct's doc comment).
        let ptr = unsafe { self.buffer.add(field_offset) as *mut T };
        LolaMethodInArgMaybeUninit {
            ptr,
            _phantom: PhantomData,
        }
    }
}

/// Owns the shared `SkeletonBase` this handler was registered against (via `LolaProviderInfo`, which
/// is cheap to clone — it's `Arc`-backed, see `producer.rs`'s `SkeletonInstanceManager`) plus the
/// resolved `SkeletonMethodBinding*`.
pub struct LolaMethodHandler<Args: MethodArgs, Return: CommData, B: FFIBridge> {
    binding: NonNull<SkeletonMethodBinding>,
    _provider: LolaProviderInfo<B>,
    bridge: B,
    _phantom: PhantomData<(Args, Return)>,
}

// SAFETY: mirrors NativeSkeletonHandle<B>'s own unsafe Send+Sync impls in producer.rs — binding is a
// pointer into memory owned by the C++-side SkeletonMethod<Signature>/SkeletonMethodBinding, kept
// alive by _provider (Arc-backed), with no interior mutability visible from Rust beyond what
// FFIBridge's methods already treat as thread-safe. Sync (unlike LolaMethodCaller, which only needs
// Send) because a handler, once registered, may legitimately be invoked from a different C++ thread
// than the one that registered it — RegisterHandler itself is only called once, synchronously, from
// `register_handler` below.
unsafe impl<Args: MethodArgs, Return: CommData, B: FFIBridge> Send for LolaMethodHandler<Args, Return, B> {}
unsafe impl<Args: MethodArgs, Return: CommData, B: FFIBridge> Sync for LolaMethodHandler<Args, Return, B> {}

impl<Args: MethodArgs, Return: CommData, B: FFIBridge> MethodHandler<Args, Return, LolaRuntimeImpl<B>>
    for LolaMethodHandler<Args, Return, B>
{
    fn new(method_name: &str, instance_info: LolaProviderInfo<B>) -> Result<Self>
    where
        Self: Sized,
    {
        // SAFETY: instance_info.skeleton_ptr() is a valid SkeletonBase* for as long as instance_info
        // (kept alive below via _provider) is alive; interface_id()/method_name are valid UTF-8
        // strings borrowed for the duration of this FFI call only.
        let raw_binding = unsafe {
            instance_info.bridge().get_method_from_skeleton(
                instance_info.skeleton_ptr(),
                instance_info.interface_id(),
                method_name,
            )
        };
        let binding = NonNull::new(raw_binding)
            .ok_or(Error::MethodError(MethodFailedReason::MethodHandlerCreationFailed))?;

        Ok(Self {
            binding,
            bridge: instance_info.bridge().clone(),
            _provider: instance_info,
            _phantom: PhantomData,
        })
    }

    fn register_handler<F>(&self, handler: F)
    where
        F: MethodHandlerCall<Args, Return>,
    {
        // Boxed once here, invoked (potentially many times, once per incoming call) by
        // mw_com_impl_call_method_handler via the FatPtr transmute below — the same
        // Box::into_raw+transmute-to-FatPtr pattern the event receive-handler path already uses in
        // consumer.rs's init_async_receive, just with a method-shaped callback signature instead of a
        // plain FnMut().
        let boxed_handler: BoxedMethodHandlerFn = Box::new(move |_quality_type, in_args, return_buf| {
            // SAFETY: in_args, when Some, points at a live buffer holding an Args value serialized
            // with the same MethodArgsCodec layout this handler's Args type computes — the caller's
            // ProxyMethod<Signature>::operator() serialized it (via invoke_with_copy on the other
            // process/thread) using the identical C++-side SerializeArgs algorithm this codec mirrors.
            // When None (Args has no fields, i.e. arity 0 / Args == ()), MethodArgsCodec::read_from for
            // () never dereferences its argument, so a null pointer here is fine.
            let args = match in_args {
                Some((ptr, _len)) => unsafe { Args::read_from(ptr as *const u8) },
                None => unsafe { Args::read_from(core::ptr::null()) },
            };
            let result: Return = handler.call(&args);
            if let Some((ptr, _len)) = return_buf {
                // SAFETY: ptr points at the live return-value buffer the C++ binding allocated for
                // this call (sized/aligned for Return per CreateDataTypeSizeInfoFromTypes<ReturnType>()
                // on the C++ side — a single type, so no field-ordering ambiguity, see MethodArgsCodec's
                // doc comment). Writing here is what makes the result visible back on the caller's
                // side once DoCall returns.
                unsafe {
                    (ptr as *mut Return).write(result);
                }
            }
            // return_buf == None means this method returns void (Return == ()): nothing to write back,
            // and `result` (a zero-sized ()) is simply dropped.
        });

        // SAFETY: self.binding is a valid SkeletonMethodBinding* for the lifetime of self (kept alive
        // by _provider). See register_method_handler_raw's own doc comment for the known
        // handler-disposal gap this milestone accepts (there is currently no unregister hook to free
        // this box through on success; it intentionally leaks for the lifetime of the process).
        let registered = register_method_handler_raw(&self.bridge, self.binding, boxed_handler);
        if !registered {
            log::error!(
                "LolaMethodHandler::register_handler: mw_com_skeleton_method_register_handler failed; \
                 the handler was NOT registered with the underlying binding"
            );
        }
    }
}

/// Caller for a field's Get operation: `MethodType::kGet` on the C++ side, registered under the
/// field's `{name}_get` member name (see this file's module doc comment). Distinct from
/// `LolaMethodCaller<(), T, B>` as its own named type only because `score_com_concept::Runtime`
/// requires a distinct `FieldGetCaller` associated type (so the `interface!` macro can route field
/// getters to a different consumer-struct field name than a same-signature plain Method would use) —
/// the implementation itself is a pure delegation.
pub struct LolaFieldGetCaller<T: CommData + Debug, B: FFIBridge> {
    inner: LolaMethodCaller<(), T, B>,
}

impl<T: CommData + Debug, B: FFIBridge> MethodCaller<(), T, LolaRuntimeImpl<B>> for LolaFieldGetCaller<T, B> {
    fn new(method_name: &str, instance_info: LolaConsumerInfo<B>) -> Result<Self>
    where
        Self: Sized,
    {
        Ok(Self {
            inner: <LolaMethodCaller<(), T, B> as MethodCaller<(), T, LolaRuntimeImpl<B>>>::new(
                method_name,
                instance_info,
            )?,
        })
    }

    fn invoke_with_copy<'a>(&'a self, args: ()) -> impl Future<Output = Result<LolaMethodReturnSample<T>>> + 'a {
        self.inner.invoke_with_copy(args)
    }

    fn allocate(&self) -> Result<<() as MethodArgsAllocate<LolaMethodInArgAllocator>>::UninitTuple>
    where
        (): MethodArgsAllocate<LolaMethodInArgAllocator>,
    {
        self.inner.allocate()
    }

    fn invoke_zero_copy<'a>(
        &'a self,
        ptrs: <() as MethodArgsPtrTuple<LolaRuntimeImpl<B>>>::PtrTuple,
    ) -> impl Future<Output = Result<LolaMethodReturnSample<T>>> + 'a
    where
        (): MethodArgsPtrTuple<LolaRuntimeImpl<B>>,
    {
        self.inner.invoke_zero_copy(ptrs)
    }
}

/// Caller for a field's Set operation: `MethodType::kSet` on the C++ side, registered under the
/// field's `{name}_set` member name. See `LolaFieldGetCaller`'s doc comment — same pure-delegation
/// shape, wrapping `LolaMethodCaller<(T,), T, B>` instead.
pub struct LolaFieldSetCaller<T: CommData + Debug, B: FFIBridge> {
    inner: LolaMethodCaller<(T,), T, B>,
}

impl<T: CommData + Debug, B: FFIBridge> MethodCaller<(T,), T, LolaRuntimeImpl<B>> for LolaFieldSetCaller<T, B> {
    fn new(method_name: &str, instance_info: LolaConsumerInfo<B>) -> Result<Self>
    where
        Self: Sized,
    {
        Ok(Self {
            inner: <LolaMethodCaller<(T,), T, B> as MethodCaller<(T,), T, LolaRuntimeImpl<B>>>::new(
                method_name,
                instance_info,
            )?,
        })
    }

    fn invoke_with_copy<'a>(&'a self, args: (T,)) -> impl Future<Output = Result<LolaMethodReturnSample<T>>> + 'a {
        self.inner.invoke_with_copy(args)
    }

    fn allocate(&self) -> Result<<(T,) as MethodArgsAllocate<LolaMethodInArgAllocator>>::UninitTuple>
    where
        (T,): MethodArgsAllocate<LolaMethodInArgAllocator>,
    {
        self.inner.allocate()
    }

    fn invoke_zero_copy<'a>(
        &'a self,
        ptrs: <(T,) as MethodArgsPtrTuple<LolaRuntimeImpl<B>>>::PtrTuple,
    ) -> impl Future<Output = Result<LolaMethodReturnSample<T>>> + 'a
    where
        (T,): MethodArgsPtrTuple<LolaRuntimeImpl<B>>,
    {
        self.inner.invoke_zero_copy(ptrs)
    }
}

#[cfg(test)]
mod test {
    use super::*;
    use std::sync::{Arc, Mutex};
    use std::task::{Context, Poll, Wake, Waker};

    use bridge_ffi_mock::{MockFFIBridge, MockPointerAllocator, SharedMockBridge};
    use bridge_ffi_rs::{HandleType, InstanceSpecifier, ProxyBase, SkeletonBase};
    use crate::producer::{NativeSkeletonHandle, SkeletonInstanceManager};
    use score_com_concept::MethodArgsCodec;

    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    #[repr(C)]
    struct MethodTestArg {
        value: i32,
    }

    unsafe impl score_com_concept::Reloc for MethodTestArg {}

    impl score_com_concept::CommData for MethodTestArg {
        const ID: &'static str = "MethodTestArg";
    }

    struct NoopWake;
    impl Wake for NoopWake {
        fn wake(self: Arc<Self>) {}
    }

    /// Polls a future once, expecting immediate completion. `invoke_with_copy`'s body has no real
    /// `.await` point (every FFI call inside it is synchronous), so it always resolves on the first
    /// poll — there is no real executor in this test, just enough of one to drive that single poll.
    fn block_on_ready<F: Future>(fut: F) -> F::Output {
        let waker = Waker::from(Arc::new(NoopWake));
        let mut cx = Context::from_waker(&waker);
        let mut fut = Box::pin(fut);
        match fut.as_mut().poll(&mut cx) {
            Poll::Ready(value) => value,
            Poll::Pending => panic!("invoke_with_copy should resolve on the first poll"),
        }
    }

    /// End-to-end round trip through the real `MethodArgsCodec` serialize/deserialize path (PR #6's
    /// test plan item 1): `invoke_with_copy` serializes `(MethodTestArg,)` into a mock in-args buffer
    /// via the real `write_into`; the mocked `proxy_method_do_call` plays the C++ side's part by
    /// reading that buffer back with the same codec (`read_from`) and writing a transformed
    /// `MethodTestArg` into the mock return-value buffer; `invoke_with_copy` then reads it back. Only
    /// the FFI boundary itself is mocked — the codec and call-sequencing logic under test is real.
    #[test]
    fn test_method_call_round_trip_copy_path() {
        let mut mock = MockFFIBridge::new();
        let mut seq = mockall::Sequence::new();

        let proxy_alloc = MockPointerAllocator::<ProxyBase>::new();
        let binding_alloc = MockPointerAllocator::<ProxyMethodBinding>::new();
        let prox = proxy_alloc.clone();

        mock.expect_create_proxy()
            .in_sequence(&mut seq)
            .returning(move |_, _| prox.allocate());

        let in_args_size = <(MethodTestArg,) as MethodArgsCodec>::layout().0;
        let mut in_args_buf = vec![0u8; in_args_size];
        let mut return_buf = vec![0u8; core::mem::size_of::<MethodTestArg>()];
        // Captured as usize (not *mut u8/*mut MethodTestArg) because mockall's closures must be
        // Send, and raw pointers aren't — cast back to a pointer inside each closure instead.
        let in_args_addr = in_args_buf.as_mut_ptr() as usize;
        let in_args_len = in_args_buf.len();
        let return_addr = return_buf.as_mut_ptr() as usize;
        let return_len = return_buf.len();

        mock.expect_proxy_method_get_in_args_buffer()
            .in_sequence(&mut seq)
            .returning(move |_, _| Some((in_args_addr as *mut u8, in_args_len)));
        mock.expect_proxy_method_get_return_value_buffer()
            .in_sequence(&mut seq)
            .returning(move |_, _| Some((return_addr as *mut u8, return_len)));
        mock.expect_proxy_method_do_call().in_sequence(&mut seq).returning(move |_, _| {
            // SAFETY: plays the C++ side of this call for the test: in_args_addr/return_addr were
            // just handed out (and are still live) via the two mocked buffer-accessors above, and
            // in_args_addr already holds a value invoke_with_copy serialized with this exact codec.
            let (arg,): (MethodTestArg,) =
                unsafe { <(MethodTestArg,) as MethodArgsCodec>::read_from(in_args_addr as *const u8) };
            unsafe {
                (return_addr as *mut MethodTestArg).write(MethodTestArg { value: arg.value * 2 });
            }
            true
        });

        let prox_cleanup = proxy_alloc.clone();
        mock.expect_destroy_proxy().in_sequence(&mut seq).returning(move |ptr| {
            assert!(prox_cleanup.free(ptr), "destroy_proxy called with unknown pointer");
        });

        let bridge = SharedMockBridge::new(mock);
        let handle = HandleType::default();
        let proxy = NativeProxyBase::<SharedMockBridge>::new(&bridge, "TestInterface", &handle)
            .expect("NativeProxyBase::new should succeed with mock create_proxy");
        let binding =
            NonNull::new(binding_alloc.allocate()).expect("mock binding allocation should not fail");

        let caller = LolaMethodCaller::<(MethodTestArg,), MethodTestArg, SharedMockBridge> {
            binding,
            _proxy: proxy,
            bridge,
            _phantom: PhantomData,
        };

        let sample = block_on_ready(caller.invoke_with_copy((MethodTestArg { value: 21 },)))
            .expect("invoke_with_copy should succeed with a fully-mocked FFI round trip");
        assert_eq!(*sample, MethodTestArg { value: 42 });

        // Drops _proxy (NativeProxyBase), triggering destroy_proxy — must happen before checking
        // that the proxy pointer was freed.
        drop(caller);
        proxy_alloc.assert_all_freed();
    }

    // --- E2E protection prototype for communication#1062, see docs/design-notes.md §3 ---
    //
    // docs/design-notes.md §3.4 found that a real E2E header can't be silently added to an existing
    // Method's Args/Return without growing the C++/Rust-agreed buffer size for that method's actual
    // type — the in-args/return-value buffers above are sized exactly to `MethodArgsCodec::layout()`.
    // So here the *user's own* argument/return type (`ProtectedArg`, standing in for what an
    // interface! declaration would define once opted into E2E) already carries the 16-byte Profile
    // 4m-shaped header as one of its fields, the same way a real interface would need to declare
    // enough room for it up front (matching how AUTOSAR's own E2E Transformer needs its I-Signal/PDU
    // pre-sized for the header at config time - this isn't something a transformer conjures for free
    // either). `protect_header`/`check_header` below are the same CRC-32/AUTOSAR (Profile 4m field
    // order) logic already verified standalone in docs/prototypes/e2e_profile4m_crc.rs, now exercised
    // through the *real* `LolaMethodCaller::invoke_with_copy` and the real `MethodArgsCodec`
    // serialize/deserialize path - not just the algorithm in isolation.

    const E2E_POLY: u32 = 0xF4AC_FB13;
    const E2E_INIT: u32 = 0xFFFF_FFFF;
    const E2E_XOROUT: u32 = 0xFFFF_FFFF;

    fn e2e_reflect_poly() -> u32 {
        let mut v = E2E_POLY;
        let mut r = 0u32;
        for _ in 0..32 {
            r = (r << 1) | (v & 1);
            v >>= 1;
        }
        r
    }

    fn e2e_crc32(data: &[u8]) -> u32 {
        let mut crc = E2E_INIT;
        for &byte in data {
            crc ^= byte as u32;
            for _ in 0..8 {
                crc = if crc & 1 != 0 { (crc >> 1) ^ e2e_reflect_poly() } else { crc >> 1 };
            }
        }
        crc ^ E2E_XOROUT
    }

    /// Builds a 16-byte Profile 4m-shaped header protecting `payload` (CRC over
    /// Length|Counter|DataID|MsgType/Result/SourceID|payload).
    fn e2e_protect_header(data_id: u32, counter: u16, is_response: bool, payload: &[u8]) -> [u8; 16] {
        let length = (12 + payload.len()) as u16;
        let type_result_source: u32 = ((is_response as u32) << 30) | (0x1234567 & 0x0FFF_FFFF);
        let mut crc_input = Vec::with_capacity(12 + payload.len());
        crc_input.extend_from_slice(&length.to_be_bytes());
        crc_input.extend_from_slice(&counter.to_be_bytes());
        crc_input.extend_from_slice(&data_id.to_be_bytes());
        crc_input.extend_from_slice(&type_result_source.to_be_bytes());
        crc_input.extend_from_slice(payload);
        let crc = e2e_crc32(&crc_input);

        let mut header = [0u8; 16];
        header[0..2].copy_from_slice(&length.to_be_bytes());
        header[2..4].copy_from_slice(&counter.to_be_bytes());
        header[4..8].copy_from_slice(&data_id.to_be_bytes());
        header[8..12].copy_from_slice(&crc.to_be_bytes());
        header[12..16].copy_from_slice(&type_result_source.to_be_bytes());
        header
    }

    #[derive(Debug, PartialEq, Eq)]
    enum E2ECheckError {
        WrongDataId,
        WrongCrc,
    }

    /// The receive-side counterpart, run against a header + the payload it's supposed to protect.
    /// Note: `core::result::Result`, spelled out - the bare `Result` name in scope here is
    /// `score_com_concept::Result` (a 1-parameter alias fixed to `score_com_concept::Error`, imported
    /// for the rest of this file's real FFI code), which can't hold an `E2ECheckError`.
    fn e2e_check_header(
        header: &[u8; 16],
        payload: &[u8],
        expected_data_id: u32,
    ) -> core::result::Result<u16, E2ECheckError> {
        let length = u16::from_be_bytes([header[0], header[1]]);
        let counter = u16::from_be_bytes([header[2], header[3]]);
        let data_id = u32::from_be_bytes([header[4], header[5], header[6], header[7]]);
        let received_crc = u32::from_be_bytes([header[8], header[9], header[10], header[11]]);
        let type_result_source = &header[12..16];
        if data_id != expected_data_id {
            return Err(E2ECheckError::WrongDataId);
        }
        let mut crc_input = Vec::with_capacity(12 + payload.len());
        crc_input.extend_from_slice(&length.to_be_bytes());
        crc_input.extend_from_slice(&counter.to_be_bytes());
        crc_input.extend_from_slice(&data_id.to_be_bytes());
        crc_input.extend_from_slice(type_result_source);
        crc_input.extend_from_slice(payload);
        if e2e_crc32(&crc_input) != received_crc {
            return Err(E2ECheckError::WrongCrc);
        }
        Ok(counter)
    }

    const E2E_TEST_DATA_ID: u32 = 0x0a0b0c0d;

    /// Stands in for what an `interface!`-declared Method's argument/return type would look like once
    /// opted into E2E protection: the 16-byte header is just another field, sized in up front - see
    /// this test module's own doc comment above for why that's unavoidable, not a shortcut taken here.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    #[repr(C)]
    struct ProtectedArg {
        header: [u8; 16],
        value: i32,
    }

    unsafe impl score_com_concept::Reloc for ProtectedArg {}

    impl score_com_concept::CommData for ProtectedArg {
        const ID: &'static str = "ProtectedArg";
    }

    /// Real end-to-end round trip: `LolaMethodCaller::invoke_with_copy` protects the request, the
    /// mocked FFI plays the skeleton side (checking the request, then protecting its response), and
    /// the caller checks the response - proving the *wired* path (real MethodArgsCodec serialize/
    /// deserialize, real invoke_with_copy call sequencing), not just the standalone CRC algorithm.
    #[test]
    fn test_method_call_with_e2e_protection_round_trip() {
        let mut mock = MockFFIBridge::new();
        let mut seq = mockall::Sequence::new();

        let proxy_alloc = MockPointerAllocator::<ProxyBase>::new();
        let binding_alloc = MockPointerAllocator::<ProxyMethodBinding>::new();
        let prox = proxy_alloc.clone();

        mock.expect_create_proxy().in_sequence(&mut seq).returning(move |_, _| prox.allocate());

        let in_args_size = <(ProtectedArg,) as MethodArgsCodec>::layout().0;
        let mut in_args_buf = vec![0u8; in_args_size];
        let mut return_buf = vec![0u8; core::mem::size_of::<ProtectedArg>()];
        let in_args_addr = in_args_buf.as_mut_ptr() as usize;
        let in_args_len = in_args_buf.len();
        let return_addr = return_buf.as_mut_ptr() as usize;
        let return_len = return_buf.len();

        mock.expect_proxy_method_get_in_args_buffer()
            .in_sequence(&mut seq)
            .returning(move |_, _| Some((in_args_addr as *mut u8, in_args_len)));
        mock.expect_proxy_method_get_return_value_buffer()
            .in_sequence(&mut seq)
            .returning(move |_, _| Some((return_addr as *mut u8, return_len)));
        mock.expect_proxy_method_do_call().in_sequence(&mut seq).returning(move |_, _| {
            // Plays the skeleton side: check the incoming request first (as a real handler would,
            // before ever touching the application payload), then compute the response and protect it.
            let (arg,): (ProtectedArg,) =
                unsafe { <(ProtectedArg,) as MethodArgsCodec>::read_from(in_args_addr as *const u8) };
            let request_counter = e2e_check_header(&arg.header, &arg.value.to_be_bytes(), E2E_TEST_DATA_ID)
                .expect("request should check out - it was never corrupted in this test");

            let result_value = arg.value * 2;
            let response_header =
                e2e_protect_header(E2E_TEST_DATA_ID, request_counter, true, &result_value.to_be_bytes());
            unsafe {
                (return_addr as *mut ProtectedArg).write(ProtectedArg { header: response_header, value: result_value });
            }
            true
        });

        let prox_cleanup = proxy_alloc.clone();
        mock.expect_destroy_proxy().in_sequence(&mut seq).returning(move |ptr| {
            assert!(prox_cleanup.free(ptr), "destroy_proxy called with unknown pointer");
        });

        let bridge = SharedMockBridge::new(mock);
        let handle = HandleType::default();
        let proxy = NativeProxyBase::<SharedMockBridge>::new(&bridge, "TestInterface", &handle)
            .expect("NativeProxyBase::new should succeed with mock create_proxy");
        let binding = NonNull::new(binding_alloc.allocate()).expect("mock binding allocation should not fail");

        let caller = LolaMethodCaller::<(ProtectedArg,), ProtectedArg, SharedMockBridge> {
            binding,
            _proxy: proxy,
            bridge,
            _phantom: PhantomData,
        };

        let request_value: i32 = 21;
        let request_counter: u16 = 0;
        let request_header =
            e2e_protect_header(E2E_TEST_DATA_ID, request_counter, false, &request_value.to_be_bytes());
        let request = ProtectedArg { header: request_header, value: request_value };

        let sample = block_on_ready(caller.invoke_with_copy((request,)))
            .expect("invoke_with_copy should succeed with a fully-mocked FFI round trip");
        // `*sample`, not `sample.value` - `LolaMethodReturnSample<ProtectedArg>` has its own private
        // `value: ProtectedArg` field (visible from this child module), which field access resolves
        // to *before* deref-coercing into `ProtectedArg`'s own `value: i32` field.
        let response: ProtectedArg = *sample;
        assert_eq!(response.value, 42);
        e2e_check_header(&response.header, &response.value.to_be_bytes(), E2E_TEST_DATA_ID)
            .expect("response should check out - the real end-to-end path protected it correctly");

        drop(caller);
        proxy_alloc.assert_all_freed();
    }

    /// Same wiring as the round-trip test above, except the mocked transport corrupts one byte of the
    /// request's payload *after* the client protected it but *before* the (mocked) skeleton reads it -
    /// standing in for real transport-level corruption - proving the handler's own check() call
    /// catches it rather than silently processing corrupted data.
    #[test]
    fn test_method_call_with_e2e_protection_catches_corruption() {
        let mut mock = MockFFIBridge::new();
        let mut seq = mockall::Sequence::new();

        let proxy_alloc = MockPointerAllocator::<ProxyBase>::new();
        let binding_alloc = MockPointerAllocator::<ProxyMethodBinding>::new();
        let prox = proxy_alloc.clone();

        mock.expect_create_proxy().in_sequence(&mut seq).returning(move |_, _| prox.allocate());

        let in_args_size = <(ProtectedArg,) as MethodArgsCodec>::layout().0;
        let mut in_args_buf = vec![0u8; in_args_size];
        let mut return_buf = vec![0u8; core::mem::size_of::<ProtectedArg>()];
        let in_args_addr = in_args_buf.as_mut_ptr() as usize;
        let in_args_len = in_args_buf.len();
        let return_addr = return_buf.as_mut_ptr() as usize;
        let return_len = return_buf.len();

        mock.expect_proxy_method_get_in_args_buffer()
            .in_sequence(&mut seq)
            .returning(move |_, _| Some((in_args_addr as *mut u8, in_args_len)));
        mock.expect_proxy_method_get_return_value_buffer()
            .in_sequence(&mut seq)
            .returning(move |_, _| Some((return_addr as *mut u8, return_len)));
        mock.expect_proxy_method_do_call().in_sequence(&mut seq).returning(move |_, _| {
            // SAFETY: simulates bit-flip corruption occurring in transit, between the client's protect
            // (already-serialized into in_args_addr by invoke_with_copy) and this handler's read.
            unsafe {
                let corrupt_byte = (in_args_addr as *mut u8).add(16 + 3); // last byte of `value`
                *corrupt_byte ^= 0xFF;
            }
            let (arg,): (ProtectedArg,) =
                unsafe { <(ProtectedArg,) as MethodArgsCodec>::read_from(in_args_addr as *const u8) };
            let check_result = e2e_check_header(&arg.header, &arg.value.to_be_bytes(), E2E_TEST_DATA_ID);
            assert_eq!(
                check_result,
                Err(E2ECheckError::WrongCrc),
                "corrupted request should be rejected by check(), not silently processed"
            );
            // A real handler would refuse to call the application method body at all here; this
            // prototype just returns `false` (call failed) to make that outcome visible to the test.
            false
        });

        let prox_cleanup = proxy_alloc.clone();
        mock.expect_destroy_proxy().in_sequence(&mut seq).returning(move |ptr| {
            assert!(prox_cleanup.free(ptr), "destroy_proxy called with unknown pointer");
        });

        let bridge = SharedMockBridge::new(mock);
        let handle = HandleType::default();
        let proxy = NativeProxyBase::<SharedMockBridge>::new(&bridge, "TestInterface", &handle)
            .expect("NativeProxyBase::new should succeed with mock create_proxy");
        let binding = NonNull::new(binding_alloc.allocate()).expect("mock binding allocation should not fail");

        let caller = LolaMethodCaller::<(ProtectedArg,), ProtectedArg, SharedMockBridge> {
            binding,
            _proxy: proxy,
            bridge,
            _phantom: PhantomData,
        };

        let request_value: i32 = 21;
        let request_header = e2e_protect_header(E2E_TEST_DATA_ID, 0, false, &request_value.to_be_bytes());
        let request = ProtectedArg { header: request_header, value: request_value };

        let result = block_on_ready(caller.invoke_with_copy((request,)));
        assert!(result.is_err(), "the call should fail (CallFailed) since the mocked handler rejected it");

        drop(caller);
        proxy_alloc.assert_all_freed();
    }

    // --- Generic, transparent wrapper (follow-up to the above): `E2E<T>` + `ProtectedMethodCaller`/
    // `ProtectedMethodHandler`, so application code calling or handling a method never names E2E at
    // all - mirroring CP's own E2E Transformer transparency (§3.3), instead of the hand-written
    // `ProtectedArg` above, which only ever worked for that one struct.

    /// Generic envelope: wraps any method payload `T` with a Profile 4m-shaped header. A real
    /// `interface!`-declared method opting into E2E would use `E2E<Args>`/`E2E<Return>` as its actual
    /// wire type (per this module's earlier finding: the header can't be added for free, it has to be
    /// part of a type someone declares up front) - but code using `ProtectedMethodCaller`/
    /// `ProtectedMethodHandler` below never names this type itself.
    #[derive(Debug, Clone, Copy)]
    #[repr(C)]
    struct E2E<T> {
        header: [u8; 16],
        payload: T,
    }

    unsafe impl<T: score_com_concept::Reloc> score_com_concept::Reloc for E2E<T> {}

    impl<T: score_com_concept::CommData> score_com_concept::CommData for E2E<T> {
        // Prototype simplification: CommData::ID is meant to identify a wire type uniquely, so a real
        // implementation would need a distinct ID per T (e.g. composed from T::ID); every E2E<T>
        // sharing one ID is fine for these single-interface mocked tests, not for a real system with
        // more than one E2E-protected method.
        const ID: &'static str = "E2E";
    }

    /// Reads `v`'s own bytes for CRC purposes. Sound for the plain, no-padding `CommData` types this
    /// prototype exercises (bare `i32`/`(i32,)`); a real implementation would want a crate like
    /// `bytemuck`/`zerocopy` (or a `CommData`-level no-padding guarantee) rather than this raw cast.
    fn e2e_as_bytes<T>(v: &T) -> &[u8] {
        unsafe { core::slice::from_raw_parts((v as *const T).cast::<u8>(), core::mem::size_of::<T>()) }
    }

    impl<T> E2E<T> {
        fn protect(data_id: u32, counter: u16, is_response: bool, payload: T) -> Self {
            let header = e2e_protect_header(data_id, counter, is_response, e2e_as_bytes(&payload));
            E2E { header, payload }
        }

        /// This envelope's own counter, read without re-validating - so a handler can echo it back
        /// into its response header even when building an error response.
        fn counter(&self) -> u16 {
            u16::from_be_bytes([self.header[2], self.header[3]])
        }

        /// Validates the header against the current payload bytes and, only if it checks out, hands
        /// the payload back by value (`ptr::read`, matching how `do_call_and_read_return` above
        /// already treats `CommData` values as safely bitwise-copyable rather than requiring `Copy`).
        fn check(&self, expected_data_id: u32) -> core::result::Result<T, E2ECheckError> {
            e2e_check_header(&self.header, e2e_as_bytes(&self.payload), expected_data_id)?;
            Ok(unsafe { core::ptr::read(&self.payload) })
        }
    }

    /// Wraps a `LolaMethodCaller<(E2E<Args>,), E2E<Return>, B>` so that application code calling
    /// through this type only ever sees plain `Args`/`Return` - `protect()`/`check()` happen inside
    /// `invoke()`, never at the call site. This is what an `interface!`-generated Consumer would plug
    /// in for a method marked as E2E-protected, in place of a plain `LolaMethodCaller`.
    struct ProtectedMethodCaller<Args: MethodArgs, Return: CommData, B: FFIBridge> {
        inner: LolaMethodCaller<(E2E<Args>,), E2E<Return>, B>,
        data_id: u32,
        next_counter: Cell<u16>,
    }

    impl<Args: MethodArgs, Return: CommData, B: FFIBridge> ProtectedMethodCaller<Args, Return, B> {
        fn invoke<'a>(&'a self, args: Args) -> impl Future<Output = Result<Return>> + 'a {
            async move {
                let counter = self.next_counter.get();
                self.next_counter.set(counter.wrapping_add(1));
                let protected_args = E2E::protect(self.data_id, counter, false, args);
                let sample = self.inner.invoke_with_copy((protected_args,)).await?;
                (*sample)
                    .check(self.data_id)
                    .map_err(|_| Error::MethodError(MethodFailedReason::CallFailed))
            }
        }
    }

    /// Same idea on the provider side: application code registers a plain handler closure exactly as
    /// it would for an unprotected method (`F: MethodHandlerCall<Args, Return>`, the same bound
    /// `LolaMethodHandler::register_handler` itself takes) - this wrapper is what checks the incoming
    /// request and protects the outgoing response around it.
    struct ProtectedMethodHandler<Args: MethodArgs, Return: CommData, B: FFIBridge> {
        inner: LolaMethodHandler<(E2E<Args>,), E2E<Return>, B>,
    }

    impl<Args: MethodArgs, Return: CommData, B: FFIBridge> ProtectedMethodHandler<Args, Return, B> {
        fn register_handler<F>(&self, data_id: u32, handler: F)
        where
            F: MethodHandlerCall<Args, Return>,
        {
            self.inner.register_handler(move |wrapped: &E2E<Args>| -> E2E<Return> {
                match wrapped.check(data_id) {
                    Ok(args) => {
                        let result = handler.call(&args);
                        E2E::protect(data_id, wrapped.counter(), true, result)
                    }
                    Err(_) => {
                        // Prototype limitation: MethodHandlerCall::call has no error return, so a
                        // rejected request can't cleanly refuse to answer at all here - it answers
                        // with an all-zero header (which will itself fail the caller's own check(),
                        // still surfacing as a failure rather than silently returning a bogus
                        // "valid-looking" value). A real design needs its own answer for "how does a
                        // handler refuse a corrupted call", not just this workaround - and
                        // `zeroed()` is not generically sound for every possible `Return` (e.g. one
                        // containing a non-nullable pointer), only for the plain numeric/struct
                        // CommData types this prototype (and this codebase so far) actually uses.
                        E2E { header: [0u8; 16], payload: unsafe { core::mem::zeroed() } }
                    }
                }
            });
        }
    }

    /// Reconstructs the boxed handler closure `ProtectedMethodHandler::register_handler` (via the
    /// inner `LolaMethodHandler`) handed to the mocked `skeleton_method_register_handler`, and calls
    /// it exactly as `mw_com_impl_call_method_handler` would on the real C++ side. Frees the box
    /// afterwards so the test doesn't leak it. Copied from `field_producer.rs`'s own test module,
    /// which needs the identical dance for `LolaFieldPublisher`'s get/set handlers.
    unsafe fn call_and_free_handler(
        fat_ptr: FatPtr,
        quality_type: u8,
        in_args: Option<(*mut u8, usize)>,
        return_buf: Option<(*mut u8, usize)>,
    ) {
        type Callback = dyn FnMut(u8, Option<(*mut u8, usize)>, Option<(*mut u8, usize)>);
        // SAFETY: fat_ptr was just captured from a mocked skeleton_method_register_handler call,
        // whose real (non-mocked) counterpart reconstructs it exactly this way.
        let callable: &mut Callback = unsafe { core::mem::transmute(fat_ptr) };
        callable(quality_type, in_args, return_buf);
        // Reclaims and drops the box register_method_handler_raw leaked into C++'s "ownership".
        drop(unsafe { Box::from_raw(callable as *mut Callback) });
    }

    /// Proves `ProtectedMethodCaller` is genuinely generic and transparent: the "application" value
    /// being sent/received here is a plain `MethodTestArg` - nothing in this test names `ProtectedArg`
    /// (the hand-written, single-purpose type from the round-trip test above) or touches `E2E<_>` at
    /// the call site. The mocked "skeleton" side still calls `E2E::check`/`E2E::protect` directly
    /// here (the *handler*-side wrapper is exercised separately by
    /// `test_protected_method_handler_generic_round_trip` below).
    #[test]
    fn test_protected_method_caller_generic_round_trip() {
        // MethodTestArg, not a raw i32: this crate requires every wire type to implement
        // score_com_concept::CommData itself (an ID const identifying it), which primitive types
        // like i32 don't get for free (no blanket impl) - MethodTestArg (defined earlier in this
        // test module) already does. The point being demonstrated is genericity over *any* CommData
        // type, not specifically over primitives.
        type WireArgs = (E2E<(MethodTestArg,)>,);
        type WireReturn = E2E<MethodTestArg>;

        let mut mock = MockFFIBridge::new();
        let mut seq = mockall::Sequence::new();

        let proxy_alloc = MockPointerAllocator::<ProxyBase>::new();
        let binding_alloc = MockPointerAllocator::<ProxyMethodBinding>::new();
        let prox = proxy_alloc.clone();

        mock.expect_create_proxy().in_sequence(&mut seq).returning(move |_, _| prox.allocate());

        let in_args_size = <WireArgs as MethodArgsCodec>::layout().0;
        let mut in_args_buf = vec![0u8; in_args_size];
        let mut return_buf = vec![0u8; core::mem::size_of::<WireReturn>()];
        let in_args_addr = in_args_buf.as_mut_ptr() as usize;
        let in_args_len = in_args_buf.len();
        let return_addr = return_buf.as_mut_ptr() as usize;
        let return_len = return_buf.len();

        mock.expect_proxy_method_get_in_args_buffer()
            .in_sequence(&mut seq)
            .returning(move |_, _| Some((in_args_addr as *mut u8, in_args_len)));
        mock.expect_proxy_method_get_return_value_buffer()
            .in_sequence(&mut seq)
            .returning(move |_, _| Some((return_addr as *mut u8, return_len)));
        mock.expect_proxy_method_do_call().in_sequence(&mut seq).returning(move |_, _| {
            let (wrapped,): WireArgs =
                unsafe { <WireArgs as MethodArgsCodec>::read_from(in_args_addr as *const u8) };
            let response = match wrapped.check(E2E_TEST_DATA_ID) {
                Ok((value,)) => {
                    let result = MethodTestArg { value: value.value * 2 }; // the "application logic"
                    E2E::protect(E2E_TEST_DATA_ID, wrapped.counter(), true, result)
                }
                Err(_) => E2E { header: [0u8; 16], payload: MethodTestArg { value: 0 } },
            };
            unsafe {
                (return_addr as *mut WireReturn).write(response);
            }
            true
        });

        let prox_cleanup = proxy_alloc.clone();
        mock.expect_destroy_proxy().in_sequence(&mut seq).returning(move |ptr| {
            assert!(prox_cleanup.free(ptr), "destroy_proxy called with unknown pointer");
        });

        let bridge = SharedMockBridge::new(mock);
        let handle = HandleType::default();
        let proxy = NativeProxyBase::<SharedMockBridge>::new(&bridge, "TestInterface", &handle)
            .expect("NativeProxyBase::new should succeed with mock create_proxy");
        let binding = NonNull::new(binding_alloc.allocate()).expect("mock binding allocation should not fail");

        let caller = ProtectedMethodCaller {
            inner: LolaMethodCaller::<WireArgs, WireReturn, SharedMockBridge> {
                binding,
                _proxy: proxy,
                bridge,
                _phantom: PhantomData,
            },
            data_id: E2E_TEST_DATA_ID,
            next_counter: Cell::new(0),
        };

        // The point being demonstrated: this call site is completely E2E-unaware - plain
        // MethodTestArg in, plain MethodTestArg out, exactly as if this were an ordinary,
        // unprotected method call.
        let result = block_on_ready(caller.invoke((MethodTestArg { value: 21 },)));
        assert_eq!(result.expect("protected call should succeed").value, 42);

        drop(caller);
        proxy_alloc.assert_all_freed();
    }

    /// Same generic wrapper, but the mocked transport corrupts the request in transit - proving
    /// `ProtectedMethodCaller::invoke` surfaces the failure to a caller that never touches E2E types
    /// itself (unlike `test_method_call_with_e2e_protection_catches_corruption` above, which asserts
    /// on the raw `E2ECheckError` inside the mocked handler).
    #[test]
    fn test_protected_method_caller_generic_catches_corruption() {
        type WireArgs = (E2E<(MethodTestArg,)>,);
        type WireReturn = E2E<MethodTestArg>;

        let mut mock = MockFFIBridge::new();
        let mut seq = mockall::Sequence::new();

        let proxy_alloc = MockPointerAllocator::<ProxyBase>::new();
        let binding_alloc = MockPointerAllocator::<ProxyMethodBinding>::new();
        let prox = proxy_alloc.clone();

        mock.expect_create_proxy().in_sequence(&mut seq).returning(move |_, _| prox.allocate());

        let in_args_size = <WireArgs as MethodArgsCodec>::layout().0;
        let mut in_args_buf = vec![0u8; in_args_size];
        let mut return_buf = vec![0u8; core::mem::size_of::<WireReturn>()];
        let in_args_addr = in_args_buf.as_mut_ptr() as usize;
        let in_args_len = in_args_buf.len();
        let return_addr = return_buf.as_mut_ptr() as usize;
        let return_len = return_buf.len();

        mock.expect_proxy_method_get_in_args_buffer()
            .in_sequence(&mut seq)
            .returning(move |_, _| Some((in_args_addr as *mut u8, in_args_len)));
        mock.expect_proxy_method_get_return_value_buffer()
            .in_sequence(&mut seq)
            .returning(move |_, _| Some((return_addr as *mut u8, return_len)));
        mock.expect_proxy_method_do_call().in_sequence(&mut seq).returning(move |_, _| {
            // Corrupt the request in transit, same technique as the earlier corruption test.
            unsafe {
                let corrupt_byte = (in_args_addr as *mut u8).add(16);
                *corrupt_byte ^= 0xFF;
            }
            let (wrapped,): WireArgs =
                unsafe { <WireArgs as MethodArgsCodec>::read_from(in_args_addr as *const u8) };
            assert_eq!(
                wrapped.check(E2E_TEST_DATA_ID),
                Err(E2ECheckError::WrongCrc),
                "corrupted request should be rejected, not silently processed"
            );
            false
        });

        let prox_cleanup = proxy_alloc.clone();
        mock.expect_destroy_proxy().in_sequence(&mut seq).returning(move |ptr| {
            assert!(prox_cleanup.free(ptr), "destroy_proxy called with unknown pointer");
        });

        let bridge = SharedMockBridge::new(mock);
        let handle = HandleType::default();
        let proxy = NativeProxyBase::<SharedMockBridge>::new(&bridge, "TestInterface", &handle)
            .expect("NativeProxyBase::new should succeed with mock create_proxy");
        let binding = NonNull::new(binding_alloc.allocate()).expect("mock binding allocation should not fail");

        let caller = ProtectedMethodCaller {
            inner: LolaMethodCaller::<WireArgs, WireReturn, SharedMockBridge> {
                binding,
                _proxy: proxy,
                bridge,
                _phantom: PhantomData,
            },
            data_id: E2E_TEST_DATA_ID,
            next_counter: Cell::new(0),
        };

        let result = block_on_ready(caller.invoke((MethodTestArg { value: 21 },)));
        assert!(result.is_err(), "the call should fail since the mocked handler rejected the corrupted request");

        drop(caller);
        proxy_alloc.assert_all_freed();
    }

    /// The provider-side counterpart to `test_protected_method_caller_generic_round_trip`: registers a
    /// plain `Fn(&MethodTestArg) -> MethodTestArg` closure through `ProtectedMethodHandler` (which
    /// never sees `E2E<_>`), then - playing the C++ side exactly as `field_producer.rs`'s
    /// `test_field_get_set_round_trip` does - feeds the registered handler a *protected* request
    /// buffer and reads back the response, asserting the response's own E2E header checks out. Only
    /// `create_skeleton`/`skeleton_method_register_handler`/`destroy_skeleton` are mocked; the
    /// wrapper's check/protect logic, the inner `LolaMethodHandler`'s FFI-registration dance, and the
    /// `MethodArgsCodec` (de)serialization are all real.
    #[test]
    fn test_protected_method_handler_generic_round_trip() {
        type AppArgs = (MethodTestArg,);
        type AppReturn = MethodTestArg;

        let mut mock = MockFFIBridge::new();
        let mut seq = mockall::Sequence::new();

        let skeleton_alloc = MockPointerAllocator::<SkeletonBase>::new();
        let binding_alloc = MockPointerAllocator::<SkeletonMethodBinding>::new();

        let skel = skeleton_alloc.clone();
        mock.expect_create_skeleton().in_sequence(&mut seq).returning(move |_, _| skel.allocate());

        let captured: Arc<Mutex<Option<FatPtr>>> = Arc::new(Mutex::new(None));
        let cap = captured.clone();
        mock.expect_skeleton_method_register_handler()
            .times(1)
            .in_sequence(&mut seq)
            .returning(move |_binding, fat_ptr| {
                *cap.lock().expect("lock") = Some(*fat_ptr);
                true
            });

        let skel_cleanup = skeleton_alloc.clone();
        mock.expect_destroy_skeleton().in_sequence(&mut seq).returning(move |ptr| {
            assert!(skel_cleanup.free(ptr), "destroy_skeleton called with unknown pointer");
        });

        let bridge = SharedMockBridge::new(mock);
        let spec = InstanceSpecifier::try_from("/test_instance").expect("valid instance specifier");
        let skeleton_handle = NativeSkeletonHandle::<SharedMockBridge>::new(&bridge, "", &spec)
            .expect("create_skeleton mock should not fail");
        let provider = LolaProviderInfo::new_for_test(
            score_com_concept::InstanceSpecifier::new("/test_instance").expect("valid instance specifier"),
            "TestInterface",
            SkeletonInstanceManager(Arc::new(skeleton_handle)),
            bridge.clone(),
        );

        let handler = ProtectedMethodHandler {
            inner: LolaMethodHandler::<(E2E<AppArgs>,), E2E<AppReturn>, SharedMockBridge> {
                binding: NonNull::new(binding_alloc.allocate()).expect("mock binding allocation should not fail"),
                _provider: provider,
                bridge: bridge.clone(),
                _phantom: PhantomData,
            },
        };

        // The point: this closure is completely E2E-unaware - plain MethodTestArg in and out.
        handler.register_handler(E2E_TEST_DATA_ID, |arg: &MethodTestArg| MethodTestArg {
            value: arg.value * 2,
        });
        let fat_ptr = captured
            .lock()
            .expect("lock")
            .take()
            .expect("register_handler should have registered a handler");

        // Play the caller side: serialize a *protected* (E2E<AppArgs>,) into the in-args buffer.
        let request = E2E::protect(E2E_TEST_DATA_ID, 0, false, (MethodTestArg { value: 21 },));
        let in_args_size = <(E2E<AppArgs>,) as MethodArgsCodec>::layout().0;
        let mut in_buf = vec![0u8; in_args_size];
        // SAFETY: in_buf is exactly in_args_size bytes, matching (E2E<AppArgs>,)'s own layout.
        unsafe { (request,).write_into(in_buf.as_mut_ptr()) };
        let mut return_buf = vec![0u8; core::mem::size_of::<E2E<AppReturn>>()];

        unsafe {
            call_and_free_handler(
                fat_ptr,
                0,
                Some((in_buf.as_mut_ptr(), in_buf.len())),
                Some((return_buf.as_mut_ptr(), return_buf.len())),
            );
        }

        // SAFETY: return_buf was just populated by the handler invoked above.
        let response = unsafe { (return_buf.as_ptr() as *const E2E<AppReturn>).read() };
        let payload = response.check(E2E_TEST_DATA_ID).expect("response header should check out");
        assert_eq!(payload.value, 42);

        drop(handler);
        skeleton_alloc.assert_all_freed();
    }
}
