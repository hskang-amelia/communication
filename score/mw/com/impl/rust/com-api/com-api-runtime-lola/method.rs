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

//! LoLa runtime implementation of Rust `Method<T>` calls and field get/set.
//!
//! Method calls go through the type-erased C++ method bindings (`mw_com_get_method_from_proxy`/
//! `_skeleton`, in-args/return-value buffers, `mw_com_proxy_method_do_call`,
//! `mw_com_skeleton_method_register_handler`). Calls are synchronous and wrapped in an already-resolved
//! `Future`; the C++ binding supports one call in flight (`kCallQueueSize == 1`), so every call uses
//! queue position 0.
//!
//! Field get/set use the typed field FFI instead (see `LolaFieldAccess`).
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
    MethodHandler, MethodHandlerCall, MethodInArgAllocator, MethodInArgMaybeUninit, MethodInArgPtr, MethodReturnSample,
    Result, ZeroCopyArgs,
};
use score_log as log;

use crate::consumer::{LolaConsumerInfo, NativeProxyBase};
use crate::producer::LolaProviderInfo;
use crate::LolaRuntimeImpl;

/// Return value of a method call on the consumer side, copied out of the C++ return-value buffer.
/// "Zero-copy" in this module only refers to the argument side (`invoke_zero_copy`).
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

// The copy path (`invoke_with_copy`, `LolaMethodHandler::register_handler`) packs `Args` field by field
// with `score_com_concept::MethodArgsCodec`, matching the C++ `type_erased_storage.h` layout; a Rust
// tuple's own layout is not guaranteed to match it.

/// Boxed skeleton-method handler in the flattened FFI shape: quality type, then optional `(ptr, len)`
/// in-args and return-value buffers (see `mw_com_impl_call_method_handler`, bridge_ffi_lola.rs).
type BoxedMethodHandlerFn = Box<dyn FnMut(u8, Option<(*mut u8, usize)>, Option<(*mut u8, usize)>) + Send + 'static>;

/// Whether `Return` is `()` (a void method). `Return` is generic in the concept traits, so this is a
/// `TypeId` check (`CommData: Reloc: 'static`).
fn is_void<Return: 'static>() -> bool {
    TypeId::of::<Return>() == TypeId::of::<()>()
}

/// Owns its own `ProxyBase` (as each event subscriber in consumer.rs does) and the resolved
/// `ProxyMethodBinding*`.
pub struct LolaMethodCaller<Args: MethodArgs, Return: CommData, B: FFIBridge> {
    // Owned by the C++ proxy behind `_proxy`; only valid while `_proxy` is alive.
    binding: NonNull<ProxyMethodBinding>,
    _proxy: NativeProxyBase<B>,
    bridge: B,
    _phantom: PhantomData<(Args, Return)>,
}

// SAFETY: as for NativeProxyBase (consumer.rs): `binding` points into the C++ proxy owned by `_proxy`.
unsafe impl<Args: MethodArgs, Return: CommData, B: FFIBridge> Send for LolaMethodCaller<Args, Return, B> {}

