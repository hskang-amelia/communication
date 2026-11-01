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

//! Cross-process check of the Rust Method/Field support on the LoLa runtime.
//!
//! `rust_method_lola_e2e producer <config>` offers `CalcInterface` and serves until killed.
//! `rust_method_lola_e2e consumer <config>` finds it, calls the method and the field's getter and
//! setter, and exits 0 only if every answer is right. `run_test.sh` starts both.

use std::path::Path;
use std::process::ExitCode;
use std::time::{Duration, Instant};

use futures::executor::block_on;
use rust_method_lola_e2e_gen::{CalcInterface, Operands, Total};
use score_com::{
    Builder, FindServiceSpecifier, InstanceSpecifier, LolaRuntimeBuilderImpl, Runtime, RuntimeBuilder,
    ServiceDiscovery,
};

const INSTANCE: &str = "/Test/Calc/Instance";
const INITIAL: Total = Total { value: 7 };

fn runtime(config: &Path) -> score_com::LolaRuntimeImpl {
    let mut builder = LolaRuntimeBuilderImpl::new();
    builder.load_config(config);
    builder.build().expect("runtime")
}

fn producer(config: &Path) -> ExitCode {
    let rt = runtime(config);
    let producer = rt
        .producer_builder::<CalcInterface>(InstanceSpecifier::new(INSTANCE).expect("specifier"))
        .build()
        .expect("producer");
    let _offered = producer
        .init()
        .register_set_handler_counter(|v: Total| println!("PRODUCER set counter {}", v.value))
        .update_counter(INITIAL)
        .expect("initial value")
        .register_add_handler(|ops: Operands| {
            println!("PRODUCER add {} {}", ops.a, ops.b);
            Total { value: ops.a + ops.b }
        })
        .offer()
        .expect("offer");
    println!("PRODUCER offered");
    loop {
        std::thread::sleep(Duration::from_secs(1));
    }
}

fn consumer(config: &Path) -> ExitCode {
    let rt = runtime(config);
    let deadline = Instant::now() + Duration::from_secs(20);
    let builder = loop {
        let found = rt
            .find_service::<CalcInterface>(FindServiceSpecifier::Specific(
                InstanceSpecifier::new(INSTANCE).expect("specifier"),
            ))
            .get_available_instances()
            .expect("discovery");
        if let Some(b) = found.into_iter().next() {
            break b;
        }
        if Instant::now() > deadline {
            println!("FAIL: service not found");
            return ExitCode::FAILURE;
        }
        std::thread::sleep(Duration::from_millis(200));
    };
    let consumer = builder.build().expect("consumer");
    let mut ok = true;

    match block_on(consumer.add(Operands { a: 2, b: 40 })) {
        Ok(sample) if *sample == (Total { value: 42 }) => println!("OK method add = 42"),
        other => {
            println!("FAIL method add: {other:?}");
            ok = false;
        }
    }
    match block_on(consumer.get_counter()) {
        Ok(sample) if *sample == INITIAL => println!("OK field get = {}", sample.value),
        other => {
            println!("FAIL field get: {other:?}");
            ok = false;
        }
    }
    match block_on(consumer.set_counter(Total { value: 9 })) {
        Ok(sample) => println!("OK field set, confirmed {}", sample.value),
        Err(e) => {
            println!("FAIL field set: {e:?}");
            ok = false;
        }
    }
    if ok {
        println!("RESULT: PASS");
        ExitCode::SUCCESS
    } else {
        println!("RESULT: FAIL");
        ExitCode::FAILURE
    }
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().collect();
    let (Some(mode), Some(config)) = (args.get(1), args.get(2)) else {
        eprintln!("usage: rust_method_lola_e2e producer|consumer <mw_com_config.json>");
        return ExitCode::FAILURE;
    };
    match mode.as_str() {
        "producer" => producer(Path::new(config)),
        "consumer" => consumer(Path::new(config)),
        _ => ExitCode::FAILURE,
    }
}
