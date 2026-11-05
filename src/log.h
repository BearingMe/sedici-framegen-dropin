#pragma once

#include <filesystem>
#include <string_view>

// Directory containing this DLL. Used for the .orig lookup and the log file.
std::filesystem::path dll_directory();

// MFSRFG_LOG: 0 = errors only, 1 = normal (default), 2 = verbose.
int log_level() noexcept;

// Always written, regardless of the configured level.
void log_error(std::string_view message) noexcept;

// Written when log_level() >= level.
void log_event(int level, std::string_view message) noexcept;
