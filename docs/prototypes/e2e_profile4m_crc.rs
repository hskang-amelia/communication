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

// Standalone prototype: an E2E Profile 4m-shaped header protect/check, for
// eclipse-score/communication#1062 ("Improvement: E2E protection for Rust Method/Field APIs").
// See docs/design-notes.md §3 for context, and §3.0 for how the specifications are drawn on. This
// file is written from the protocol's field layout; everything below is its own.
//
// NOT part of any Bazel or Cargo build target - this repo's Rust code is Bazel-only, and this fork's
// sandbox at the time this was written could build C++ via Bazel but not Rust (the `ferrocene` Rust
// toolchain needs a newer glibc than the sandbox has). Verified instead with a bare `rustc`, outside
// any build system:
//     rustc -O e2e_profile4m_crc.rs -o e2e_profile4m_crc && ./e2e_profile4m_crc
//
// CRC parameters: width=32, poly=0xF4ACFB13, init=0xFFFFFFFF, refin=true, refout=true,
// xorout=0xFFFFFFFF - the publicly documented "CRC-32/AUTOSAR" variant. `FO_PRS_E2EProtocol` states
// only the polynomial and defers the rest to a separate CRC library document, so the implementation
// is validated against that variant's own public catalog check value instead (input ASCII
// "123456789" -> 0x1697d06a), the standard cross-implementation sanity check for it.

const POLY: u32 = 0xF4AC_FB13;
const INIT: u32 = 0xFFFF_FFFF;
const XOROUT: u32 = 0xFFFF_FFFF;

fn crc32_autosar(data: &[u8]) -> u32 {
    let mut crc = INIT;
    for &byte in data {
        crc ^= byte as u32;
        for _ in 0..8 {
            crc = if crc & 1 != 0 { (crc >> 1) ^ reflect_poly() } else { crc >> 1 };
        }
    }
    crc ^ XOROUT
}

// Reflected form of POLY, precomputed once for the refin=true/refout=true (LSB-first) table-free algorithm.
fn reflect_poly() -> u32 {
    let mut v = POLY;
    let mut r = 0u32;
    for _ in 0..32 {
        r = (r << 1) | (v & 1);
        v >>= 1;
    }
    r
}

/// Profile 4m header (16 bytes total, prototype only):
/// Length(2B) | Counter(2B) | DataID(4B) | CRC(4B) | MsgType(2b)/MsgResult(2b)/SourceID(28b) packed in 4B.
/// Field order and widths follow the Profile 4m layout; the byte packing below is this file's own.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum MessageType {
    Request = 0,
    Response = 1,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum MessageResult {
    Ok = 0,
    Error = 1,
}

struct Profile4mHeader {
    length: u16,
    counter: u16,
    data_id: u32,
    message_type: MessageType,
    message_result: MessageResult,
    source_id: u32, // 28 bits actually used
}

impl Profile4mHeader {
    /// CRC input: the entire header except the CRC field itself, then the user data.
    fn crc_input(&self, payload: &[u8]) -> Vec<u8> {
        let mut buf = Vec::with_capacity(8 + payload.len());
        buf.extend_from_slice(&self.length.to_be_bytes());
        buf.extend_from_slice(&self.counter.to_be_bytes());
        buf.extend_from_slice(&self.data_id.to_be_bytes());
        let type_result_source: u32 = ((self.message_type as u32) << 30)
            | ((self.message_result as u32) << 28)
            | (self.source_id & 0x0FFF_FFFF);
        buf.extend_from_slice(&type_result_source.to_be_bytes());
        buf.extend_from_slice(payload);
        buf
    }
}

#[derive(Debug, PartialEq, Eq)]
enum CheckResult {
    Ok,
    WrongCrc,
    WrongSequence, // counter didn't advance as expected
}

