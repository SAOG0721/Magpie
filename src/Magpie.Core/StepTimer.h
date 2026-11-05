#pragma once

namespace Magpie {

enum class StepTimerStatus {
	WaitForNewFrame,
	WaitForFPSLimiter,
	ForceNewFrame
};

class StepTimer {
public:
	StepTimer() = default;

	StepTimer(const StepTimer&) = delete;
	StepTimer(StepTimer&&) = delete;

	void Initialize(float minFrameRate, std::optional<float> maxFrameRate, bool strictStart = false) noexcept;
	void CaptureStarting() noexcept {
		if (_strictStart) _nextFrameStartTime = std::chrono::steady_clock::now();
	}

	StepTimerStatus WaitForNextFrame(bool waitForNewFrame, bool& fpsUpdated,
		HANDLE frameArrivedEvent = nullptr,
		std::optional<std::chrono::nanoseconds> outputRefreshDelay = {}) noexcept;

	void PrepareForRender(bool isNewCaptureFrame = true) noexcept;

	uint32_t FrameCount() const noexcept {
		return _frameCount;
	}

	// 从前端线程调用
	uint32_t FPS() const noexcept {
		return _framesPerSecond.load(std::memory_order_relaxed);
	}
	uint32_t RenderUpdatesPerSecond() const noexcept { return _updatesPerSecond.load(std::memory_order_relaxed); }

private:
	bool _strictStart = false;
	bool _HasMinInterval() const noexcept;
	bool _HasMaxInterval() const noexcept;

	void _WaitForMsgAndTimer(std::chrono::nanoseconds time,
		HANDLE frameArrivedEvent = nullptr) noexcept;

	bool _UpdateFPS(std::chrono::time_point<std::chrono::steady_clock> now) noexcept;

	std::chrono::nanoseconds _minInterval{};
	std::chrono::nanoseconds _maxInterval{ std::numeric_limits<std::chrono::nanoseconds::rep>::max() };
	wil::unique_handle _hTimer;

	std::chrono::time_point<std::chrono::steady_clock> _thisFrameStartTime;
	// Minimum-FPS redraws have their own anchor. Repainting the same capture
	// must not postpone admission of the next content frame.
	std::chrono::time_point<std::chrono::steady_clock> _lastRenderStartTime;
	std::chrono::time_point<std::chrono::steady_clock> _nextFrameStartTime;
	std::chrono::time_point<std::chrono::steady_clock> _lastSecondTime;

	uint32_t _frameCount = 0;
	std::atomic<uint32_t> _framesPerSecond = 0;
	uint32_t _framesThisSecond = 0;
	uint32_t _updatesThisSecond = 0;
	std::atomic<uint32_t> _updatesPerSecond = 0;
};

}
