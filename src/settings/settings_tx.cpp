#include "settings/settings_tx.h"

#include "settings/config_patch.h"
#include "limiter.h"
#include "settings/live.h"
#include "diagnostics/log.h"

#include <atomic>
#include <cmath>
#include <mutex>
#include <new>

namespace sedici {
namespace {

std::mutex g_mutex; // protects in-memory settings and edit state only
std::mutex g_io_mutex; // serializes persistence and path changes; never held by apply
std::atomic<SettingsIoTestHook> g_io_test_hook{nullptr};
std::atomic<SettingsIoTestHook> g_after_write_test_hook{nullptr};
std::atomic<bool> g_fail_after_write{false};
std::atomic<bool> g_persistence_uncertain{false};
uint64_t g_revision = 1;
uint64_t g_edit_generation = 0;
bool g_saving = false;

std::filesystem::path g_file;
bool g_file_overridden = false;

// Edit-session baseline.
bool g_baseline_valid = false;
uint64_t g_edit_revision = 0;
LiveValues g_baseline;
ConfigFileStamp g_baseline_stamp;

std::filesystem::path config_path()
{
    return g_file_overridden ? g_file : dll_directory() / L"sedici.toml";
}

LiveValues read_live() noexcept
{
    return live_snapshot();
}

// Returns an empty string when valid.
std::string validate(const LiveValues& v)
{
    if (!std::isfinite(v.limiter_fps) || v.limiter_fps < 0.0 || v.limiter_fps > 1000.0)
        return "limiter.fps must be between 0 and 1000";
    if (v.log_level < 0 || v.log_level > 2) return "log.level must be 0, 1 or 2";
    if (v.scale_mode != UiScaleMode::Auto && v.scale_mode != UiScaleMode::Fixed)
        return "ui.scale_mode is not valid";
    if (!std::isfinite(v.ui_scale) || v.ui_scale < 0.5f || v.ui_scale > 2.0f)
        return "ui.scale must be between 0.5 and 2.0";
    return {};
}

// Caller holds g_mutex.
ApplyResult apply_locked(uint64_t baseRevision, const LiveValues& candidate)
{
    ApplyResult result;
    result.revision = g_revision;
    if (const std::string problem = validate(candidate); !problem.empty()) {
        result.status = ApplyStatus::Rejected;
        result.reason = problem;
        return result;
    }
    if (baseRevision != g_revision) {
        result.status = ApplyStatus::Conflict;
        result.reason = "settings changed since this request was built";
        return result;
    }
    const Settings& config = settings();
    const LiveValues before = read_live();
    if ((config.limiter.fps_source == Source::Env && candidate.limiter_fps != before.limiter_fps) ||
        (config.log.level_source == Source::Env && candidate.log_level != before.log_level)) {
        result.status = ApplyStatus::Rejected;
        result.reason = "an environment override controls this setting";
        return result;
    }
    if (before == candidate) {
        result.status = ApplyStatus::Unchanged;
        return result;
    }

    result.status = ApplyStatus::Applied;
    // Publish the candidate as one coherent snapshot. Callers that require
    // multiple fields together must read live_snapshot() once.
    live_apply(candidate);
    ++g_revision;
    result.revision = g_revision;
    if (before.limiter_fps != candidate.limiter_fps) limiter_on_config_changed();
    return result;
}

struct SaveFlag {
    ~SaveFlag()
    {
        try { std::lock_guard lock(g_mutex); g_saving = false; } catch (...) {}
    }
};

} // namespace

SettingsSnapshot settings_snapshot() noexcept
{
    std::lock_guard lock(g_mutex);
    return SettingsSnapshot{g_revision, read_live()};
}

ApplyResult settings_apply(uint64_t baseRevision, const LiveValues& candidate) noexcept
{
    try {
        std::lock_guard lock(g_mutex);
        return apply_locked(baseRevision, candidate);
    } catch (...) {
        ApplyResult failed;
        failed.status = ApplyStatus::Rejected;
        // Do not allocate while handling an allocation failure.
        return failed;
    }
}

void settings_use_file(const std::filesystem::path& path) noexcept
{
    try {
        std::lock_guard io(g_io_mutex);
        std::lock_guard lock(g_mutex);
        g_file = path;
        g_file_overridden = true;
        g_baseline_valid = false;
        ++g_edit_generation;
    } catch (...) {
    }
}

uint64_t settings_start_edit() noexcept
{
    try {
        std::lock_guard lock(g_mutex);
        g_baseline_valid = false;
        if (++g_edit_generation == 0) ++g_edit_generation;
        return g_edit_generation;
    } catch (...) { return 0; }
}

void settings_invalidate_edit() noexcept
{
    (void)settings_start_edit();
}

bool settings_begin_edit_for(uint64_t generation) noexcept
{
    if (!generation) return false;
    try {
        std::lock_guard io(g_io_mutex);
        std::filesystem::path path;
        LiveValues values;
        uint64_t revision = 0;
        {
            std::lock_guard lock(g_mutex);
            if (generation != g_edit_generation) return false;
            g_baseline_valid = false;
            values = read_live();
            revision = g_revision;
            path = config_path();
        }
        if (const SettingsIoTestHook hook = g_io_test_hook.exchange(nullptr, std::memory_order_acq_rel)) hook();
        const ConfigFileStamp stamp = config_file_stamp(path);
        std::lock_guard lock(g_mutex);
        if (!stamp.valid || generation != g_edit_generation || revision != g_revision) return false;
        g_baseline = values;
        g_edit_revision = revision;
        g_baseline_stamp = stamp;
        g_baseline_valid = true;
        g_persistence_uncertain.store(false, std::memory_order_release);
        return true;
    } catch (...) { return false; }
}

bool settings_begin_edit() noexcept
{
    return settings_begin_edit_for(settings_start_edit());
}

ApplyResult settings_apply_edit(uint64_t baseRevision, const LiveValues& candidate) noexcept
{
    try {
        std::lock_guard lock(g_mutex);
        if (g_saving) return {ApplyStatus::Conflict, g_revision, "save is in progress"};
        if (g_persistence_uncertain.load(std::memory_order_acquire))
            return {ApplyStatus::Conflict, g_revision, "file state is uncertain; reopen the menu"};
        if (!g_baseline_valid || g_edit_revision != g_revision) {
            return {ApplyStatus::Conflict, g_revision, "settings changed outside this edit"};
        }
        ApplyResult result = apply_locked(baseRevision, candidate);
        if (result.status == ApplyStatus::Applied || result.status == ApplyStatus::Unchanged)
            g_edit_revision = result.revision;
        return result;
    } catch (...) {
        return {}; // Rejected; avoid allocating while handling failure.
    }
}

ApplyResult settings_cancel_edit() noexcept
{
    try {
        std::lock_guard lock(g_mutex);
        if (g_saving) return {ApplyStatus::Conflict, g_revision, "save is in progress"};
        if (g_persistence_uncertain.load(std::memory_order_acquire))
            return {ApplyStatus::Rejected, g_revision, "file state is uncertain; reopen the menu"};
        if (!g_baseline_valid)
            return {ApplyStatus::Rejected, g_revision, "edit baseline is unavailable"};
        if (g_edit_revision != g_revision)
            return {ApplyStatus::Conflict, g_revision, "settings changed outside this edit"};
        ApplyResult result = apply_locked(g_revision, g_baseline);
        if (result.status == ApplyStatus::Applied || result.status == ApplyStatus::Unchanged)
            g_edit_revision = result.revision;
        return result;
    } catch (...) {
        ApplyResult failed;
        failed.status = ApplyStatus::Rejected;
        return failed;
    }
}

void settings_arm_io_test_hook(SettingsIoTestHook hook) noexcept
{
    g_io_test_hook.store(hook, std::memory_order_release);
}

void settings_arm_after_write_test_hook(SettingsIoTestHook hook) noexcept
{
    g_after_write_test_hook.store(hook, std::memory_order_release);
}

void settings_fail_after_write_once() noexcept
{
    g_fail_after_write.store(true, std::memory_order_release);
}

SettingsSaveTicket settings_capture_save_ticket() noexcept
{
    try {
        std::lock_guard lock(g_mutex);
        return {g_edit_generation, g_revision,
                g_baseline_valid && !g_saving && g_edit_revision == g_revision &&
                !g_persistence_uncertain.load(std::memory_order_acquire)};
    } catch (...) { return {}; }
}

static SaveResult settings_save_impl(const SettingsSaveTicket* ticket) noexcept
{
    SaveResult out;
    bool wrote = false;
    std::string wroteUnverified;
    try {
        // Allocate this before any replacement so even an allocation failure
        // afterwards can report that the file may already have changed.
        wroteUnverified = "Save was written, but verification failed; reopen the menu";
        // Persistence operations and test path changes are serialized separately
        // from live settings. Neither file inspection nor writing holds g_mutex.
        std::lock_guard io(g_io_mutex);
        std::filesystem::path path;
        ConfigFileStamp baselineStamp;
        LiveValues values;
        ConfigValues file;
        uint64_t savedRevision = 0, generation = 0;
        // Build success messages before writing so allocation failure cannot
        // report a failed save after the file was already replaced.
        std::string savedMessage = "Saved";
        std::string newerMessage = "Saved an earlier revision; newer settings remain unsaved";
        std::string postWriteConflict = "Save was written, but the file changed immediately afterward; reopen the menu";
        {
            std::lock_guard lock(g_mutex);
            if (!g_baseline_valid || g_persistence_uncertain.load(std::memory_order_acquire)) {
                out.message = "Save failed: file state is unavailable";
                return out;
            }
            if (ticket && (!ticket->valid || ticket->generation != g_edit_generation ||
                           ticket->revision != g_revision)) {
                out.conflict = true;
                out.message = "settings changed since Save was requested";
                return out;
            }
            if (g_edit_revision != g_revision) {
                out.conflict = true;
                out.message = "settings changed outside this edit; not saved";
                return out;
            }
            path = config_path();
            baselineStamp = g_baseline_stamp;
            values = read_live();
            savedRevision = g_revision;
            generation = g_edit_generation;
            const Settings& config = settings();
            if (config.limiter.fps_source != Source::Env) {
                file.write_limiter_fps = true;
                file.limiter_fps = values.limiter_fps;
            }
            if (config.log.level_source != Source::Env) {
                file.write_log = true;
                file.log_level = values.log_level;
            }
            // UI mode is a startup preference, not this edit session's view state.
            file.write_ui_scale = true;
            file.ui_scale_auto = values.scale_mode == UiScaleMode::Auto;
            file.ui_scale = values.ui_scale;
            g_saving = true;
        }
        SaveFlag clearSaving;
        if (const SettingsIoTestHook hook = g_io_test_hook.exchange(nullptr, std::memory_order_acq_rel)) hook();
        const ConfigFileStamp current = config_file_stamp(path);
        if (!config_same_stamp(current, baselineStamp)) {
            out.conflict = true;
            out.message = "sedici.toml changed or is unavailable; not saved";
            log_error("sedici: overlay save refused: file changed or unavailable");
            return out;
        }
        std::string error;
        bool fileConflict = false;
        ConfigFileStamp writtenStamp;
        if (!config_save(path, file, error, &baselineStamp, &fileConflict, &writtenStamp)) {
            out.conflict = fileConflict;
            out.message = "Save failed: " + error;
            log_error(out.message);
            return out;
        }
        wrote = true;
        if (g_fail_after_write.exchange(false, std::memory_order_acq_rel)) throw std::bad_alloc();
        if (const SettingsIoTestHook hook = g_after_write_test_hook.exchange(nullptr, std::memory_order_acq_rel)) hook();
        const ConfigFileStamp savedStamp = config_file_stamp(path);
        const bool matchesWritten = config_same_stamp(savedStamp, writtenStamp);
        bool unchanged = false;
        {
            std::lock_guard lock(g_mutex);
            // The I/O mutex prevents session/path replacement while saving.
            if (generation == g_edit_generation) {
                g_baseline_valid = matchesWritten;
                if (matchesWritten) {
                    g_baseline = values;
                    g_baseline_stamp = savedStamp;
                    unchanged = g_revision == savedRevision;
                }
            }
        }
        if (!matchesWritten) {
            out.conflict = true;
            out.message = std::move(postWriteConflict);
            return out;
        }
        out.ok = true;
        out.message = unchanged ? std::move(savedMessage) : std::move(newerMessage);
        log_event(1, "sedici: overlay saved a settings snapshot");
        return out;
    } catch (...) {
        out.ok = false;
        if (wrote) {
            g_persistence_uncertain.store(true, std::memory_order_release);
            out.conflict = true;
            out.message = std::move(wroteUnverified);
        } else {
            out.message.clear(); // Avoid allocating while handling failure.
        }
        return out;
    }
}

SaveResult settings_save() noexcept
{
    return settings_save_impl(nullptr);
}

SaveResult settings_save_for(SettingsSaveTicket ticket) noexcept
{
    return settings_save_impl(&ticket);
}

} // namespace sedici
