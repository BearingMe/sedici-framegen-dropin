#include "settings/config.h"
#include "settings/config_patch.h"
#include "abi/token_namespace.h"
#include "gpu/retirement_policy.h"
#include "gpu/queue_policy.h"
#include "gpu/slot_policy.h"
#include "imgui_config.h"
#include "runtime/bookkeeping_faults.h"
#include "presentation/limiter.h"
#include "presentation/limiter_schedule.h"
#include "settings/live.h"
#include "diagnostics/log.h"
#include "overlay.h"
#include "settings/settings_tx.h"
#include "diagnostics/diag.h"
#include "settings/settings_io.h"
#include "ui.h"
#include "ui_input.h"
#include "ffx_api_framegeneration_dx12.h"

#include <windows.h>
#include <algorithm>
#include <dxgi.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <wrl/client.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" const char g_sedici_proxy_marker[] = "SEDICI_PROXY_V1";

namespace {
int failures = 0;
int checks = 0;
HANDLE settingsIoArrived = nullptr;
HANDLE settingsIoResume = nullptr;
void pause_settings_io() noexcept
{
    SetEvent(settingsIoArrived);
    (void)WaitForSingleObject(settingsIoResume, 5000);
}
void check(bool ok, const char* message)
{
    ++checks;
    if (!ok) ++failures;
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", message);
}
void write(const std::filesystem::path& path, const std::string& text)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << text;
    if (!file) throw std::runtime_error("test fixture write failed");
}
std::string read(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

bool validate_final(const sedici::LiveValues& v)
{
    return v.limiter_fps >= 20.0 && v.limiter_fps <= 64.0;
}

bool wait_settings_io(sedici::SettingsIoCompletion& completion)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    bool received = false;
    while (std::chrono::steady_clock::now() < deadline) {
        if (!received) received = sedici::settings_io_poll(completion);
        if (received && !sedici::settings_io_busy()) return true;
        std::this_thread::yield();
    }
    return false;
}

