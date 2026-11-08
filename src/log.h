#pragma once

#include <filesystem>
#include <string_view>

// Directory containing this DLL. Used for the config and log files.
std::filesystem::path dll_directory();

// MFSRFG_LOG (legacy) or `[log] level` in sedici.toml:
// 0 = errors only, 1 = normal (default), 2 = verbose.
int log_level() noexcept;

// Always written, regardless of the configured level.
void log_error(std::string_view message) noexcept;

// Always written and never consults log_level(). Used by the configuration
// loader, which must be able to report problems while the settings that carry
// the log level are still being built.
void log_raw(std::string_view message) noexcept;

// Written when log_level() >= level.
void log_event(int level, std::string_view message) noexcept;
