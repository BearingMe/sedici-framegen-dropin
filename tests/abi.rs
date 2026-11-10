//! ABI-level checks: the built DLL exports exactly the five FFX entry points, and the `pNext`
//! chain walker behaves on the inputs a game (or a fuzzer) can hand it.
//!
//! The export check reads the built DLL's PE export table with a small safe parser; the layout
//! asserts are compile-time in `src/adapters/driving/ffx_abi/types.rs`. The walker cases run in
//! process against the same public entry points the game calls.
//!
//! Calling the `unsafe extern "system"` exports is the whole point of the chain cases; the
//! crate's `unsafe_code` deny is aimed at production code, and every call below says why it is
//! sound.
#![allow(unsafe_code)]

use std::path::{Path, PathBuf};

/// The five exports, in `exports.def` order.
const EXPORTS: [&str; 5] = [
    "ffxCreateContext",
    "ffxDestroyContext",
    "ffxConfigure",
    "ffxQuery",
    "ffxDispatch",
];

fn dll_path() -> PathBuf {
    // The test binary lives in `target/<profile>/deps`; the cdylib lands one directory up under
    // the crate name.
    let profile = std::env::current_exe()
        .expect("test executable path")
        .parent()
        .and_then(Path::parent)
        .expect("target profile directory")
        .to_path_buf();
    profile.join("amd_fidelityfx_framegeneration_dx12.dll")
}

/// Reads the names in a PE image's export directory. Safe: every offset is bounds-checked against
/// the file bytes, and a malformed image returns an empty list rather than panicking.
fn read_exports(bytes: &[u8]) -> Vec<String> {
    fn u16_at(bytes: &[u8], offset: usize) -> Option<u16> {
        bytes.get(offset..offset + 2).map(|s| u16::from_le_bytes([s[0], s[1]]))
    }
    fn u32_at(bytes: &[u8], offset: usize) -> Option<u32> {
        bytes.get(offset..offset + 4).map(|s| u32::from_le_bytes([s[0], s[1], s[2], s[3]]))
    }
    fn name_at(bytes: &[u8], offset: usize) -> Option<String> {
        let rest = bytes.get(offset..)?;
        let end = rest.iter().position(|&b| b == 0)?;
        std::str::from_utf8(&rest[..end]).ok().map(str::to_owned)
    }

    let mut out = Vec::new();
    // DOS header: e_lfanew at 0x3c.
    let Some(pe) = u32_at(bytes, 0x3c).map(|v| v as usize) else { return out };
    if bytes.get(pe..pe + 4) != Some(b"PE\0\0") {
        return out;
    }
    let coff = pe + 4;
    let Some(sections) = u16_at(bytes, coff + 2).map(usize::from) else { return out };
    let Some(optional_header_size) = u16_at(bytes, coff + 16).map(usize::from) else { return out };
    // The optional header follows the 20-byte COFF header; it is not a pointed-to offset.
    let optional = coff + 20;
    // PE32+ (magic 0x20b) keeps the export directory at data-directory index 0; the directory
    // array starts 112 bytes into the optional header.
    let magic = u16_at(bytes, optional).unwrap_or(0);
    let directories = if magic == 0x20b { optional + 112 } else { optional + 96 };
    let Some(export_rva) = u32_at(bytes, directories) else { return out };
    let Some(export_size) = u32_at(bytes, directories + 4) else { return out };
    if export_rva == 0 || export_size == 0 {
        return out;
    }

    // Map an RVA to a file offset through the section table.
    let section_table = optional + optional_header_size;
    let mut export_offset = None;
    for index in 0..sections {
        let section = section_table + index * 40;
        let Some(virtual_size) = u32_at(bytes, section + 8) else { return out };
        let Some(virtual_address) = u32_at(bytes, section + 12) else { return out };
        let Some(raw_size) = u32_at(bytes, section + 16) else { return out };
        let Some(raw_pointer) = u32_at(bytes, section + 20) else { return out };
        let span = virtual_size.max(raw_size);
        let start = virtual_address;
        if export_rva >= start && export_rva < start.saturating_add(span) {
            export_offset = Some((raw_pointer as usize) + (export_rva - start) as usize);
            break;
        }
    }
    let Some(directory) = export_offset else { return out };

    let Some(name_count) = u32_at(bytes, directory + 24) else { return out };
    let Some(names_rva) = u32_at(bytes, directory + 32) else { return out };
    let Some(names) = map_rva(bytes, sections, section_table, names_rva) else { return out };
    for index in 0..name_count as usize {
        let Some(name_rva) = u32_at(bytes, names + index * 4).map(|v| v as usize) else { break };
        let Some(offset) = map_rva(bytes, sections, section_table, name_rva as u32) else { continue };
        if let Some(name) = name_at(bytes, offset) {
            out.push(name);
        }
    }
    out
}

