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

//! Field producer implementation for Lola runtime.
//!
//! Implemented this fork, 2026-09-08 (previously `todo!()` placeholders — see
//! `docs/design-notes.md` §1.4 for this fork's earlier Method<T> FFI work this builds on, and
//! `field_consumer.rs`'s module doc comment for the C++-side confirmation that a Field composes a
//! real `ProxyEvent`/`SkeletonEvent` (for `WithNotifier`) plus up to two real
//! `ProxyMethod`/`SkeletonMethod` instances (for `WithGetter`/`WithSetter`), each built through the
//! ordinary event/method binding factories — there is no field-specific binding machinery on the C++
//! side to bridge separately).
//!
//! `LolaFieldPublisher<T, B>` therefore composes, rather than reimplements:
//! - `update()`/`allocate()`/`FieldSampleMut::update()` (the `WithNotifier` "commit and notify
//!   subscribers" path) delegate to a `LolaPublisher<T, B>` (`producer.rs`'s already-fully-implemented
//!   Event publisher) held internally — `Publisher::send()` is a *default* trait method built on
//!   `allocate()`+`write()`+`EventSampleMut::send()`, all of which `LolaPublisher` already implements,
//!   so `FieldPublisher::update(value)` is simply `self.notifier.send(value)`.
//! - `register_set_handler`/`register_get_handler` reuse the exact same low-level
//!   `SkeletonMethodBinding` FFI dance `LolaMethodHandler::register_handler` uses (`method.rs`'s
//!   `register_method_handler_raw` + `mw_com_skeleton_method_register_handler` +
//!   `mw_com_impl_call_method_handler`) — but do NOT go through `LolaMethodHandler`/
//!   `MethodHandlerCall` itself: `FieldPublisher`'s callback signatures take the field's value type
//!   **by value** (`Fn(T) -> T` for set, `Fn() -> T` for get — see `score_com_concept::field_concept`),
//!   whereas `MethodHandlerCall::call(&self, args: &Args) -> Return` always hands the callback
//!   argument **by reference**. Bridging that mismatch through `MethodHandlerCall` would require an
//!   adapter that clones `T` out of a `&T`, which would impose an unwanted `T: Clone` bound `T:
//!   CommData` doesn't otherwise need. Since `score_com_concept::MethodArgsCodec::read_from` already
//!   deserializes a whole `Args` tuple **by value** (via `ptr::read`, an owned move — no cloning), the
//!   closures built below just call it directly instead of going through `MethodHandlerCall` at all.
//!
//! A field lacking a given capability tag (no `WithNotifier` / no `WithGetter` / no `WithSetter`)
//! simply has no `EXPORT_MW_COM_EVENT`/`EXPORT_MW_COM_METHOD` registered for the corresponding C++
//! member, so the relevant FFI lookup in `new()` below returns null — stored as `None` rather than a
//! construction error, since `LolaFieldPublisher<T, B>` (unlike the macro-generated `Producer`
//! struct's compile-time type-state validator) has no way to know at its own type level which tags
//! apply. Calling `update()`/`register_set_handler()`/`register_get_handler()` when the corresponding
//! piece is `None` returns/logs an error instead of panicking — in well-formed generated code this
//! should never actually happen, since the type-state validator only ever calls the methods a field's
//! tags actually support.
//!
//! Verification status: same caveat as the rest of this fork's Method<T>/Field<T> work — completely
//! unverified by any compiler (no Cargo.toml in this repository, no Bazel in this sandbox).

use core::fmt::Debug;
use core::ptr::NonNull;

use bridge_ffi_rs::{FFIBridge, SkeletonMethodBinding};
use score_com_concept::{
    CommData, Error, EventFailedReason, EventSampleMut, FieldPublisher, FieldSampleMut, MethodArgsCodec,
    Publisher, Result, SampleMaybeUninit as SampleMaybeUninitTrait, SampleMut,
};
use score_log as log;

use crate::method::{register_method_handler_raw, BoxedMethodHandlerFn};
use crate::{LolaProviderInfo, LolaPublisher, LolaRuntimeImpl, LolaSampleMaybeUninit, LolaSampleMut};

