//! Adapters: the only layer allowed to use `unsafe`. Each module exposes a safe interface.
#![allow(unsafe_code)]
pub mod driven;
pub mod driving;