fn map_rva(bytes: &[u8], sections: usize, section_table: usize, rva: u32) -> Option<usize> {
    for index in 0..sections {
        let section = section_table + index * 40;
        let field = |offset: usize| {
            bytes
                .get(section + offset..section + offset + 4)
                .map(|s| u32::from_le_bytes([s[0], s[1], s[2], s[3]]))
        };
        let (virtual_size, virtual_address, raw_size, raw_pointer) =
            (field(8)?, field(12)?, field(16)?, field(20)?);
        let span = virtual_size.max(raw_size);
        if rva >= virtual_address && rva < virtual_address.saturating_add(span) {
            return Some((raw_pointer as usize) + (rva - virtual_address) as usize);
        }
    }
    None
}

#[test]
fn the_built_dll_exports_exactly_the_five_entry_points() {
    let path = dll_path();
    if !path.exists() {
        // `cargo test` may skip the cdylib on a fresh profile; `cargo build` produces it first.
        eprintln!("skipping: {} is not built yet", path.display());
        return;
    }
    let bytes = std::fs::read(&path).expect("read the built DLL");
    let mut exports = read_exports(&bytes);
    exports.sort();
    let mut expected: Vec<String> = EXPORTS.iter().map(|name| (*name).to_owned()).collect();
    expected.sort();
    assert_eq!(exports, expected, "the DLL must export exactly the five ffx entry points");
}

#[test]
fn exports_def_names_the_same_five() {
    let def = std::fs::read_to_string(concat!(env!("CARGO_MANIFEST_DIR"), "/exports.def"))
        .expect("read exports.def");
    let mut named: Vec<String> = def
        .lines()
        .map(str::trim)
        .filter(|line| !line.is_empty() && !line.starts_with(';') && !line.starts_with("LIBRARY") && !line.starts_with("EXPORTS"))
        .map(|line| line.split_whitespace().next().unwrap_or("").to_owned())
        .collect();
    named.sort();
    let mut expected: Vec<String> = EXPORTS.iter().map(|name| (*name).to_owned()).collect();
    expected.sort();
    assert_eq!(named, expected);
}

// -------------------------------------------------------------------------------------------------
// pNext chain cases, through the real exports
// -------------------------------------------------------------------------------------------------

/// Test-local mirror of `ffxApiHeader`. The export only reads `type` and `pNext`, and the raw
/// descriptor types are private to the adapter, so a plain two-field struct is enough to build
/// the chains a game builds.
#[repr(C)]
struct Header {
    type_: u64,
    p_next: *const Header,
}

const UNKNOWN_TYPE: u64 = 0x7fff_fffe;
const FG_CREATE: u64 = 0x0002_0001;

type ContextSlot = *mut *mut std::ffi::c_void;

use amd_fidelityfx_framegeneration_dx12::adapters::driving::ffx_abi::exports::{
    ffxConfigure, ffxCreateContext, ffxDestroyContext, ffxDispatch, ffxQuery,
};

/// A token value the router never issued: with no service installed, every call on it is a
/// documented error, which still runs the whole chain walk.
fn unissued_context() -> usize {
    0x10
}

fn as_desc(header: *const Header) -> *const amd_fidelityfx_framegeneration_dx12::adapters::driving::ffx_abi::types::ffxApiHeader {
    header.cast()
}