void settings_checks(const std::filesystem::path& file)
{
    using namespace sedici;
    live_initialize(settings());
    settings_use_file(file);
    write(file, "[limiter]\n# keep this comment\nfps = 0\n");

    const SettingsSnapshot s0 = settings_snapshot();
    LiveValues next = s0.values;
    next.limiter_fps = 37.0;
    const ApplyResult applied = settings_apply(s0.revision, next);
    check(applied.status == ApplyStatus::Applied && applied.revision == s0.revision + 1,
          "settings: a valid candidate is applied and acknowledged with a new revision");
    check(live_limiter_fps() == 37.0, "settings: the live value follows the acknowledged snapshot");

    const ApplyResult same = settings_apply(applied.revision, next);
    check(same.status == ApplyStatus::Unchanged && same.revision == applied.revision,
          "settings: an identical candidate changes nothing and keeps the revision");

    LiveValues stale = next;
    stale.limiter_fps = 40.0;
    const ApplyResult conflict = settings_apply(s0.revision, stale);
    check(conflict.status == ApplyStatus::Conflict && live_limiter_fps() == 37.0 &&
              conflict.revision == applied.revision,
          "settings: a stale request is a conflict and applies nothing");

    LiveValues bad = next;
    bad.limiter_fps = 2000.0;
    check(settings_apply(applied.revision, bad).status == ApplyStatus::Rejected &&
              live_limiter_fps() == 37.0,
          "settings: an out-of-range value is rejected, not clamped");
    LiveValues partial = next;
    partial.limiter_fps = 41.0;
    partial.log_level = 9;
    check(settings_apply(applied.revision, partial).status == ApplyStatus::Rejected &&
              live_limiter_fps() == 37.0,
          "settings: a candidate with one invalid field applies none of its fields");

    const uint64_t superseded = settings_start_edit();
    settings_invalidate_edit();
    check(!settings_begin_edit_for(superseded),
          "settings: an invalidated edit generation cannot publish a baseline");
    check(settings_begin_edit(), "settings: initialize a current edit baseline");
    const SettingsSaveTicket oldTicket = settings_capture_save_ticket();
    (void)settings_change_edit([](LiveValues& v) { v.limiter_fps = 38.0; });
    const std::string beforeStaleTicket = read(file);
    const SaveResult staleTicket = settings_save_for(oldTicket);
    check(!staleTicket.ok && staleTicket.conflict && read(file) == beforeStaleTicket,
          "settings: a stale Save ticket cannot write the file");
    (void)settings_cancel_edit();

    // Edit session: cancel returns to the acknowledged baseline.
    settings_begin_edit();
    check(settings_change_edit([](LiveValues& v) { v.limiter_fps = 50.0; v.log_level = 2; }).status ==
              ApplyStatus::Applied && live_limiter_fps() == 50.0 && live_log_level() == 2,
          "settings: a control change applies live");
    check(settings_cancel_edit().status == ApplyStatus::Applied && live_limiter_fps() == 37.0 &&
              live_log_level() == settings_snapshot().values.log_level,
          "settings: cancel restores the baseline as one complete change");

    // Save writes the acknowledged values, keeps comments, and moves the baseline,
    // so a later cancel does not revert values that are on disk.
    settings_begin_edit();
    (void)settings_change_edit([](LiveValues& v) { v.limiter_fps = 45.0; });
    const SaveResult saved = settings_save();
    const std::string onDisk = read(file);
    check(saved.ok && onDisk.find("fps = 45") != std::string::npos &&
              onDisk.find("# keep this comment") != std::string::npos,
          "settings: save persists the acknowledged value and keeps comments");
    (void)settings_change_edit([](LiveValues& v) { v.limiter_fps = 60.0; });
    (void)settings_cancel_edit();
    check(live_limiter_fps() == 45.0, "settings: cancel after save returns to the saved value");

    // External edit between begin_edit and save: refused, file untouched.
    settings_begin_edit();
    write(file, "[limiter]\nfps = 12 # edited elsewhere, longer than before\n");
    (void)settings_change_edit([](LiveValues& v) { v.limiter_fps = 33.0; });
    const SaveResult refused = settings_save();
    check(!refused.ok && refused.conflict &&
              read(file) == "[limiter]\nfps = 12 # edited elsewhere, longer than before\n" &&
              live_limiter_fps() == 33.0,
          "settings: an externally edited file is not overwritten and the live value is kept");

    // Same-size edits with a restored write timestamp must still conflict.
    const std::string sameSizeOriginal = std::string("[limiter]") + char(10) + "fps = 12" + char(10);
    const std::string sameSizeChanged = std::string("[limiter]") + char(10) + "fps = 13" + char(10);
    write(file, sameSizeOriginal);
    check(settings_begin_edit(), "settings: capture baseline for same-metadata edit");
    const ConfigFileStamp originalStamp = config_file_stamp(file);
    HANDLE attributes = CreateFileW(file.c_str(), FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (attributes == INVALID_HANDLE_VALUE) throw std::runtime_error("timestamp fixture open failed");
    FILETIME originalWrite{};
    const bool readTime = GetFileTime(attributes, nullptr, nullptr, &originalWrite) != 0;
    write(file, sameSizeChanged);
    const bool restoredTime = readTime && SetFileTime(attributes, nullptr, nullptr, &originalWrite) != 0;
    CloseHandle(attributes);
    const ConfigFileStamp changedStamp = config_file_stamp(file);
    const SaveResult sameMetadata = settings_save();
    check(restoredTime && originalStamp.valid && changedStamp.valid &&
              originalStamp.size == changedStamp.size && originalStamp.last_write == changedStamp.last_write &&
              originalStamp.sha256 != changedStamp.sha256 && !sameMetadata.ok && sameMetadata.conflict &&
              read(file) == sameSizeChanged,
          "settings: same-size external edit with restored timestamp is not overwritten");

    // Replacing a file with an identical copy and timestamp still changes its
    // identity; the edit baseline must not silently cross that replacement.
    write(file, sameSizeOriginal);
    check(settings_begin_edit(), "settings: capture baseline before file replacement");
    const ConfigFileStamp identityBefore = config_file_stamp(file);
    const auto replacementFile = file.parent_path() / L"settings-replacement-fixture.tmp";
    if (std::filesystem::exists(replacementFile)) throw std::runtime_error("replacement fixture already exists");
    write(replacementFile, sameSizeOriginal);
    HANDLE replacementAttributes = CreateFileW(replacementFile.c_str(), FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (replacementAttributes == INVALID_HANDLE_VALUE) throw std::runtime_error("replacement fixture open failed");
    FILETIME replacementTime{};
    replacementTime.dwLowDateTime = static_cast<DWORD>(identityBefore.last_write);
    replacementTime.dwHighDateTime = static_cast<DWORD>(identityBefore.last_write >> 32);
    const bool timestampSet = SetFileTime(replacementAttributes, nullptr, nullptr, &replacementTime) != 0;
    CloseHandle(replacementAttributes);
    const bool replaced = timestampSet && MoveFileExW(replacementFile.c_str(), file.c_str(),
                                                        MOVEFILE_REPLACE_EXISTING) != 0;
    if (!replaced) std::filesystem::remove(replacementFile);
    const ConfigFileStamp identityAfter = config_file_stamp(file);
    const SaveResult replacedSave = settings_save();
    check(replaced && identityBefore.valid && identityAfter.valid &&
              identityBefore.size == identityAfter.size && identityBefore.sha256 == identityAfter.sha256 &&
              identityBefore.last_write == identityAfter.last_write &&
              identityBefore.file_id != identityAfter.file_id && !replacedSave.ok && replacedSave.conflict,
          "settings: replacement with identical content and timestamp invalidates the baseline");

    // Force an edit after the candidate temporary file is written but before
    // replacement. The final recheck must preserve the external file.
    write(file, sameSizeOriginal);
    check(settings_begin_edit(), "settings: capture baseline before replacement race");
    (void)settings_change_edit([](LiveValues& v) { v.limiter_fps = 34.0; });
    settingsIoArrived = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    settingsIoResume = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!settingsIoArrived || !settingsIoResume) throw std::runtime_error("replacement race fixture failed");
    config_arm_before_replace_hook(&pause_settings_io);
    SaveResult replacementRace;
    std::thread racingSave([&] { replacementRace = settings_save(); });
    const bool beforeReplace = WaitForSingleObject(settingsIoArrived, 5000) == WAIT_OBJECT_0;
    bool externalWrote = false;
    if (beforeReplace) {
        try { write(file, sameSizeChanged); externalWrote = true; } catch (...) {}
    }
    SetEvent(settingsIoResume);
    racingSave.join();
    config_arm_before_replace_hook(nullptr);
    CloseHandle(settingsIoArrived);
    CloseHandle(settingsIoResume);
    settingsIoArrived = settingsIoResume = nullptr;
    check(beforeReplace && externalWrote && !replacementRace.ok && replacementRace.conflict &&
              read(file) == sameSizeChanged && live_limiter_fps() == 34.0,
          "settings: an external edit before replacement is detected and preserved");
    (void)settings_cancel_edit();

    // K43: an edit that lands after the final check and before the swap is put
    // back, not overwritten, and no sibling is left behind.
    write(file, sameSizeOriginal);
    check(settings_begin_edit(), "settings: capture baseline before swap race");
    (void)settings_change_edit([](LiveValues& v) { v.limiter_fps = 34.0; });
    settingsIoArrived = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    settingsIoResume = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!settingsIoArrived || !settingsIoResume) throw std::runtime_error("swap race fixture failed");
    config_arm_before_swap_hook(&pause_settings_io);
    SaveResult swapRace;
    std::thread swappingSave([&] { swapRace = settings_save(); });
    const bool beforeSwap = WaitForSingleObject(settingsIoArrived, 5000) == WAIT_OBJECT_0;
    bool swapExternal = false;
    if (beforeSwap) {
        try { write(file, sameSizeChanged); swapExternal = true; } catch (...) {}
    }
    SetEvent(settingsIoResume);
    swappingSave.join();
    config_arm_before_swap_hook(nullptr);
    CloseHandle(settingsIoArrived);
    CloseHandle(settingsIoResume);
    settingsIoArrived = settingsIoResume = nullptr;
    size_t siblings = 0;
    for (const auto& entry : std::filesystem::directory_iterator(file.parent_path()))
        if (entry.path().filename().wstring().find(L".sedici-") != std::wstring::npos) ++siblings;
    check(beforeSwap && swapExternal && !swapRace.ok && swapRace.conflict &&
              read(file) == sameSizeChanged && siblings == 0,
          "settings: an external edit between the final check and the swap is restored");
    (void)settings_cancel_edit();

    // An external edit immediately after our replacement must not become the
    // acknowledged baseline for a later save.
    write(file, sameSizeOriginal);
    check(settings_begin_edit(), "settings: capture baseline before post-write race");
    (void)settings_change_edit([](LiveValues& v) { v.limiter_fps = 34.0; });
    settingsIoArrived = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    settingsIoResume = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!settingsIoArrived || !settingsIoResume) throw std::runtime_error("post-write fixture failed");
    settings_arm_after_write_test_hook(&pause_settings_io);
    SaveResult postWriteRace;
    std::thread postWriteSave([&] { postWriteRace = settings_save(); });
    const bool afterWrite = WaitForSingleObject(settingsIoArrived, 5000) == WAIT_OBJECT_0;
    bool postWriteExternal = false;
    if (afterWrite) {
        try { write(file, sameSizeChanged); postWriteExternal = true; } catch (...) {}
    }
    SetEvent(settingsIoResume);
    postWriteSave.join();
    settings_arm_after_write_test_hook(nullptr);
    CloseHandle(settingsIoArrived);
    CloseHandle(settingsIoResume);
    settingsIoArrived = settingsIoResume = nullptr;
    check(afterWrite && postWriteExternal && !postWriteRace.ok && postWriteRace.conflict &&
              postWriteRace.message.find("was written") != std::string::npos &&
              read(file) == sameSizeChanged && live_limiter_fps() == 34.0 &&
              settings_cancel_edit().status == ApplyStatus::Rejected,
          "settings: a post-write external edit is not accepted as the saved baseline");
    check(settings_begin_edit(), "settings: reopen after post-write conflict");
    (void)settings_change_edit([](LiveValues& v) { v.limiter_fps = 33.0; });

    // Failure after the replacement must report that the file was written and
    // prevent another Save until a new baseline is explicitly acquired.
    write(file, sameSizeOriginal);
    check(settings_begin_edit(), "settings: capture baseline before post-write failure");
    (void)settings_change_edit([](LiveValues& v) { v.limiter_fps = 34.0; });
    settings_fail_after_write_once();
    const SaveResult postWriteFailure = settings_save();
    const SaveResult blockedRetry = settings_save();
    check(!postWriteFailure.ok && postWriteFailure.conflict &&
              postWriteFailure.message.find("was written") != std::string::npos &&
              read(file).find("fps = 34") != std::string::npos && !blockedRetry.ok &&
              settings_cancel_edit().status == ApplyStatus::Rejected,
          "settings: post-write failure reports the write and blocks reuse of an uncertain baseline");
    check(settings_begin_edit(), "settings: reacquire baseline after post-write failure");
    (void)settings_change_edit([](LiveValues& v) { v.limiter_fps = 33.0; });

    // An outside writer must not be silently undone by Cancel or persisted as
    // if it were part of this menu edit.
    settings_begin_edit();
    (void)settings_change_edit([](LiveValues& v) { v.limiter_fps = 32.0; });
    (void)settings_change([](LiveValues& v) { v.limiter_fps = 33.0; });
    check(settings_cancel_edit().status == ApplyStatus::Conflict && live_limiter_fps() == 33.0,
          "settings: cancel refuses to overwrite an outside change");
    check(settings_save().conflict, "settings: save refuses a conflicting outside change");

    // While Save is paused outside the settings lock, an unrelated live writer
    // can proceed. Cancel is rejected until the captured snapshot finishes saving.
    check(settings_begin_edit(), "settings: begin edit before concurrent save");
    (void)settings_change_edit([](LiveValues& v) { v.limiter_fps = 44.0; });
    settingsIoArrived = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    settingsIoResume = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE changeDone = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!settingsIoArrived || !settingsIoResume || !changeDone)
        throw std::runtime_error("settings synchronization fixture failed");
    settings_arm_io_test_hook(&pause_settings_io);
    SaveResult concurrentSave;
    std::thread saving([&] { concurrentSave = settings_save(); });
    const bool saveArrived = WaitForSingleObject(settingsIoArrived, 5000) == WAIT_OBJECT_0;
    ApplyResult outsideChange, concurrentCancel;
    std::thread changing;
    bool changeCompleted = false;
    if (saveArrived) {
        changing = std::thread([&] {
            outsideChange = settings_change([](LiveValues& v) { v.limiter_fps = 33.0; });
            concurrentCancel = settings_cancel_edit();
            SetEvent(changeDone);
        });
        changeCompleted = WaitForSingleObject(changeDone, 1000) == WAIT_OBJECT_0;
    }
    SetEvent(settingsIoResume);
    saving.join();
    if (changing.joinable()) changing.join();
    settings_arm_io_test_hook(nullptr);
    CloseHandle(settingsIoArrived);
    CloseHandle(settingsIoResume);
    CloseHandle(changeDone);
    settingsIoArrived = settingsIoResume = nullptr;
    check(saveArrived && changeCompleted && outsideChange.status == ApplyStatus::Applied &&
              concurrentCancel.status == ApplyStatus::Conflict,
          "settings: live change proceeds and cancel is rejected during file I/O");
    check(concurrentSave.ok && concurrentSave.message.find("earlier revision") != std::string::npos &&
              read(file).find("fps = 44") != std::string::npos && live_limiter_fps() == 33.0,
          "settings: save reports the exact captured revision without discarding a newer live change");

    // A worker whose edit generation is invalidated during file inspection
    // must not publish an old baseline into the next UI session.
    settingsIoArrived = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    settingsIoResume = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!settingsIoArrived || !settingsIoResume) throw std::runtime_error("async settings fixture failed");
    const uint64_t oldGeneration = settings_start_edit();
    settings_arm_io_test_hook(&pause_settings_io);
    const bool asyncStarted = settings_io_start_begin(oldGeneration, 101);
    const bool asyncArrived = asyncStarted && WaitForSingleObject(settingsIoArrived, 5000) == WAIT_OBJECT_0;
    if (asyncArrived) settings_invalidate_edit();
    SetEvent(settingsIoResume);
    SettingsIoCompletion completion;
    const bool asyncFinished = asyncStarted && wait_settings_io(completion);
    settings_arm_io_test_hook(nullptr);
    CloseHandle(settingsIoArrived);
    CloseHandle(settingsIoResume);
    settingsIoArrived = settingsIoResume = nullptr;
    check(asyncArrived && asyncFinished && completion.kind == SettingsIoKind::BeginEdit &&
              completion.viewEpoch == 101 && !completion.beginOk,
          "settings I/O: invalidated asynchronous begin cannot publish its baseline");
    const uint64_t currentGeneration = settings_start_edit();
    SettingsIoCompletion currentBegin;
    check(settings_io_start_begin(currentGeneration, 102) && wait_settings_io(currentBegin) &&
              currentBegin.kind == SettingsIoKind::BeginEdit && currentBegin.viewEpoch == 102 && currentBegin.beginOk &&
              currentBegin.workerThreadId != 0 && currentBegin.workerThreadId != GetCurrentThreadId(),
          "settings I/O: a current edit begins on a different bounded worker thread");
    (void)settings_change_edit([](LiveValues& v) { v.limiter_fps = 33.0; });
    const SettingsSaveTicket asyncTicket = settings_capture_save_ticket();
    SettingsIoCompletion asyncSaved;
    check(settings_io_start_save(asyncTicket, 102) && wait_settings_io(asyncSaved) &&
              asyncSaved.kind == SettingsIoKind::Save && asyncSaved.viewEpoch == 102 && asyncSaved.save.ok &&
              asyncSaved.workerThreadId != 0 && asyncSaved.workerThreadId != GetCurrentThreadId(),
          "settings I/O: a ticketed snapshot saves on a different worker thread");

    // An asynchronous Save owns its exact ticketed snapshot even if another
    // writer publishes a newer revision while the worker is in file I/O.
    check(settings_begin_edit(), "settings I/O: begin before concurrent worker save");
    (void)settings_change_edit([](LiveValues& v) { v.limiter_fps = 44.0; });
    const SettingsSaveTicket concurrentTicket = settings_capture_save_ticket();
    settingsIoArrived = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    settingsIoResume = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE asyncChangeDone = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!settingsIoArrived || !settingsIoResume || !asyncChangeDone)
        throw std::runtime_error("async save fixture failed");
    settings_arm_io_test_hook(&pause_settings_io);
    const bool saveStarted = settings_io_start_save(concurrentTicket, 103);
    const bool savePaused = saveStarted && WaitForSingleObject(settingsIoArrived, 5000) == WAIT_OBJECT_0;
    const bool duplicateRejected = savePaused && !settings_io_start_save(concurrentTicket, 103);
    ApplyResult asyncChange;
    std::thread concurrentWriter;
    bool asyncChangeCompleted = false;
    if (savePaused) {
        concurrentWriter = std::thread([&] {
            asyncChange = settings_change([](LiveValues& v) { v.limiter_fps = 33.0; });
            SetEvent(asyncChangeDone);
        });
        asyncChangeCompleted = WaitForSingleObject(asyncChangeDone, 1000) == WAIT_OBJECT_0;
    }
    SetEvent(settingsIoResume);
    if (concurrentWriter.joinable()) concurrentWriter.join();
    SettingsIoCompletion concurrentCompletion;
    const bool saveFinished = saveStarted && wait_settings_io(concurrentCompletion);
    settings_arm_io_test_hook(nullptr);
    CloseHandle(settingsIoArrived);
    CloseHandle(settingsIoResume);
    CloseHandle(asyncChangeDone);
    settingsIoArrived = settingsIoResume = nullptr;
    check(savePaused && duplicateRejected && asyncChangeCompleted && saveFinished &&
              asyncChange.status == ApplyStatus::Applied && concurrentCompletion.save.ok &&
              concurrentCompletion.viewEpoch == 103 &&
              concurrentCompletion.save.message.find("earlier revision") != std::string::npos &&
              read(file).find("fps = 44") != std::string::npos && live_limiter_fps() == 33.0,
          "settings I/O: one worker saves its ticket while newer live settings remain unsaved");

    // The output callback must return while edit-baseline I/O is deliberately
    // paused. This is a threading check, not full UI interaction acceptance.
    settingsIoArrived = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    settingsIoResume = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE renderReturned = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!settingsIoArrived || !settingsIoResume || !renderReturned)
        throw std::runtime_error("UI I/O fixture failed");
    settings_arm_io_test_hook(&pause_settings_io);
    live_set_ui_mode(UiMode::Full);
    std::thread rendering([&] {
        ui_on_present(nullptr);
        SetEvent(renderReturned);
    });
    const bool uiIoArrived = WaitForSingleObject(settingsIoArrived, 5000) == WAIT_OBJECT_0;
    const bool uiReturned = WaitForSingleObject(renderReturned, 1000) == WAIT_OBJECT_0;
    SetEvent(settingsIoResume);
    rendering.join();
    const auto ioDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (settings_io_busy() && std::chrono::steady_clock::now() < ioDeadline)
        std::this_thread::yield();
    const bool workerFinished = !settings_io_busy();
    ui_on_present(nullptr); // Consume the completion for this Full-menu epoch.
    live_set_ui_mode(UiMode::Off);
    ui_on_present(nullptr); // Invalidate that epoch without file I/O.
    settings_arm_io_test_hook(nullptr);
    CloseHandle(settingsIoArrived);
    CloseHandle(settingsIoResume);
    CloseHandle(renderReturned);
    settingsIoArrived = settingsIoResume = nullptr;
    check(uiIoArrived && uiReturned && workerFinished,
          "settings I/O: output callback returns while file inspection is paused");

    // A write that cannot happen is a result, not an exception, and not live damage.
    const auto blocked = file.parent_path() / L"settings-blocked.toml";
    std::filesystem::create_directory(blocked);
    settings_use_file(blocked);
    settings_begin_edit();
    const SaveResult failed = settings_save();
    check(!failed.ok && !failed.conflict && failed.message.rfind("Save failed", 0) == 0 &&
              live_limiter_fps() == 33.0,
          "settings: a failed save is reported and leaves the live values alone");
    std::filesystem::remove(blocked);
    settings_use_file(file);

    settings_use_file(file.parent_path() / L"invalid|edit.toml");
    settings_begin_edit();
    check(settings_cancel_edit().status == ApplyStatus::Rejected && !settings_save().ok,
          "settings: an unavailable edit baseline rejects cancel and save");
    settings_use_file(file);

    // Alternating complete candidates must never produce a mixed snapshot.
    LiveValues first = live_snapshot();
    LiveValues second = first;
    first.limiter_fps = 40.0; first.ui_scale = 1.0f;
    second.limiter_fps = 80.0; second.ui_scale = 2.0f;
    live_apply(first);
    std::atomic<bool> done{false};
    std::atomic<bool> mixed{false};
    std::thread publisher([&] {
        for (int i = 0; i < 10000; ++i) live_apply((i & 1) ? first : second);
        done.store(true);
    });
    while (!done.load()) {
        const LiveValues v = live_snapshot();
        if (!((v.limiter_fps == 40.0 && v.ui_scale == 1.0f) ||
              (v.limiter_fps == 80.0 && v.ui_scale == 2.0f))) mixed = true;
    }
    publisher.join();
    check(!mixed.load(), "settings: complete live snapshots never contain mixed candidates");

    // Concurrent writers: every Applied result is exactly one revision step.
    const uint64_t startRevision = settings_snapshot().revision;
    std::atomic<uint64_t> appliedCount{0};
    std::vector<std::thread> writers;
    for (int t = 0; t < 4; ++t) {
        writers.emplace_back([&, t] {
            for (int i = 0; i < 200; ++i) {
                const double fps = 20.0 + t * 10.0 + (i % 5);
                if (settings_change([fps](LiveValues& v) { v.limiter_fps = fps; }).status ==
                    ApplyStatus::Applied)
                    appliedCount.fetch_add(1);
            }
        });
    }
    for (auto& writer : writers) writer.join();
    check(settings_snapshot().revision == startRevision + appliedCount.load() && appliedCount > 0,
          "settings: concurrent changes never lose or double-count a revision");
    check(validate_final(settings_snapshot().values), "settings: concurrent changes leave a valid state");
}