impl<Args: MethodArgs, Return: CommData, B: FFIBridge> LolaMethodCaller<Args, Return, B> {
    /// Calls the method (queue position 0) once the in-args buffer is filled, and reads the return value.
    fn do_call_and_read_return(&self) -> Result<Return> {
        let return_ptr = if is_void::<Return>() {
            None
        } else {
            // SAFETY: self.binding is valid while _proxy is alive. Requested before DoCall, as
            // ProxyMethod<Signature>::operator() does.
            let (ptr, _len) = unsafe {
                self.bridge
                    .proxy_method_get_return_value_buffer(self.binding.as_ptr(), 0)
            }
            .ok_or(Error::MethodError(MethodFailedReason::ReturnValueBufferUnavailable))?;
            Some(ptr)
        };

        // SAFETY: self.binding is valid; the caller has filled any in-args buffer.
        let call_ok = unsafe { self.bridge.proxy_method_do_call(self.binding.as_ptr(), 0) };
        if !call_ok {
            return Err(Error::MethodError(MethodFailedReason::CallFailed));
        }

        let value = if let Some(ptr) = return_ptr {
            // SAFETY: a successful DoCall leaves a live Return in this buffer, sized and aligned for it.
            unsafe { (ptr as *const Return).read() }
        } else {
            // is_void::<Return>() proved Return == (), so this is a same-type, zero-sized copy.
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
        // One ProxyBase per caller, as LolaSubscribableImpl does per event subscriber.
        let handle = instance_info.get_handle().ok_or(Error::ConsumerError(
            score_com_concept::ConsumerFailedReason::ServiceHandleNotFound,
        ))?;
        let proxy = NativeProxyBase::new(instance_info.bridge(), instance_info.interface_id(), handle)?;

        // SAFETY: proxy.as_ptr() is a valid, just-created ProxyBase*.
        let raw_binding = unsafe {
            instance_info
                .bridge()
                .get_method_from_proxy(proxy.as_ptr(), instance_info.interface_id(), method_name)
        };
        let binding =
            NonNull::new(raw_binding).ok_or(Error::MethodError(MethodFailedReason::MethodCallerCreationFailed))?;

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
                // SAFETY: self.binding is valid; only called for methods with in-arguments, as
                // GetInArgsBuffer requires.
                let (buf_ptr, buf_len) =
                    unsafe { self.bridge.proxy_method_get_in_args_buffer(self.binding.as_ptr(), 0) }
                        .ok_or(Error::MethodError(MethodFailedReason::InArgsBufferUnavailable))?;
                // A smaller buffer means the C++ and Rust argument layouts disagree; writing would
                // overflow it.
                if buf_len < size {
                    return Err(Error::MethodError(MethodFailedReason::InArgsBufferUnavailable));
                }
                // SAFETY: buf_ptr is valid for buf_len >= size bytes and aligned by the C++ binding.
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
            // SAFETY: as in invoke_with_copy.
            let (ptr, _len) = unsafe { self.bridge.proxy_method_get_in_args_buffer(self.binding.as_ptr(), 0) }
                .ok_or(Error::MethodError(MethodFailedReason::InArgsBufferUnavailable))?;
            ptr
        } else {
            // Args == (): alloc_uninit never calls allocate::<T>(), so the null buffer is never used.
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
            // The arguments were already written into the C++ in-args buffer through allocate().
            let value = self.do_call_and_read_return()?;
            Ok(LolaMethodReturnSample { value })
        }
    }
}

/// Initialized method argument inside the C++ in-args buffer. The pointer is not read back: the C++
/// call already knows the buffer, the type exists for the `MethodInArgPtr<T>` contract.
#[allow(dead_code)]
pub struct LolaMethodInArgPtr<T> {
    ptr: *mut T,
    _phantom: PhantomData<T>,
}

impl<T> MethodInArgPtr<T> for LolaMethodInArgPtr<T> {}

/// Not yet initialized method argument slot inside the C++ in-args buffer.
pub struct LolaMethodInArgMaybeUninit<T> {
    ptr: *mut T,
    _phantom: PhantomData<T>,
}

impl<T> MethodInArgMaybeUninit<T> for LolaMethodInArgMaybeUninit<T> {
    type Ptr = LolaMethodInArgPtr<T>;

    fn write(self, val: T) -> ZeroCopyArgs<LolaMethodInArgPtr<T>> {
        // SAFETY: self.ptr is this argument's slot in the live in-args buffer (LolaMethodInArgAllocator).
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

/// Hands out argument slots at successive offsets of the in-args buffer, using the same layout as
/// `MethodArgsCodec`. Correct because `alloc_uninit` calls `allocate::<T>()` once per field in order,
/// and a fresh allocator is created per call.
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
        // SAFETY: self.buffer is valid for Args::layout().0 bytes, and allocate() is called once per
        // field in order, so the slot stays inside it.
        let ptr = unsafe { self.buffer.add(field_offset) as *mut T };
        LolaMethodInArgMaybeUninit {
            ptr,
            _phantom: PhantomData,
        }
    }
}

