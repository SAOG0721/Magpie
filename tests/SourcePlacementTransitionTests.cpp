#include <windows.h>
#include "../src/Magpie.Core/include/SourcePlacementTransition.h"
#include <cassert>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <utility>

using namespace std::chrono_literals;
static int checks = 0;
static void Check(bool value, const char* message) {
	++checks;
	if (!value) throw std::runtime_error(message);
}

static void TestDeadline() {
	using Magpie::SourcePlacementTransition;
	using Time = SourcePlacementTransition::Clock::time_point;
	SourcePlacementTransition transition;
	const Time origin{}; // Zero is a valid first timestamp, not an idle sentinel.
	Check(!transition.ShouldDeferPlacement(false, true, true, origin), "Unchanged state was deferred");
	Check(transition.ShouldDeferPlacement(true, true, true, origin), "First flag-only sample was not deferred");
	for (int elapsed = 1; elapsed < 100; ++elapsed) {
		Check(transition.ShouldDeferPlacement(true, true, true, origin + elapsed * 1ms),
			"Wait ended before its fixed deadline");
	}
	Check(!transition.ShouldDeferPlacement(true, true, true, origin + 100ms), "100ms boundary was deferred");
	Check(!transition.ShouldDeferPlacement(true, true, true, origin + 101ms), "Expired deadline was renewed");
	Check(!transition.ShouldDeferPlacement(true, true, true, origin + 2s), "Repeated timeout opened a new wait");

	Check(!transition.ShouldDeferPlacement(true, false, true, origin + 3s), "Changed rectangle was deferred");
	Check(transition.ShouldDeferPlacement(true, true, true, origin + 4s), "Geometry completion did not clear old wait");
	Check(!transition.ShouldDeferPlacement(false, true, true, origin + 4010ms), "Candidate reversion was deferred");
	Check(transition.ShouldDeferPlacement(true, true, true, origin + 4020ms), "Candidate reversion did not reset wait");
	Check(transition.ShouldDeferPlacement(true, true, true, origin + 4119ms), "New transition inherited old deadline");
	Check(!transition.ShouldDeferPlacement(true, true, true, origin + 4120ms), "New transition deadline was extended");

	Check(!transition.ShouldDeferPlacement(true, true, false, origin + 5s), "Hidden/minimized source was deferred");
	Check(transition.ShouldDeferPlacement(true, true, true, origin + 6s), "Ineligible state retained expired deadline");
	transition.Reset();
	Check(transition.ShouldDeferPlacement(true, true, true, origin + 6070ms), "New source did not get its own wait");
	Check(transition.ShouldDeferPlacement(true, true, true, origin + 6169ms), "Reset retained the previous source deadline");
	Check(!transition.ShouldDeferPlacement(true, true, true, origin + 6170ms), "Reset wait exceeded 100ms");
}

inline bool operator==(const RECT& a, const RECT& b) {
	return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}
inline bool operator==(const SIZE& a, const SIZE& b) { return a.cx == b.cx && a.cy == b.cy; }
static HWND sourceWindow = reinterpret_cast<HWND>(uintptr_t(1));
static HWND otherWindow = reinterpret_cast<HWND>(uintptr_t(2));
static bool sourceExists = true, sourceVisible = true;
static RECT observedRect{};
static WINDOWPLACEMENT observedPlacement{sizeof(WINDOWPLACEMENT)};
static BOOL IsWindowForTest(HWND) { return sourceExists; }
static BOOL IsWindowVisibleForTest(HWND) { return sourceVisible; }
static BOOL GetWindowPlacementForTest(HWND, WINDOWPLACEMENT* placement) { *placement = observedPlacement; return TRUE; }
static BOOL GetWindowRectForTest(HWND, RECT* rect) { *rect = observedRect; return TRUE; }
static HMONITOR MonitorFromWindowForTest(HWND, DWORD) { return reinterpret_cast<HMONITOR>(uintptr_t(1)); }
static BOOL GetMonitorInfoForTest(HMONITOR, MONITORINFO* info) {
	info->rcMonitor = info->rcWork = RECT{0, 0, 2560, 1440};
	return TRUE;
}
namespace fmt {
template<class... Args> static std::string_view format(std::string_view text, Args&&...) { return text; }
}
namespace Magpie {
struct Logger {
	static Logger& Get() { static Logger logger; return logger; }
	void Info(std::string_view) {} void Win32Error(std::string_view) {}
};
struct Win32Helper {
	template<class T> static T* LoadSystemFunction(const wchar_t*, const char*) { return nullptr; }
	static bool IsWindowHung(HWND) { return false; }
	static SIZE GetSizeOfRect(const RECT& rect) { return {rect.right - rect.left, rect.bottom - rect.top}; }
	static void OffsetRect(RECT& rect, LONG x, LONG y) {
		rect.left += x; rect.right += x; rect.top += y; rect.bottom += y;
	}
};
enum class SourceForegroundKind { None, Source };
struct ScalingWindow {
	static ScalingWindow& Get() { static ScalingWindow window; return window; }
	static unsigned RunId() { return 1; }
	SourceForegroundKind ClassifyForeground(HWND window) {
		return window == sourceWindow ? SourceForegroundKind::Source : SourceForegroundKind::None;
	}
	bool IsSessionForeground(HWND window) { return window == sourceWindow; }
	HWND SourceRoot() { return sourceWindow; }
};
static bool IsWindowMoving(HWND) { return false; }
static bool IsPrimaryMouseButtonDown() { return false; }
struct SrcTracker {
	bool UpdateState(HWND, bool, bool, bool&, bool&, bool&, bool&, bool&) noexcept;
	bool UpdateForeground(HWND) noexcept;
	HWND _hWnd = sourceWindow;
	RECT _windowRect{50, 50, 1499, 751}, _windowFrameRect{}, _srcRect{};
	bool _isSessionForeground = true, _isSourceForeground = true, _isMaximized = false, _isMoving = false;
	SourcePlacementTransition _placementTransition;
	bool _placementChangeDeferred = false;
};
#define IsWindow IsWindowForTest
#define IsWindowVisible IsWindowVisibleForTest
#define GetWindowPlacement GetWindowPlacementForTest
#define GetWindowRect GetWindowRectForTest
#define MonitorFromWindow MonitorFromWindowForTest
#undef GetMonitorInfo
#define GetMonitorInfo GetMonitorInfoForTest
#include "SourcePlacementTracker.inc"
#undef IsWindow
#undef IsWindowVisible
#undef GetWindowPlacement
#undef GetWindowRect
#undef MonitorFromWindow
#undef GetMonitorInfo
}

