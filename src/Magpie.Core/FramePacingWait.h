#pragma once
#include <algorithm>
#include <chrono>

namespace Magpie {

// Return to the caller on messages; do not dispatch them inside a render
// operation or keep a shared-resource mutex locked across this wait.
// Return the OS wait result, or WAIT_TIMEOUT without waiting if already due.
inline DWORD WaitForFramePacing(std::chrono::nanoseconds remaining,
	wil::unique_handle& timer, HANDLE event = nullptr) noexcept {
	if (remaining.count() <= 0) return WAIT_TIMEOUT;
	if (!timer) {
		timer.reset(CreateWaitableTimerExW(nullptr, nullptr,
			CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_MODIFY_STATE | SYNCHRONIZE));
		if (!timer) timer.reset(CreateWaitableTimerExW(nullptr, nullptr, 0,
			TIMER_MODIFY_STATE | SYNCHRONIZE));
	}
	LARGE_INTEGER due{ .QuadPart = -std::max<int64_t>(1, (remaining.count() + 99) / 100) };
	if (timer && SetWaitableTimerEx(timer.get(), &due, 0, nullptr, nullptr, nullptr, 0)) {
		HANDLE handles[]{timer.get(), event};
		return MsgWaitForMultipleObjectsEx(event ? 2 : 1, handles, INFINITE,
			QS_ALLINPUT, MWMO_INPUTAVAILABLE);
	} else {
		const DWORD timeout = static_cast<DWORD>(std::min<int64_t>(
			(remaining.count() + 999'999) / 1'000'000, 50));
		return MsgWaitForMultipleObjectsEx(event ? 1 : 0, event ? &event : nullptr,
			timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
	}
}

// Shared by DXGI and XeSS. A message-pump wait may consume the auto-reset
// capacity event, so BeginFrame must reuse that token instead of waiting twice.
class FrameLatencyGate {
public:
	DWORD TryAcquire(HANDLE event, DWORD timeout = 0) noexcept {
		const DWORD result = _acquired ? WAIT_OBJECT_0 : WaitForSingleObject(event, timeout);
		_acquired = result == WAIT_OBJECT_0;
		_waiting = result == WAIT_TIMEOUT;
		return result;
	}
	bool Wait(HANDLE event, DWORD timeout, DWORD& result) noexcept {
		if (!_waiting || _acquired || !event) return false;
		result = MsgWaitForMultipleObjectsEx(1, &event, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
		if (result == WAIT_OBJECT_0) {
			_acquired = true;
			_waiting = false;
		} else if (result == WAIT_FAILED) {
			_waiting = false;
		}
		return true;
	}
	void Reset() noexcept { _acquired = _waiting = false; }
private:
	bool _acquired = false;
	bool _waiting = false;
};

}
