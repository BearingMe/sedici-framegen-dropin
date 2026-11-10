//! Sedici: a drop-in replacement for `amd_fidelityfx_framegeneration_dx12.dll`.
//!
//! Hexagonal layout (enforced by `tests/architecture.rs`):
//! - [`domain`]: pure rules, no I/O, no OS types.
//! - [`application`]: use cases and the ports (traits) they need; depends on `domain` only.
//! - [`adapters`]: everything that touches Windows, COM, the GPU, files or the game's ABI. This is
//!   the only place `unsafe` is allowed; each adapter keeps it behind a safe interface.
//! - [`entry`]: the composition root that wires adapters into the application.
#![deny(unsafe_code)]

pub mod adapters;
pub mod application;
pub mod domain;
pub mod entry;

/// The byte string `tools/deploy.ps1` and `tools/restore.ps1` look for to recognise a Sedici build.
///
/// It is data in the image, never an export: the DLL must export exactly the five `ffx*` entry
/// points (AGENTS.md). The C++ defined it in `dllmain.cpp`; the Rust build keeps it as a retained
/// static that [`entry::ensure_started`] reads once, so the linker cannot drop it under LTO.
#[used]
pub static SEDICI_PROXY_MARKER: [u8; 15] = *b"SEDICI_PROXY_V1";
