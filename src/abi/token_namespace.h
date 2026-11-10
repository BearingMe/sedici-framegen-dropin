#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace sedici::fg {

// Bookkeeping for a caller-owned reserved address range. Values are opaque:
// they are never dereferenced. The caller serializes issue/contains_issued and
// keeps the range reserved for at least as long as any token can be presented.
class TokenNamespace {
public:
    TokenNamespace() = default;
    TokenNamespace(uintptr_t base, size_t bytes, uintptr_t stride) noexcept
    {
        if (base == 0 || stride == 0 || bytes < stride || bytes > std::numeric_limits<uintptr_t>::max() - base)
            return;
        m_base = base;
        m_stride = stride;
        m_capacity = bytes / stride;
    }

    void* issue() noexcept
    {
        if (!m_base || m_issued >= m_capacity) return nullptr;
        return reinterpret_cast<void*>(m_base + m_stride * m_issued++);
    }

    bool contains_issued(const void* token) const noexcept
    {
        if (!m_base || !token) return false;
        const uintptr_t value = reinterpret_cast<uintptr_t>(token);
        if (value < m_base) return false;
        const uintptr_t delta = value - m_base;
        return delta % m_stride == 0 && delta / m_stride < m_issued;
    }

private:
    uintptr_t m_base = 0;
    uintptr_t m_stride = 0;
    size_t m_capacity = 0;
    size_t m_issued = 0;
};

// Reserve lazily before issuing a token. A failed reservation leaves the
// namespace uninitialized and issues no identity; the caller can retry later.
// `reserve` must return a process-lifetime region of at least `bytes` or null.
template <typename Reserve>
void* issue_reserved_token(TokenNamespace& tokens, bool& reserved, size_t bytes, uintptr_t stride,
                           Reserve&& reserve) noexcept
{
    if (!reserved) {
        void* region = reserve(bytes);
        if (!region) return nullptr;
        tokens = TokenNamespace(reinterpret_cast<uintptr_t>(region), bytes, stride);
        reserved = true;
    }
    return tokens.issue();
}

} // namespace sedici::fg