pub struct LolaFieldPublisher<T: CommData + Debug, B: FFIBridge> {
    /// `WithNotifier`: present iff the field was tagged with it (see this file's module doc comment).
    notifier: Option<LolaPublisher<T, B>>,
    /// `WithGetter`: present iff the field was tagged with it.
    get_binding: Option<NonNull<SkeletonMethodBinding>>,
    /// `WithSetter`: present iff the field was tagged with it.
    set_binding: Option<NonNull<SkeletonMethodBinding>>,
    bridge: B,
    /// Keeps the shared skeleton (and hence `get_binding`/`set_binding`) alive — `notifier`, if
    /// present, already keeps it alive on its own via its own `SkeletonInstanceManager`, but a field
    /// with only `WithGetter`/`WithSetter` (no `WithNotifier`) would have no other owner of that
    /// lifetime without this.
    _provider: LolaProviderInfo<B>,
}

impl<T: CommData + Debug, B: FFIBridge> FieldPublisher<T, LolaRuntimeImpl<B>> for LolaFieldPublisher<T, B> {
    type SampleMaybeUninit<'a>
        = LolaFieldSampleMaybeUninit<'a, T, B>
    where
        Self: 'a;

    fn new(identifier: &'static str, instance_info: LolaProviderInfo<B>) -> Result<Self> {
        // A missing WithNotifier tag means no EXPORT_MW_COM_EVENT was registered for `identifier` on
        // the C++ side, so LolaPublisher::new (which requires both a valid SkeletonEventBase* and a
        // valid TypeOperations lookup) fails — treated here as "no notifier", not a hard error.
        let notifier = LolaPublisher::<T, B>::new(identifier, instance_info.clone()).ok();

        let get_name = format!("{identifier}_get");
        let set_name = format!("{identifier}_set");
        // SAFETY: instance_info.skeleton_ptr() is a valid SkeletonBase* for the lifetime of
        // instance_info (kept alive below via _provider); interface_id()/get_name/set_name are valid
        // UTF-8 strings borrowed for the duration of these two FFI calls only. A null result (no
        // WithGetter/WithSetter tag, so no EXPORT_MW_COM_METHOD registered under this name) is a
        // normal, expected outcome here, not a failure — see this file's module doc comment.
        let get_binding = NonNull::new(unsafe {
            instance_info.bridge().get_method_from_skeleton(
                instance_info.skeleton_ptr(),
                instance_info.interface_id(),
                &get_name,
            )
        });
        let set_binding = NonNull::new(unsafe {
            instance_info.bridge().get_method_from_skeleton(
                instance_info.skeleton_ptr(),
                instance_info.interface_id(),
                &set_name,
            )
        });

        Ok(Self {
            notifier,
            get_binding,
            set_binding,
            bridge: instance_info.bridge().clone(),
            _provider: instance_info,
        })
    }