/// Keeps the shared skeleton alive (via `LolaProviderInfo`) and holds the resolved
/// `SkeletonMethodBinding*`.
pub struct LolaMethodHandler<Args: MethodArgs, Return: CommData, B: FFIBridge> {
    binding: NonNull<SkeletonMethodBinding>,
    _provider: LolaProviderInfo<B>,
    bridge: B,
    _phantom: PhantomData<(Args, Return)>,
}

// SAFETY: as for NativeSkeletonHandle (producer.rs): `binding` points into the C++ skeleton kept alive by
// `_provider`. Sync because the registered handler may run on another C++ thread.
unsafe impl<Args: MethodArgs, Return: CommData, B: FFIBridge> Send for LolaMethodHandler<Args, Return, B> {}
unsafe impl<Args: MethodArgs, Return: CommData, B: FFIBridge> Sync for LolaMethodHandler<Args, Return, B> {}

impl<Args: MethodArgs, Return: CommData, B: FFIBridge> MethodHandler<Args, Return, LolaRuntimeImpl<B>>
    for LolaMethodHandler<Args, Return, B>
{
    fn new(method_name: &str, instance_info: LolaProviderInfo<B>) -> Result<Self>
    where
        Self: Sized,
    {
        // SAFETY: skeleton_ptr() is valid while instance_info (kept as _provider) is alive.
        let raw_binding = unsafe {
            instance_info.bridge().get_method_from_skeleton(
                instance_info.skeleton_ptr(),
                instance_info.interface_id(),
                method_name,
            )
        };
        let binding =
            NonNull::new(raw_binding).ok_or(Error::MethodError(MethodFailedReason::MethodHandlerCreationFailed))?;

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
        let boxed_handler: BoxedMethodHandlerFn = Box::new(move |_quality_type, in_args, return_buf| {
            // SAFETY: in_args, when Some, holds an Args value in the MethodArgsCodec layout. When None
            // (Args == ()), read_from never dereferences its argument.
            let args = match in_args {
                Some((ptr, _len)) => unsafe { Args::read_from(ptr as *const u8) },
                None => unsafe { Args::read_from(core::ptr::null()) },
            };
            let result: Return = handler.call(&args);
            if let Some((ptr, _len)) = return_buf {
                // SAFETY: ptr is the live return-value buffer for this call, sized and aligned for
                // Return by the C++ binding.
                unsafe {
                    (ptr as *mut Return).write(result);
                }
            }
        });

        let raw = Box::into_raw(boxed_handler);
        // SAFETY: FatPtr is a binary-compatible reinterpretation of a `dyn Trait` fat pointer, as in the
        // event receive-handler path (consumer.rs).
        let fat_ptr: FatPtr = unsafe { core::mem::transmute(raw) };
        // SAFETY: self.binding is valid while self._provider is alive; fat_ptr references the closure
        // above. On success C++ keeps it for the process lifetime (SkeletonMethodBinding has no
        // unregister hook).
        let registered = unsafe {
            self.bridge
                .skeleton_method_register_handler(self.binding.as_ptr(), &fat_ptr)
        };
        if !registered {
            // C++ never took ownership, so free the closure here.
            drop(unsafe { Box::from_raw(raw) });
            log::error!(
                "LolaMethodHandler::register_handler: mw_com_skeleton_method_register_handler failed; \
                 the handler was NOT registered with the underlying binding"
            );
        }
    }
}

/// Proxy and names shared by `LolaFieldGetCaller`/`LolaFieldSetCaller`.
///
/// The C++ field getter/setter return `score::Result<T>`, not `T`, so these do not use the method
/// buffers: they call the typed `ProxyField<T>::Get()`/`Set()` through the field FFI registered by
/// `EXPORT_MW_COM_FIELD` (registry_bridge_macro.h), which copies plain `T` values across.
struct LolaFieldAccess<B: FFIBridge> {
    proxy: NativeProxyBase<B>,
    bridge: B,
    interface_id: &'static str,
    field_name: String,
}

