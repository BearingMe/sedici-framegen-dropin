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
