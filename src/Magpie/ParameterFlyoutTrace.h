#pragma once
#include "Logger.h"
#include <atomic>

namespace Magpie {

// Process-local diagnostic opt-in. No setting is saved and no popup behavior is
// changed. Set only in an isolated test process, before launching it.
inline bool IsParameterFlyoutTraceEnabled() noexcept {
	static const bool enabled = [] {
		wchar_t value[2]{};
		return GetEnvironmentVariableW(L"MAGPIE_TRACE_PARAMETER_FLYOUT", value, 2) == 1 && value[0] == L'1';
	}();
	return enabled;
}

inline void TraceParameterFlyout(const char* event, uintptr_t object, int64_t a = 0, int64_t b = 0) {
	if (!IsParameterFlyoutTraceEnabled()) return;
	static std::atomic<uint64_t> sequence = 0;
	LARGE_INTEGER counter{};
	QueryPerformanceCounter(&counter);
	Logger::Get().Info(fmt::format("ParameterFlyoutTrace seq={} qpc={} pid={} tid={} event={} object=0x{:x} a={} b={}",
		++sequence, counter.QuadPart, GetCurrentProcessId(), GetCurrentThreadId(), event, object, a, b));
}

inline void TraceParameterFlyoutWindow(const char* event, HWND window, WPARAM wParam, LPARAM lParam) {
	if (!IsParameterFlyoutTraceEnabled()) return;
	TraceParameterFlyout(event, uintptr_t(window), int64_t(wParam), int64_t(lParam));
	const HWND foreground = GetForegroundWindow();
	DWORD process = 0;
	GetWindowThreadProcessId(foreground, &process);
	TraceParameterFlyout("host.foreground", uintptr_t(foreground), process, intptr_t(GetWindow(foreground, GW_OWNER)));
	RECT client{};
	GetClientRect(window, &client);
	TraceParameterFlyout("host.client", uintptr_t(window), client.right - client.left, client.bottom - client.top);
}

} // namespace Magpie
