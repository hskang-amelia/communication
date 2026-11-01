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

//! The Calc interface of the cross-process Method/Field test: `add` is a method, `counter` a
//! field with getter, setter and notifier. `calc_gen.h`/`calc_gen.cpp` are its C++ side.

// `FieldPublisher` must be in scope for the field code `interface!` generates (it calls the
// trait's methods unqualified).
use score_com::{interface, CommData, FieldPublisher, ProviderInfo, Publisher, Reloc, Subscriber};

#[derive(Debug, Clone, Copy, PartialEq, Eq, Reloc, CommData)]
#[repr(C)]
#[comm_data(id = "Operands")]
pub struct Operands {
    pub a: i32,
    pub b: i32,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Reloc, CommData)]
#[repr(C)]
#[comm_data(id = "Total")]
pub struct Total {
    pub value: i32,
}

interface!(
    interface Calc {
        Id = "CalcInterface",
        counter: Field<Total, WithGetter + WithSetter + WithNotifier>,
        add(Operands) -> Total,
    }
);