void queue_policy_checks()
{
    sedici::fg::FixedQueue<int, 3> queue;
    int a = 1, b = 2, c = 3, d = 4;
    check(queue.empty() && queue.push_back(std::move(a)) && queue.push_back(std::move(b)) &&
              queue.push_back(std::move(c)) && queue.size() == 3 && !queue.push_back(std::move(d)),
          "present queue: bounded ring accepts capacity and refuses overflow");
    check(queue.front() == 1, "present queue: first item retains FIFO order");
    queue.pop_front();
    check(queue.push_back(std::move(d)) && queue.front() == 2,
          "present queue: released slot can be reused without allocation");
    queue.pop_front();
    const bool third = queue.front() == 3;
    queue.pop_front();
    const bool fourth = queue.front() == 4;
    queue.pop_front();
    check(third && fourth && queue.empty(), "present queue: wraparound preserves FIFO order");
}

void retirement_policy_checks()
{
    uint32_t nextSlot = 2, selected = 99;
    const HRESULT slotFailure = sedici::fg::acquire_frame_slot(
        nextSlot, 3, selected, [](uint32_t) noexcept { return E_FAIL; });
    check(slotFailure == E_FAIL && nextSlot == 2 && selected == 99,
          "retirement: failed slot wait does not advance or publish a slot");
    const HRESULT slotNotReady = sedici::fg::acquire_frame_slot(
        nextSlot, 3, selected, [](uint32_t) noexcept { return S_FALSE; });
    check(FAILED(slotNotReady) && nextSlot == 2 && selected == 99,
          "retirement: a non-ready success status does not advance a slot");
    const HRESULT slotSuccess = sedici::fg::acquire_frame_slot(
        nextSlot, 3, selected, [](uint32_t slot) noexcept { return slot == 2 ? S_OK : E_FAIL; });
    check(slotSuccess == S_OK && selected == 2 && nextSlot == 0,
          "retirement: successful slot wait advances the ring once");
    using sedici::fg::FenceProgress;
    check(sedici::fg::classify_fence(5, 6) == FenceProgress::Pending &&
              sedici::fg::classify_fence(6, 6) == FenceProgress::Complete &&
              sedici::fg::classify_fence(UINT64_MAX, 6) == FenceProgress::DeviceRemoved,
          "retirement: removed-device sentinel is not a completed fence");
    check(FAILED(sedici::fg::removed_device_error(S_OK)) &&
              sedici::fg::removed_device_error(E_FAIL) == E_FAIL,
          "retirement: removed-fence sentinel never produces success");
    int waits = 0;
    const HRESULT failedSignal = sedici::fg::signal_then_wait(
        []() noexcept { return E_FAIL; }, [&]() noexcept { ++waits; return S_OK; });
    check(failedSignal == E_FAIL && waits == 0,
          "retirement: failed signal is returned without waiting or reporting completion");
    const HRESULT failedWait = sedici::fg::signal_then_wait(
        []() noexcept { return S_OK; }, [&]() noexcept { ++waits; return E_ABORT; });
    check(failedWait == E_ABORT && waits == 1,
          "retirement: an accepted signal propagates the subsequent wait result");
}

