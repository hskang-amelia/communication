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

//! Cross-process checks of the Rust Method/Field support on the LoLa runtime.
//!
//! `rust_method_lola_e2e producer <config>` offers `CalcInterface` and serves until killed.
//! `rust_method_lola_e2e consumer <config> <scenario>` runs one scenario against it and exits 0
//! only if every step gave the right answer. A watchdog ends the process with exit code 3 when a
//! step hangs, so a call that never returns is reported as a failure instead of stalling the run.
//! `run_test.sh` starts the processes for each scenario.

use std::path::{Path, PathBuf};
use std::process::ExitCode;
use std::sync::mpsc;
use std::sync::Mutex;
use std::time::{Duration, Instant};

use futures::executor::block_on;
use rust_method_lola_e2e_gen::{CalcConsumer, CalcInterface, Operands, Total, Wide};
use score_com::{
    Builder, FieldPublisher, FindServiceSpecifier, InstanceSpecifier, LolaRuntimeBuilderImpl, LolaRuntimeImpl,
    Runtime, RuntimeBuilder, SampleContainer, ServiceDiscovery, Subscriber, Subscription,
};

const INSTANCE: &str = "/Test/Calc/Instance";
const INITIAL: Total = Total { value: 7 };
const SLOW_MS: u64 = 300;

// ---------------------------------------------------------------- watchdog

static STEP: Mutex<Option<(String, Instant)>> = Mutex::new(None);

/// Marks the start of a step that must finish within `limit`.
fn step(name: &str, limit: Duration) {
    println!("STEP {name}");
    if let Ok(mut s) = STEP.lock() {
        *s = Some((name.to_owned(), Instant::now() + limit));
    }
}

fn start_watchdog() {
    std::thread::spawn(|| loop {
        std::thread::sleep(Duration::from_millis(100));
        let expired = STEP
            .lock()
            .ok()
            .and_then(|s| s.as_ref().filter(|(_, d)| Instant::now() > *d).map(|(n, _)| n.clone()));
        if let Some(name) = expired {
            println!("HANG at step {name}");
            println!("RESULT: FAIL");
            std::process::exit(3);
        }
    });
}

// ---------------------------------------------------------------- shared

fn runtime(config: &Path) -> LolaRuntimeImpl {
    let mut builder = LolaRuntimeBuilderImpl::new();
    builder.load_config(config);
    builder.build().expect("runtime")
}

fn specifier() -> InstanceSpecifier {
    InstanceSpecifier::new(INSTANCE).expect("specifier")
}

fn wide(seed: u8) -> Wide {
    Wide {
        a: seed,
        b: 0x0102_0304_0506_0708 ^ u64::from(seed),
        c: 0xBEEF ^ u16::from(seed),
        d: f64::from(seed) + 0.25,
    }
}

/// What `mix` returns: every member combined, so a member at a wrong offset shows.
fn mixed(x: Wide, y: Wide) -> Wide {
    Wide {
        a: x.a.wrapping_add(y.a),
        b: x.b ^ y.b.rotate_left(8),
        c: x.c.wrapping_add(y.c),
        d: x.d * 2.0 + y.d,
    }
}

// ---------------------------------------------------------------- producer

fn producer(config: &Path) -> ExitCode {
    let rt = runtime(config);
    let producer = rt.producer_builder::<CalcInterface>(specifier()).build().expect("producer");
    // reset(v) asks the main loop to publish v as the field's new value.
    let (to_main, resets) = mpsc::channel::<Total>();
    let to_main = Mutex::new(to_main);
    let offered = producer
        .init()
        .register_set_handler_counter(|v: Total| {
            println!("PRODUCER set counter {}", v.value);
            v
        })
        .update_counter(INITIAL)
        .expect("initial value")
        // Method handlers get their arguments by reference.
        .register_add_handler(|ops: &Operands| Total { value: ops.a.wrapping_add(ops.b) })
        .register_mix_handler(|x: &Wide, y: &Wide| mixed(*x, *y))
        .register_ping_handler(|| Total { value: 4242 })
        .register_reset_handler(move |v: &Total| {
            if let Ok(tx) = to_main.lock() {
                let _ = tx.send(*v);
            }
        })
        .register_slow_handler(|v: &Total| {
            std::thread::sleep(Duration::from_millis(SLOW_MS));
            Total { value: v.value + 1 }
        })
        .offer()
        .expect("offer");
    println!("PRODUCER offered");
    for v in resets {
        match offered.counter.update(v) {
            Ok(()) => println!("PRODUCER counter updated to {}", v.value),
            Err(e) => println!("PRODUCER counter update failed: {e:?}"),
        }
    }
    ExitCode::SUCCESS
}

// ---------------------------------------------------------------- consumer

type Consumer = CalcConsumer<LolaRuntimeImpl>;

