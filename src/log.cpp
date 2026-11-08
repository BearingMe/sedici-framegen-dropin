#include "log.h"

#include "live.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kLogFileName[] = L"sedici.log";

// Ring geometry. 256 lines of up to 511 bytes is 128 KB, allocated with the state.
constexpr size_t kSlots = 256;
constexpr size_t kLineMax = 512;

// Per-message rate limit: a burst of kBurst lines, then kRefillPerSecond. Digits
// are ignored when comparing messages, so "frame 7" and "frame 8" are one message.
constexpr double kBurst = 8.0;
constexpr double kRefillPerSecond = 2.0;
constexpr size_t kBuckets = 32;
constexpr size_t kKeyChars = 96;

std::filesystem::path module_directory()
{
    try {
        HMODULE module = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR>(&module_directory), &module)) {
            return {};
        }

        std::vector<wchar_t> buffer(512);
        for (;;) {
            const DWORD length = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (!length) return {};
            if (length < buffer.size()) {
                return std::filesystem::path(buffer.data(), buffer.data() + length).parent_path();
            }
            if (buffer.size() >= 32768) return {};
            buffer.resize(buffer.size() * 2);
        }
    } catch (...) {
        return {};
    }
}

double elapsed_ms() noexcept
{
    static const auto start = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(now - start).count();
}

struct Slot {
    char text[kLineMax];
    uint16_t length = 0;
    double ms = 0.0;
};

struct Bucket {
    uint64_t key = 0;
    double tokens = kBurst;
    double last = 0.0;
    uint32_t suppressed = 0;
};

// Heap allocated and never freed: the writer thread may still be running while
// static destructors execute at process exit.
struct State {
    std::mutex mutex;
    std::condition_variable wake;  // the writer waits here
    std::condition_variable idle;  // log_flush waits here
    Slot ring[kSlots];
    size_t head = 0;
    size_t count = 0;
    bool writing = false;
    bool writer_running = false; // a writer thread exists (guarded by mutex)
    bool outage_reported = false;
    uint64_t dropped_unreported = 0;
    uint64_t lost_unreported = 0;
    uint64_t failures_in_outage = 0;
    sedici::LogStats stats;
    Bucket buckets[kBuckets];
    std::filesystem::path directory_override;
    sedici::LogSinkFault fault = sedici::LogSinkFault::None;
    unsigned fault_delay_ms = 0;
};

State& state()
{
    static State* instance = new State;
    return *instance;
}

uint64_t message_key(std::string_view message) noexcept
{
    uint64_t hash = 1469598103934665603ull;
    size_t taken = 0;
    for (const char c : message) {
        if (taken++ >= kKeyChars) break;
        if (c >= '0' && c <= '9') continue;
        hash ^= static_cast<unsigned char>(c);
        hash *= 1099511628211ull;
    }
    return hash ? hash : 1;
}

// Caller holds the state mutex. Returns true to admit the line; `repeats` receives
// how many similar lines were suppressed since the last admitted one.
bool rate_limit(State& s, std::string_view message, double now_ms, uint32_t& repeats) noexcept
{
    repeats = 0;
    const uint64_t key = message_key(message);
    Bucket& bucket = s.buckets[key % kBuckets];
    if (bucket.key != key) {
        bucket = Bucket{};
        bucket.key = key;
        bucket.last = now_ms;
    }
    bucket.tokens = std::min(kBurst, bucket.tokens + (now_ms - bucket.last) * (kRefillPerSecond / 1000.0));
    bucket.last = now_ms;
    if (bucket.tokens < 1.0) {
        ++bucket.suppressed;
        ++s.stats.suppressed;
        return false;
    }
    bucket.tokens -= 1.0;
    repeats = bucket.suppressed;
    bucket.suppressed = 0;
    return true;
}

// Appends "[time ms] text" for one slot.
void format_slot(std::string& out, const Slot& slot)
{
    char stamp[48];
    const int n = std::snprintf(stamp, sizeof(stamp), "[%.3f ms] ", slot.ms);
    out.append(stamp, n > 0 ? static_cast<size_t>(n) : 0);
    out.append(slot.text, slot.length);
    out.push_back('\n');
}

