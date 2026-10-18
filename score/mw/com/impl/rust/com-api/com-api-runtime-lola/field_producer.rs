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
//! A C++ `SkeletonField<T>` owns a `SkeletonEvent<T>` (the field value and its notifications) plus up
//! to two `SkeletonMethod`s for get/set, and implements field semantics on top of them: `Update()`
//! before `OfferService` stores the initial value the field needs to be offered, `RegisterSetHandler()`
//! marks the setter as served (OfferService fails with `kSetHandlerNotSet` otherwise) and publishes the
//! accepted value to subscribers, and `PrepareOffer()` installs the get handler, which answers with the
//! latest value. `LolaFieldPublisher` therefore goes through that typed API (the field FFI registered by
//! `EXPORT_MW_COM_FIELD`, registry_bridge_macro.h) rather than the method bindings underneath it.
//!
//! - `update()` → `SkeletonField<T>::Update()` (works before and after offering).
//! - `allocate()` / `FieldSampleMut::update()` → the field's `SkeletonEvent<T>` through the event FFI
//!   (`EXPORT_MW_COM_FIELD` also registers the field's event under the field name), which is what
//!   `SkeletonField<T>::Allocate()`/`Update(SampleAllocateePtr)` do as well. Only available after offering.
//! - `register_set_handler()` → `SkeletonField<T>::RegisterSetHandler()`.
//! - `register_get_handler()`: LoLa has no custom get handler. The getter always answers with the latest
//!   value set through `update()` or an accepted set call, so the callback is not installed (logged).

use core::fmt::Debug;

use bridge_ffi_rs::FFIBridge;
use score_com_concept::{
    CommData, Error, EventFailedReason, EventSampleMut, FieldPublisher, FieldSampleMut, Publisher, Result,
    SampleMaybeUninit as SampleMaybeUninitTrait, SampleMut,
};
use score_log as log;

use crate::{LolaProviderInfo, LolaPublisher, LolaRuntimeImpl, LolaSampleMaybeUninit, LolaSampleMut};

/// Boxed field set-handler closure handed to C++: receives a pointer to the requested value and
/// replaces it in place with the value to accept. Invoked by `bridge_ffi_lola`'s
/// `mw_com_impl_call_field_set_handler`.
pub(crate) type BoxedFieldSetHandlerFn = Box<dyn FnMut(*mut u8) + Send + 'static>;

pub struct LolaFieldPublisher<T: CommData + Debug, B: FFIBridge> {
    /// The field's event, for zero-copy `allocate()`. `None` if the field's event cannot be found
    /// (field not exported via `EXPORT_MW_COM_FIELD`).
    notifier: Option<LolaPublisher<T, B>>,
    /// Field name as registered on the C++ side.
    identifier: &'static str,
    bridge: B,
    /// Keeps the shared skeleton alive for the field FFI calls below.
    provider: LolaProviderInfo<B>,
}

impl<T: CommData + Debug, B: FFIBridge> FieldPublisher<T, LolaRuntimeImpl<B>> for LolaFieldPublisher<T, B> {
    type SampleMaybeUninit<'a>
        = LolaFieldSampleMaybeUninit<'a, T, B>
    where
        Self: 'a;

