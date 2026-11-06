#include "config_patch.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <string_view>
#include <vector>

namespace sedici {
namespace {

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

bool iequals(std::string_view lhs, std::string_view rhs)
{
    if (lhs.size() != rhs.size()) return false;
    for (size_t i = 0; i < lhs.size(); ++i) {
        const char a = lhs[i] >= 'A' && lhs[i] <= 'Z' ? static_cast<char>(lhs[i] + 32) : lhs[i];
        const char b = rhs[i] >= 'A' && rhs[i] <= 'Z' ? static_cast<char>(rhs[i] + 32) : rhs[i];
        if (a != b) return false;
    }
    return true;
}

std::string format_number(double value)
{
    std::ostringstream out;
    out << std::fixed << std::setprecision(3) << value;
    std::string text = out.str();
    while (!text.empty() && text.back() == '0') text.pop_back();
    if (!text.empty() && text.back() == '.') text.pop_back();
    return text;
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
        if (iequals(section_name(lines[i].text), name)) {
            begin = i;
            end = lines.size();
            for (size_t j = i + 1; j < lines.size(); ++j) {
                if (!section_name(lines[j].text).empty()) {
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
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return stamp;
        stamp.exists = true;
        stamp.last_write = (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) |
                           data.ftLastWriteTime.dwLowDateTime;
        stamp.size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    } catch (...) {
    }
    return stamp;
}

bool config_save(const std::filesystem::path& path, const ConfigValues& values,
                 std::string& error) noexcept
{
    try {
        std::string text;
        if (config_file_stamp(path).exists) {
            std::ifstream stream(path, std::ios::binary);
            if (!stream) {
                error = "could not open sedici.toml for reading";
                return false;
            }
            text.assign(std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{});
        }

        const std::string eol = dominant_eol(text);
        std::vector<Line> lines = split_lines(text);

        if (values.write_limiter_fps) {
            apply_value(lines, "limiter", "fps", format_number(values.limiter_fps), eol);
        }
        if (values.write_pacing) {
            apply_value(lines, "pacing", "mode", quoted(values.pacing), eol);
        }
        if (values.write_async) {
            apply_value(lines, "async", "mode", quoted(values.async), eol);
        }
        if (values.write_log) {
            apply_value(lines, "log", "level", std::to_string(values.log_level), eol);
        }
        if (values.write_ui_mode) {
            apply_value(lines, "ui", "mode", quoted(values.ui_mode), eol);
        }
        if (values.write_ui_scale) {
            apply_value(lines, "ui", "scale",
                        values.ui_scale_auto ? quoted("auto") : format_number(values.ui_scale),
                        eol);
        }

        std::string output = join_lines(lines);
        if (!output.empty() && output.back() != '\n') output += eol;

        std::filesystem::path temporary = path;
        temporary += L".tmp";
        {
            std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
            if (!stream) {
                error = "could not open the temporary file for writing";
                return false;
            }
            stream.write(output.data(), static_cast<std::streamsize>(output.size()));
            stream.flush();
            if (!stream) {
                error = "could not write the temporary file";
                stream.close();
                DeleteFileW(temporary.c_str());
                return false;
            }
        }

        if (!MoveFileExW(temporary.c_str(), path.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            error = "could not replace sedici.toml (error " + std::to_string(GetLastError()) + ")";
            DeleteFileW(temporary.c_str());
            return false;
        }
        return true;
    } catch (...) {
        error = "unexpected failure while saving sedici.toml";
        return false;
    }
}

} // namespace sedici
