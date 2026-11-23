#pragma once
#include "ScalingOptions.h"
#include "EffectDesc.h"
#include "EffectParameterRestart.h"
#include "FramePresentationTiming.h"
#include "SrcTracker.h"
#include "WindowBase.h"
#include "SourceWindowCoordinator.h"
#include "ScalingWindowRetirement.h"
#include "Win32Helper.h"
#include <deque>

namespace Magpie {

class CursorManager;

class ScalingWindow final : public WindowBaseT<ScalingWindow> {
	using base_type = WindowBaseT<ScalingWindow>;
	friend base_type;

public:
	static ScalingWindow& Get() noexcept {
		static ScalingWindow instance;
		return instance;
	}

	// 用于检查当前缩放是否结束
	static uint32_t RunId() noexcept {
		return _runId.load(std::memory_order_relaxed);
	}

	static void Dispatcher(const winrt::DispatcherQueue& value) noexcept {
		_dispatcher = value;
	}

	static const winrt::DispatcherQueue& Dispatcher() noexcept {
		return _dispatcher;
	}

	void Start(HWND hwndSrc, ScalingOptions&& options) noexcept;

	void Stop() noexcept;
	void Destroy() noexcept;
	bool CompletePendingDestroy() noexcept;
	bool IsRetiring() const noexcept { return _retirement.Pending(); }
	void PollSourceState() noexcept;
	bool SourceIdentityValid() const noexcept { return _sourceCoordinator.Ready(); }
	HWND SourceRoot() const noexcept { return _sourceCoordinator.Source().root; }
	SourceForegroundKind ClassifyForeground(HWND foreground) const noexcept;
	bool IsSessionForeground(HWND foreground) const noexcept;
	bool IsSourceForeground() const noexcept { return ClassifyForeground(Win32Helper::GetRealForegroundWindow()) == SourceForegroundKind::Source; }
	bool IsParameterForeground(HWND foreground, bool outputTransition = false) const noexcept;
	HWND SourceActivationTarget() const noexcept;
	void RequestStop(uint32_t runId) noexcept {
		if (Handle() && !_isDestroying && runId == RunId()) _stopRequested = true;
	}
	bool ProcessPendingStop() noexcept {
		if (!_stopRequested || HasHeldParameterInput()) return false;
		_stopRequested = false;
		Stop();
		return !Handle();
	}
	bool HasHeldParameterInput() const noexcept;
	bool ProcessPendingSourceTransition() noexcept;
	bool HasPendingSourceTransition() const noexcept { return _pendingSourceTransition != 0; }
	bool IsSourceStateCheckDeferred() const noexcept { return _sourceStateCheckDeferred; }

	void ToggleScaling(bool isWindowedMode) noexcept;

	// Shared with the main-thread task-switch hook, including initialization,
	// toolbar mode changes and the interval between automatic restarts.
	static bool SessionWindowedMode() noexcept {
		return _sessionWindowedMode.load(std::memory_order_acquire);
	}
	static void SessionWindowedMode(bool windowed) noexcept {
		_sessionWindowedMode.store(windowed, std::memory_order_release);
	}

	void SwitchToolbarState() noexcept;

	void Render() noexcept;
	void RenderOverlay() noexcept;
	bool RenderNextDLSSFGFrame() noexcept;
	bool HasPendingDLSSFGFrame() const noexcept { return !_dlssFgFrameJobs.empty(); }
	bool HasPendingFrontendRender() const noexcept { return _frontendRenderPending; }
	bool HasUrgentOverlayInput() const noexcept;

	const RECT& RendererRect() const noexcept {
		return _rendererRect;
	}

	const ScalingOptions& Options() const noexcept {
		return _options;
	}
	void UpdateScreenshotContent(ScreenshotContent content) noexcept {
		_options.screenshotContent = NormalizeScreenshotContent(static_cast<uint32_t>(content));
	}

	class SrcTracker& SrcTracker() noexcept {
		return _srcTracker;
	}

	const class SrcTracker& SrcTracker() const noexcept {
		return _srcTracker;
	}

	class Renderer& Renderer() noexcept {
		return *_renderer;
	}

	const class Renderer& Renderer() const noexcept {
		return *_renderer;
	}

