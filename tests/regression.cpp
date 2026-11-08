#include "config.h"
#include "config_patch.h"
#include "fg_token_namespace.h"
#include "fg_retirement_policy.h"
#include "fg_queue_policy.h"
#include "fg_slot_policy.h"
#include "imgui_config.h"
#include "limiter.h"
#include "live.h"
#include "log.h"
#include "overlay.h"
#include "settings_tx.h"
#include "settings_io.h"
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
        sedici::ui_on_present(&params);
        check(sedici::live_ui_mode() != sedici::UiMode::Off, "GPU overlay remains enabled");
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
    sedici::live_set_limiter_fps(0);
    check(static_cast<double>(after - before) / static_cast<double>(frequency.QuadPart) >= 0.0005,
          "live limiter actually delays Present after silent uncapped startup");
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
            gpu_checks(mode == "gpu-full");
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