impl<B: FFIBridge> LolaFieldAccess<B> {
    /// `method_name` is the `interface!` macro's `"{field}_get"`/`"{field}_set"` member name;
    /// `suffix` is stripped from it to get the field name the C++ registry knows.
    fn new(method_name: &str, suffix: &str, instance_info: LolaConsumerInfo<B>) -> Result<Self> {
        let handle = instance_info.get_handle().ok_or(Error::ConsumerError(
            score_com_concept::ConsumerFailedReason::ServiceHandleNotFound,
        ))?;
        let proxy = NativeProxyBase::new(instance_info.bridge(), instance_info.interface_id(), handle)?;
        let field_name = method_name.strip_suffix(suffix).unwrap_or(method_name).to_owned();
        Ok(Self {
            proxy,
            bridge: instance_info.bridge().clone(),
            interface_id: instance_info.interface_id(),
            field_name,
        })
    }

    fn get<T: CommData>(&self) -> Result<T> {
        let mut value = core::mem::MaybeUninit::<T>::uninit();
        // SAFETY: the proxy pointer is valid for the lifetime of self (owned by proxy); value is
        // valid, aligned storage for one T, initialized by C++ on success.
        unsafe {
            self.bridge.proxy_field_get(
                self.proxy.as_ptr(),
                self.interface_id,
                &self.field_name,
                value.as_mut_ptr() as *mut u8,
            )
        }
        .map_err(|code| Error::MethodError(MethodFailedReason::FieldAccessFailed { code }))?;
        // SAFETY: proxy_field_get returned Ok, so C++ copy-constructed a T into value.
        Ok(unsafe { value.assume_init() })
    }

    fn set<T: CommData>(&self, new_value: &T) -> Result<T> {
        let mut accepted = core::mem::MaybeUninit::<T>::uninit();
        // SAFETY: as in get; new_value is a live T for the duration of the call.
        unsafe {
            self.bridge.proxy_field_set(
                self.proxy.as_ptr(),
                self.interface_id,
                &self.field_name,
                new_value as *const T as *const u8,
                accepted.as_mut_ptr() as *mut u8,
            )
        }
        .map_err(|code| Error::MethodError(MethodFailedReason::FieldAccessFailed { code }))?;
        // SAFETY: proxy_field_set returned Ok, so C++ copy-constructed the accepted T into accepted.
        Ok(unsafe { accepted.assume_init() })
    }
}

/// Caller for a field's Get operation (`ProxyField<T>::Get()`, WithGetter tag). Created by the
/// `interface!` macro under the member name `"{field}_get"`. See `LolaFieldAccess` for why this does not
/// use `LolaMethodCaller`.
pub struct LolaFieldGetCaller<T: CommData + Debug, B: FFIBridge> {
    access: LolaFieldAccess<B>,
    _phantom: PhantomData<T>,
}

impl<T: CommData + Debug, B: FFIBridge> MethodCaller<(), T, LolaRuntimeImpl<B>> for LolaFieldGetCaller<T, B> {
    fn new(method_name: &str, instance_info: LolaConsumerInfo<B>) -> Result<Self>
    where
        Self: Sized,
    {
        Ok(Self {
            access: LolaFieldAccess::new(method_name, "_get", instance_info)?,
            _phantom: PhantomData,
        })
    }