// Writes `text` to the log file (or simulates the configured fault). Called without
// the state mutex; runs on the writer thread, never on a game thread unless the
// writer could not be started.
bool write_text(const std::string& text) noexcept
{
    unsigned delay = 0;
    bool failOpen = false;
    std::filesystem::path override_directory;
    {
        State& s = state();
        std::lock_guard lock(s.mutex);
        delay = s.fault == sedici::LogSinkFault::Slow ? s.fault_delay_ms : 0;
        failOpen = s.fault == sedici::LogSinkFault::FailOpen;
        override_directory = s.directory_override;
    }
    if (delay) Sleep(delay);
    if (failOpen) return false;
    try {
        const auto directory = override_directory.empty() ? module_directory() : override_directory;
        if (directory.empty()) return false;
        std::ofstream file(directory / kLogFileName, std::ios::app | std::ios::binary);
        if (!file) return false;
        file << text;
        file.flush();
        return static_cast<bool>(file);
    } catch (...) {
        return false;
    }
}

// Takes up to `limit` lines. Caller holds the state mutex.
std::string take_batch(State& s, size_t limit, size_t& taken)
{
    std::string out;
    taken = 0;
    while (s.count > 0 && taken < limit) {
        format_slot(out, s.ring[s.head]);
        s.head = (s.head + 1) % kSlots;
        --s.count;
        ++taken;
    }
    return out;
}

// Writes one batch and updates the sink accounting. Called without the mutex.
void flush_batch(const std::string& text, size_t lines) noexcept
{
    State& s = state();
    std::string prefix;
    uint64_t droppedIncluded = 0, lostIncluded = 0;
    {
        std::lock_guard lock(s.mutex);
        droppedIncluded = s.dropped_unreported;
        lostIncluded = s.outage_reported ? s.lost_unreported : 0;
        if (s.dropped_unreported) {
            char buffer[160];
            std::snprintf(buffer, sizeof(buffer), "[%.3f ms] log: %llu line(s) were dropped because the queue was full\n",
                          elapsed_ms(), static_cast<unsigned long long>(s.dropped_unreported));
            prefix += buffer;
        }
        if (s.lost_unreported && s.outage_reported) {
            char buffer[200];
            std::snprintf(buffer, sizeof(buffer),
                          "[%.3f ms] log: the file was unwritable (%llu failed write(s)); %llu line(s) were lost\n",
                          elapsed_ms(), static_cast<unsigned long long>(s.failures_in_outage),
                          static_cast<unsigned long long>(s.lost_unreported));
            prefix += buffer;
        }
    }
    const bool ok = write_text(prefix + text);
    std::lock_guard lock(s.mutex);
    if (ok) {
        s.stats.written += lines;
        s.stats.sink_ok = true;
        // Subtract only what this batch reported: more lines may have been dropped
        // while it was being written, and those belong to the next report.
        s.dropped_unreported -= std::min(s.dropped_unreported, droppedIncluded);
        s.lost_unreported -= std::min(s.lost_unreported, lostIncluded);
        s.failures_in_outage = 0;
        s.outage_reported = false;
    } else {
        ++s.stats.sink_failures;
        ++s.failures_in_outage;
        s.stats.lost += lines;
        s.lost_unreported += lines;
        s.stats.sink_ok = false;
        if (!s.outage_reported) {
            s.outage_reported = true;
            // The file is the thing that failed, so say it where a debugger can see it.
            OutputDebugStringA("sedici: sedici.log cannot be written; log lines are being discarded\n");
        }
    }
}

// How long an idle writer keeps its thread (and its reference to this module)
// before exiting. Bursts of lines share one thread; a quiet log holds nothing.
constexpr std::chrono::milliseconds kWriterLinger{500};

DWORD WINAPI writer_thread(LPVOID self) noexcept
{
    State& s = state();
    SetThreadDescription(GetCurrentThread(), L"Sedici log");
    for (;;) {
        std::string text;
        size_t lines = 0;
        {
            std::unique_lock lock(s.mutex);
            if (!s.wake.wait_for(lock, kWriterLinger, [&] { return s.count > 0; })) {
                // Idle: leave under the same lock enqueue() uses to decide whether a
                // writer exists, then give back the module reference this thread
                // took. If that was the last reference the DLL unloads here, on a
                // thread that is not running any of its code afterwards.
                s.writer_running = false;
                s.idle.notify_all();
                lock.unlock();
                FreeLibraryAndExitThread(static_cast<HMODULE>(self), 0);
            }
            s.writing = true;
            text = take_batch(s, 64, lines);
        }
        flush_batch(text, lines);
        bool sinkOk = true;
        {
            std::lock_guard lock(s.mutex);
            s.writing = false;
            sinkOk = s.stats.sink_ok;
            if (s.count == 0) s.idle.notify_all();
        }
        if (!sinkOk) Sleep(100); // do not spin against a dead file
    }
}