/// "Protect": build the 16-byte header + payload frame a caller/handler would actually send.
fn protect(data_id: u32, counter: u16, msg_type: MessageType, payload: &[u8]) -> Vec<u8> {
    let header = Profile4mHeader {
        length: (12 + payload.len()) as u16, // header bytes excluding the CRC field itself, plus payload
        counter,
        data_id,
        message_type: msg_type,
        message_result: MessageResult::Ok, // fixed OK for requests, per the spec's own note
        source_id: 0x1234567, // stand-in "this caller's id" for the prototype
    };
    let crc = crc32_autosar(&header.crc_input(payload));

    let mut frame = Vec::with_capacity(16 + payload.len());
    frame.extend_from_slice(&header.length.to_be_bytes());
    frame.extend_from_slice(&header.counter.to_be_bytes());
    frame.extend_from_slice(&header.data_id.to_be_bytes());
    frame.extend_from_slice(&crc.to_be_bytes());
    let type_result_source: u32 = ((header.message_type as u32) << 30)
        | ((header.message_result as u32) << 28)
        | (header.source_id & 0x0FFF_FFFF);
    frame.extend_from_slice(&type_result_source.to_be_bytes());
    frame.extend_from_slice(payload);
    frame
}

/// "Check": the counterpart a proxy/skeleton would run on a received frame before handing the payload
/// to user code - mirrors how CP's E2E Transformer runs Check() transparently on receive.
fn check<'a>(frame: &'a [u8], expected_data_id: u32, last_counter: &mut Option<u16>) -> Result<&'a [u8], CheckResult> {
    if frame.len() < 16 {
        return Err(CheckResult::WrongCrc);
    }
    let length = u16::from_be_bytes([frame[0], frame[1]]);
    let counter = u16::from_be_bytes([frame[2], frame[3]]);
    let data_id = u32::from_be_bytes([frame[4], frame[5], frame[6], frame[7]]);
    let received_crc = u32::from_be_bytes([frame[8], frame[9], frame[10], frame[11]]);
    let type_result_source = u32::from_be_bytes([frame[12], frame[13], frame[14], frame[15]]);
    let payload = &frame[16..];

    if data_id != expected_data_id {
        return Err(CheckResult::WrongCrc); // wrong Data ID is also a protection failure
    }

    let header = Profile4mHeader {
        length,
        counter,
        data_id,
        message_type: if (type_result_source >> 30) & 0b11 == 0 { MessageType::Request } else { MessageType::Response },
        message_result: if (type_result_source >> 28) & 0b11 == 0 { MessageResult::Ok } else { MessageResult::Error },
        source_id: type_result_source & 0x0FFF_FFFF,
    };
    let computed_crc = crc32_autosar(&header.crc_input(payload));
    if computed_crc != received_crc {
        return Err(CheckResult::WrongCrc);
    }

    if let Some(prev) = *last_counter {
        if counter != prev.wrapping_add(1) {
            *last_counter = Some(counter);
            return Err(CheckResult::WrongSequence);
        }
    }
    *last_counter = Some(counter);
    Ok(payload)
}

fn main() {
    // --- Sanity check: the public CRC-32/AUTOSAR catalog check value ---
    let catalog_crc = crc32_autosar(b"123456789");
    println!(
        "[catalog check] got {:#010x}  expected 0x1697d06a  match={}",
        catalog_crc,
        catalog_crc == 0x1697d06a
    );

    // --- Now the actual Method-call protect/check round trip this issue is about ---
    let data_id = 0xC0FFEE_u32 & 0x0FFF_FFFF; // stand-in "this method's" id
    let payload = b"hello, method call payload";

    let request_frame = protect(data_id, 0, MessageType::Request, payload);
    let mut last_counter = None;
    match check(&request_frame, data_id, &mut last_counter) {
        Ok(recovered) => println!(
            "[round trip]   ok, recovered payload = {:?} (matches original: {})",
            String::from_utf8_lossy(recovered),
            recovered == payload
        ),
        Err(e) => println!("[round trip]   FAILED: {:?}", e),
    }

    // --- Prove it actually catches corruption, the same way the #767 regression test proved the fix ---
    let mut corrupted = request_frame.clone();
    let last = corrupted.len() - 1;
    corrupted[last] ^= 0xFF; // flip a payload bit
    match check(&corrupted, data_id, &mut last_counter.clone()) {
        Ok(_) => println!("[corruption]   NOT CAUGHT - bug in this prototype"),
        Err(e) => println!("[corruption]   caught as expected: {:?}", e),
    }

    // --- Prove the sequence check catches a replayed/duplicated frame ---
    match check(&request_frame, data_id, &mut last_counter) {
        Ok(_) => println!("[replay]       NOT CAUGHT - counter didn't advance, should have failed"),
        Err(e) => println!("[replay]       caught as expected: {:?}", e),
    }
}