/// Finds the service and builds a consumer, or `None` when it is not found within `within`.
fn connect(rt: &LolaRuntimeImpl, within: Duration) -> Option<Consumer> {
    let deadline = Instant::now() + within;
    loop {
        match rt
            .find_service::<CalcInterface>(FindServiceSpecifier::Specific(specifier()))
            .get_available_instances()
        {
            Ok(found) => {
                if let Some(b) = found.into_iter().next() {
                    match b.build() {
                        Ok(c) => return Some(c),
                        Err(e) => {
                            println!("consumer build failed: {e:?}");
                            return None;
                        }
                    }
                }
            }
            Err(e) => println!("discovery error: {e:?}"),
        }
        if Instant::now() > deadline {
            return None;
        }
        std::thread::sleep(Duration::from_millis(100));
    }
}

struct Checks(bool);

impl Checks {
    fn check(&mut self, what: &str, ok: bool, detail: String) {
        if ok {
            println!("OK {what}");
        } else {
            println!("FAIL {what}: {detail}");
            self.0 = false;
        }
    }

    fn finish(self) -> ExitCode {
        if self.0 {
            println!("RESULT: PASS");
            ExitCode::SUCCESS
        } else {
            println!("RESULT: FAIL");
            ExitCode::FAILURE
        }
    }
}

fn add(c: &Consumer, a: i32, b: i32) -> Result<i32, String> {
    block_on(c.add(Operands { a, b }))
        .map(|s| s.value)
        .map_err(|e| format!("{e:?}"))
}

fn get(c: &Consumer) -> Result<i32, String> {
    block_on(c.get_counter()).map(|s| s.value).map_err(|e| format!("{e:?}"))
}

fn scenario_basic(c: &Consumer, k: &mut Checks) {
    let t = Duration::from_secs(5);
    step("add", t);
    let r = add(c, 2, 40);
    k.check("method add(2,40) = 42", r == Ok(42), format!("{r:?}"));
    step("add negative", t);
    let r = add(c, -5, 3);
    k.check("method add(-5,3) = -2", r == Ok(-2), format!("{r:?}"));
    step("ping", t);
    let r = block_on(c.ping()).map(|s| s.value).map_err(|e| format!("{e:?}"));
    k.check("method with no arguments", r == Ok(4242), format!("{r:?}"));
    step("mix", t);
    let (x, y) = (wide(3), wide(200));
    let r = block_on(c.mix(x, y)).map(|s| *s).map_err(|e| format!("{e:?}"));
    k.check(
        "two arguments with mixed alignment",
        r.as_ref().ok() == Some(&mixed(x, y)),
        format!("{r:?} expected {:?}", mixed(x, y)),
    );
    step("get initial", t);
    let r = get(c);
    k.check("field get returns the initial value", r == Ok(INITIAL.value), format!("{r:?}"));
    step("set", t);
    let r = block_on(c.set_counter(Total { value: 9 })).map(|s| s.value).map_err(|e| format!("{e:?}"));
    k.check("field set returns the confirmed value", r == Ok(9), format!("{r:?}"));
    step("get after set", t);
    let r = get(c);
    k.check("field get after set returns the set value", r == Ok(9), format!("{r:?}"));
    step("reset (void method)", t);
    let r = block_on(c.reset(Total { value: 55 })).map(|_| ()).map_err(|e| format!("{e:?}"));
    k.check("void method returns", r.is_ok(), format!("{r:?}"));
    step("get after producer update", Duration::from_secs(5));
    let deadline = Instant::now() + Duration::from_secs(3);
    let mut last = get(c);
    while last != Ok(55) && Instant::now() < deadline {
        std::thread::sleep(Duration::from_millis(50));
        last = get(c);
    }
    k.check("field get sees the producer's update", last == Ok(55), format!("{last:?}"));
}

fn scenario_notify(rt: &LolaRuntimeImpl, c: Consumer, k: &mut Checks) {
    let t = Duration::from_secs(5);
    step("subscribe", t);
    let subscription = match c.counter.subscribe(4) {
        Ok(s) => s,
        Err(e) => {
            k.check("subscribe to the field", false, format!("{e:?}"));
            return;
        }
    };
    println!("OK subscribe to the field");
    // `subscribe` consumed the field member, so a second consumer of the same runtime calls.
    step("second consumer", Duration::from_secs(10));
    let Some(caller) = connect(rt, Duration::from_secs(5)) else {
        k.check("second consumer", false, "not found".into());
        return;
    };
    step("reset 77", t);
    let r = block_on(caller.reset(Total { value: 77 })).map(|_| ());
    k.check("void method returns", r.is_ok(), format!("{r:?}"));
    step("notification", Duration::from_secs(6));
    let deadline = Instant::now() + Duration::from_secs(4);
    let mut seen = Vec::new();
    while Instant::now() < deadline && !seen.contains(&77) {
        let mut buf = SampleContainer::new(4);
        match subscription.try_receive(&mut buf, 4) {
            Ok(_) => {
                while let Some(s) = buf.pop_front() {
                    seen.push(s.value);
                }
            }
            Err(e) => {
                k.check("try_receive", false, format!("{e:?}"));
                return;
            }
        }
        std::thread::sleep(Duration::from_millis(50));
    }
    k.check("subscriber is notified of the update", seen.contains(&77), format!("received {seen:?}"));
}

