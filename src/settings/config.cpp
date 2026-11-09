#include "settings/config.h"
#include "settings/target_rate.h"

#include "settings/live.h"
#include "diagnostics/log.h"

#include <windows.h>

// The vendored single-header toml++ (v3.4.0, MIT).
//
// Two compile-time switches, both documented by the library:
//  - TOML_EXCEPTIONS 0 makes `toml::parse` return a `parse_result` instead of
//    throwing `toml::parse_error`. Nothing about configuration parsing should be
//    able to throw across an ffx* export boundary, so the no-throw form is used
//    and every failure becomes a `parse_result` we inspect.
//  - TOML_ENABLE_FORMATTERS 0 drops the serialisers; Sedici only parses.
//
// The header file itself is unmodified.
#define TOML_EXCEPTIONS 0
#define TOML_ENABLE_FORMATTERS 0
#pragma warning(push)
#pragma warning(disable : 4127 4244 4245 4267 4324 4456 4457 4458 4459 4702 4800 4996)
#include "toml.hpp"
#pragma warning(pop)

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace sedici {
namespace {

using toml::node;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

char ascii_lower(char c) noexcept
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
}

// Case-insensitive compare, ASCII only. TOML values here are all ASCII
// keywords, so this avoids locale surprises.
bool iequals(std::string_view lhs, std::string_view rhs) noexcept
{
    if (lhs.size() != rhs.size()) return false;
    for (size_t i = 0; i < lhs.size(); ++i) {
        if (ascii_lower(lhs[i]) != ascii_lower(rhs[i])) return false;
    }
    return true;
}

// Reads an environment variable. An unset variable and an empty one are both
// reported as "absent", which is what the previous milestone-0 behaviour did.
bool env_string(const char* name, std::string& out)
{
    char buffer[256]{};
    SetLastError(ERROR_SUCCESS);
    const DWORD length = GetEnvironmentVariableA(name, buffer, sizeof(buffer));
    if (length == 0 || length >= sizeof(buffer)) return false; // absent, empty, or implausibly long
    out.assign(buffer, buffer + length);
    return true;
}

// Strict non-negative decimal. Rejects signs, whitespace and trailing junk, so
// an override such as "36.5 " or "-1" is reported instead of being half-parsed.
// Fractions are allowed for the limiter target.
bool parse_decimal(std::string_view text, double& out) noexcept
{
    if (text.empty()) return false;
    double value = 0.0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) return false;
    if (!std::isfinite(value) || value < 0.0) return false;
    out = value;
    return true;
}

// "auto" (any case) or a number within [0, 1000].
bool parse_target(std::string_view text, double& number, bool& automatic) noexcept
{
    std::string lower(text);
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower == "auto") {
        number = 0.0;
        automatic = true;
        return true;
    }
    double parsed = 0.0;
    if (!parse_decimal(text, parsed) || parsed > 1000.0) return false;
    number = parsed;
    automatic = false;
    return true;
}

// 37 -> "37", 36.5 -> "36.5", 36.500 -> "36.5". Used in the log so a whole
// number does not acquire a decimal point.
std::string format_number(double value)
{
    std::ostringstream out;
    out << std::fixed << std::setprecision(3) << value;
    std::string text = out.str();
    while (!text.empty() && text.back() == '0') text.pop_back();
    if (!text.empty() && text.back() == '.') text.pop_back();
    return text;
}

