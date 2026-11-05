#include "log.h"

#include <windows.h>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kLogFileName[] = L"sedici.log";

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

double elapsed_ms()
{
    static const auto start = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(now - start).count();
}

void write_line(std::string_view message) noexcept
{
    try {
        const auto directory = module_directory();
        if (directory.empty()) return;

        static std::mutex mutex;
        std::lock_guard<std::mutex> lock(mutex);

        std::ofstream file(directory / kLogFileName, std::ios::app | std::ios::binary);
        if (!file) return;

        std::ostringstream out;
        out << '[' << std::fixed << std::setprecision(3) << elapsed_ms() << " ms] ";
        for (char c : message) out.put(c == '\n' || c == '\r' ? ' ' : c);
        out << '\n';
        file << out.str();
        file.flush();
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
    static const int level = [] {
        char value[8]{};
        const DWORD length = GetEnvironmentVariableA("MFSRFG_LOG", value, sizeof(value));
        if (length == 1 && value[0] >= '0' && value[0] <= '2') return value[0] - '0';
        return 1;
    }();
    return level;
}

void log_error(std::string_view message) noexcept
{
    write_line(message);
}

void log_event(int level, std::string_view message) noexcept
{
    if (log_level() >= level) write_line(message);
}
