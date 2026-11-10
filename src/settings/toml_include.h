// The one place Sedici includes the vendored single-header toml++ (v3.4.0, MIT). The header file itself is
// unmodified.
//
// Every translation unit that uses toml++ must include it through this file, never "toml.hpp" directly:
// the two switches below change the definition of classes that live in the plain `toml::v3` namespace
// (`toml::parse_error` is a plain class with TOML_EXCEPTIONS 0 and derives from std::runtime_error without it).
// Two units compiled with different switches break the one-definition rule, and the linker merges their inline
// members. That was K145: UBSan reported an invalid `bool` in `std::optional<parse_error>` and the Debug build read
// freed memory in `parse_error::description()` (K143).
//
//  - TOML_EXCEPTIONS 0 makes `toml::parse` return a `parse_result` instead of throwing `toml::parse_error`.
//    Nothing about configuration parsing should be able to throw across an ffx* export boundary, so the no-throw
//    form is used and every failure becomes a `parse_result` we inspect.
//  - TOML_ENABLE_FORMATTERS 0 drops the serialisers; Sedici only parses.
#pragma once

#define TOML_EXCEPTIONS 0
#define TOML_ENABLE_FORMATTERS 0
#pragma warning(push)
#pragma warning(disable : 4127 4244 4245 4267 4324 4456 4457 4458 4459 4702 4800 4996)
#include "toml.hpp"
#pragma warning(pop)