void token_namespace_checks()
{
    sedici::fg::TokenNamespace tokens(0x1000, 32, 16);
    void* const first = tokens.issue();
    check(!tokens.contains_issued(reinterpret_cast<void*>(0x1010)),
          "reserved but unissued token is not classified as issued");
    void* const second = tokens.issue();
    check(first && second && first != second && tokens.contains_issued(first) &&
              tokens.contains_issued(second),
          "token namespace issues distinct stable identities");
    check(!tokens.issue() && tokens.contains_issued(first) && tokens.contains_issued(second),
          "token exhaustion rejects creation without recycling issued identities");
    check(!tokens.contains_issued(reinterpret_cast<void*>(0x1001)) &&
              !tokens.contains_issued(reinterpret_cast<void*>(0x1020)) &&
              !tokens.contains_issued(reinterpret_cast<void*>(0x2000)),
          "token namespace excludes unaligned, unissued and foreign values");
    sedici::fg::TokenNamespace invalid(0x1000, 32, 0);
    check(!invalid.issue(), "invalid token namespace is unusable");
    sedici::fg::TokenNamespace lazy;
    bool reserved = false;
    unsigned attempts = 0;
    const auto failReserve = [&](size_t) noexcept -> void* { ++attempts; return nullptr; };
    check(!sedici::fg::issue_reserved_token(lazy, reserved, 32, 16, failReserve) &&
              !reserved && attempts == 1,
          "token reservation failure issues no token and remains retryable");
    const auto succeedReserve = [&](size_t) noexcept -> void* {
        ++attempts; return reinterpret_cast<void*>(0x3000);
    };
    check(sedici::fg::issue_reserved_token(lazy, reserved, 32, 16, succeedReserve) &&
              reserved && attempts == 2,
          "token namespace can reserve after an earlier failure");
}

void settings_capability_checks(bool sediciBackend, bool environment)
{
    using namespace sedici;
    const SettingsSnapshot base = settings_snapshot();
    if (sediciBackend) {
        LiveValues limiter = base.values;
        limiter.limiter_fps = 25.0;
        check(settings_apply(base.revision, limiter).status == ApplyStatus::Applied &&
                  live_limiter_fps() == 25.0,
              "settings: real-frame limiter is available");
    }
    if (environment) {
        const Settings& config = settings();
        check(config.limiter.fps_source == Source::Env && config.log.level_source == Source::Env,
              "settings: test overrides are recorded as environment sources");
        LiveValues candidate = base.values;
        candidate.limiter_fps = candidate.limiter_fps == 45.0 ? 30.0 : 45.0;
        const bool limiterRejected = settings_apply(base.revision, candidate).status == ApplyStatus::Rejected;
        candidate = base.values;
        candidate.log_level = candidate.log_level == 2 ? 1 : 2;
        const bool logRejected = settings_apply(base.revision, candidate).status == ApplyStatus::Rejected;
        check(limiterRejected && logRejected &&
                  settings_snapshot().revision == base.revision && settings_snapshot().values == base.values,
              "settings: environment-controlled fields reject live overrides without partial changes");
    }
}