	class CursorManager& CursorManager() noexcept {
		return *_cursorManager;
	}

	const class CursorManager& CursorManager() const noexcept {
		return *_cursorManager;
	}

	class Renderer* TryGetRenderer() noexcept { return _renderer.get(); }
	bool IsParameterInputWindow(HWND hwnd) const noexcept;
	void UpdateToolbarShortcutLabels(ToolbarShortcutLabels labels) noexcept;
	void UpdateMainUI(SessionWindowIdentity identity) noexcept;
	class CursorManager* TryGetCursorManager() noexcept { return _cursorManager.get(); }

	bool IsSrcRepositioning() const noexcept {
		return _isSrcRepositioning;
	}

	void RestartAfterSrcRepositioned() noexcept;
	void RestartWithEffectParameters(
		std::vector<EffectOption>&& effects,
		FrameRefreshSettings frameRefresh
	) noexcept;

	void CleanAfterSrcRepositioned() noexcept;
	bool QueueEffectParameterRestart(uint32_t effectIdx, uint32_t parameterIdx, float value,
		bool waitForOverlaySave) noexcept;
	void ProcessPendingParameterRestart() noexcept;
	bool IsWaitingForParameterRestart() const noexcept { return _parameterRestartQueue.IsWaiting(); }
	void UpdateWaitingEffectParameter(uint32_t effectIdx, const std::string& parameter, float value) noexcept;

	bool IsResizingOrMoving() const noexcept {
		return _isResizingOrMoving;
	}

	bool IsFirstFramePending() const noexcept {
		return _isFirstFrame;
	}

	winrt::hstring GetLocalizedString(std::wstring_view resName) const;

	void ShowToast(std::wstring_view msg) const noexcept {
		_options.showToast(Handle(), msg);
	}

	void ShowError(ScalingError error) const noexcept {
		if (const auto showError = _options.showError) {
			showError(_srcTracker.Handle(), error);
		}
	}

protected:
	LRESULT _MessageHandler(UINT msg, WPARAM wParam, LPARAM lParam) noexcept;

private:
	bool _isDestroying = false;
	bool _stopRequested = false;
	// 1 = source reposition, 2 = stop. A stop takes precedence over a restart.
	mutable uint8_t _pendingSourceTransition = 0;
	bool _sourceStateCheckDeferred = false;
	const char* _sourceStateChangeReason = "unspecified";
	RECT _sourceRectBeforeCheck{};
	ScalingWindow() noexcept;
	~ScalingWindow() noexcept;

	ScalingError _StartImpl(HWND hwndSrc, bool& rendererInitializationFailed) noexcept;
	void _BeginDestroy(HWND window) noexcept;
	void _FinishDestroy() noexcept;
	void _HideOutputForSourceTransition() noexcept;
	bool _EstablishWindowCoordination() noexcept;
	void _CancelParameterRestart() noexcept;

	// 确保渲染窗口长宽比不变，且限制最小和最大尺寸。必须提供 width 和 height 之一，另一个
	// 应为 0。如果 isRendererSize 为真，传入的 width 和 height 为渲染矩形尺寸，否则为缩
	// 放窗口尺寸。返回时 width 和 height 是新的缩放窗口尺寸。
	bool _CalcWindowedScalingWindowSize(int& width, int& height, bool isRendererSize, uint32_t dpi = 0) const noexcept;

	RECT _CalcWindowedRendererRect() const noexcept;

	ScalingError _CalcFullscreenRendererRect(uint32_t& monitorCount) noexcept;

	SIZE _AdjustFullscreenWindowSize(SIZE size, uint32_t dpi = 0) const noexcept;

	ScalingError _InitialMoveSrcWindowInFullscreen() noexcept;

	bool _Show() noexcept;
	bool _PrepareFrontendRender() noexcept;
	void _CompleteFrontendRender(bool submitted, bool fromDLSSFGQueue) noexcept;

	bool _UpdateSrcState(
		bool& isSrcRepositioning,
		bool& srcFocusedChanged
	) noexcept;

	bool _CheckForegroundFor3DGameMode(HWND hwndFore) const noexcept;

	void _SetWindowProps() const noexcept;