    #[allow(clippy::manual_async_fn)]
    fn invoke_with_copy<'a>(&'a self, _args: ()) -> impl Future<Output = Result<LolaMethodReturnSample<T>>> + 'a {
        async move {
            let value = self.access.get::<T>()?;
            Ok(LolaMethodReturnSample { value })
        }
    }

    fn allocate(&self) -> Result<<() as MethodArgsAllocate<LolaMethodInArgAllocator>>::UninitTuple>
    where
        (): MethodArgsAllocate<LolaMethodInArgAllocator>,
    {
        // A getter has no arguments: alloc_uninit for () never calls allocate::<T>(), so the null
        // buffer is never dereferenced.
        let allocator = LolaMethodInArgAllocator::new(core::ptr::null_mut());
        Ok(<() as MethodArgsAllocate<LolaMethodInArgAllocator>>::alloc_uninit(
            &allocator,
        ))
    }

    #[allow(clippy::manual_async_fn)]
    fn invoke_zero_copy<'a>(
        &'a self,
        _ptrs: <() as MethodArgsPtrTuple<LolaRuntimeImpl<B>>>::PtrTuple,
    ) -> impl Future<Output = Result<LolaMethodReturnSample<T>>> + 'a
    where
        (): MethodArgsPtrTuple<LolaRuntimeImpl<B>>,
    {
        async move {
            let value = self.access.get::<T>()?;
            Ok(LolaMethodReturnSample { value })
        }
    }
}

/// Caller for a field's Set operation (`ProxyField<T>::Set()`, WithSetter tag). Created by the
/// `interface!` macro under the member name `"{field}_set"`. Returns the value the provider accepted,
/// which its set handler may have changed. Zero-copy arguments are not supported: `ProxyField<T>::Set`
/// takes its argument by reference and copies it itself.
pub struct LolaFieldSetCaller<T: CommData + Debug, B: FFIBridge> {
    access: LolaFieldAccess<B>,
    _phantom: PhantomData<T>,
}

impl<T: CommData + Debug, B: FFIBridge> MethodCaller<(T,), T, LolaRuntimeImpl<B>> for LolaFieldSetCaller<T, B> {
    fn new(method_name: &str, instance_info: LolaConsumerInfo<B>) -> Result<Self>
    where
        Self: Sized,
    {
        Ok(Self {
            access: LolaFieldAccess::new(method_name, "_set", instance_info)?,
            _phantom: PhantomData,
        })
    }

    #[allow(clippy::manual_async_fn)]
    fn invoke_with_copy<'a>(&'a self, args: (T,)) -> impl Future<Output = Result<LolaMethodReturnSample<T>>> + 'a {
        async move {
            let (new_value,) = args;
            let value = self.access.set::<T>(&new_value)?;
            Ok(LolaMethodReturnSample { value })
        }
    }

    fn allocate(&self) -> Result<<(T,) as MethodArgsAllocate<LolaMethodInArgAllocator>>::UninitTuple>
    where
        (T,): MethodArgsAllocate<LolaMethodInArgAllocator>,
    {
        Err(Error::MethodError(MethodFailedReason::ZeroCopyNotSupported))
    }

    #[allow(clippy::manual_async_fn)]
    fn invoke_zero_copy<'a>(
        &'a self,
        _ptrs: <(T,) as MethodArgsPtrTuple<LolaRuntimeImpl<B>>>::PtrTuple,
    ) -> impl Future<Output = Result<LolaMethodReturnSample<T>>> + 'a
    where
        (T,): MethodArgsPtrTuple<LolaRuntimeImpl<B>>,
    {
        // Unreachable through the public API (allocate() fails first), but must not touch any buffer.
        async move { Err(Error::MethodError(MethodFailedReason::ZeroCopyNotSupported)) }
    }
}

#[cfg(test)]
mod test {
    use super::*;
    use std::sync::Arc;
    use std::task::{Context, Poll, Wake, Waker};

    use bridge_ffi_mock::{MockFFIBridge, MockPointerAllocator, SharedMockBridge};
    use bridge_ffi_rs::{HandleType, ProxyBase};
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