// Wide -> UTF-8, used so the log file stays readable and ASCII-compatible even
// when the game lives under a non-ASCII install path.
std::string narrow(const std::wstring& text)
{
    if (text.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string out(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        out.data(), length, nullptr, nullptr);
    return out;
}

const char* type_name(toml::node_type type) noexcept
{
    switch (type) {
    case toml::node_type::none: return "nothing";
    case toml::node_type::table: return "a table";
    case toml::node_type::array: return "an array";
    case toml::node_type::string: return "a string";
    case toml::node_type::integer: return "an integer";
    case toml::node_type::floating_point: return "a float";
    case toml::node_type::boolean: return "a boolean";
    case toml::node_type::date: return "a date";
    case toml::node_type::time: return "a time";
    case toml::node_type::date_time: return "a date-time";
    }
    return "an unknown type";
}

// "line:column" of a node, or empty when the parser did not record a position.
std::string position_of(const node& where)
{
    const toml::source_position position = where.source().begin;
    if (!position) return {};
    std::ostringstream out;
    out << position.line << ':' << position.column;
    return out.str();
}

// Records a per-key problem. The caller has already decided to fall back.
void problem_at(std::vector<std::string>& problems, const node& where,
                std::string_view key, const std::string& message)
{
    std::ostringstream line;
    line << "sedici.toml";
    if (std::string position = position_of(where); !position.empty()) line << ':' << position;
    line << ": " << key << ": " << message;
    problems.push_back(line.str());
}

void problem_plain(std::vector<std::string>& problems, const std::string& message)
{
    problems.push_back("sedici.toml: " + message);
}

// ---------------------------------------------------------------------------
// Typed key access
// ---------------------------------------------------------------------------

enum class Read {
    Missing,
    Ok,
    WrongType,
};

Read read_string(const node* value, std::string& out)
{
    if (!value) return Read::Missing;
    const auto text = value->value<std::string>();
    if (!text) return Read::WrongType;
    out = *text;
    return Read::Ok;
}

// Accepts both TOML floats and integers, so `variance_factor = 1` is valid as
// well as `1.0`.
Read read_number(const node* value, double& out)
{
    if (!value) return Read::Missing;
    if (const auto number = value->value<double>()) {
        out = *number;
        return Read::Ok;
    }
    if (const auto integer = value->value<int64_t>()) {
        out = static_cast<double>(*integer);
        return Read::Ok;
    }
    return Read::WrongType;
}

Read read_integer(const node* value, int64_t& out)
{
    if (!value) return Read::Missing;
    const auto integer = value->value<int64_t>();
    if (!integer) return Read::WrongType;
    out = *integer;
    return Read::Ok;
}

Read read_boolean(const node* value, bool& out)
{
    if (!value) return Read::Missing;
    const auto flag = value->value<bool>();
    if (!flag) return Read::WrongType;
    out = *flag;
    return Read::Ok;
}

// Looks up `key` inside `[section]`. Returns nullptr when either is absent, or
// when `section` is not a table (that case is reported separately by
// check_section so the message names the section rather than every key).
const node* section_key(const toml::table& root, std::string_view section, std::string_view key)
{
    const node* table = root[section].node();
    if (!table || !table->is_table()) return nullptr;
    return table->as_table()->get(key);
}

void check_section(const toml::table& root, std::string_view name, std::vector<std::string>& problems)
{
    const node* section = root[name].node();
    if (!section || section->is_table()) return;
    problem_at(problems, *section, name,
               std::string("expected a table, got ") + type_name(section->type()) +
                   " - ignoring the whole [" + std::string(name) + "] section");
}

// ---------------------------------------------------------------------------
// File loading
// ---------------------------------------------------------------------------

void note_removed_keys(const toml::table& root, std::vector<std::string>& notes);
void note_unknown_keys(const toml::table& root, std::vector<std::string>& problems);
void read_fg(const toml::table& root, FrameGenerationSettings& fg,
             std::vector<std::string>& problems);

// "vblank" is the name finding 008 shipped with; it still selects vsync.
bool parse_present(std::string_view text, FrameGenerationSettings::Present& out) noexcept
{
    using Present = FrameGenerationSettings::Present;
    if (iequals(text, "vsync") || iequals(text, "vblank")) out = Present::Vsync;
    else if (iequals(text, "vrr")) out = Present::Vrr;
    else if (iequals(text, "game")) out = Present::Game;
    else return false;
    return true;
}

bool parse_multiplier(std::string_view text, FrameGenerationSettings::Multiplier& out) noexcept
{
    using Multiplier = FrameGenerationSettings::Multiplier;
    if (iequals(text, "2x") || iequals(text, "2")) out = Multiplier::X2;
    else if (iequals(text, "1.75x") || iequals(text, "1.75")) out = Multiplier::X1_75;
    else if (iequals(text, "1.5x") || iequals(text, "1.5")) out = Multiplier::X1_5;
    else if (iequals(text, "1.25x") || iequals(text, "1.25")) out = Multiplier::X1_25;
    else if (iequals(text, "adaptive")) out = Multiplier::Adaptive;
    else return false;
    return true;
}

bool parse_tear_guard(std::string_view text, FrameGenerationSettings::TearGuard& out) noexcept
{
    using TearGuard = FrameGenerationSettings::TearGuard;
    if (iequals(text, "scanline")) out = TearGuard::Scanline;
    else if (iequals(text, "timer")) out = TearGuard::Timer;
    else if (iequals(text, "off")) out = TearGuard::Off;
    else return false;
    return true;
}

void read_limiter(const toml::table& root, Settings& config, std::vector<std::string>& problems);
void read_log(const toml::table& root, LogSettings& log, std::vector<std::string>& problems);
void read_ui(const toml::table& root, UiSettings& ui, std::vector<std::string>& problems);

// Reads `sedici.toml` and applies every key it contains.
//
// The parsed table lives in this frame for the whole call, so the per-key
// readers can safely hold views into it.
void load_from_file(Settings& config, std::vector<std::string>& problems,
                    std::vector<std::string>& notes)
{
    const std::filesystem::path directory = dll_directory();
    if (directory.empty()) return;

    const std::filesystem::path file = directory / L"sedici.toml";
    config.path = file.wstring();

    std::error_code error_code;
    if (!std::filesystem::is_regular_file(file, error_code) || error_code) return;
    config.file_exists = true;

    std::string text;
    {
        std::ifstream stream(file, std::ios::binary);
        if (!stream) {
            problem_plain(problems,
                          "could not be opened for reading - using built-in defaults for every key");
            return;
        }
        text.assign(std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{});
    }

    // The second argument is pinned to string_view: toml++ has both a
    // `std::string_view` and a `std::string&&` overload for the source path, and
    // a bare literal makes the call ambiguous.
    auto result = toml::parse(text, std::string_view{"sedici.toml"});
    if (!result) {
        const toml::parse_error& error = result.error();
        std::ostringstream line;
        line << "sedici.toml:" << error.source().begin.line << ':' << error.source().begin.column
             << ": parse error: " << error.description()
             << " - using built-in defaults for every key";
        problems.push_back(line.str());
        return;
    }

    const toml::table& root = result.table();
    config.file_parsed = true;

    note_removed_keys(root, notes);
    note_unknown_keys(root, problems);
    read_fg(root, config.fg, problems);
    read_limiter(root, config, problems);
    read_log(root, config.log, problems);
    read_ui(root, config.ui, problems);
}

// ---------------------------------------------------------------------------
// Per-key reading
// ---------------------------------------------------------------------------

// Sections and keys from the era when Sedici forwarded to AMD's frame generation.
// They no longer do anything; an old sedici.toml still loads, and one note says
// why those values are ignored.
void note_removed_keys(const toml::table& root, std::vector<std::string>& notes)
{
    std::string found;
    const auto add = [&](const char* name) {
        if (!found.empty()) found += ", ";
        found += name;
    };
    if (root["pacing"]) add("[pacing]");
    if (root["async"]) add("[async]");
    if (section_key(root, "fg", "mode")) add("fg.mode");
    if (!found.empty())
        notes.push_back("sedici.toml: " + found +
                        " no longer exist (Sedici is the only frame generation backend) - ignored");
}

// A key or section Sedici does not read is reported with its line, so a typo is
// not silently ignored. Removed forwarding-era keys have their own note.
void note_unknown_keys(const toml::table& root, std::vector<std::string>& problems)
{
    struct Known { std::string_view section; std::vector<std::string_view> keys; };
    static const Known known[] = {
        {"fg", {"present", "multiplier", "target_fps", "tear_guard", "mode"}},
        {"limiter", {"fps", "spin_us"}},
        {"log", {"level"}},
        {"ui", {"hotkey", "mode", "scale"}},
    };
    const auto where = [](const toml::node& node) {
        return "sedici.toml:" + std::to_string(node.source().begin.line) + ": ";
    };
    for (auto&& [name, node] : root) {
        const std::string_view section = name.str();
        if (section == "pacing" || section == "async") continue; // noted as removed
        const Known* match = nullptr;
        for (const auto& k : known)
            if (k.section == section) match = &k;
        if (!match) {
            problems.push_back(where(node) + "unknown " + (node.is_table() ? "section [" : "key ") +
                               std::string(section) + (node.is_table() ? "]" : "") +
                               " - ignored (expected [fg], [limiter], [log] or [ui])");
            continue;
        }
        const toml::table* table = node.as_table();
        if (!table) continue; // a wrong-typed section is reported by its reader
        for (auto&& [key, value] : *table) {
            bool found = false;
            for (const auto& k : match->keys)
                if (k == key.str()) found = true;
            if (!found)
                problems.push_back(where(value) + "unknown key " + std::string(section) + "." +
                                   std::string(key.str()) + " - ignored");
        }
    }
}

void read_fg(const toml::table& root, FrameGenerationSettings& fg,
             std::vector<std::string>& problems)
{
    check_section(root, "fg", problems);

    std::string value;
    const node* entry = section_key(root, "fg", "present");
    switch (read_string(entry, value)) {
    case Read::Missing:
        break;
    case Read::WrongType:
        problem_at(problems, *entry, "fg.present",
                   std::string("expected a string, got ") + type_name(entry->type()) +
                       " - using default \"vrr\"");
        break;
    case Read::Ok:
        if (FrameGenerationSettings::Present parsed{}; parse_present(value, parsed)) {
            fg.present = parsed;
            fg.present_source = Source::File;
        } else {
            problem_at(problems, *entry, "fg.present",
                       "unknown value \"" + value +
                           "\" (expected \"vsync\", \"vrr\" or \"game\") - using default \"vrr\"");
        }
        break;
    }

    entry = section_key(root, "fg", "multiplier");
    switch (read_string(entry, value)) {
    case Read::Missing:
        break;
    case Read::WrongType:
        problem_at(problems, *entry, "fg.multiplier",
                   std::string("expected a string, got ") + type_name(entry->type()) +
                       " - using default \"2x\"");
        break;
    case Read::Ok:
        if (FrameGenerationSettings::Multiplier parsed{}; parse_multiplier(value, parsed)) {
            fg.multiplier = parsed;
            fg.multiplier_source = Source::File;
        } else {
            problem_at(problems, *entry, "fg.multiplier",
                       "unknown value \"" + value +
                           "\" (expected \"2x\", \"1.75x\", \"1.5x\", \"1.25x\" or \"adaptive\")"
                           " - using default \"2x\"");
        }
        break;
    }

    {
        entry = section_key(root, "fg", "target_fps");
        if (entry) {
            double number = 0.0;
            bool automatic = false;
            std::string text;
            if (read_string(entry, text) == Read::Ok) {
                if (parse_target(text, number, automatic)) {
                    fg.target_fps = number;
                    fg.target_auto = automatic;
                    fg.target_fps_source = Source::File;
                } else {
                    problem_at(problems, *entry, "fg.target_fps",
                               "unknown value \"" + text +
                                   "\" (expected a number or \"auto\") - using default 0 (uncapped)");
                }
            } else if (read_number(entry, number) == Read::Ok) {
                if (!std::isfinite(number) || number < 0.0 || number > 1000.0) {
                    problem_at(problems, *entry, "fg.target_fps",
                               "must be within [0, 1000] (0 = uncapped) - using default 0");
                } else {
                    fg.target_fps = number;
                    fg.target_auto = false;
                    fg.target_fps_source = Source::File;
                }
            } else {
                problem_at(problems, *entry, "fg.target_fps",
                           std::string("expected a number or \"auto\", got ") + type_name(entry->type()) +
                               " - using default 0 (uncapped)");
            }
        }
    }

    entry = section_key(root, "fg", "tear_guard");
    switch (read_string(entry, value)) {
    case Read::Missing:
        break;
    case Read::WrongType:
        problem_at(problems, *entry, "fg.tear_guard",
                   std::string("expected a string, got ") + type_name(entry->type()) +
                       " - using default \"scanline\"");
        break;
    case Read::Ok:
        if (FrameGenerationSettings::TearGuard parsed{}; parse_tear_guard(value, parsed)) {
            fg.tear_guard = parsed;
            fg.tear_guard_source = Source::File;
        } else {
            problem_at(problems, *entry, "fg.tear_guard",
                       "unknown value \"" + value +
                           "\" (expected \"scanline\", \"timer\" or \"off\") - using default \"scanline\"");
        }
        break;
    }
}

void read_limiter(const toml::table& root, Settings& config,
                  std::vector<std::string>& problems)
{
    LimiterSettings& limiter = config.limiter;
    check_section(root, "limiter", problems);
    // fps ---------------------------------------------------------------
    {
        double value = 0.0;
        const node* entry = section_key(root, "limiter", "fps");
        switch (read_number(entry, value)) {
        case Read::Missing:
            break;
        case Read::WrongType:
            problem_at(problems, *entry, "limiter.fps",
                       std::string("expected a number, got ") + type_name(entry->type()) +
                           " - ignored (limiter.fps is replaced by fg.target_fps)");
            break;
        case Read::Ok:
            if (!std::isfinite(value) || value < 0.0 || value > 1000.0) {
                problem_at(problems, *entry, "limiter.fps",
                           "must be within [0, 1000] (0 = off) - ignored");
            } else {
                config.legacy_limiter_fps = value;
                config.legacy_limiter_source = Source::File;
            }
            break;
        }
    }

    // spin_us -----------------------------------------------------------
    {
        int64_t value = 0;
        const node* entry = section_key(root, "limiter", "spin_us");
        switch (read_integer(entry, value)) {
        case Read::Missing:
            break;
        case Read::WrongType:
            problem_at(problems, *entry, "limiter.spin_us",
                       std::string("expected an integer, got ") + type_name(entry->type()) +
                           " - using default 1000");
            break;
        case Read::Ok:
            if (value < 0 || value > 100000) {
                problem_at(problems, *entry, "limiter.spin_us",
                           "must be within [0, 100000] microseconds - using default 1000");
            } else {
                limiter.spin_us = static_cast<int>(value);
                limiter.spin_us_source = Source::File;
            }
            break;
        }
    }
}

void read_log(const toml::table& root, LogSettings& log, std::vector<std::string>& problems)
{
    check_section(root, "log", problems);

    int64_t value = 0;
    const node* entry = section_key(root, "log", "level");
    switch (read_integer(entry, value)) {
    case Read::Missing:
        break;
    case Read::WrongType:
        problem_at(problems, *entry, "log.level",
                   std::string("expected an integer, got ") + type_name(entry->type()) +
                       " - using default 1");
        break;
    case Read::Ok:
        if (value < 0 || value > 2) {
            problem_at(problems, *entry, "log.level",
                       "must be 0, 1 or 2 - using default 1");
        } else {
            log.level = static_cast<int>(value);
            log.level_source = Source::File;
        }
        break;
    }
}

bool valid_hotkey(std::string_view text) noexcept
{
    if (text.empty() || text.size() > 32) return false;

    int modifiers = 0;
    bool has_key = false;
    size_t start = 0;
    for (;;) {
        const size_t plus = text.find('+', start);
        const size_t end = plus == std::string_view::npos ? text.size() : plus;
        const std::string_view part = text.substr(start, end - start);
        if (part.empty()) return false;

        if (iequals(part, "alt") || iequals(part, "ctrl") || iequals(part, "shift")) {
            ++modifiers;
        } else if (!has_key) {
            if (part.size() >= 2 && ascii_lower(part[0]) == 'f') {
                int number = 0;
                for (size_t i = 1; i < part.size(); ++i) {
                    if (part[i] < '0' || part[i] > '9') return false;
                    number = number * 10 + (part[i] - '0');
                }
                if (number < 1 || number > 12) return false;
            } else if (part.size() != 1 ||
                       !((part[0] >= 'a' && part[0] <= 'z') ||
                         (part[0] >= '0' && part[0] <= '9'))) {
                return false;
            }
            has_key = true;
        } else {
            return false;
        }

        if (plus == std::string_view::npos) break;
        start = plus + 1;
    }
    return modifiers > 0 && has_key;
}

void read_ui(const toml::table& root, UiSettings& ui, std::vector<std::string>& problems)
{
    check_section(root, "ui", problems);

    // mode ---------------------------------------------------------------
    {
        std::string value;
        const node* entry = section_key(root, "ui", "mode");
        switch (read_string(entry, value)) {
        case Read::Missing:
            break;
        case Read::WrongType:
            problem_at(problems, *entry, "ui.mode",
                       std::string("expected a string, got ") + type_name(entry->type()) +
                           " - using default \"off\"");
            break;
        case Read::Ok:
            if (iequals(value, "off")) {
                ui.mode = UiMode::Off;
                ui.mode_source = Source::File;
            } else if (iequals(value, "mini")) {
                ui.mode = UiMode::Mini;
                ui.mode_source = Source::File;
            } else if (iequals(value, "full")) {
                ui.mode = UiMode::Full;
                ui.mode_source = Source::File;
            } else {
                problem_at(problems, *entry, "ui.mode",
                           "unknown value \"" + value +
                               "\" (expected \"off\", \"mini\" or \"full\") - using default \"off\"");
            }
            break;
        }
    }

    // scale --------------------------------------------------------------
    {
        const node* entry = section_key(root, "ui", "scale");
        if (entry) {
            if (const auto text = entry->value<std::string>()) {
                if (iequals(*text, "auto")) {
                    ui.scale_mode = UiScaleMode::Auto;
                    ui.scale_source = Source::File;
                } else {
                    problem_at(problems, *entry, "ui.scale",
                               "unknown value \"" + *text +
                                   "\" (expected \"auto\" or a number in [0.5, 2.0]) - using "
                                   "default \"auto\"");
                }
            } else {
                double value = 0.0;
                bool has_number = false;
                if (const auto number = entry->value<double>()) {
                    value = *number;
                    has_number = true;
                } else if (const auto integer = entry->value<int64_t>()) {
                    value = static_cast<double>(*integer);
                    has_number = true;
                }

                if (has_number) {
                    if (!std::isfinite(value) || value < 0.5 || value > 2.0) {
                        problem_at(problems, *entry, "ui.scale",
                                   "must be within [0.5, 2.0] - using default \"auto\"");
                    } else {
                        ui.scale = static_cast<float>(value);
                        ui.scale_mode = UiScaleMode::Fixed;
                        ui.scale_source = Source::File;
                    }
                } else {
                    problem_at(problems, *entry, "ui.scale",
                               std::string("expected \"auto\" or a number, got ") +
                                   type_name(entry->type()) + " - using default \"auto\"");
                }
            }
        }
    }

    // hotkey -------------------------------------------------------------
    {
        std::string value;
        const node* entry = section_key(root, "ui", "hotkey");
        switch (read_string(entry, value)) {
        case Read::Missing:
            break;
        case Read::WrongType:
            problem_at(problems, *entry, "ui.hotkey",
                       std::string("expected a string, got ") + type_name(entry->type()) +
                           " - using default \"alt+f10\"");
            break;
        case Read::Ok:
            if (valid_hotkey(value)) {
                for (char& c : value) c = ascii_lower(c);
                ui.hotkey = value;
                ui.hotkey_source = Source::File;
            } else {
                problem_at(problems, *entry, "ui.hotkey",
                           "expected modifier+key such as \"alt+f10\" - using default \"alt+f10\"");
            }
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Environment overrides
// ---------------------------------------------------------------------------
// Environment variables win over the file. An unrecognised value is reported
// and then ignored, so the file (or the default) stays in effect rather than
// silently falling back to a default the user did not ask for.
void apply_env_overrides(Settings& config, std::vector<std::string>& problems)
{
    std::string value;

    if (env_string("SEDICI_LIMIT_FPS", value)) {
        double parsed = 0.0;
        if (parse_decimal(value, parsed) && parsed <= 1000.0) {
            config.legacy_limiter_fps = parsed;
            config.legacy_limiter_source = Source::Env;
        } else {
            problems.push_back("SEDICI_LIMIT_FPS=\"" + value +
                               "\": expected a number within [0, 1000] (0 = off) - override "
                               "ignored");
        }
    }

    if (env_string("SEDICI_FG_PRESENT", value)) {
        if (FrameGenerationSettings::Present parsed{}; parse_present(value, parsed)) {
            config.fg.present = parsed;
            config.fg.present_source = Source::Env;
        } else {
            problems.push_back("SEDICI_FG_PRESENT=\"" + value +
                               "\": unknown value (expected \"vsync\", \"vrr\" or \"game\") - "
                               "override ignored");
        }
    }

    if (env_string("SEDICI_FG_MULTIPLIER", value)) {
        if (FrameGenerationSettings::Multiplier parsed{}; parse_multiplier(value, parsed)) {
            config.fg.multiplier = parsed;
            config.fg.multiplier_source = Source::Env;
        } else {
            problems.push_back("SEDICI_FG_MULTIPLIER=\"" + value +
                               "\": unknown value (expected \"2x\", \"1.75x\", \"1.5x\", \"1.25x\" or "
                               "\"adaptive\") - override ignored");
        }
    }

    if (env_string("SEDICI_FG_TARGET_FPS", value)) {
        double parsed = 0.0;
        bool automatic = false;
        if (parse_target(value, parsed, automatic)) {
            config.fg.target_fps = parsed;
            config.fg.target_auto = automatic;
            config.fg.target_fps_source = Source::Env;
        } else {
            problems.push_back("SEDICI_FG_TARGET_FPS=\"" + value +
                               "\": expected a number within [0, 1000] or \"auto\" - override ignored");
        }
    }

    if (env_string("SEDICI_FG_TEAR_GUARD", value)) {
        if (FrameGenerationSettings::TearGuard parsed{}; parse_tear_guard(value, parsed)) {
            config.fg.tear_guard = parsed;
            config.fg.tear_guard_source = Source::Env;
        } else {
            problems.push_back("SEDICI_FG_TEAR_GUARD=\"" + value +
                               "\": unknown value (expected \"scanline\", \"timer\" or \"off\") - "
                               "override ignored");
        }
    }

    // Legacy variable, kept working unchanged: one digit, 0 to 2.
    if (env_string("MFSRFG_LOG", value)) {
        if (value.size() == 1 && value[0] >= '0' && value[0] <= '2') {
            config.log.level = value[0] - '0';
            config.log.level_source = Source::Env;
        } else {
            problems.push_back("MFSRFG_LOG=\"" + value +
                               "\": expected 0, 1 or 2 - override ignored");
        }
    }
}

// ---------------------------------------------------------------------------
// Once-at-load reporting
// ---------------------------------------------------------------------------

// Output frames per real frame of the nominal schedule, used to turn a legacy
// real-frame cap into an output target.
double nominal_output_ratio(const FrameGenerationSettings& fg) noexcept
{
    using M = FrameGenerationSettings::Multiplier;
    if (fg.present != FrameGenerationSettings::Present::Vrr) return 2.0;
    switch (fg.multiplier) {
    case M::X1_75: return 1.75;
    case M::X1_5: return 1.5;
    case M::X1_25: return 1.25;
    case M::Adaptive: return 1.0;
    case M::X2: break;
    }
    return 2.0;
}

// Folds the legacy real-frame cap into the one target and notes the spin window.
// Priority: SEDICI_FG_TARGET_FPS, SEDICI_LIMIT_FPS, fg.target_fps, limiter.fps.
void finalize_target(Settings& config, std::vector<std::string>& notes)
{
    FrameGenerationSettings& fg = config.fg;
    if (config.legacy_limiter_fps >= 0.0) {
        const bool legacyEnv = config.legacy_limiter_source == Source::Env;
        const char* name = legacyEnv ? "SEDICI_LIMIT_FPS" : "limiter.fps";
        const bool wins = legacyEnv ? fg.target_fps_source != Source::Env
                                    : fg.target_fps_source == Source::Default;
        if (wins) {
            const double ratio = nominal_output_ratio(fg);
            fg.target_fps = legacy_limiter_to_target(config.legacy_limiter_fps, ratio);
            fg.target_auto = false;
            fg.target_fps_source = config.legacy_limiter_source;
            notes.push_back(std::string("sedici: ") + name + " is deprecated (it capped real frames); read as "
                            "fg.target_fps = " + format_number(fg.target_fps) + " output fps (" +
                            format_number(config.legacy_limiter_fps) + " x " + format_number(ratio) +
                            "). Use fg.target_fps, a number or \"auto\"");
        } else {
            notes.push_back(std::string("sedici: ") + name + " is ignored because fg.target_fps is set");
        }
    }
    if (!fg.target_auto && fg.target_fps > 0.0) {
        const double cap = fg.target_fps / nominal_output_ratio(fg);
        const long interval_us = std::lround(1000000.0 / cap);
        if (static_cast<double>(config.limiter.spin_us) >= static_cast<double>(interval_us)) {
            notes.push_back(
                "sedici.toml: limiter.spin_us: " + std::to_string(config.limiter.spin_us) +
                " us is not shorter than the " + std::to_string(interval_us) +
                " us frame interval; the timer wait is skipped and the limiter spins for the "
                "whole wait");
        }
    }
}

std::string target_text(const FrameGenerationSettings& fg)
{
    return fg.target_auto ? std::string("\"auto\"") : format_number(fg.target_fps);
}

std::string format_float(float value)
{
    std::ostringstream out;
    out << value;
    return out.str();
}

void log_settings(const Settings& config, const std::vector<std::string>& problems,
                  const std::vector<std::string>& notes)
{
    // Problems first: the reader should see what was ignored before the
    // effective values that resulted from it.
    for (const std::string& line : problems) log_raw(line);
    for (const std::string& line : notes) log_raw(line);

    if (!config.file_exists) {
        if (config.path.empty()) {
            log_raw("sedici: could not resolve the DLL directory; using built-in defaults");
        } else {
            log_raw("sedici: no sedici.toml beside the DLL; using built-in defaults");
        }
    } else if (!config.file_parsed) {
        log_raw("sedici: sedici.toml was found but could not be used; using built-in defaults");
    } else {
        log_raw("sedici: read " + narrow(config.path) + " (once, on the first ffx call)");
    }

    struct Row {
        const char* key;
        std::string value;
        Source source;
    };

    const std::vector<Row> rows = {
        {"fg.present", std::string("\"") + present_name(config.fg.present) + "\"",
         config.fg.present_source},
        {"fg.tear_guard", std::string("\"") + tear_guard_name(config.fg.tear_guard) + "\"",
         config.fg.tear_guard_source},
        {"fg.multiplier", std::string("\"") + multiplier_name(config.fg.multiplier) + "\"",
         config.fg.multiplier_source},
        {"fg.target_fps", target_text(config.fg), config.fg.target_fps_source},
        {"limiter.spin_us", std::to_string(config.limiter.spin_us),
         config.limiter.spin_us_source},
        {"ui.mode",
         config.ui.mode == UiMode::Mini ? "\"mini\""
         : config.ui.mode == UiMode::Full ? "\"full\""
                                          : "\"off\"",
         config.ui.mode_source},
        {"ui.scale",
         config.ui.scale_mode == UiScaleMode::Auto ? "\"auto\"" : format_number(config.ui.scale),
         config.ui.scale_source},
        {"ui.hotkey", "\"" + config.ui.hotkey + "\"", config.ui.hotkey_source},
        {"log.level", std::to_string(config.log.level), config.log.level_source},
    };

    size_t width = 0;
    for (const Row& row : rows) width = std::max(width, std::string_view(row.key).size());

    for (const Row& row : rows) {
        std::ostringstream line;
        line << "sedici:   " << std::left << std::setw(static_cast<int>(width) + 2) << row.key
             << "= " << row.value;
        line << " [" << source_name(row.source) << ']';
        log_raw(line.str());
    }

    // One line that answers the only question that matters when reading a log
    // from a play session: is the fix on, and is anything being rewritten?
    std::ostringstream effective;
    effective << "sedici: effective: target fps ";
    if (config.fg.target_auto) {
        effective << "auto (floor(Hz - Hz^2/3600) of the monitor, uncapped if unknown; spin "
                  << config.limiter.spin_us << " us)";
    } else if (config.fg.target_fps > 0.0) {
        effective << format_number(config.fg.target_fps) << " output fps (spin "
                  << config.limiter.spin_us << " us)";
    } else {
        effective << "uncapped";
    }
    log_raw(effective.str());
}

} // namespace

const char* present_name(FrameGenerationSettings::Present present) noexcept
{
    switch (present) {
    case FrameGenerationSettings::Present::Game: return "game";
    case FrameGenerationSettings::Present::Vrr: return "vrr";
    case FrameGenerationSettings::Present::Vsync: break;
    }
    return "vsync";
}

const char* tear_guard_name(FrameGenerationSettings::TearGuard guard) noexcept
{
    switch (guard) {
    case FrameGenerationSettings::TearGuard::Timer: return "timer";
    case FrameGenerationSettings::TearGuard::Off: return "off";
    case FrameGenerationSettings::TearGuard::Scanline: break;
    }
    return "scanline";
}

const char* multiplier_name(FrameGenerationSettings::Multiplier multiplier) noexcept
{
    switch (multiplier) {
    case FrameGenerationSettings::Multiplier::X1_75: return "1.75x";
    case FrameGenerationSettings::Multiplier::X1_5: return "1.5x";
    case FrameGenerationSettings::Multiplier::X1_25: return "1.25x";
    case FrameGenerationSettings::Multiplier::Adaptive: return "adaptive";
    case FrameGenerationSettings::Multiplier::X2: break;
    }
    return "2x";
}

const char* source_name(Source source) noexcept
{
    switch (source) {
    case Source::File: return "file";
    case Source::Env: return "env";
    case Source::Default: break;
    }
    return "default";
}

const Settings& settings() noexcept
{
    static const Settings value = [] {
        Settings config;
        std::vector<std::string> problems;
        std::vector<std::string> notes;

        try {
            load_from_file(config, problems, notes);
            apply_env_overrides(config, problems);
            finalize_target(config, notes);
        } catch (...) {
            // A parse or allocation failure must never reach the game. Anything
            // already applied stays; the rest stays at its default.
            problems.push_back(
                "sedici: internal error while reading the configuration - using built-in "
                "defaults for every key that was not already read");
        }

        // log_raw() bypasses the level filter and, crucially, cannot call back
        // into settings(), so it is safe during this initialisation.
        live_initialize(config);

        try {
            log_settings(config, problems, notes);
        } catch (...) {
        }

        return config;
    }();

    return value;
}

} // namespace sedici