void config_checks(const std::filesystem::path& path)
{
    sedici::ConfigValues values;
    const auto invalid = path.parent_path() / L"invalid|config.toml";
    std::string invalidError;
    check(!sedici::config_file_stamp(invalid).valid &&
              !sedici::config_save(invalid, values, invalidError) && !invalidError.empty(),
          "config: an attribute-query error is not treated as a missing file");
    values.write_log = true;
    values.log_level = 2;
    std::string error;
    write(path, "[log] # keep\r\nnote = 0.2\r\n\r\n"
                "[[profiles]]\r\nmode = \"unrelated\" # keep too\r\n");
    auto unrelated = path;
    unrelated += L".tmp";
    if (std::filesystem::exists(unrelated)) throw std::runtime_error("temporary fixture already exists");
    write(unrelated, "do not overwrite");
    check(sedici::config_save(path, values, error), "save accepts array-of-table boundary");
    check(read(unrelated) == "do not overwrite", "save leaves an existing .tmp file untouched");
    std::filesystem::remove(unrelated);
    const std::string output = read(path);
    check(output.find("level = 2\r\n") < output.find("[[profiles]]"),
          "log value stays in its own table");
    check(output.find("mode = \"unrelated\" # keep too\r\n") != std::string::npos,
          "array table and comments stay unchanged");

    write(path, "[Log]\nlevel = 7\n");
    check(sedici::config_save(path, values, error), "case-sensitive target table can be appended");
    check(read(path).find("[Log]\nlevel = 7") != std::string::npos,
          "differently cased table is untouched");

    const std::string multiline = "[log]\ntext = '''\nlevel = \"inside string\"\n'''\n";
    write(path, multiline);
    check(!sedici::config_save(path, values, error), "unsafe multiline patch is rejected");
    check(read(path) == multiline, "rejected save preserves every original byte");

    const std::string quoted = "[log]\n\"level\" = 0\n";
    write(path, quoted);
    check(!sedici::config_save(path, values, error), "unsupported quoted key fails safely");
    check(read(path) == quoted, "quoted-key rejection does not overwrite file");

    write(path, "[log]\nlevel = 1 # keep\n[limiter]\nfps = 0\n");
    values.write_limiter_fps = true;
    values.limiter_fps = 36.5;
    check(sedici::config_save(path, values, error), "fractional FPS saves with comments");
    check(read(path).find("fps = 36.5") != std::string::npos, "fractional FPS survives patch");

    values.limiter_fps = 36.9999;
    values.write_ui_scale = true;
    values.ui_scale_auto = false;
    values.ui_scale = 0.9999f;
    check(sedici::config_save(path, values, error), "numbers rounding upward to integers can be saved");
    const std::string rounded = read(path);
    check(rounded.find("fps = 37\n") != std::string::npos,
          "FPS semantic validation uses the rounded integer");
    check(rounded.find("scale = 1\n") != std::string::npos,
          "scale semantic validation uses the rounded integer");
}

constexpr UINT marker = WM_APP + 77;
int destroyed = 0;
bool release_clip_on_capture_changed = false;
int capture_changes = 0;
int forwardedHotkeyUps = 0;
LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM w, LPARAM l)
{
    if (msg == marker) return GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (msg == WM_NCDESTROY) ++destroyed;
    if ((msg == WM_SYSKEYUP || msg == WM_KEYUP) && w == VK_F10) {
        ++forwardedHotkeyUps;
        return 313;
    }
    if (msg == WM_CAPTURECHANGED && release_clip_on_capture_changed) {
        ++capture_changes;
        ClipCursor(nullptr);
    }
    return DefWindowProcW(hwnd, msg, w, l);
}
WNDPROC above_original = nullptr;
LRESULT CALLBACK above_proc(HWND hwnd, UINT msg, WPARAM w, LPARAM l)
{
    return CallWindowProcW(above_original, hwnd, msg, w, l);
}