fn as_desc_mut(header: *const Header) -> *mut amd_fidelityfx_framegeneration_dx12::adapters::driving::ffx_abi::types::ffxApiHeader {
    header.cast_mut().cast()
}

#[rstest::rstest]
#[case::null_descriptor(1)]
#[case::cyclic_chain(2)]
#[case::unknown_type(3)]
#[allow(unused_assignments)] // the cycle is built by patching a node the walk will follow
fn chain_cases_never_crash_and_answer_a_documented_code(#[case] scenario: u8) {
    // A token Sedici never issued: the service is absent, so the walk runs and the answer is the
    // documented local error for each call.
    let mut token = unissued_context();
    let context = &mut token as *mut usize as ContextSlot;

    match scenario {
        1 => {
            // SAFETY: the context slot is owned by this test; the descriptor is null, which every
            // export checks before touching game memory.
            let code = unsafe { ffxConfigure(context, std::ptr::null()) };
            assert_eq!(code, 1, "a null configure desc on a Sedici context is FFX_API_RETURN_ERROR");
        }
        2 => {
            // Two headers pointing at each other: the walk must stop at the first repeat.
            let mut tail = Header { type_: UNKNOWN_TYPE, p_next: std::ptr::null() };
            let head = Header { type_: UNKNOWN_TYPE, p_next: &tail };
            tail.p_next = &head;
            assert_eq!(tail.p_next, &head as *const Header); // the cycle exists before the call
            // SAFETY: both headers live in this frame for the whole call, and the export only
            // reads them.
            let code = unsafe { ffxDispatch(context, as_desc(&head)) };
            assert_eq!(code, 1, "a dispatch on an unissued context is FFX_API_RETURN_ERROR");
        }
        _ => {
            let chain = Header { type_: UNKNOWN_TYPE, p_next: std::ptr::null() };
            let mut handle: *mut std::ffi::c_void = std::ptr::null_mut();
            // SAFETY: `handle` is owned and writable; the chain is one live readable header.
            let code = unsafe { ffxCreateContext(&mut handle, as_desc_mut(&chain), std::ptr::null()) };
            assert_eq!(code, 2, "an unrecognised create chain is FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE");
            assert!(handle.is_null());
        }
    }
}

#[rstest::rstest]
#[case::create_on_null(1)]
#[case::query_on_null(2)]
fn null_handles_are_parameter_errors(#[case] scenario: u8) {
    let context: ContextSlot = std::ptr::null_mut();
    let chain = Header { type_: FG_CREATE, p_next: std::ptr::null() };
    // SAFETY: a null context and one readable header; the exports reject before any game write.
    unsafe {
        match scenario {
            1 => {
                assert_eq!(ffxCreateContext(context, as_desc_mut(&chain), std::ptr::null()), 6);
                assert_eq!(ffxDestroyContext(context, std::ptr::null()), 6);
            }
            _ => assert_eq!(ffxQuery(context, as_desc_mut(&chain)), 6),
        }
    }
}

#[test]
fn a_recognised_create_chain_is_a_local_error_until_the_router_is_installed() {
    // The chain names frame generation with the exact descriptor shape the SDK declares, so the
    // answer is FFX_API_RETURN_ERROR (nothing can be created yet), never UNKNOWN_DESCTYPE.
    #[repr(C)]
    struct FgCreate {
        header: Header,
        flags: u32,
        display: [u32; 2],
        max_render: [u32; 2],
        format: u32,
    }
    let chain = FgCreate {
        header: Header { type_: FG_CREATE, p_next: std::ptr::null() },
        flags: 1 << 5,
        display: [1920, 1080],
        max_render: [1280, 720],
        format: 14,
    };
    let mut handle: *mut std::ffi::c_void = std::ptr::null_mut();
    // SAFETY: `handle` is owned and writable and `chain` is a live, correctly shaped descriptor.
    let code = unsafe { ffxCreateContext(&mut handle, as_desc_mut(&chain.header), std::ptr::null()) };
    assert_eq!(code, 1);
    assert!(handle.is_null());
}
