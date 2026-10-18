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

//! Arity-parameterised blanket impls for method argument tuples.
//!
//! # Single configuration point
//!
//! To raise (or lower) the maximum number of arguments a method may have, edit the
//! `impl_all_arities!` invocation at the bottom of this file.  Add one
//! more `(TypeIdent, arg_ident)` pair per additional argument.  Everything else -
//! `Reloc`, `CommData`, `MethodArgs`, `MethodArgsPtrTuple`, `MethodArgsAllocate`, `MethodCallInput` (zero-copy
//! path), and `MethodHandlerCall` - is generated using macros.
//!
//! Note: `_gen_method_wrapper!` in `interface_macros.rs` self-generates its argument
//! identifiers via a counting recursive macro, so it has no separate limit to keep in
//! sync - raising the arity here is the only change needed.
//! (I don't think clippy linting will support this many arguments,
//!  so we may need to reduce the limit to 4 to 5 in the future based on project clippy linting rules.)
//!
//! # Arity 0 special case
//!
//! Arity 0 (`()`) is handled separately in `method_concept.rs` because the zero-tuple
//! has no positional variables to destructure.
//! This macro covers arities 1 through 8 (inclusive) by default, but can be extended to higher arities if needed.

use crate::{
    CommData, MethodArgs, MethodArgsAllocate, MethodArgsCodec, MethodArgsPtrTuple, MethodCallInput,
    MethodCaller, MethodHandlerCall, MethodInArgAllocator, Reloc, Result, Runtime, ZeroCopyArgs,
};
use core::future::Future;

