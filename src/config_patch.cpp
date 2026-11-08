#include "config_patch.h"

#include <windows.h>
#include <bcrypt.h>
#include "toml.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <locale>
#include <stdexcept>
#include <sstream>
#include <string_view>
#include <vector>

namespace sedici {
namespace {

std::atomic<ConfigBeforeReplaceHook> g_before_replace_hook{nullptr};

struct Line {
    std::string text;
    std::string eol;
};

std::string trim(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
    return std::string(text);
}

std::string format_number(double value)
{
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::fixed << std::setprecision(3) << value;
    std::string text = out.str();
    while (!text.empty() && text.back() == '0') text.pop_back();
    if (!text.empty() && text.back() == '.') text.pop_back();
    return text;
}

double parse_number(const std::string& text)
{
    std::istringstream stream(text);
    stream.imbue(std::locale::classic());
    double value = 0;
    if (!(stream >> value)) throw std::runtime_error("invalid numeric setting");
    return value;
}

std::vector<Line> split_lines(const std::string& text)
{
    std::vector<Line> lines;
    size_t start = 0;
    while (start < text.size()) {
        const size_t newline = text.find('\n', start);
        const size_t end = newline == std::string::npos ? text.size() : newline;
        Line line;
        line.text = text.substr(start, end - start);
        if (!line.text.empty() && line.text.back() == '\r') {
            line.text.pop_back();
            line.eol = "\r\n";
        } else if (newline != std::string::npos) {
            line.eol = "\n";
        }
        lines.push_back(std::move(line));
        if (newline == std::string::npos) break;
        start = newline + 1;
    }
    return lines;
}

std::string join_lines(const std::vector<Line>& lines)
{
    std::string text;
    for (const Line& line : lines) {
        text += line.text;
        text += line.eol;
    }
    return text;
}

std::string dominant_eol(const std::string& text)
{
    return text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
}

size_t find_comment(std::string_view line)
{
    bool in_single = false;
    bool in_double = false;
    bool escaped = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (in_double && c == '\\' && !escaped) {
            escaped = true;
            continue;
        }
        if (c == '"' && !in_single && !escaped) in_double = !in_double;
        if (c == '\'' && !in_double) in_single = !in_single;
        if (c == '#' && !in_single && !in_double) return i;
        escaped = false;
    }
    return std::string_view::npos;
}

std::string section_name(std::string_view line)
{
    const size_t comment = find_comment(line);
    const std::string code = trim(line.substr(0, comment));
    if (code.size() < 3 || code.front() != '[' || code.back() != ']') return {};
    if (code.size() >= 4 && code[1] == '[') return {}; // array of tables
    return trim(std::string_view(code).substr(1, code.size() - 2));
}

bool replace_key(Line& line, std::string_view key, std::string_view value)
{
    const size_t comment = find_comment(line.text);
    const std::string_view code(line.text.data(), comment == std::string::npos
                                                    ? line.text.size()
                                                    : comment);
    const size_t equals = code.find('=');
    if (equals == std::string_view::npos) return false;
    if (trim(code.substr(0, equals)) != key) return false;

    size_t value_start = equals + 1;
    while (value_start < code.size() && (code[value_start] == ' ' || code[value_start] == '\t')) {
        ++value_start;
    }
    if (value_start >= code.size()) {
        std::string out;
        out.append(code.substr(0, equals + 1));
        out += ' ';
        out.append(value);
        if (comment != std::string::npos) out.append(line.text.substr(comment));
        line.text = std::move(out);
        return true;
    }

    size_t value_end = code.size();
    while (value_end > value_start &&
           (code[value_end - 1] == ' ' || code[value_end - 1] == '\t')) {
        --value_end;
    }

    std::string out;
    out.append(code.substr(0, value_start));
    out.append(value);
    out.append(code.substr(value_end));
    if (comment != std::string::npos) out.append(line.text.substr(comment));
    line.text = std::move(out);
    return true;
}

bool find_section(const std::vector<Line>& lines, std::string_view name, size_t& begin,
                  size_t& end)
{
    begin = lines.size();
    end = lines.size();
    for (size_t i = 0; i < lines.size(); ++i) {
        if (section_name(lines[i].text) == name) {
            begin = i;
            end = lines.size();
            for (size_t j = i + 1; j < lines.size(); ++j) {
                const std::string code = trim(std::string_view(lines[j].text).substr(
                    0, find_comment(lines[j].text)));
                if (!code.empty() && code.front() == '[') {
                    end = j;
                    break;
                }
            }
            return true;
        }
    }
    return false;
}

void apply_value(std::vector<Line>& lines, std::string_view section, std::string_view key,
                 std::string_view value, const std::string& eol)
{
    size_t begin = 0;
    size_t end = 0;
    if (!find_section(lines, section, begin, end)) {
        if (!lines.empty() && lines.back().eol.empty()) lines.back().eol = eol;
        if (!lines.empty() && !lines.back().text.empty()) lines.push_back(Line{"", eol});
        lines.push_back(Line{"[" + std::string(section) + "]", eol});
        lines.push_back(Line{std::string(key) + " = " + std::string(value), eol});
        return;
    }

    for (size_t i = begin + 1; i < end; ++i) {
        if (replace_key(lines[i], key, value)) return;
    }

    if (end > 0 && lines[end - 1].eol.empty()) lines[end - 1].eol = eol;

    size_t insert = end;
    while (insert > begin + 1 && trim(lines[insert - 1].text).empty()) --insert;
    if (insert > 0 && lines[insert - 1].eol.empty()) lines[insert - 1].eol = eol;
    lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(insert),
                 Line{std::string(key) + " = " + std::string(value), eol});
}

