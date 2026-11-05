#include "pch.h"
#include "StepTimer.h"
#include "FrameTrace.h"
#include "FramePacingWait.h"

using namespace std::chrono;

namespace Magpie {

void StepTimer::Initialize(float minFrameRate, std::optional<float> maxFrameRate, bool strictStart) noexcept {
	assert(minFrameRate >= 0);
	_strictStart = strictStart;
	_minInterval = {};
	_maxInterval = nanoseconds::max();
	if (minFrameRate > 0) {
		_maxInterval = duration_cast<nanoseconds>(duration<float>(1 / minFrameRate));
	}

	if (maxFrameRate) {
		assert(*maxFrameRate > 0);
		_minInterval = duration_cast<nanoseconds>(duration<float>(1 / *maxFrameRate));
		assert(_minInterval <= _maxInterval);

		if (_HasMaxInterval()) {
			// 确保最大帧间隔是最小帧间隔的整数倍，这能使帧间隔保持稳定，代价是实际最小帧率可能比要求的稍高一点
			_maxInterval = _maxInterval / _minInterval * _minInterval;
		}
	}
}

// 渲染新帧时以该次循环开始捕获的时间点作为新帧的开始时间，只有这个时间点是我们可以控制的。
// 下图中的 wait 包括等待最大帧率限制和等待新帧，WaitForNextFrame 在此期间多次执行。
// PrepareForRender 在 render 开始前执行。
// 
// _thisFrameStartTime      _nextFrameStartTime
//         │                         │
// ────────▼─────────┬────────┬──────▼─────────
//    wait │ capture │ render │ wait │ capture
//
StepTimerStatus StepTimer::WaitForNextFrame(bool waitForNewFrame, bool& fpsUpdated,
	HANDLE frameArrivedEvent, std::optional<nanoseconds> outputRefreshDelay) noexcept {
	fpsUpdated = false;

	// 不断更新 _nextFrameStartTime 直到新帧到达
	_nextFrameStartTime = steady_clock::now();

	if (_thisFrameStartTime == time_point<steady_clock>{}) {
		// 等待第一帧，无需更新 FPS
		if (waitForNewFrame) {
			FrameTrace::Scope traceSource(FrameTrace::Event::CaptureSourceWait, -1, frameArrivedEvent != nullptr);
			const DWORD result = MsgWaitForMultipleObjectsEx(frameArrivedEvent ? 1 : 0,
				frameArrivedEvent ? &frameArrivedEvent : nullptr, INFINITE,
				QS_ALLINPUT, MWMO_INPUTAVAILABLE);
			FrameTrace::Mark(FrameTrace::Event::CaptureWake, result, frameArrivedEvent != nullptr);
		}

		return StepTimerStatus::WaitForNewFrame;
	}

	// 包括当前帧的捕获时间和渲染时间以及渲染完成后已经等待的时间
	const nanoseconds delta = _nextFrameStartTime - _thisFrameStartTime;
	const nanoseconds renderDelta = _nextFrameStartTime - _lastRenderStartTime;

	if (renderDelta >= _maxInterval) {
		return StepTimerStatus::ForceNewFrame;
	}

	// 没有新帧也应更新 FPS。作为性能优化，强制帧无需更新，因为 PrepareForRender 必定会执行
	fpsUpdated = _UpdateFPS(_nextFrameStartTime);
	// a is the signed remaining nanoseconds at this observation, b the software
	// minimum period. Together with CaptureStart these expose scheduler
	// lateness without treating a source notification as capture acceptance.
	FrameTrace::Mark(FrameTrace::Event::CaptureDeadline,
		(_HasMinInterval() ? _minInterval - delta : 0ns).count(), _minInterval.count());

	if (delta < _minInterval) {
		FrameTrace::Scope traceTimer(FrameTrace::Event::CaptureTimerWait, (_minInterval - delta).count(), _strictStart);
		_WaitForMsgAndTimer(_minInterval - delta);
		return StepTimerStatus::WaitForFPSLimiter;
	}

	// 有的捕获方法当有新帧时会有消息到达
	if (waitForNewFrame) {
		FrameTrace::Scope traceSource(FrameTrace::Event::CaptureSourceWait);
		if (outputRefreshDelay) {
			const auto wait = std::max(0ns, std::min(*outputRefreshDelay, _maxInterval-renderDelta));
			traceSource.Data(wait.count(), frameArrivedEvent != nullptr);
			_WaitForMsgAndTimer(wait, frameArrivedEvent);
		} else if (_HasMaxInterval()) {
			traceSource.Data((_maxInterval - renderDelta).count(), frameArrivedEvent != nullptr);
			_WaitForMsgAndTimer(_maxInterval - renderDelta, frameArrivedEvent);
		} else {
			traceSource.Data(nanoseconds(500ms).count(), frameArrivedEvent != nullptr);
			// 没有最小帧率限制则只需等待消息。为了及时更新 FPS，每次等待 500ms
			const DWORD result = MsgWaitForMultipleObjectsEx(frameArrivedEvent ? 1 : 0,
				frameArrivedEvent ? &frameArrivedEvent : nullptr, 500,
				QS_ALLINPUT, MWMO_INPUTAVAILABLE);
			FrameTrace::Mark(FrameTrace::Event::CaptureWake, result, frameArrivedEvent != nullptr);
		}
	}

	return StepTimerStatus::WaitForNewFrame;
}

void StepTimer::PrepareForRender(bool isNewCaptureFrame) noexcept {
	// 进入新一帧，计算此帧的开始时间
	if (isNewCaptureFrame && _HasMinInterval() && !_strictStart) {
		// 限制最大帧率时帧间隔必须是最小帧间隔的整数倍，_nextFrameStartTime 需要稍微向前修正。
		// 出于同样的原因，最大帧间隔应是最小帧间隔的整数倍。
		_thisFrameStartTime = _nextFrameStartTime -
			(_nextFrameStartTime - _thisFrameStartTime) % _minInterval;
	} else if (isNewCaptureFrame) {
		_thisFrameStartTime = _nextFrameStartTime;
	}
	_lastRenderStartTime = _nextFrameStartTime;

	if (isNewCaptureFrame) ++_framesThisSecond;
	++_updatesThisSecond;
	++_frameCount;

	_UpdateFPS(steady_clock::now());
}

void StepTimer::_WaitForMsgAndTimer(std::chrono::nanoseconds time,
	HANDLE frameArrivedEvent) noexcept {
	if (time.count() <= 0) return;
	const DWORD result = WaitForFramePacing(time, _hTimer, frameArrivedEvent);
	FrameTrace::Mark(FrameTrace::Event::CaptureWake, result, frameArrivedEvent ? 3 : 2);
}

// FPS 更新时返回 true
bool StepTimer::_UpdateFPS(time_point<steady_clock> now) noexcept {
	if (_lastSecondTime == time_point<steady_clock>{}) {
		// 第一帧
		_lastSecondTime = now;
	} else {
		const nanoseconds delta = now - _lastSecondTime;
		if (delta < 1s) {
			return false;
		}

		_lastSecondTime = now - delta % 1s;
	}

	const uint32_t oldFPS = _framesPerSecond.exchange(_framesThisSecond, std::memory_order_relaxed);
	_updatesPerSecond.store(_updatesThisSecond, std::memory_order_relaxed);
	const bool changed = oldFPS != _framesThisSecond;
	_framesThisSecond = 0;
	_updatesThisSecond = 0;

	return changed;
}

bool StepTimer::_HasMinInterval() const noexcept {
	return _minInterval.count() != 0;
}

bool StepTimer::_HasMaxInterval() const noexcept {
	return _maxInterval.count() != std::numeric_limits<nanoseconds::rep>::max();
}

}