/// Internal recursive macro.  Do not invoke directly - use `impl_all_arities!` below.
#[doc(hidden)]
macro_rules! impl_all_arities {
    ( $( ($T:ident, $a:ident) ),+ $(,)? ) => {
        impl_all_arities!(@step [] [] [$( ($T, $a) ),+]);
    };

    (@step [$($T:ident),*] [$($a:ident),*] []) => {};

    (
        @step [$($T:ident),*] [$($a:ident),*]
        [($nextT:ident, $nextA:ident) $(, ($restT:ident, $restA:ident))*]
    ) => {

        unsafe impl<$($T: Reloc,)* $nextT: Reloc> Reloc for ($($T,)* $nextT,) {}

        impl<$($T: CommData,)* $nextT: CommData> CommData for ($($T,)* $nextT,) {
            // Placeholder ID - the tuple structure itself serves as the identifier.
            const ID: &'static str = stringify!(($($T,)* $nextT,));
        }

        impl<$($T: CommData,)* $nextT: CommData> MethodArgs for ($($T,)* $nextT,) {}

        // Packs/unpacks this tuple field-by-field using sequential natural-alignment placement
        // (as if the compiler laid out `struct { T1 a; T2 b; ...; }`) — see `MethodArgsCodec`'s
        // doc comment. `$T`/`$a` pairs mirror this macro's own naming.
        impl<$($T: CommData,)* $nextT: CommData> MethodArgsCodec for ($($T,)* $nextT,) {
            fn layout() -> (usize, usize) {
                let mut size: usize = 0;
                let mut align: usize = 1;
                $(
                    let field_align = core::mem::align_of::<$T>();
                    let padding = if size % field_align == 0 { 0 } else { field_align - (size % field_align) };
                    size += padding + core::mem::size_of::<$T>();
                    if field_align > align {
                        align = field_align;
                    }
                )*
                let field_align = core::mem::align_of::<$nextT>();
                let padding = if size % field_align == 0 { 0 } else { field_align - (size % field_align) };
                size += padding + core::mem::size_of::<$nextT>();
                if field_align > align {
                    align = field_align;
                }
                if size % align != 0 {
                    size += align - (size % align);
                }
                (size, align)
            }

            unsafe fn write_into(self, buf: *mut u8) {
                #[allow(non_snake_case)]
                let ($($a,)* $nextA,) = self;
                let mut offset: usize = 0;
                $(
                    let field_align = core::mem::align_of::<$T>();
                    let padding = if offset % field_align == 0 { 0 } else { field_align - (offset % field_align) };
                    offset += padding;
                    // SAFETY: caller guarantees buf is valid for Self::layout().0 bytes, aligned; this
                    // offset + size_of::<$T>() never exceeds that by construction (same algorithm as
                    // layout() above, walked in the same field order).
                    unsafe { (buf.add(offset) as *mut $T).write($a); }
                    offset += core::mem::size_of::<$T>();
                )*
                let field_align = core::mem::align_of::<$nextT>();
                let padding = if offset % field_align == 0 { 0 } else { field_align - (offset % field_align) };
                offset += padding;
                unsafe { (buf.add(offset) as *mut $nextT).write($nextA); }
                offset += core::mem::size_of::<$nextT>();
                let _ = offset;
            }

            unsafe fn read_from(buf: *const u8) -> Self {
                let mut offset: usize = 0;
                $(
                    let field_align = core::mem::align_of::<$T>();
                    let padding = if offset % field_align == 0 { 0 } else { field_align - (offset % field_align) };
                    offset += padding;
                    // SAFETY: see write_into's SAFETY comment; caller additionally guarantees buf
                    // holds a live, initialized value serialized with this same layout.
                    #[allow(non_snake_case)]
                    let $a = unsafe { (buf.add(offset) as *const $T).read() };
                    offset += core::mem::size_of::<$T>();
                )*
                let field_align = core::mem::align_of::<$nextT>();
                let padding = if offset % field_align == 0 { 0 } else { field_align - (offset % field_align) };
                offset += padding;
                #[allow(non_snake_case)]
                let $nextA = unsafe { (buf.add(offset) as *const $nextT).read() };
                offset += core::mem::size_of::<$nextT>();
                let _ = offset;
                ($($a,)* $nextA,)
            }
        }

        impl<$($T: CommData,)* $nextT: CommData, R: Runtime + ?Sized>
            MethodArgsPtrTuple<R> for ($($T,)* $nextT,)
        {
            type PtrTuple = (
                $( ZeroCopyArgs<<R::MethodInArgAllocator as MethodInArgAllocator>::MethodInArgPtr<$T>>, )*
                ZeroCopyArgs<<R::MethodInArgAllocator as MethodInArgAllocator>::MethodInArgPtr<$nextT>>,
            );
        }

        impl<$($T: CommData,)* $nextT: CommData, _Alloc: MethodInArgAllocator>
            MethodArgsAllocate<_Alloc> for ($($T,)* $nextT,)
        {
            type UninitTuple = (
                $( _Alloc::MethodInArgMaybeUninit<$T>, )*
                _Alloc::MethodInArgMaybeUninit<$nextT>,
            );

            fn alloc_uninit(allocator: &_Alloc) -> Self::UninitTuple {
                ($( allocator.allocate::<$T>(), )* allocator.allocate::<$nextT>(),)
            }
        }

        // Accepts a tuple of `ZeroCopyArgs<Alloc::MethodInArgPtr<T>>` values and dispatches to
        // `invoke_zero_copy`. The `Ptr = Self::MethodInArgPtr<T>` constraint on MethodInArgAllocator
        // ensures these types unify with what `write()` returns.
        // The copy path is covered by the arity-agnostic blanket impl in `method_concept.rs`.
        impl<$($T: CommData,)* $nextT: CommData, Return: CommData, R: Runtime + ?Sized>
            MethodCallInput<($($T,)* $nextT,), Return, R>
            for ($( ZeroCopyArgs<<R::MethodInArgAllocator as MethodInArgAllocator>::MethodInArgPtr<$T>>, )* ZeroCopyArgs<<R::MethodInArgAllocator as MethodInArgAllocator>::MethodInArgPtr<$nextT>>,)
        where
            R::MethodCaller<($($T,)* $nextT,), Return>:
                MethodCaller<($($T,)* $nextT,), Return, R>,
            // Equality constraint: lets the compiler unify Self with PtrTuple.
            ($($T,)* $nextT,): MethodArgsPtrTuple<R, PtrTuple =
                ($( ZeroCopyArgs<<R::MethodInArgAllocator as MethodInArgAllocator>::MethodInArgPtr<$T>>, )* ZeroCopyArgs<<R::MethodInArgAllocator as MethodInArgAllocator>::MethodInArgPtr<$nextT>>,)
            >,
        {
            fn invoke<'a>(
                self,
                caller: &'a R::MethodCaller<($($T,)* $nextT,), Return>,
            ) -> impl Future<Output = Result<R::MethodReturnSample<Return>>> + 'a
            where
                R::MethodCaller<($($T,)* $nextT,), Return>:
                    MethodCaller<($($T,)* $nextT,), Return, R> + 'a,
            {
                // `self` IS <Args as MethodArgsPtrTuple<R>>::PtrTuple by the equality
                // constraint above, so pass it directly to invoke_zero_copy.
                <R::MethodCaller<($($T,)* $nextT,), Return> as
                    MethodCaller<($($T,)* $nextT,), Return, R>>::invoke_zero_copy(
                    caller,
                    self,
                )
            }
        }
        impl<F, $($T,)* $nextT, Return> MethodHandlerCall<($($T,)* $nextT,), Return> for F
        where
            F: Fn($(&$T,)* &$nextT,) -> Return + Send + Sync + 'static,
        {
            fn call(&self, args: &($($T,)* $nextT,)) -> Return {
                #[allow(non_snake_case)]
                let ($($a,)* $nextA,) = args;
                (self)($($a,)* $nextA,)
            }
        }

        impl_all_arities!(
            @step [$($T,)* $nextT] [$($a,)* $nextA]
            [$( ($restT, $restA) ),*]
        );
    };
}

// Single configuration point
//
// To raise the maximum method argument count:
//    Add one more `(TypeIdent, arg_ident)` pair below.
//
// `_gen_method_wrapper!` (`interface_macros.rs`) has no fixed arity limit of its own,
// so raising the limit here is the only change needed.
//
// Current limit: 8 arguments.
impl_all_arities!(
    (T1, a0),
    (T2, a1),
    (T3, a2),
    (T4, a3),
    (T5, a4),
    (T6, a5),
    (T7, a6),
    (T8, a7),
);