std::string quoted(std::string_view value)
{
    return "\"" + std::string(value) + "\"";
}

} // namespace

ConfigFileStamp config_file_stamp(const std::filesystem::path& path) noexcept
{
    ConfigFileStamp stamp;
    try {
        struct FileGuard {
            HANDLE value = INVALID_HANDLE_VALUE;
            ~FileGuard() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
        } file;
        file.value = CreateFileW(path.c_str(), GENERIC_READ,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (file.value == INVALID_HANDLE_VALUE) {
            const DWORD error = GetLastError();
            stamp.valid = error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
            return stamp;
        }
        BY_HANDLE_FILE_INFORMATION before{}, after{};
        if (!GetFileInformationByHandle(file.value, &before) ||
            (before.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return stamp;

        struct AlgorithmGuard {
            BCRYPT_ALG_HANDLE value = nullptr;
            ~AlgorithmGuard() { if (value) BCryptCloseAlgorithmProvider(value, 0); }
        } algorithm;
        if (BCryptOpenAlgorithmProvider(&algorithm.value, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
            return stamp;
        DWORD objectBytes = 0, received = 0;
        if (BCryptGetProperty(algorithm.value, BCRYPT_OBJECT_LENGTH,
                              reinterpret_cast<PUCHAR>(&objectBytes), sizeof(objectBytes), &received, 0) < 0 ||
            received != sizeof(objectBytes) || objectBytes == 0) return stamp;
        std::vector<unsigned char> object(objectBytes);
        struct HashGuard {
            BCRYPT_HASH_HANDLE value = nullptr;
            ~HashGuard() { if (value) BCryptDestroyHash(value); }
        } hash;
        if (BCryptCreateHash(algorithm.value, &hash.value, object.data(), objectBytes, nullptr, 0, 0) < 0)
            return stamp;
        unsigned char buffer[16384];
        uint64_t total = 0;
        for (;;) {
            DWORD count = 0;
            if (!ReadFile(file.value, buffer, sizeof(buffer), &count, nullptr)) return stamp;
            if (count == 0) break;
            if (BCryptHashData(hash.value, buffer, count, 0) < 0) return stamp;
            total += count;
        }
        if (BCryptFinishHash(hash.value, stamp.sha256.data(),
                             static_cast<ULONG>(stamp.sha256.size()), 0) < 0 ||
            !GetFileInformationByHandle(file.value, &after)) return ConfigFileStamp{};
        const uint64_t size = (static_cast<uint64_t>(before.nFileSizeHigh) << 32) | before.nFileSizeLow;
        if (total != size || before.nFileSizeHigh != after.nFileSizeHigh ||
            before.nFileSizeLow != after.nFileSizeLow ||
            before.ftLastWriteTime.dwHighDateTime != after.ftLastWriteTime.dwHighDateTime ||
            before.ftLastWriteTime.dwLowDateTime != after.ftLastWriteTime.dwLowDateTime ||
            before.dwVolumeSerialNumber != after.dwVolumeSerialNumber ||
            before.nFileIndexHigh != after.nFileIndexHigh || before.nFileIndexLow != after.nFileIndexLow)
            return ConfigFileStamp{};
        stamp.exists = true;
        stamp.valid = true;
        stamp.size = size;
        stamp.last_write = (static_cast<uint64_t>(before.ftLastWriteTime.dwHighDateTime) << 32) |
                           before.ftLastWriteTime.dwLowDateTime;
        stamp.file_id = (static_cast<uint64_t>(before.nFileIndexHigh) << 32) | before.nFileIndexLow;
        stamp.volume_serial = before.dwVolumeSerialNumber;
    } catch (...) {
        return ConfigFileStamp{};
    }
    return stamp;
}

bool config_same_stamp(const ConfigFileStamp& a, const ConfigFileStamp& b) noexcept
{
    if (!a.valid || !b.valid || a.exists != b.exists) return false;
    if (!a.exists) return true;
    return a.last_write == b.last_write && a.size == b.size && a.file_id == b.file_id &&
           a.volume_serial == b.volume_serial && a.sha256 == b.sha256;
}

void config_arm_before_replace_hook(ConfigBeforeReplaceHook hook) noexcept
{
    g_before_replace_hook.store(hook, std::memory_order_release);
}

bool config_save(const std::filesystem::path& path, const ConfigValues& values,
                 std::string& error, const ConfigFileStamp* expectedStamp, bool* conflict,
                 ConfigFileStamp* writtenStamp)
{
    if (conflict) *conflict = false;
    try {
        std::string text;
        const ConfigFileStamp initial = config_file_stamp(path);
        if (!initial.valid) {
            error = "could not determine the state of sedici.toml";
            return false;
        }
        if (expectedStamp && !config_same_stamp(initial, *expectedStamp)) {
            if (conflict) *conflict = true;
            error = "sedici.toml changed before save";
            return false;
        }
        if (initial.exists) {
            std::ifstream stream(path, std::ios::binary);
            if (!stream) {
                error = "could not open sedici.toml for reading";
                return false;
            }
            text.assign(std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{});
        }

        const std::string eol = dominant_eol(text);
        std::vector<Line> lines = split_lines(text);
        // Validate with the real parser, not the line patcher's limited syntax.
        // Comparing the complete result also protects unrelated multiline text,
        // quoted/dotted keys, arrays of tables, and case-sensitive table names.
        toml::table expected = toml::parse(text);
        const auto expect = [&](std::string_view section, std::string_view key, auto value) {
            if (!expected.contains(section)) expected.insert(section, toml::table{});
            auto* table = expected.get_as<toml::table>(section);
            if (!table) throw std::runtime_error("target section is not a table");
            table->insert_or_assign(key, value);
        };

        if (values.write_limiter_fps) {
            apply_value(lines, "limiter", "fps", format_number(values.limiter_fps), eol);
            // The patcher emits whole numbers as TOML integers.
            const std::string number = format_number(values.limiter_fps);
            if (number.find('.') == std::string::npos)
                expect("limiter", "fps", static_cast<int64_t>(std::stoll(number)));
            else expect("limiter", "fps", parse_number(number));
        }
        if (values.write_pacing) {
            apply_value(lines, "pacing", "mode", quoted(values.pacing), eol);
            expect("pacing", "mode", values.pacing);
        }
        if (values.write_async) {
            apply_value(lines, "async", "mode", quoted(values.async), eol);
            expect("async", "mode", values.async);
        }
        if (values.write_log) {
            apply_value(lines, "log", "level", std::to_string(values.log_level), eol);
            expect("log", "level", values.log_level);
        }
        if (values.write_ui_mode) {
            apply_value(lines, "ui", "mode", quoted(values.ui_mode), eol);
            expect("ui", "mode", values.ui_mode);
        }
        if (values.write_ui_scale) {
            apply_value(lines, "ui", "scale",
                        values.ui_scale_auto ? quoted("auto") : format_number(values.ui_scale),
                         eol);
            if (values.ui_scale_auto) expect("ui", "scale", "auto");
            else {
                const std::string number = format_number(values.ui_scale);
                if (number.find('.') == std::string::npos)
                    expect("ui", "scale", static_cast<int64_t>(std::stoll(number)));
                else expect("ui", "scale", parse_number(number));
            }
        }

        std::string output = join_lines(lines);
        if (!output.empty() && output.back() != '\n') output += eol;
        if (toml::parse(output) != expected) {
            error = "unsupported TOML layout; original file was not changed";
            return false;
        }

        // Create a private, unique sibling with CREATE_NEW. A fixed .tmp name
        // could truncate someone else's file before save failed (K49).
        static std::atomic<unsigned long long> sequence{0};
        std::filesystem::path temporary;
        HANDLE file = INVALID_HANDLE_VALUE;
        for (unsigned attempt = 0; attempt != 64; ++attempt) {
            temporary = path;
            temporary += L".sedici-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                         std::to_wstring(sequence.fetch_add(1, std::memory_order_relaxed)) + L".tmp";
            file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE) break;
            if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS) break;
        }
        if (file == INVALID_HANDLE_VALUE) {
            error = "could not create a unique temporary file";
            return false;
        }
        bool written = true;
        size_t offset = 0;
        while (offset < output.size()) {
            const DWORD chunk = static_cast<DWORD>(std::min<size_t>(output.size() - offset, 1u << 20));
            DWORD count = 0;
            if (!WriteFile(file, output.data() + offset, chunk, &count, nullptr) || count != chunk) {
                written = false;
                break;
            }
            offset += count;
        }
        if (written) written = FlushFileBuffers(file) != 0;
        if (!CloseHandle(file)) written = false;
        if (!written) {
            error = "could not write the temporary file";
            DeleteFileW(temporary.c_str());
            return false;
        }

        ConfigFileStamp stagedStamp;
        if (writtenStamp) {
            stagedStamp = config_file_stamp(temporary);
            if (!stagedStamp.valid || !stagedStamp.exists) {
                error = "could not verify the temporary settings file";
                DeleteFileW(temporary.c_str());
                return false;
            }
        }
        if (const ConfigBeforeReplaceHook hook = g_before_replace_hook.exchange(nullptr, std::memory_order_acq_rel))
            hook();
        // This narrows, but cannot eliminate, the external-writer race: another
        // process can still replace the path after this check and before rename.
        if (expectedStamp && !config_same_stamp(config_file_stamp(path), *expectedStamp)) {
            if (conflict) *conflict = true;
            error = "sedici.toml changed before replacement";
            DeleteFileW(temporary.c_str());
            return false;
        }
        if (!MoveFileExW(temporary.c_str(), path.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            error = "could not replace sedici.toml (error " + std::to_string(GetLastError()) + ")";
            DeleteFileW(temporary.c_str());
            return false;
        }
        if (writtenStamp) *writtenStamp = stagedStamp;
        return true;
    } catch (const toml::parse_error&) {
        error = "invalid or unsupported TOML layout; original file was not changed";
        return false;
    } catch (...) {
        error = "unexpected failure while saving sedici.toml";
        return false;
    }
}

} // namespace sedici