    fn allocate(&self) -> Result<Self::SampleMaybeUninit<'_>> {
        let notifier = self
            .notifier
            .as_ref()
            .ok_or(Error::EventError(EventFailedReason::EventNotAvailable))?;
        Ok(LolaFieldSampleMaybeUninit {
            inner: notifier.allocate()?,
        })
    }

    fn update(&self, value: T) -> Result<()> {
        let notifier = self
            .notifier
            .as_ref()
            .ok_or(Error::EventError(EventFailedReason::EventNotAvailable))?;
        // Publisher::send is a default trait method (allocate + write + EventSampleMut::send) —
        // this is the field equivalent of Event's own copy-path publish.
        notifier.send(value)
    }

    fn register_set_handler(&self, callback: impl Fn(T) -> T + Send + 'static) {
        let Some(binding) = self.set_binding else {
            log::error!(
                "LolaFieldPublisher::register_set_handler: no set binding available for this field \
                 (WithSetter tag missing, or mw_com_get_method_from_skeleton failed to find \
                 \"{{name}}_set\" at construction time) — the handler was NOT registered"
            );
            return;
        };
        // See this file's module doc comment for why this doesn't go through
        // LolaMethodHandler/MethodHandlerCall: <(T,) as MethodArgsCodec>::read_from gives us `T` by
        // value directly (an owned ptr::read), matching callback's own `Fn(T) -> T` signature exactly.
        let boxed: BoxedMethodHandlerFn = Box::new(move |_quality_type, in_args, return_buf| {
            // SAFETY: in_args, when Some, points at a live buffer holding a (T,) value
            // serialized with the same MethodArgsCodec layout this field's Set caller
            // (LolaFieldSetCaller<T, B> in method.rs, itself a LolaMethodCaller<(T,), T, B>) used
            // to write it via invoke_with_copy on the consumer side.
            let (val,) = match in_args {
                Some((ptr, _len)) => unsafe { <(T,) as MethodArgsCodec>::read_from(ptr as *const u8) },
                None => unsafe { <(T,) as MethodArgsCodec>::read_from(core::ptr::null()) },
            };
            let result = callback(val);
            if let Some((ptr, _len)) = return_buf {
                // SAFETY: ptr points at the live return-value buffer the C++ binding allocated
                // for this call, sized/aligned for T per CreateDataTypeSizeInfoFromTypes<T>() —
                // a single type, so no field-ordering ambiguity (see MethodArgsCodec's doc comment
                // in method.rs). Writing here is what makes the confirmed value visible back to
                // the consumer's set_{name}() caller once DoCall returns.
                unsafe {
                    (ptr as *mut T).write(result);
                }
            }
        });
        if !register_method_handler_raw(&self.bridge, binding, boxed) {
            log::error!(
                "LolaFieldPublisher::register_set_handler: mw_com_skeleton_method_register_handler \
                 failed; the handler was NOT registered with the underlying binding"
            );
        }
    }

    fn register_get_handler(&self, callback: impl Fn() -> T + Send + 'static) {
        let Some(binding) = self.get_binding else {
            log::error!(
                "LolaFieldPublisher::register_get_handler: no get binding available for this field \
                 (WithGetter tag missing, or mw_com_get_method_from_skeleton failed to find \
                 \"{{name}}_get\" at construction time) — the handler was NOT registered"
            );
            return;
        };
        let boxed: BoxedMethodHandlerFn = Box::new(move |_quality_type, _in_args, return_buf| {
            // A getter has no in-arguments (Args == ()), so in_args is always None here — nothing
            // to deserialize.
            let result = callback();
            if let Some((ptr, _len)) = return_buf {
                // SAFETY: see register_set_handler's identical comment above.
                unsafe {
                    (ptr as *mut T).write(result);
                }
            }
        });
        if !register_method_handler_raw(&self.bridge, binding, boxed) {
            log::error!(
                "LolaFieldPublisher::register_get_handler: mw_com_skeleton_method_register_handler \
                 failed; the handler was NOT registered with the underlying binding"
            );
        }
    }
}

/// Zero-copy field sample handle, committed via `FieldSampleMut::update()`.
/// Pure delegating wrapper around `LolaSampleMut<'a, T, B>` (`producer.rs`'s Event zero-copy sample) —
/// see this file's module doc comment.
#[derive(Debug)]
pub struct LolaFieldSampleMut<'a, T: CommData + Debug, B: FFIBridge> {
    inner: LolaSampleMut<'a, T, B>,
}

impl<'a, T: CommData + Debug, B: FFIBridge> core::ops::Deref for LolaFieldSampleMut<'a, T, B> {
    type Target = T;
    fn deref(&self) -> &T {
        &self.inner
    }
}
impl<'a, T: CommData + Debug, B: FFIBridge> core::ops::DerefMut for LolaFieldSampleMut<'a, T, B> {
    fn deref_mut(&mut self) -> &mut T {
        &mut self.inner
    }
}

impl<'a, T: CommData + Debug, B: FFIBridge> SampleMut<T> for LolaFieldSampleMut<'a, T, B> {}

impl<'a, T: CommData + Debug, B: FFIBridge> FieldSampleMut<T> for LolaFieldSampleMut<'a, T, B> {
    fn update(self) -> Result<()> {
        // Mirrors EventSampleMut::send() for events — commits the write and notifies subscribers.
        self.inner.send()
    }
}

/// A single uninitialised field slot, obtained via `LolaFieldPublisher::allocate()`.
/// Pure delegating wrapper around `LolaSampleMaybeUninit<'a, T, B>` — see this file's module doc
/// comment.
#[derive(Debug)]
pub struct LolaFieldSampleMaybeUninit<'a, T: CommData + Debug, B: FFIBridge> {
    inner: LolaSampleMaybeUninit<'a, T, B>,
}