    fn new(identifier: &'static str, instance_info: LolaProviderInfo<B>) -> Result<Self> {
        let notifier = LolaPublisher::<T, B>::new(identifier, instance_info.clone()).ok();
        Ok(Self {
            notifier,
            identifier,
            bridge: instance_info.bridge().clone(),
            provider: instance_info,
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
        // SAFETY: skeleton_ptr() is valid while self.provider is alive; value is a live T for the call
        // (C++ copies it).
        let updated = unsafe {
            self.bridge.skeleton_field_update(
                self.provider.skeleton_ptr(),
                self.provider.interface_id(),
                self.identifier,
                &value as *const T as *const u8,
            )
        };
        if updated {
            Ok(())
        } else {
            Err(Error::EventError(EventFailedReason::SendingDataFailed))
        }
    }

    fn register_set_handler(&self, callback: impl Fn(T) -> T + Send + 'static) {
        let handler: BoxedFieldSetHandlerFn = Box::new(move |value_ptr: *mut u8| {
            let value_ptr = value_ptr as *mut T;
            // SAFETY: C++ (SkeletonField<T>'s set handler) passes a pointer to its live, initialized T,
            // valid for the duration of this call. The value is moved out and the accepted one moved
            // back in, so the slot holds exactly one live T again when C++ resumes.
            unsafe {
                let requested = value_ptr.read();
                value_ptr.write(callback(requested));
            }
        });
        let raw = Box::into_raw(handler);
        // SAFETY: FatPtr is a binary-compatible reinterpretation of a `dyn Trait` fat pointer, the same
        // technique method.rs's register_method_handler_raw relies on.
        let fat_ptr: bridge_ffi_rs::FatPtr = unsafe { core::mem::transmute(raw) };
        // SAFETY: skeleton_ptr() is valid while self.provider is alive; fat_ptr references the closure
        // boxed above.
        let registered = unsafe {
            self.bridge.skeleton_field_register_set_handler(
                self.provider.skeleton_ptr(),
                self.provider.interface_id(),
                self.identifier,
                &fat_ptr,
            )
        };
        if !registered {
            // C++ never took ownership, so the box is freed here instead of leaking.
            drop(unsafe { Box::from_raw(raw) });
            log::error!(
                "LolaFieldPublisher::register_set_handler: the set handler was NOT registered (field not exported \
                 via EXPORT_MW_COM_FIELD, no WithSetter tag, or registration failed)"
            );
        }
    }

    fn register_get_handler(&self, _callback: impl Fn() -> T + Send + 'static) {
        // SkeletonField<T> installs its own get handler in PrepareOffer(), answering with the latest
        // field value, and offers no way to replace it.
        log::warn!(
            "LolaFieldPublisher::register_get_handler: LoLa answers field Get with the latest updated value; \
             the custom get handler is not used"
        );
    }
}

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

impl<'a, T: CommData + Debug, B: FFIBridge> AsMut<core::mem::MaybeUninit<T>> for LolaFieldSampleMaybeUninit<'a, T, B> {
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

    use crate::producer::{NativeSkeletonHandle, SkeletonInstanceManager};
    use bridge_ffi_mock::{MockFFIBridge, MockPointerAllocator, SharedMockBridge};
    use bridge_ffi_rs::{FatPtr, SkeletonBase};
    use mockall::Sequence;

    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    #[repr(C)]
    struct FieldTestValue {
        value: i32,
    }

    unsafe impl score_com_concept::Reloc for FieldTestValue {}

    impl score_com_concept::CommData for FieldTestValue {
        const ID: &'static str = "FieldTestValue";
    }

    /// Calls the set handler closure exactly as `mw_com_impl_call_field_set_handler`
    /// (bridge_ffi_lola.rs) does, then frees the box, which has no real C++ owner here.
    unsafe fn call_and_free_set_handler(fat_ptr: FatPtr, value: *mut u8) {
        type Callback = dyn FnMut(*mut u8);
        // SAFETY: fat_ptr was captured from the mocked skeleton_field_register_set_handler call below.
        let callable: &mut Callback = unsafe { core::mem::transmute(fat_ptr) };
        callable(value);
        drop(unsafe { Box::from_raw(callable as *mut Callback) });
    }

    /// `update()` goes to SkeletonField::Update and `register_set_handler()` to
    /// SkeletonField::RegisterSetHandler, by field name; the set handler replaces the requested value
    /// in place; `register_get_handler()` makes no FFI call at all (any unexpected call would fail the
    /// mock).
    #[test]
    fn test_field_update_and_set_handler_use_field_ffi() {
        let mut mock = MockFFIBridge::new();
        let mut seq = Sequence::new();

        let skeleton_alloc = MockPointerAllocator::<SkeletonBase>::new();
        let skel = skeleton_alloc.clone();
        mock.expect_create_skeleton()
            .in_sequence(&mut seq)
            .returning(move |_, _| skel.allocate());

        let updated: Arc<Mutex<Option<FieldTestValue>>> = Arc::new(Mutex::new(None));
        let upd = updated.clone();
        mock.expect_skeleton_field_update()
            .times(1)
            .in_sequence(&mut seq)
            .returning(move |_skeleton, interface_id, field_id, value| {
                assert_eq!(interface_id, "TestInterface");
                assert_eq!(field_id, "speed");
                // SAFETY: update() passes a pointer to a live FieldTestValue.
                *upd.lock().expect("lock") = Some(unsafe { (value as *const FieldTestValue).read() });
                true
            });

        let captured_set: Arc<Mutex<Option<FatPtr>>> = Arc::new(Mutex::new(None));
        let cap_set = captured_set.clone();
        mock.expect_skeleton_field_register_set_handler()
            .times(1)
            .in_sequence(&mut seq)
            .returning(move |_skeleton, interface_id, field_id, fat_ptr| {
                assert_eq!(interface_id, "TestInterface");
                assert_eq!(field_id, "speed");
                *cap_set.lock().expect("lock") = Some(*fat_ptr);
                true
            });

        let skel_cleanup = skeleton_alloc.clone();
        mock.expect_destroy_skeleton()
            .in_sequence(&mut seq)
            .returning(move |ptr| {
                assert!(skel_cleanup.free(ptr), "destroy_skeleton called with unknown pointer");
            });

        let bridge = SharedMockBridge::new(mock);
        let spec = bridge_ffi_rs::InstanceSpecifier::try_from("/test_instance").expect("valid instance specifier");
        let skeleton_handle = NativeSkeletonHandle::<SharedMockBridge>::new(&bridge, "", &spec)
            .expect("create_skeleton mock should not fail");
        let provider = LolaProviderInfo::new_for_test(
            score_com_concept::InstanceSpecifier::new("/test_instance").expect("valid instance specifier"),
            "TestInterface",
            SkeletonInstanceManager(Arc::new(skeleton_handle)),
            bridge.clone(),
        );
        let publisher = LolaFieldPublisher::<FieldTestValue, SharedMockBridge> {
            notifier: None,
            identifier: "speed",
            bridge: bridge.clone(),
            provider,
        };

        publisher
            .update(FieldTestValue { value: 7 })
            .expect("update should succeed when the field FFI does");
        assert_eq!(*updated.lock().expect("lock"), Some(FieldTestValue { value: 7 }));

        publisher.register_set_handler(|requested: FieldTestValue| FieldTestValue {
            value: requested.value.min(100),
        });
        let set_fat_ptr = captured_set
            .lock()
            .expect("lock")
            .take()
            .expect("register_set_handler should have registered a handler");
        let mut slot = FieldTestValue { value: 250 };
        unsafe { call_and_free_set_handler(set_fat_ptr, &mut slot as *mut FieldTestValue as *mut u8) };
        assert_eq!(slot, FieldTestValue { value: 100 });

        publisher.register_get_handler(|| FieldTestValue { value: 99 });

        drop(publisher);
        skeleton_alloc.assert_all_freed();
    }
}
