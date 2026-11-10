#pragma once

#include <array>
#include <cstddef>
#include <type_traits>
#include <utility>

namespace sedici::fg {

// The swapchain admits at most this many presenter jobs, including the job
// currently executing. Queue insertion therefore needs no allocation.
inline constexpr size_t kPresenterQueueCapacity = 3;

template <typename T, size_t Capacity>
class FixedQueue {
    static_assert(Capacity > 0);
    static_assert(std::is_nothrow_move_assignable_v<T>, "queue entries must move without allocating or throwing");

public:
    bool empty() const noexcept { return m_count == 0; }
    size_t size() const noexcept { return m_count; }
    T& front() noexcept { return m_entries[m_head]; }
    const T& front() const noexcept { return m_entries[m_head]; }

    bool push_back(T&& value) noexcept
    {
        if (m_count == Capacity) return false;
        m_entries[(m_head + m_count) % Capacity] = std::move(value);
        ++m_count;
        return true;
    }

    void pop_front() noexcept
    {
        m_entries[m_head] = T{};
        m_head = (m_head + 1) % Capacity;
        --m_count;
    }

private:
    std::array<T, Capacity> m_entries{};
    size_t m_head = 0;
    size_t m_count = 0;
};

} // namespace sedici::fg