	void _UpdateWindowProps() const noexcept;

	void _UpdateTouchProps(const RECT& srcRect) const noexcept;

	void _RemoveWindowProps() const noexcept;

	static LRESULT CALLBACK _RendererWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

	void _ResizeRenderer() noexcept;

	void _MoveRenderer() noexcept;

	static LRESULT CALLBACK _BorderHelperWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

	void _CreateBorderHelperWindows() noexcept;

	void _RepostionBorderHelperWindows() noexcept;

	RECT _CalcSrcTouchRect() const noexcept;

	void _UpdateTouchHoleWindows(bool onInit) noexcept;

	void _UpdateFrameMargins() const noexcept;

	winrt::fire_and_forget _UpdateFocusStateAsync() const noexcept;

	bool _CalcTopmostState() const noexcept;

	bool _IsBorderless() const noexcept;

	void _UpdateRendererRect() noexcept;

	bool _EnsureCaptionVisibleOnScreen() noexcept;

	void _UpdateWindowRectFromWindowPos(const WINDOWPOS& windowPos) noexcept;

	void _DelayedStop(bool onSrcHung = false, bool onSrcRepositioning = false) const noexcept;

	static inline std::atomic<uint32_t> _runId = 0;
	static inline std::atomic<bool> _sessionWindowedMode = false;
	static inline winrt::DispatcherQueue _dispatcher{ nullptr };

	RECT _windowRect{};
	RECT _rendererRect{};
	HWND _hwndRenderer = NULL;

	uint32_t _currentDpi = USER_DEFAULT_SCREEN_DPI;
	uint32_t _topBorderThicknessInClient = 0;
	// Win11 中“无边框”窗口的边框在客户区内
	uint32_t _nonTopBorderThicknessInClient = 0;

	ScalingOptions _options;
	EffectParameterRestartQueue _parameterRestartQueue;
	uint64_t _parameterRestartSaveRevision = 0;
	struct RestartParameter {
		EffectParameterDesc description;
		EffectParameterApplyMode applyMode;
	};
	std::vector<std::vector<RestartParameter>> _restartParameters;
	OverlaySessionState _restartOverlayState;
	std::optional<OverlaySessionState> _repositionOverlayState;
	std::optional<bool> _pendingWindowedMode;
	std::optional<std::pair<std::vector<EffectOption>, FrameRefreshSettings>> _pendingManualParameterRestart;
	std::unique_ptr<class Renderer> _renderer;
	std::unique_ptr<class CursorManager> _cursorManager;

	class SrcTracker _srcTracker;
	SourceWindowCoordinator _sourceCoordinator;
	ScalingWindowRetirement _retirement;
	unsigned _windowCallbackDepth = 0;
	bool _finalizingRetirement = false;
	bool _ngxFaultedAtRetirementStart = false;
	std::chrono::steady_clock::time_point _nextSourceStateCheck{};

	winrt::ResourceLoader _resourceLoader{ nullptr };

	wil::unique_mutex_nothrow _exclModeMutex;

	std::array<wil::unique_hwnd, 4> _hwndResizeHelpers{};
	std::array<wil::unique_hwnd, 4> _hwndTouchHoles{};

	ScalingError _runtimeError = ScalingError::NoError;
	std::string _startupFailureContext;
	uint32_t _startupFailureSystemError = 0;

	// 窗口缩放时切换到全屏缩放或最小化前保存尺寸供以后恢复
	LONG _lastWindowedRendererWidth = 0;

	// 第一帧渲染完成后再显示
	bool _isFirstFrame = false;
	bool _isResizingOrMoving = false;
	// 用于区分调整大小和移动
	bool _isPreparingForResizing = false;
	bool _isMovingDueToSrcMoved = false;
	bool _shouldWaitForRender = false;
	bool _areResizeHelperWindowsVisible = false;
	bool _isSrcRepositioning = false;
	struct DLSSFGFrameJob {
		uint32_t sharedTextureSlot = 0;
		uint32_t sharedTextureGeneration = 0;
		PresentationJobTiming timing;
	};
	std::deque<DLSSFGFrameJob> _dlssFgFrameJobs;
	bool _frontendRenderPending = false;
};

}