    /// Copy-path round trip: `invoke_with_copy` serializes the arguments, the mocked
    /// `proxy_method_do_call` plays the C++ side (reads them back, writes a return value), and the
    /// caller reads the result. Only the FFI boundary is mocked.
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
        mock.expect_proxy_method_do_call()
            .in_sequence(&mut seq)
            .returning(move |_, _| {
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
        let binding = NonNull::new(binding_alloc.allocate()).expect("mock binding allocation should not fail");

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

    /// Field get/set callers go through the typed field FFI (`proxy_field_get`/`proxy_field_set`) under
    /// the field name, not through method buffers: get returns the value C++ wrote, set passes the
    /// requested value and returns the accepted one, and a C++ error code surfaces as
    /// `FieldAccessFailed`. Zero-copy set is refused.
    #[test]
    fn test_field_get_and_set_callers_use_field_ffi() {
        let mut mock = MockFFIBridge::new();
        let proxy_alloc = MockPointerAllocator::<ProxyBase>::new();
        let prox = proxy_alloc.clone();
        mock.expect_create_proxy()
            .times(2)
            .returning(move |_, _| prox.allocate());

        mock.expect_proxy_field_get()
            .times(1)
            .returning(|_proxy, interface_id, field_id, out_value| {
                assert_eq!(interface_id, "TestInterface");
                assert_eq!(field_id, "speed");
                // SAFETY: plays the C++ side: out_value is valid storage for one MethodTestArg.
                unsafe { (out_value as *mut MethodTestArg).write(MethodTestArg { value: 30 }) };
                Ok(())
            });
        let mut set_calls = 0;
        mock.expect_proxy_field_set().times(2).returning(
            move |_proxy, _interface_id, field_id, in_value, out_value| {
                assert_eq!(field_id, "speed");
                set_calls += 1;
                if set_calls == 2 {
                    return Err(7);
                }
                // SAFETY: plays the C++ side: in_value is a live MethodTestArg, out_value valid storage.
                let requested = unsafe { (in_value as *const MethodTestArg).read() };
                unsafe {
                    (out_value as *mut MethodTestArg).write(MethodTestArg {
                        value: requested.value.min(100),
                    })
                };
                Ok(())
            },
        );

        let prox_cleanup = proxy_alloc.clone();
        mock.expect_destroy_proxy().times(2).returning(move |ptr| {
            assert!(prox_cleanup.free(ptr), "destroy_proxy called with unknown pointer");
        });

        let bridge = SharedMockBridge::new(mock);
        let handle = HandleType::default();
        let make_access = |bridge: &SharedMockBridge| LolaFieldAccess {
            proxy: NativeProxyBase::<SharedMockBridge>::new(bridge, "TestInterface", &handle)
                .expect("NativeProxyBase::new should succeed with mock create_proxy"),
            bridge: bridge.clone(),
            interface_id: "TestInterface",
            field_name: "speed".to_owned(),
        };

        let getter = LolaFieldGetCaller::<MethodTestArg, SharedMockBridge> {
            access: make_access(&bridge),
            _phantom: PhantomData,
        };
        let sample = block_on_ready(getter.invoke_with_copy(())).expect("get should succeed");
        assert_eq!(*sample, MethodTestArg { value: 30 });

        let setter = LolaFieldSetCaller::<MethodTestArg, SharedMockBridge> {
            access: make_access(&bridge),
            _phantom: PhantomData,
        };
        let accepted =
            block_on_ready(setter.invoke_with_copy((MethodTestArg { value: 250 },))).expect("set should succeed");
        assert_eq!(*accepted, MethodTestArg { value: 100 });

        match block_on_ready(setter.invoke_with_copy((MethodTestArg { value: 1 },))) {
            Err(Error::MethodError(MethodFailedReason::FieldAccessFailed { code })) => assert_eq!(code, 7),
            Err(other) => panic!("expected FieldAccessFailed, got {other:?}"),
            Ok(_) => panic!("expected FieldAccessFailed, got Ok"),
        }
        assert!(matches!(
            setter.allocate(),
            Err(Error::MethodError(MethodFailedReason::ZeroCopyNotSupported))
        ));

        drop(getter);
        drop(setter);
        proxy_alloc.assert_all_freed();
    }
}
