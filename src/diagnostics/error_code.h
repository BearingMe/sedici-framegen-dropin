#pragma once

// Stable error codes for the rejections Sedici returns to the game (P4.3, the
// typed-outcome part of the P2.10 deferrals). A code is part of the log contract:
// it never changes meaning and is never reused. The FFX return value stays what
// AGENTS.md and the README define; the code says *why* inside that value.
//
// The free-text reasons come from many places, so the code is derived from the
// reason by one table here, and a reason that matches nothing is E_REJECTED.

#include <cstdint>
#include <string_view>

namespace sedici {

enum class ErrorCode : uint16_t {
    Rejected = 0,           // E_REJECTED: no more specific code
    StaleHandle = 1,        // E_STALE_HANDLE: a destroyed Sedici context
    NullDescriptor = 2,     // E_NULL_DESCRIPTOR
    InvalidDescriptor = 3,  // E_INVALID_DESCRIPTOR: missing or contradictory parts
    Unsupported = 4,        // E_UNSUPPORTED: a type, format, flag or query Sedici does not implement
    NamespaceExhausted = 5, // E_NAMESPACE: the reserved handle namespace is unavailable
    Allocation = 6,         // E_ALLOCATION: bookkeeping allocation failed (owner-approved exception)
    WrapFailed = 7,         // E_WRAP_FAILED: the swapchain could not be wrapped
    NotRetired = 8,         // E_NOT_RETIRED: destroy while GPU work has not retired
    SlotBusy = 9,           // E_SLOT_BUSY: the prepare slot is still in flight
    RetirementTimeout = 10, // E_RETIREMENT_TIMEOUT
};

inline const char* error_code_name(ErrorCode code) noexcept
{
    switch (code) {
    case ErrorCode::StaleHandle:
        return "E_STALE_HANDLE";
    case ErrorCode::NullDescriptor:
        return "E_NULL_DESCRIPTOR";
    case ErrorCode::InvalidDescriptor:
        return "E_INVALID_DESCRIPTOR";
    case ErrorCode::Unsupported:
        return "E_UNSUPPORTED";
    case ErrorCode::NamespaceExhausted:
        return "E_NAMESPACE";
    case ErrorCode::Allocation:
        return "E_ALLOCATION";
    case ErrorCode::WrapFailed:
        return "E_WRAP_FAILED";
    case ErrorCode::NotRetired:
        return "E_NOT_RETIRED";
    case ErrorCode::SlotBusy:
        return "E_SLOT_BUSY";
    case ErrorCode::RetirementTimeout:
        return "E_RETIREMENT_TIMEOUT";
    case ErrorCode::Rejected:
        break;
    }
    return "E_REJECTED";
}

// Order matters: the first matching fragment wins.
inline ErrorCode classify_reason(std::string_view reason) noexcept
{
    struct Rule {
        std::string_view fragment;
        ErrorCode code;
    };
    static constexpr Rule rules[] = {
        {"destroy before GPU retirement", ErrorCode::NotRetired},
        {"retirement timed out", ErrorCode::RetirementTimeout},
        {"prepare slot", ErrorCode::SlotBusy},
        {"on a destroyed context", ErrorCode::StaleHandle},
        {"null descriptor", ErrorCode::NullDescriptor},
        {"namespace", ErrorCode::NamespaceExhausted},
        {"allocation failed", ErrorCode::Allocation},
        {"wrap failed", ErrorCode::WrapFailed},
        {"unsupported", ErrorCode::Unsupported},
        {"unimplemented", ErrorCode::Unsupported},
        {"names both", ErrorCode::InvalidDescriptor},
        {"without a", ErrorCode::InvalidDescriptor},
        {"empty display size", ErrorCode::InvalidDescriptor},
    };
    for (const Rule& rule : rules)
        if (reason.find(rule.fragment) != std::string_view::npos) return rule.code;
    return ErrorCode::Rejected;
}

} // namespace sedici