impl<'a, T: CommData + Debug, B: FFIBridge> AsMut<core::mem::MaybeUninit<T>>
    for LolaFieldSampleMaybeUninit<'a, T, B>
{
    fn as_mut(&mut self) -> &mut core::mem::MaybeUninit<T> {
        self.inner.as_mut()
    }
}

impl<'a, T: CommData + Debug, B: FFIBridge> SampleMaybeUninitTrait<T> for LolaFieldSampleMaybeUninit<'a, T, B> {
    type SampleMut = LolaFieldSampleMut<'a, T, B>;

    fn write(self, value: T) -> LolaFieldSampleMut<'a, T, B> {
        LolaFieldSampleMut {
            inner: self.inner.write(value),
        }
    }

    unsafe fn assume_init(self) -> LolaFieldSampleMut<'a, T, B> {
        LolaFieldSampleMut {
            // SAFETY: forwarded from this function's own caller-provided safety contract.
            inner: unsafe { self.inner.assume_init() },
        }
    }
}

#[cfg(test)]
mod test {
    use super::*;
    use std::sync::{Arc, Mutex};

    use bridge_ffi_mock::{MockFFIBridge, MockPointerAllocator, SharedMockBridge};
    use bridge_ffi_rs::{FatPtr, SkeletonBase};
    use crate::producer::{NativeSkeletonHandle, SkeletonInstanceManager};
    use mockall::Sequence;
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

    /// Reconstructs the boxed handler closure `register_set_handler`/`register_get_handler` handed to
    /// the (mocked) `skeleton_method_register_handler`, and calls it exactly as
    /// `mw_com_impl_call_method_handler` would on the real C++ side — see that function's doc comment
    /// in `bridge_ffi_lola.rs`. Frees the box afterwards so the test doesn't leak it.
    unsafe fn call_and_free_handler(
        fat_ptr: FatPtr,
        quality_type: u8,
        in_args: Option<(*mut u8, usize)>,
        return_buf: Option<(*mut u8, usize)>,
    ) {
        type Callback = dyn FnMut(u8, Option<(*mut u8, usize)>, Option<(*mut u8, usize)>);
        // SAFETY: fat_ptr was just captured from the mocked skeleton_method_register_handler call
        // below, whose real (non-mocked) production counterpart reconstructs it exactly this way —
        // see mw_com_impl_call_method_handler in bridge_ffi_lola.rs.
        let callable: &mut Callback = unsafe { core::mem::transmute(fat_ptr) };
        callable(quality_type, in_args, return_buf);
        // Reclaims and drops the box register_method_handler_raw leaked into C++'s "ownership" on
        // success — there is no real C++ side here to eventually free it via process exit.
        drop(unsafe { Box::from_raw(callable as *mut Callback) });
    }