struct StateResult {
	bool alive, invisible = false, focused = false, rect = false, size = false, moving = false;
};
static StateResult Sample(Magpie::SrcTracker& tracker, HWND foreground = otherWindow) {
	StateResult result{};
	result.alive = tracker.UpdateState(foreground, false, false,
		result.invisible, result.focused, result.rect, result.size, result.moving);
	return result;
}
static void TestTrackerCommit() {
	using Magpie::SrcTracker;
	const RECT smallRect{50, 50, 1499, 751}, maximizedRect{-8, -8, 2568, 1400};
	observedPlacement.showCmd = SW_SHOWMAXIMIZED;
	observedRect = smallRect;
	SrcTracker tracker;
	auto result = Sample(tracker);
	Check(result.alive && tracker._placementChangeDeferred, "Real tracker did not defer flag-only sample");
	Check(!tracker._isMaximized && tracker._windowRect == smallRect, "Deferred sample committed placement baseline");
	Check(tracker._isSessionForeground && tracker._isSourceForeground && !result.focused,
		"Deferred sample consumed foreground notification");
	Check(!result.rect && !result.size && !result.invisible, "Deferred sample requested source transition");
	observedRect = maximizedRect;
	result = Sample(tracker);
	Check(result.alive && !tracker._placementChangeDeferred && tracker._isMaximized,
		"Geometry completion did not commit maximized state");
	Check(result.focused && !tracker._isSessionForeground && !tracker._isSourceForeground,
		"Foreground change was lost after placement settled");
	Check(result.rect && result.size, "Completed placement did not request reconstruction");
	result = Sample(tracker);
	Check(!result.focused, "Foreground notification was repeated");

	SrcTracker restored;
	restored._isMaximized = true;
	restored._windowRect = maximizedRect;
	observedPlacement.showCmd = SW_SHOWNORMAL;
	Check(Sample(restored).alive && restored._placementChangeDeferred && restored._isMaximized,
		"Restore committed its state before geometry changed");
	observedRect = smallRect;
	result = Sample(restored);
	Check(result.alive && !restored._placementChangeDeferred && !restored._isMaximized && result.size,
		"Restore completion did not request reconstruction");

	SrcTracker sameSize;
	observedPlacement.showCmd = SW_SHOWMAXIMIZED;
	sameSize._placementTransition.ShouldDeferPlacement(true, true, true,
		Magpie::SourcePlacementTransition::Clock::now() - 101ms);
	result = Sample(sameSize);
	Check(result.alive && !sameSize._placementChangeDeferred && sameSize._isMaximized && result.size,
		"Timed-out same-size state change did not commit and request reconstruction");

	SrcTracker cancelled;
	observedRect = smallRect;
	observedPlacement.showCmd = SW_SHOWMAXIMIZED;
	Check(Sample(cancelled).alive && cancelled._placementChangeDeferred, "Cancellation setup failed");
	observedPlacement.showCmd = SW_SHOWNORMAL;
	result = Sample(cancelled);
	Check(result.alive && !cancelled._placementChangeDeferred && !cancelled._isMaximized && !result.size,
		"Reverted candidate still requested placement change");
	Check(result.focused, "Candidate cancellation lost foreground change");

	SrcTracker hidden;
	observedPlacement.showCmd = SW_SHOWMAXIMIZED;
	Check(Sample(hidden).alive && hidden._placementChangeDeferred, "Hidden setup failed");
	sourceVisible = false;
	result = Sample(hidden);
	Check(result.alive && result.invisible && !hidden._placementChangeDeferred,
		"Source hide was delayed by placement wait");
	sourceVisible = true;

	SrcTracker minimized;
	minimized._isMaximized = true;
	observedPlacement.showCmd = SW_SHOWMINIMIZED;
	observedPlacement.rcNormalPosition = smallRect;
	observedPlacement.ptMinPosition.x = smallRect.left;
	result = Sample(minimized);
	Check(result.alive && !result.invisible && !minimized._placementChangeDeferred && !minimized._isMaximized,
		"Existing first-stage minimization handling changed");
	Check(result.focused, "Minimization early return lost foreground change");
	observedPlacement.ptMinPosition.x = -32000;
	result = Sample(minimized);
	Check(result.alive && result.invisible && !minimized._placementChangeDeferred,
		"Completed minimization was delayed");

	sourceExists = false;
	Check(!Sample(minimized).alive, "Destroyed source was retained for placement wait");
	sourceExists = true;
}

int main() {
	TestDeadline();
	TestTrackerCommit();
	std::cout << "PASS: " << checks << " source placement deadline and production tracker checks\n";
}
