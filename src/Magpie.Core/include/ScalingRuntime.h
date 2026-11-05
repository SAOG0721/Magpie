#pragma once
#include "Event.h"
#include <optional>

namespace Magpie {

struct EffectOption;
struct FrameRefreshSettings;
struct ToolbarShortcutLabels;
struct SessionWindowIdentity;
enum class OverlayAction;
enum class ScreenshotContent : uint32_t;

enum class ScalingState {
	Idle,
	Starting,
	Scaling,
	Stopping,
	Waiting
};

class ScalingRuntime {
public:
	ScalingRuntime();
	~ScalingRuntime();

	bool Start(HWND hwndSrc, struct ScalingOptions&& options, bool force);

	void ToggleScaling(bool isWindowedMode);

	void SwitchToolbarState();
	void InvokeOverlayAction(OverlayAction action);

	void Stop();
	// Main-thread system-key notification; cancels pending fullscreen restarts too.
	bool StopForTaskSwitch();
	void UpdateFrameRefreshSettings(FrameRefreshSettings settings, std::optional<bool> simpleMode = std::nullopt, bool policyChanged = false);
	void UpdateToolbarShortcutLabels(ToolbarShortcutLabels labels);
	void UpdateScreenshotContent(ScreenshotContent content);
	void UpdateMainUI(SessionWindowIdentity identity);

	uint32_t RunId() const noexcept;
	void UpdateEffectParameterFromSettings(uint32_t modeIdx, std::wstring modeName,
		uint32_t effectIdx, EffectOption effect, std::string parameter, float value);

	bool RestartWithEffectParameters(
		HWND hwndSource,
		HWND hwndScaling,
		uint32_t scalingRunId,
		std::vector<EffectOption>&& effects,
		FrameRefreshSettings frameRefresh
	);

	ScalingState State() const noexcept {
		return _state.load(std::memory_order_relaxed);
	}

	// 调用者应处理线程同步
	MultithreadEvent<ScalingState> StateChanged;

private:
	void _ScalingThreadProc() noexcept;

	// 确保 _dispatcher 完成初始化
	const winrt::DispatcherQueue& _Dispatcher() noexcept;

	void _State(ScalingState value);

	std::thread _scalingThread;

	winrt::DispatcherQueue _dispatcher{ nullptr };
	std::atomic<bool> _dispatcherInitialized = false;
	// 只能在主线程访问，省下检查 _dispatcherInitialized 的开销
	bool _dispatcherInitializedCache = false;

	std::atomic<ScalingState> _state = ScalingState::Idle;
	std::atomic<uint64_t> _commandGeneration = 0;
	std::atomic<bool> _quitting = false;
};

}