    /// Field get/set round trip (PR #6's test plan item 2): registers a Set and a Get handler via
    /// `LolaFieldPublisher`, then — playing the C++ side, exactly as `test_method_call_round_trip_copy_path`
    /// (`method.rs`) does for a plain Method call — invokes each registered handler directly through a
    /// mock in-args/return buffer pair. Only `skeleton_method_register_handler`/`create_skeleton`/
    /// `destroy_skeleton` are mocked; the handler closures, and the `MethodArgsCodec` (de)serialization
    /// they use, are real.
    #[test]
    fn test_field_get_set_round_trip() {
        let mut mock = MockFFIBridge::new();
        let mut seq = Sequence::new();

        let skeleton_alloc = MockPointerAllocator::<SkeletonBase>::new();
        let set_binding_alloc = MockPointerAllocator::<SkeletonMethodBinding>::new();
        let get_binding_alloc = MockPointerAllocator::<SkeletonMethodBinding>::new();

        let skel = skeleton_alloc.clone();
        mock.expect_create_skeleton()
            .in_sequence(&mut seq)
            .returning(move |_, _| skel.allocate());

        let set_binding_ptr = set_binding_alloc.allocate();
        let get_binding_ptr = get_binding_alloc.allocate();

        let captured_set: Arc<Mutex<Option<FatPtr>>> = Arc::new(Mutex::new(None));
        let captured_get: Arc<Mutex<Option<FatPtr>>> = Arc::new(Mutex::new(None));
        let cap_set = captured_set.clone();
        let cap_get = captured_get.clone();

        // Two separate expectations ordered by `seq`, not disambiguated by binding pointer: both
        // `set_binding_ptr`/`get_binding_ptr` point at a zero-sized `SkeletonMethodBinding`, so
        // `Box`'s well-known ZST sentinel address makes them identical pointers — only call order
        // (register_set_handler below, then register_get_handler) tells the two calls apart.
        mock.expect_skeleton_method_register_handler()
            .times(1)
            .in_sequence(&mut seq)
            .returning(move |_binding, fat_ptr| {
                *cap_set.lock().expect("lock") = Some(*fat_ptr);
                true
            });
        mock.expect_skeleton_method_register_handler()
            .times(1)
            .in_sequence(&mut seq)
            .returning(move |_binding, fat_ptr| {
                *cap_get.lock().expect("lock") = Some(*fat_ptr);
                true
            });

        let skel_cleanup = skeleton_alloc.clone();
        mock.expect_destroy_skeleton().in_sequence(&mut seq).returning(move |ptr| {
            assert!(skel_cleanup.free(ptr), "destroy_skeleton called with unknown pointer");
        });

        let bridge = SharedMockBridge::new(mock);
        let spec = bridge_ffi_rs::InstanceSpecifier::try_from("/test_instance")
            .expect("valid instance specifier");
        let skeleton_handle = NativeSkeletonHandle::<SharedMockBridge>::new(&bridge, "", &spec)
            .expect("create_skeleton mock should not fail");
        let provider = LolaProviderInfo::new_for_test(
            score_com_concept::InstanceSpecifier::new("/test_instance").expect("valid instance specifier"),
            "TestInterface",
            SkeletonInstanceManager(Arc::new(skeleton_handle)),
            bridge.clone(),
        );

        let publisher = LolaFieldPublisher::<MethodTestArg, SharedMockBridge> {
            notifier: None,
            get_binding: NonNull::new(get_binding_ptr),
            set_binding: NonNull::new(set_binding_ptr),
            bridge: bridge.clone(),
            _provider: provider,
        };

        // --- Set round trip: consumer calls set_{name}(10), handler adds 1, returns 11. ---
        publisher.register_set_handler(|arg: MethodTestArg| MethodTestArg { value: arg.value + 1 });
        let set_fat_ptr = captured_set
            .lock()
            .expect("lock")
            .take()
            .expect("register_set_handler should have registered a handler");

        let in_args_size = <(MethodTestArg,) as MethodArgsCodec>::layout().0;
        let mut set_in_buf = vec![0u8; in_args_size];
        // SAFETY: set_in_buf is exactly in_args_size bytes, matching (MethodTestArg,)'s own layout.
        unsafe { (MethodTestArg { value: 10 },).write_into(set_in_buf.as_mut_ptr()) };
        let mut set_return_buf = vec![0u8; core::mem::size_of::<MethodTestArg>()];

        unsafe {
            call_and_free_handler(
                set_fat_ptr,
                0,
                Some((set_in_buf.as_mut_ptr(), set_in_buf.len())),
                Some((set_return_buf.as_mut_ptr(), set_return_buf.len())),
            );
        }
        // SAFETY: set_return_buf was just populated by the handler invoked above.
        let set_result = unsafe { (set_return_buf.as_ptr() as *const MethodTestArg).read() };
        assert_eq!(set_result, MethodTestArg { value: 11 });

        // --- Get round trip: consumer calls get_{name}(), handler returns a fixed value. ---
        publisher.register_get_handler(|| MethodTestArg { value: 99 });
        let get_fat_ptr = captured_get
            .lock()
            .expect("lock")
            .take()
            .expect("register_get_handler should have registered a handler");

        let mut get_return_buf = vec![0u8; core::mem::size_of::<MethodTestArg>()];
        unsafe {
            call_and_free_handler(get_fat_ptr, 0, None, Some((get_return_buf.as_mut_ptr(), get_return_buf.len())));
        }
        // SAFETY: get_return_buf was just populated by the handler invoked above.
        let get_result = unsafe { (get_return_buf.as_ptr() as *const MethodTestArg).read() };
        assert_eq!(get_result, MethodTestArg { value: 99 });

        drop(publisher);
        skeleton_alloc.assert_all_freed();
    }
}
