#pragma once

// Keep vendor code unmodified. Backend assertions must not abort the host game,
// including Release builds where the normal C assert would disappear.
namespace sedici {
[[noreturn]] void imgui_assert_failed(const char* expression);
}
#define IM_ASSERT(expression) ((expression) ? (void)0 : sedici::imgui_assert_failed(#expression))
