#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace sedici {

struct ConfigFileStamp {
    bool exists = false;
    uint64_t last_write = 0;
    uint64_t size = 0;
};

ConfigFileStamp config_file_stamp(const std::filesystem::path& path) noexcept;

struct ConfigValues {
    bool write_limiter_fps = false;
    double limiter_fps = 0.0;

    bool write_pacing = false;
    const char* pacing = "hybrid";

    bool write_async = false;
    const char* async = "game";

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
                 std::string& error) noexcept;

} // namespace sedici
