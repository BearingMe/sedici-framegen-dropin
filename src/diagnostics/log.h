#pragma once

#include <cstdint>
#include <filesystem>
#include <string_view>

// Directory containing this DLL. Used for the config and log files.
std::filesystem::path dll_directory();

// MFSRFG_LOG (legacy) or `[log] level` in sedici.toml:
// 0 = errors only, 1 = normal (default), 2 = verbose.
int log_level() noexcept;

// The settings layer publishes the live log level here whenever it changes, so
// diagnostics never has to read settings (P4.5).
void log_publish_level(int level) noexcept;

// Every function below only copies the message into a fixed ring and wakes a
// writer thread: no file I/O, no allocation and no unbounded wait on the caller's
// thread, so a slow or failing disk cannot stall a Present (P2.10). A full ring
// drops the line and counts it. Repeats of one message are rate limited, and the
// next line that gets through says how many were suppressed.

// Written regardless of the configured level.
void log_error(std::string_view message) noexcept;

// Written regardless of the level and never rate limited, and waits (bounded) for
// the line to reach the file. Used by the configuration loader, which must be able
// to report problems while the settings that carry the log level are still being
// built, and runs once at load, not on an output path.
void log_raw(std::string_view message) noexcept;

// Written when log_level() >= level.
void log_event(int level, std::string_view message) noexcept;

namespace sedici {

// What the log itself did. Counters only grow; `depth` is the current queue size.
struct LogStats {
    uint64_t queued = 0;        // lines accepted into the ring
    uint64_t written = 0;       // lines that reached sedici.log
    uint64_t dropped = 0;       // refused because the ring was full
    uint64_t suppressed = 0;    // refused by the per-message rate limit
    uint64_t lost = 0;          // accepted but discarded because the file could not be written
    uint64_t sink_failures = 0; // failed file writes (one per batch)
    uint32_t depth = 0;
    uint32_t max_depth = 0;
    bool sink_ok = true; // false from a failed write until the next success
};

LogStats log_stats() noexcept;

// Waits until the ring is empty and the writer is idle. False on timeout.
bool log_flush(unsigned timeout_ms) noexcept;

// Process exit only (DllMain, lpReserved != nullptr): the writer thread is already
// gone, so write what is still queued on this thread. Never waits on a lock.
void log_drain_at_exit() noexcept;

// Offline test seams. Never called by the DLL.
enum class LogSinkFault { None, FailOpen, Slow };
void log_test_set_directory(const std::filesystem::path& directory);
void log_test_set_sink_fault(LogSinkFault fault, unsigned delay_ms) noexcept;
void log_test_reset_rate_limits() noexcept;

} // namespace sedici