fn scenario_seq(c: &Consumer, k: &mut Checks) {
    step("2000 sequential calls", Duration::from_secs(60));
    let mut wrong = 0;
    let mut first_err = None;
    for i in 0..2000 {
        match add(c, i, i * 3) {
            Ok(v) if v == i * 4 => {}
            other => {
                wrong += 1;
                first_err.get_or_insert(format!("call {i}: {other:?}"));
            }
        }
    }
    k.check("2000 sequential calls all right", wrong == 0, format!("{wrong} wrong, first {first_err:?}"));
}

fn scenario_slow1(c: &Consumer, k: &mut Checks) {
    step("slow call", Duration::from_millis(SLOW_MS * 20));
    match block_on(c.slow(Total { value: 1 })) {
        Ok(s) => k.check("slow call", s.value == 2, format!("returned {}", s.value)),
        // Refused because the queue is full: a defined answer, not a failure.
        Err(e) => println!("DEFINED-ERROR slow call refused: {e:?}"),
    }
}

/// Waits until `file` exists (written by run_test.sh).
fn wait_for(file: &Path, limit: Duration) -> bool {
    let deadline = Instant::now() + limit;
    while !file.exists() {
        if Instant::now() > deadline {
            return false;
        }
        std::thread::sleep(Duration::from_millis(50));
    }
    true
}

fn scenario_crash(rt: &LolaRuntimeImpl, c: &Consumer, k: &mut Checks) {
    let dir = PathBuf::from(std::env::var("E2E_SYNC_DIR").unwrap_or_else(|_| ".".into()));
    step("before kill", Duration::from_secs(5));
    let r = add(c, 1, 1);
    k.check("call before the producer dies", r == Ok(2), format!("{r:?}"));
    println!("READY_FOR_KILL");
    let _ = std::fs::write(dir.join("ready"), b"");
    step("wait for kill", Duration::from_secs(30));
    if !wait_for(&dir.join("killed"), Duration::from_secs(25)) {
        k.check("producer killed by the driver", false, "no signal".into());
        return;
    }
    step("call after kill", Duration::from_secs(10));
    let r = add(c, 1, 1);
    k.check("call to a dead producer returns an error", r.is_err(), format!("{r:?}"));
    step("wait for restart", Duration::from_secs(30));
    if !wait_for(&dir.join("restarted"), Duration::from_secs(25)) {
        k.check("producer restarted by the driver", false, "no signal".into());
        return;
    }
    step("rediscover", Duration::from_secs(20));
    let Some(fresh) = connect(rt, Duration::from_secs(15)) else {
        k.check("rediscover the restarted producer", false, "not found".into());
        return;
    };
    println!("OK rediscover the restarted producer");
    step("call restarted producer", Duration::from_secs(5));
    let r = add(&fresh, 20, 22);
    k.check("call to the restarted producer", r == Ok(42), format!("{r:?}"));
    step("old consumer after restart", Duration::from_secs(10));
    let r = add(c, 1, 1);
    println!("INFO old consumer after restart: {r:?}");
}

fn consumer(config: &Path, scenario: &str) -> ExitCode {
    start_watchdog();
    let rt = runtime(config);
    let mut k = Checks(true);
    let find_within = if scenario == "early" { Duration::from_secs(30) } else { Duration::from_secs(10) };
    step("discovery", find_within + Duration::from_secs(5));
    let Some(c) = connect(&rt, find_within) else {
        if scenario == "denied" {
            println!("OK denied consumer cannot use the service (not found or not built)");
            return k.finish();
        }
        k.check("discovery", false, "service not found".into());
        return k.finish();
    };
    match scenario {
        "basic" | "early" => scenario_basic(&c, &mut k),
        "notify" => scenario_notify(&rt, c, &mut k),
        "seq" => scenario_seq(&c, &mut k),
        "slow1" => scenario_slow1(&c, &mut k),
        "crash" => scenario_crash(&rt, &c, &mut k),
        "denied" => {
            step("denied call", Duration::from_secs(10));
            let r = add(&c, 1, 2);
            k.check("denied consumer's call does not succeed", r.is_err(), format!("{r:?}"));
        }
        other => k.check("scenario", false, format!("unknown {other}")),
    }
    if let Ok(mut s) = STEP.lock() {
        *s = None;
    }
    k.finish()
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().collect();
    match (args.get(1).map(String::as_str), args.get(2)) {
        (Some("producer"), Some(config)) => producer(Path::new(config)),
        (Some("consumer"), Some(config)) => {
            consumer(Path::new(config), args.get(3).map_or("basic", String::as_str))
        }
        _ => {
            eprintln!("usage: rust_method_lola_e2e producer <config> | consumer <config> <scenario>");
            ExitCode::FAILURE
        }
    }
}
