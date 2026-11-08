#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>

namespace sedici {

struct ConfigFileStamp {
    // False means the filesystem state could not be determined; never treat an
    // attribute-query error as proof that a file is absent.
    bool valid = false;
    bool exists = false;
    uint64_t last_write = 0;
    uint64_t size = 0;
    uint64_t file_id = 0;
    uint32_t volume_serial = 0;
    std::array<uint8_t, 32> sha256{};
};

ConfigFileStamp config_file_stamp(const std::filesystem::path& path) noexcept;
bool config_same_stamp(const ConfigFileStamp& a, const ConfigFileStamp& b) noexcept;

struct ConfigValues {
    bool write_limiter_fps = false;
    double limiter_fps = 0.0;

    bool write_log = false;
    int log_level = 1;

    bool write_ui_mode = false;
    const char* ui_mode = "off";

    bool write_ui_scale = false;
    bool ui_scale_auto = true;
    float ui_scale = 1.0f;
};

// Patches values in place. Comments, unrelated keys, line endings and
// formatting are preserved. Missing keys are appended to the end of their
// section, and missing sections are appended to the file.
bool config_save(const std::filesystem::path& path, const ConfigValues& values,
                 std::string& error, const ConfigFileStamp* expectedStamp = nullptr,
                 bool* conflict = nullptr, ConfigFileStamp* writtenStamp = nullptr);

// One-shot internal seam for testing an external edit before replacement. It is
// not a DLL export and is never armed in normal runtime.
using ConfigBeforeReplaceHook = void (*)() noexcept;
void config_arm_before_replace_hook(ConfigBeforeReplaceHook hook) noexcept;

} // namespace sedici