void gpu_checks(bool full)
{
    using Microsoft::WRL::ComPtr;
    ComPtr<ID3D12Debug> debug;
    const bool validation = SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
    if (validation) debug->EnableDebugLayer();
    else std::puts("NOTE DX12 debug layer unavailable; no SDK/runtime will be downloaded");
    ComPtr<ID3D12Device> device;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
        throw std::runtime_error("create GPU test device");
    ComPtr<ID3D12InfoQueue> messages;
    device.As(&messages);
    if (messages) messages->ClearStoredMessages();
    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    if (FAILED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(),
                                        nullptr, IID_PPV_ARGS(&list))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
        throw std::runtime_error("create GPU test command objects");
    list->Close();
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = 1280;
    desc.Height = 720;
    desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ComPtr<ID3D12Resource> output;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&output))))
        throw std::runtime_error("create GPU test output");
    WNDCLASSW cls{};
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpfnWndProc = window_proc;
    cls.lpszClassName = L"SediciGpuRegression";
    RegisterClassW(&cls);
    HWND hwnd = CreateWindowW(cls.lpszClassName, L"GPU regression", WS_OVERLAPPEDWINDOW,
                               0, 0, 640, 360, nullptr, nullptr, cls.hInstance, nullptr);
    if (!hwnd) throw std::runtime_error("create GPU test window");
    sedici::ui_capture_window(hwnd);
    sedici::live_set_ui_mode(full ? sedici::UiMode::Full : sedici::UiMode::Mini);
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) throw std::runtime_error("create GPU test fence event");
    for (UINT64 frame = 1; frame <= 12; ++frame) {
        if (FAILED(allocator->Reset()) || FAILED(list->Reset(allocator.Get(), nullptr)))
            throw std::runtime_error("reset GPU test command list");
        ffxCallbackDescFrameGenerationPresent params{};
        params.device = device.Get();
        params.commandList = list.Get();
        params.outputSwapChainBuffer.resource = output.Get();
        params.outputSwapChainBuffer.state = FFX_API_RESOURCE_STATE_RENDER_TARGET;
        // Full mode only: at frame 9 the renderer fails while drawing. The
        // overlay must switch itself off and give input back; the frame, the
        // command list and the GPU stay valid.
        const bool injected = full && frame >= 9;
        if (full && frame == 9)
            sedici::fg::arm_bookkeeping_fault(sedici::fg::BookkeepingFault::OverlayDraw);
        sedici::ui_on_present(&params);
        if (!injected) check(sedici::live_ui_mode() != sedici::UiMode::Off, "GPU overlay remains enabled");
        if (FAILED(list->Close())) throw std::runtime_error("close GPU test command list");
        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        if (FAILED(queue->Signal(fence.Get(), frame)) ||
            FAILED(fence->SetEventOnCompletion(frame, event)) ||
            WaitForSingleObject(event, 5000) != WAIT_OBJECT_0)
            throw std::runtime_error("GPU test submission did not finish");
        // Exercise platform-backend rebinding as well as renderer ring reuse.
        if (frame == 6) {
            HWND replacement = CreateWindowW(cls.lpszClassName, L"Replacement", WS_OVERLAPPEDWINDOW,
                0, 0, 960, 540, nullptr, nullptr, cls.hInstance, nullptr);
            if (!replacement) throw std::runtime_error("create replacement GPU test window");
            sedici::ui_capture_window(replacement);
            DestroyWindow(hwnd);
            hwnd = replacement;
        }
    }
    CloseHandle(event);
    if (full) {
        const bool consumed = !sedici::fg::consume_bookkeeping_fault(sedici::fg::BookkeepingFault::OverlayDraw);
        sedici::fg::arm_bookkeeping_fault(sedici::fg::BookkeepingFault::None);
        check(consumed && sedici::live_ui_mode() == sedici::UiMode::Off && !sedici::ui_input_active(),
              "overlay renderer failure in Full switches the overlay off and releases input");
    }
    check(device->GetDeviceRemovedReason() == S_OK, "GPU device remains healthy");
    if (messages) {
        bool clean = true;
        for (UINT64 i = 0; i < messages->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
            SIZE_T bytes = 0;
            messages->GetMessage(i, nullptr, &bytes);
            std::vector<unsigned char> storage(bytes);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            if (SUCCEEDED(messages->GetMessage(i, message, &bytes)) &&
                message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
                std::printf("DX12: %s\n", message->pDescription);
                clean = false;
            }
        }
        check(clean, "DX12 debug layer reports no error or corruption");
    }
    DestroyWindow(hwnd);
}
// Cost of the overlay per presented frame in Off, Mini and Full: CPU time inside
// ui_on_present and the whole record-submit-wait round trip, at the 2560x1080 size of
// the owner's QA runs, without the debug layer. Timing only (INFO lines, one loose
// check); not part of the default ctest list because timing tests are sensitive to
// a busy GPU (K112). Used for QA-12.
void observer_cost()
{
    using Microsoft::WRL::ComPtr;
    using Clock = std::chrono::steady_clock;
    ComPtr<ID3D12Device> device;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
        throw std::runtime_error("create GPU test device");
    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    if (FAILED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(),
                                        nullptr, IID_PPV_ARGS(&list))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
        throw std::runtime_error("create GPU test command objects");
    list->Close();
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = 2560;
    desc.Height = 1080;
    desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ComPtr<ID3D12Resource> output;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&output))))
        throw std::runtime_error("create GPU test output");
    WNDCLASSW cls{};
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpfnWndProc = window_proc;
    cls.lpszClassName = L"SediciObserverCost";
    RegisterClassW(&cls);
    HWND hwnd = CreateWindowW(cls.lpszClassName, L"Observer cost", WS_OVERLAPPEDWINDOW,
                               0, 0, 1280, 540, nullptr, nullptr, cls.hInstance, nullptr);
    if (!hwnd) throw std::runtime_error("create GPU test window");
    sedici::ui_capture_window(hwnd);
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) throw std::runtime_error("create GPU test fence event");

    UINT64 frame = 0;
    const auto micros = [](Clock::time_point a, Clock::time_point b) {
        return std::chrono::duration<double, std::micro>(b - a).count();
    };
    struct Cost { double cpu_avg, cpu_p99, trip_avg, trip_p99; };
    const auto run = [&](sedici::UiMode mode, int frames) {
        sedici::live_set_ui_mode(mode);
        std::vector<double> cpu, trip;
        for (int i = 0; i < frames; ++i) {
            const auto begin = Clock::now();
            ++frame;
            if (FAILED(allocator->Reset()) || FAILED(list->Reset(allocator.Get(), nullptr)))
                throw std::runtime_error("reset GPU test command list");
            ffxCallbackDescFrameGenerationPresent params{};
            params.device = device.Get();
            params.commandList = list.Get();
            params.outputSwapChainBuffer.resource = output.Get();
            params.outputSwapChainBuffer.state = FFX_API_RESOURCE_STATE_RENDER_TARGET;
            const auto call = Clock::now();
            sedici::ui_on_present(&params);
            cpu.push_back(micros(call, Clock::now()));
            if (FAILED(list->Close())) throw std::runtime_error("close GPU test command list");
            ID3D12CommandList* lists[] = {list.Get()};
            queue->ExecuteCommandLists(1, lists);
            if (FAILED(queue->Signal(fence.Get(), frame)) ||
                FAILED(fence->SetEventOnCompletion(frame, event)) ||
                WaitForSingleObject(event, 5000) != WAIT_OBJECT_0)
                throw std::runtime_error("GPU test submission did not finish");
            trip.push_back(micros(begin, Clock::now()));
        }
        const auto stat = [](std::vector<double>& v, double& avg, double& p99) {
            std::sort(v.begin(), v.end());
            double sum = 0;
            for (double x : v) sum += x;
            avg = sum / v.size();
            p99 = v[v.size() * 99 / 100];
        };
        Cost c{};
        stat(cpu, c.cpu_avg, c.cpu_p99);
        stat(trip, c.trip_avg, c.trip_p99);
        return c;
    };

    (void)run(sedici::UiMode::Full, 40); // warm up: ImGui and the renderer initialise
    (void)run(sedici::UiMode::Off, 40);
    const struct { const char* name; sedici::UiMode mode; } modes[] = {
        {"Off", sedici::UiMode::Off}, {"Mini", sedici::UiMode::Mini}, {"Full", sedici::UiMode::Full}};
    double full_cpu_avg = 0;
    for (int round = 1; round <= 3; ++round) {
        for (const auto& m : modes) {
            const Cost c = run(m.mode, 300);
            std::printf("INFO overlay %-4s round %d: ui_on_present %.1f us avg, %.1f us p99; "
                        "record+submit+wait %.1f us avg, %.1f us p99\n",
                        m.name, round, c.cpu_avg, c.cpu_p99, c.trip_avg, c.trip_p99);
            if (m.mode == sedici::UiMode::Full) full_cpu_avg = std::max(full_cpu_avg, c.cpu_avg);
        }
    }
    check(full_cpu_avg < 5000.0, "overlay Full costs the caller under 5 ms of CPU per frame");
    CloseHandle(event);
    sedici::live_set_ui_mode(sedici::UiMode::Off);
    DestroyWindow(hwnd);
}
void window_checks()
{
    WNDCLASSW cls{};
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpfnWndProc = window_proc;
    cls.lpszClassName = L"SediciRegression";
    if (!RegisterClassW(&cls)) throw std::runtime_error("register test window");
    HWND a = CreateWindowW(cls.lpszClassName, L"A", WS_OVERLAPPEDWINDOW,
                           0, 0, 320, 240, nullptr, nullptr, cls.hInstance, nullptr);
    HWND b = CreateWindowW(cls.lpszClassName, L"B", WS_OVERLAPPEDWINDOW,
                           0, 0, 320, 240, nullptr, nullptr, cls.hInstance, nullptr);
    if (!a || !b) throw std::runtime_error("create test window");
    SetWindowLongPtrW(a, GWLP_USERDATA, 101);
    SetWindowLongPtrW(b, GWLP_USERDATA, 202);
    sedici::ui_capture_window(a);
    sedici::ui_capture_window(b);
    check(SendMessageW(a, marker, 0, 0) == 101, "old window retains its original procedure");
    check(SendMessageW(b, marker, 0, 0) == 202, "new window forwards its own messages");
    above_original = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
        b, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&above_proc)));
    sedici::ui_capture_window(b);
    check(SendMessageW(b, marker, 0, 0) == 202,
          "recapture below another subclass does not form a recursive chain");
    DestroyWindow(a);
    check(destroyed == 1, "old window WM_NCDESTROY reaches the game procedure");
    check(SendMessageW(b, marker, 0, 0) == 202, "old destruction leaves new window intact");
    BYTE keys[256]{};
    keys[VK_MENU] = 0x80;
    SetKeyboardState(keys);
    SendMessageW(b, WM_SYSKEYDOWN, VK_F10, 0);
    check(sedici::live_ui_mode() == sedici::UiMode::Mini, "hotkey still works after replacement");
    const LRESULT pairedUp = SendMessageW(b, WM_SYSKEYUP, VK_F10, 0);
    check(pairedUp == 0 && forwardedHotkeyUps == 0,
          "a consumed hotkey key-down also consumes its matching key-up");
    const LRESULT unpairedUp = SendMessageW(b, WM_SYSKEYUP, VK_F10, 0);
    check(unpairedUp == 313 && forwardedHotkeyUps == 1,
          "an unrelated unpaired key-up still reaches the game window");
    const HWND previous_foreground = GetForegroundWindow();
    ShowWindow(b, SW_SHOW);
    SetForegroundWindow(b);
    if (GetForegroundWindow() == b) {
        release_clip_on_capture_changed = true;
        SetCapture(b);
        check(GetCapture() == b, "game owns mouse capture before Full entry");
        sedici::live_set_ui_mode(sedici::UiMode::Full);
        SendMessageW(b, WM_APP + 1, 0, 0);
        release_clip_on_capture_changed = false;
        check(capture_changes > 0, "capture-change handler can reenter ClipCursor without deadlock");
        check(sedici::ui_input_active(), "Full capture enters on the owning window thread");
    } else {
        std::puts("NOTE foreground unavailable; active capture failure check skipped");
    }
    sedici::ui_disable("injected regression failure");
    check(!sedici::ui_input_active(), "overlay failure immediately stops cursor suppression");
    SendMessageW(b, WM_APP + 1, 0, 0);
    SendMessageW(b, WM_SYSKEYDOWN, VK_F10, 0);
    SendMessageW(b, WM_SYSKEYDOWN, VK_F10, 0);
    check(sedici::live_ui_mode() == sedici::UiMode::Off, "disabled overlay cannot reopen Full");
    keys[VK_MENU] = 0;
    SetKeyboardState(keys);
    DestroyWindow(b);
    if (previous_foreground) SetForegroundWindow(previous_foreground);
    check(destroyed == 2, "current window WM_NCDESTROY reaches the game procedure");
}

