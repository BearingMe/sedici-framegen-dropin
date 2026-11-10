//! Pure rules. Nothing here may import `windows`, `std::fs`, `std::time::Instant` or an adapter.
pub mod config;
pub mod error;
pub mod frame;
pub mod gpu_policy;
pub mod pacing;
pub mod schedule;
pub mod timing;
pub mod token;
pub mod ui_logic;
