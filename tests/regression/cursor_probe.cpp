// Test-only module: imports SetCursorPos and ClipCursor the way a game's own engine DLL would,
// so the regression host can check that the overlay's cursor interception finds imports outside
// the executable (P4.8, K132). It is loaded late, after the first scan.
#include <windows.h>

extern "C" __declspec(dllexport) BOOL probe_set_pos(int x, int y) { return SetCursorPos(x, y); }
extern "C" __declspec(dllexport) BOOL probe_clip(const RECT* rect) { return ClipCursor(rect); }