// The limiter runs from the wrapper's present hook. It can be switched on live
// after an uncapped start, and it then delays the next frame.
void limiter_live_checks()
{
    const auto qpc = [] {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        return static_cast<uint64_t>(now.QuadPart);
    };
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    limiter_before_present(qpc());
    sedici::live_set_limiter_fps(1000);
    limiter_before_present(qpc());
    const uint64_t before = qpc();
    limiter_before_present(qpc());
    const uint64_t after = qpc();
    // A live rate change resets the schedule: the next frame seeds it and does
    // not wait out the old interval.
    sedici::live_set_limiter_fps(10); // 100 ms interval
    limiter_on_config_changed();
    limiter_before_present(qpc());    // seeds
    const uint64_t held0 = qpc();
    limiter_before_present(qpc());    // waits about one interval
    const double heldMs = 1000.0 * static_cast<double>(qpc() - held0) / static_cast<double>(frequency.QuadPart);
    sedici::live_set_limiter_fps(1000);
    limiter_on_config_changed();
    const uint64_t reseed0 = qpc();
    limiter_before_present(qpc());
    const double reseedMs = 1000.0 * static_cast<double>(qpc() - reseed0) / static_cast<double>(frequency.QuadPart);
    check(heldMs >= 80.0 && reseedMs < 20.0,
          "limiter: a live rate change reseeds instead of waiting out the old interval");
    sedici::live_set_limiter_fps(0);
    check(static_cast<double>(after - before) / static_cast<double>(frequency.QuadPart) >= 0.0005,
          "live limiter actually delays Present after silent uncapped startup");
}

// The deadline decision with controlled time: no clock, no waits.
void limiter_policy_checks()
{
    const uint64_t interval = 100;
    auto step = sedici::limiter_step(1000, 0, false, interval);
    check(!step.wait && step.deadline == 1000, "limiter: the first frame seeds the schedule and never waits");

    // Steady cadence: frames arrive early every time; the deadline advances by
    // exactly one interval, so the average rate is the requested one.
    uint64_t deadline = step.deadline, waitedTotal = 0, now = 1000;
    bool exact = true;
    for (int frame = 0; frame < 1000; ++frame) {
        now += 40; // the game finishes a frame in 40 ticks
        const auto next = sedici::limiter_step(now, deadline, true, interval);
        exact = exact && next.deadline == deadline + interval && next.wait && !next.late;
        waitedTotal += next.deadline - now;
        now = next.deadline; // the limiter waits until the deadline
        deadline = next.deadline;
    }
    check(exact && deadline == 1000 + 1000 * interval && waitedTotal == 1000 * 60,
          "limiter: an early game is held to exactly one interval per frame");

    // A late frame never waits and the schedule catches up without a burst.
    step = sedici::limiter_step(deadline + interval + 10, deadline, true, interval);
    check(step.late && !step.wait && !step.reset && step.deadline == deadline + interval,
          "limiter: a late frame does not wait and keeps the schedule");
    // More than one full interval late reseeds at now instead of bursting.
    step = sedici::limiter_step(deadline + 5 * interval, deadline, true, interval);
    check(step.reset && !step.wait && step.deadline == deadline + 5 * interval,
          "limiter: a stall of several intervals reseeds the schedule (no catch-up burst)");
    // Overload: the game is slower than the cap; the limiter never waits.
    deadline = 5000;
    bool noWait = true;
    uint64_t t = 5000;
    for (int frame = 0; frame < 200; ++frame) {
        t += 150;
        const auto next = sedici::limiter_step(t, deadline, true, interval);
        noWait = noWait && !next.wait;
        deadline = next.deadline;
    }
    check(noWait, "limiter: a game slower than the cap is never delayed");
    // A zero interval (rate change in flight) degrades to no wait.
    step = sedici::limiter_step(10, 5, true, 0);
    check(!step.wait, "limiter: a zero interval never waits");
}
// P2.10(b): the log must never stall, flood or silently lose the caller, whatever the
// disk does. Everything here writes to a scratch directory, never to a game folder.
std::string slurp(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

size_t occurrences(const std::string& text, const std::string& needle)
{
    size_t n = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) ++n;
    return n;
}

std::string letters(int i)
{
    std::string s = "distinct line ";
    do { s.push_back(static_cast<char>('a' + i % 26)); i /= 26; } while (i);
    return s; // digits are ignored by the rate limiter, so only letters make lines distinct
}