// Creates the writer thread. It holds a reference to this module for as long as it
// runs, so a FreeLibrary cannot pull the code out from under it. CreateThread, not
// std::thread: no heap allocation on the caller's path.
bool start_writer() noexcept
{
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&writer_thread), &self))
        return false;
    HANDLE thread = CreateThread(nullptr, 0, &writer_thread, self, 0, nullptr);
    if (!thread) {
        FreeLibrary(self);
        return false;
    }
    CloseHandle(thread);
    return true;
}

void enqueue(std::string_view message, bool limited) noexcept
{
    try {
        State& s = state();
        const double now = elapsed_ms();
        bool needWriter = false;
        {
            std::lock_guard lock(s.mutex);
            uint32_t repeats = 0;
            if (limited && !rate_limit(s, message, now, repeats)) return;
            if (s.count == kSlots) {
                ++s.stats.dropped;
                ++s.dropped_unreported;
                return;
            }
            Slot& slot = s.ring[(s.head + s.count) % kSlots];
            size_t length = std::min(message.size(), kLineMax - 1);
            std::memcpy(slot.text, message.data(), length);
            for (size_t i = 0; i < length; ++i)
                if (slot.text[i] == '\n' || slot.text[i] == '\r') slot.text[i] = ' ';
            if (repeats) {
                char note[48];
                const int n = std::snprintf(note, sizeof(note), " [+%u similar suppressed]", repeats);
                const size_t room = kLineMax - 1 - length;
                if (n > 0) {
                    const size_t add = std::min(static_cast<size_t>(n), room);
                    std::memcpy(slot.text + length, note, add);
                    length += add;
                }
            }
            slot.length = static_cast<uint16_t>(length);
            slot.ms = now;
            ++s.count;
            ++s.stats.queued;
            s.stats.depth = static_cast<uint32_t>(s.count);
            s.stats.max_depth = std::max(s.stats.max_depth, s.stats.depth);
            needWriter = !s.writer_running;
            s.writer_running = true;
        }
        if (needWriter && !start_writer()) {
            // No thread could be created: drain on this thread rather than lose the
            // lines, and let the next call try again.
            size_t lines = 0;
            std::string text;
            {
                std::lock_guard lock(s.mutex);
                s.writer_running = false;
                text = take_batch(s, kSlots, lines);
            }
            flush_batch(text, lines);
            return;
        }
        s.wake.notify_one();
    } catch (...) {
    }
}

} // namespace

std::filesystem::path dll_directory()
{
    static const std::filesystem::path directory = module_directory();
    return directory;
}

int log_level() noexcept
{
    // Initialized from `[log] level`/MFSRFG_LOG by settings(), then updated
    // live by the overlay. Reading an atomic keeps the hot path allocation-free.
    return sedici::live_log_level();
}

void log_error(std::string_view message) noexcept
{
    enqueue(message, true);
}

void log_raw(std::string_view message) noexcept
{
    enqueue(message, false);
    (void)sedici::log_flush(2000);
}

void log_event(int level, std::string_view message) noexcept
{
    if (log_level() >= level) enqueue(message, true);
}

namespace sedici {

LogStats log_stats() noexcept
{
    try {
        State& s = state();
        std::lock_guard lock(s.mutex);
        LogStats copy = s.stats;
        copy.depth = static_cast<uint32_t>(s.count);
        return copy;
    } catch (...) {
        return {};
    }
}

bool log_flush(unsigned timeout_ms) noexcept
{
    try {
        State& s = state();
        std::unique_lock lock(s.mutex);
        return s.idle.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                               [&] { return s.count == 0 && !s.writing; });
    } catch (...) {
        return false;
    }
}

void log_drain_at_exit() noexcept
{
    try {
        State& s = state();
        std::unique_lock lock(s.mutex, std::try_to_lock);
        if (!lock.owns_lock() || s.count == 0) return;
        size_t lines = 0;
        const std::string text = take_batch(s, kSlots, lines);
        lock.unlock();
        (void)write_text(text);
    } catch (...) {
    }
}

void log_test_set_directory(const std::filesystem::path& directory)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    s.directory_override = directory;
}

void log_test_set_sink_fault(LogSinkFault fault, unsigned delay_ms) noexcept
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    s.fault = fault;
    s.fault_delay_ms = delay_ms;
}

void log_test_reset_rate_limits() noexcept
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    for (auto& bucket : s.buckets) bucket = Bucket{};
}

} // namespace sedici