void log_sink_checks()
{
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / ("sedici-logcheck-" + std::to_string(GetCurrentProcessId()));
    fs::create_directories(dir, ec);
    const fs::path file = dir / L"sedici.log";
    sedici::log_test_set_directory(dir);
    sedici::log_test_reset_rate_limits();
    using Clock = std::chrono::steady_clock;
    const auto micros = [](Clock::time_point a, Clock::time_point b) {
        return std::chrono::duration<double, std::micro>(b - a).count();
    };

    // Ordering and the cost of an accepted line on a healthy disk.
    std::vector<double> costs;
    for (int i = 0; i < 200; ++i) {
        const auto t0 = Clock::now();
        log_error(letters(i));
        costs.push_back(micros(t0, Clock::now()));
    }
    check(sedici::log_flush(5000), "log: the queue drains");
    std::sort(costs.begin(), costs.end());
    double sum = 0;
    for (double c : costs) sum += c;
    std::printf("INFO log overhead: %.1f us average, %.1f us p99 per accepted line\n",
                sum / costs.size(), costs[costs.size() * 99 / 100]);
    const std::string healthy = slurp(file);
    check(occurrences(healthy, "distinct line") == 200, "log: every line reached the file");
    check(healthy.find(letters(0)) < healthy.find(letters(1)), "log: lines keep their order");
    check(sum / costs.size() < 50.0, "log: an accepted line costs the caller under 50 us on average");

    // A message repeated in a tight loop is rate limited, counted, and the next line
    // that gets through says how many were suppressed.
    const auto before = sedici::log_stats();
    for (int i = 0; i < 1000; ++i) log_error("repeat storm " + std::to_string(i));
    check(sedici::log_flush(5000), "log: the storm drains");
    const std::string storm = slurp(file);
    const auto stormStats = sedici::log_stats();
    check(occurrences(storm, "repeat storm") <= 9, "log: a repeated message is rate limited");
    check(stormStats.suppressed - before.suppressed >= 990, "log: suppressed repeats are counted");
    Sleep(700); // one token refills at 2 per second
    log_error("repeat storm " + std::to_string(1001));
    check(sedici::log_flush(5000), "log: the follow-up drains");
    check(slurp(file).find("similar suppressed]") != std::string::npos,
          "log: the next admitted repeat reports how many were suppressed");

    // A slow disk: the caller is never held, the ring is bounded, drops are counted
    // and reported once the disk keeps up again.
    sedici::log_test_reset_rate_limits();
    sedici::log_test_set_sink_fault(sedici::LogSinkFault::Slow, 150);
    const auto slowBefore = sedici::log_stats();
    double worst = 0;
    const auto burstStart = Clock::now();
    for (int i = 0; i < 3000; ++i) {
        const auto t0 = Clock::now();
        log_error(letters(10000 + i));
        worst = std::max(worst, micros(t0, Clock::now()));
    }
    const double burstMs = micros(burstStart, Clock::now()) / 1000.0;
    const auto slowStats = sedici::log_stats();
    std::printf("INFO log with a 150 ms sink: 3000 lines in %.1f ms, worst call %.0f us, max depth %u, dropped %llu\n",
                burstMs, worst, slowStats.max_depth,
                static_cast<unsigned long long>(slowStats.dropped - slowBefore.dropped));
    check(worst < 20000.0, "log: a stalled disk never holds the caller (worst call under 20 ms)");
    check(slowStats.max_depth <= 256, "log: the queue is bounded");
    check(slowStats.dropped > slowBefore.dropped, "log: lines refused by a full queue are counted");
    sedici::log_test_set_sink_fault(sedici::LogSinkFault::None, 0);
    check(sedici::log_flush(20000), "log: the slow sink drains once it recovers");
    log_error("after the slow disk");
    check(sedici::log_flush(5000), "log: the report line drains");
    check(slurp(file).find("line(s) were dropped because the queue was full") != std::string::npos,
          "log: dropped lines are reported in the log");

    // A file that cannot be written: the caller is unaffected, the failure is
    // visible in the counters, and the log says what was lost when it comes back.
    sedici::log_test_reset_rate_limits();
    sedici::log_test_set_sink_fault(sedici::LogSinkFault::FailOpen, 0);
    const auto failBefore = sedici::log_stats();
    const auto failStart = Clock::now();
    for (int i = 0; i < 20; ++i) log_error(letters(20000 + i));
    check(micros(failStart, Clock::now()) < 20000.0, "log: a dead file never holds the caller");
    for (int i = 0; i < 100 && sedici::log_stats().sink_failures == failBefore.sink_failures; ++i) Sleep(20);
    const auto failed = sedici::log_stats();
    check(failed.sink_failures > failBefore.sink_failures && !failed.sink_ok,
          "log: a failed write is counted and the sink is marked unavailable");
    sedici::log_test_set_sink_fault(sedici::LogSinkFault::None, 0);
    log_error("after the outage");
    check(sedici::log_flush(5000), "log: the outage ends");
    const auto recovered = sedici::log_stats();
    const std::string afterOutage = slurp(file);
    check(recovered.sink_ok && recovered.lost > failBefore.lost, "log: the sink recovers and the lost lines are counted");
    check(afterOutage.find("the file was unwritable") != std::string::npos &&
          afterOutage.find("after the outage") != std::string::npos,
          "log: the outage and its loss are reported once the file is writable");

    sedici::log_test_set_directory({});
    fs::remove_all(dir, ec);
}

// K83: a worker blocked in the filesystem must not leave the menu waiting. The block
// event stands in for the hung call; nothing here touches a real file.
void settings_io_stall_checks()
{
    sedici::settings_io_test_set_stall_ms(300);
    HANDLE hang = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    sedici::settings_io_test_set_block(hang);
    const uint64_t stallsBefore = sedici::settings_io_stall_count();
    check(sedici::settings_io_start_begin(1, 41), "settings I/O: a job starts");
    check(sedici::settings_io_busy(), "settings I/O: the job is running");

    sedici::SettingsIoCompletion done;
    bool reported = false;
    const auto begin = std::chrono::steady_clock::now();
    while (!reported && std::chrono::steady_clock::now() - begin < std::chrono::seconds(3)) {
        reported = sedici::settings_io_poll(done);
        if (!reported) Sleep(10);
    }
    const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - begin).count();
    check(reported && done.stalled && done.viewEpoch == 41 && done.kind == sedici::SettingsIoKind::BeginEdit &&
          !done.save.message.empty(), "settings I/O: a hung job is reported as stalled with its epoch");
    check(waited >= 250 && waited < 1500, "settings I/O: the stall is reported after the limit, not before");
    check(sedici::settings_io_busy() && sedici::settings_io_stalled(),
          "settings I/O: the worker is still running and is marked stalled");
    check(!sedici::settings_io_start_begin(2, 42), "settings I/O: no second worker starts beside a stalled one");
    sedici::SettingsIoCompletion again;
    check(!sedici::settings_io_poll(again), "settings I/O: the stall is reported once");
    check(sedici::settings_io_stall_count() == stallsBefore + 1, "settings I/O: the stall is counted");
    const sedici::DiagSnapshot view = sedici::diag_snapshot();
    check(view.settings_io_stalls == stallsBefore + 1 && view.settings_io_stalled_now &&
          sedici::diag_notable_line().find("stalled_now=yes") != std::string::npos,
          "settings I/O: the diagnostics snapshot shows the stalled worker");

    // The filesystem recovers: the worker ends, its late result is discarded, and a
    // fresh job runs normally.
    sedici::settings_io_test_set_block(nullptr);
    SetEvent(hang);
    const auto idleStart = std::chrono::steady_clock::now();
    while (sedici::settings_io_busy() && std::chrono::steady_clock::now() - idleStart < std::chrono::seconds(5))
        Sleep(5);
    check(!sedici::settings_io_busy() && !sedici::settings_io_stalled(), "settings I/O: the worker exits once the call returns");
    check(!sedici::settings_io_poll(again), "settings I/O: the stalled job's late result is discarded");
    check(sedici::settings_io_start_begin(3, 43), "settings I/O: a new job starts after recovery");
    bool finished = false;
    const auto freshStart = std::chrono::steady_clock::now();
    while (!finished && std::chrono::steady_clock::now() - freshStart < std::chrono::seconds(5)) {
        finished = sedici::settings_io_poll(again);
        if (!finished) Sleep(5);
    }
    check(finished && !again.stalled && again.viewEpoch == 43, "settings I/O: the new job completes normally");
    sedici::settings_io_test_set_stall_ms(sedici::kSettingsIoStallMs);
    CloseHandle(hang);
}

} // namespace

int main(int argc, char** argv)
{
    const auto config = dll_directory() / L"sedici.toml";
    const auto fixture = dll_directory() / L"patch-fixture.toml";
    if (std::filesystem::exists(config) || std::filesystem::exists(fixture)) {
        std::puts("Refusing to overwrite existing regression fixtures");
        return 1;
    }
    try {
        for (const auto* name : {L"SEDICI_LIMIT_FPS", L"MFSRFG_LOG"})
            SetEnvironmentVariableW(name, nullptr);
        const std::string mode = argc > 1 ? argv[1] : "";
        if (mode == "settings-env") {
            SetEnvironmentVariableW(L"SEDICI_LIMIT_FPS", L"45");
            SetEnvironmentVariableW(L"MFSRFG_LOG", L"2");
        }
        if (mode == "settings-sedici") {
            write(config, R"toml([log]
level = 0
[limiter]
fps = 0
)toml");
        } else {
        write(config, "[log]\nlevel = 0\n[limiter]\nfps = 0\n");
        }
        (void)sedici::settings();
        if (mode == "settings-sedici" || mode == "settings-env") {
            settings_capability_checks(mode == "settings-sedici", mode == "settings-env");
        } else if (argc > 1) {
            if (mode == "observer") observer_cost();
            else gpu_checks(mode == "gpu-full");
        } else {
            config_checks(fixture);
            settings_checks(fixture);
            token_namespace_checks();
            retirement_policy_checks();
            queue_policy_checks();
            const POINT mapped = sedici::ui_input_map_point(-10, -20, 640, 360, 1280, 720);
            check(mapped.x == 0 && mapped.y == 0, "negative client coordinates map to the near edge");
            try { sedici::imgui_assert_failed("injected assertion"); }
            catch (const std::runtime_error&) { check(true, "ImGui assertion throws instead of aborting game"); }
            log_sink_checks();
            settings_io_stall_checks();
            limiter_policy_checks();
            limiter_live_checks();
            window_checks();
        }
    } catch (const std::exception& error) {
        check(false, error.what());
    }
    std::filesystem::remove(config);
    std::filesystem::remove(fixture);
    std::printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
