#include "pch.h"
#include "FrameTrace.h"
#include "ScalingWindow.h"
#include "NvidiaForegroundCompatibility.h"
#include "CommonSharedConstants.h"
#include "CursorManager.h"
#include "ExclModeHelper.h"
#include "Logger.h"
#include "Renderer.h"
#include "NgxRuntimeGuard.h"
#include "ScalingWindowOwner.h"
#include "StrHelper.h"
#include "EffectParameterValue.h"
#include "DLSSNRPerformanceParameters.h"
#include "HdrDisplayPreflight.h"
#include "Win32Helper.h"
#include "WindowHelper.h"
#include <dwmapi.h>
#include <ShellScalingApi.h>
#include <timeapi.h>
#include <winrt/Windows.Graphics.Capture.h>

namespace Magpie {

static UINT WM_MAGPIE_SCALINGCHANGED;

// 窗口模式缩放时缩放窗口应遮挡源窗口和它的阴影，在四周留出 50 x DPI 缩放的空间
static constexpr int WINDOWED_MODE_MIN_SPACE_AROUND = 2 * 50;

static void InitMessage() noexcept {
	[[maybe_unused]] static Ignore _ = []() {
		WM_MAGPIE_SCALINGCHANGED =
			RegisterWindowMessage(CommonSharedConstants::WM_MAGPIE_SCALINGCHANGED);

		return Ignore();
	}();
}

static bool IsTopmostWindow(HWND hWnd) noexcept {
	return GetWindowExStyle(hWnd) & WS_EX_TOPMOST;
}

ScalingWindow::ScalingWindow() noexcept :
	_resourceLoader(winrt::ResourceLoader::GetForViewIndependentUse(CommonSharedConstants::APP_RESOURCE_MAP_ID)) {}

ScalingWindow::~ScalingWindow() noexcept {}

static void LogRects(const RECT& srcRect, const RECT& rendererRect, const RECT& windowRect) noexcept {
	Logger::Get().Info(fmt::format("源矩形: {},{},{},{} ({}x{})",
		srcRect.left, srcRect.top, srcRect.right, srcRect.bottom,
		srcRect.right - srcRect.left, srcRect.bottom - srcRect.top));

	Logger::Get().Info(fmt::format("渲染矩形: {},{},{},{} ({}x{})",
		rendererRect.left, rendererRect.top, rendererRect.right, rendererRect.bottom,
		rendererRect.right - rendererRect.left, rendererRect.bottom - rendererRect.top));

	Logger::Get().Info(fmt::format("缩放窗口矩形: {},{},{},{} ({}x{})",
		windowRect.left, windowRect.top, windowRect.right, windowRect.bottom,
		windowRect.right - windowRect.left, windowRect.bottom - windowRect.top));
}

// Capability queries only. No capture session, window or D3D device is created.
static ScalingError CheckCapturePrerequisites(CaptureMethod method) noexcept {
	switch (method) {
	case CaptureMethod::GraphicsCapture:
		try {
			if (winrt::Windows::Graphics::Capture::GraphicsCaptureSession::IsSupported()) {
				return ScalingError::NoError;
			}
			Logger::Get().Error("Scaling preflight: Graphics Capture is unavailable in this system session");
		} catch (const winrt::hresult_error& e) {
			Logger::Get().ComError("Scaling preflight: query Graphics Capture support failed", e.code());
		}
		return ScalingError::CaptureMethodUnavailable;
	case CaptureMethod::DesktopDuplication:
		if (Win32Helper::GetOSVersion().Is20H1OrNewer()) return ScalingError::NoError;
		Logger::Get().Error("Scaling preflight: Desktop Duplication requires Windows 10 2004 or newer");
		return ScalingError::CaptureMethodUnavailable;
	case CaptureMethod::DwmSharedSurface:
		if (Win32Helper::LoadSystemFunction<void()>(L"user32.dll", "DwmGetDxSharedSurface")) {
			return ScalingError::NoError;
		}
		Logger::Get().Win32Error("Scaling preflight: DwmSharedSurface entry point is unavailable");
		return ScalingError::CaptureMethodUnavailable;
	case CaptureMethod::GDI:
		return ScalingError::NoError;
	default:
		Logger::Get().Error("Scaling preflight: select a valid capture method in the profile");
		return ScalingError::CaptureMethodUnavailable;
	}
}

static std::string DescribeScalingWindowOwner(const char* step, HWND window, HWND source, HWND owner,
	const ScalingWindowOwnerResult& result) noexcept {
	return fmt::format("Scaling window owner: step={} status={} system={} sourceClass={} outputClass={} "
		"output={:#x} requested={:#x} actual={:#x} unownedByErrorCode={} rawReturn={:#x} outputStyle={:#x} ownerStyle={:#x}", step, result.StatusName(),
		result.systemError, StrHelper::UTF16ToUTF8(Win32Helper::GetWindowClassName(source)),
		StrHelper::UTF16ToUTF8(Win32Helper::GetWindowClassName(window)),
		reinterpret_cast<uintptr_t>(window), reinterpret_cast<uintptr_t>(owner), reinterpret_cast<uintptr_t>(result.actualOwner),
		result.CanContinueUnowned(), uintptr_t(result.previousOwner), uintptr_t(result.outputStyle), uintptr_t(result.ownerStyle));
}

ScalingError ScalingWindow::_StartImpl(HWND hwndSrc, bool& rendererInitializationFailed) noexcept {
	rendererInitializationFailed = false;
	for (auto& effect : _options.effects) {
		if (NormalizeFrameGenerationParameters(effect))
			Logger::Get().Info("FG input settings normalized: " + effect.name);
	}
	if (_options.effects.empty()) return ScalingError::ScalingModeEmpty;
	if (ValidateFrameGenerationChain(_options.effects).HasConflict()) {
		Logger::Get().Error("Scaling preflight: keep one frame-generation effect in the group");
		return ScalingError::ConflictingFrameGenerationEffects;
	}
	if (NgxRuntimeGuard::IsFaulted() && std::ranges::any_of(_options.effects, [](const auto& effect) {
		return effect.name == "DLSSNR\\DLSSNR_AI_Filter" ||
			ClassifyHdrComponent(effect.name) == HdrComponentKind::RtxVideoHdr ||
			ClassifyFrameGenerationEffect(effect.name) == FrameGenerationEffectKind::DLSS;
	})) {
		Logger::Get().Error("Effect group blocked after NGX fault; fully restart Magpie");
		return ScalingError::NgxRestartRequired;
	}
	if (!_options.parameterSession) {
		_options.parameterSession = std::make_shared<EffectParameterSessionState>(
			_options.effects, _options.frameRefresh, _options.isSimpleFrameRefresh);
	}
	Logger::Get().Info(fmt::format("缩放开始\n\t程序版本: {}\n\tOS 版本: {}\n\t管理员: {}",
#ifdef MP_VERSION_STRING
		STRINGIFY(MP_VERSION_STRING),
#elif defined(MP_COMMIT_ID)
		"dev (" STRINGIFY(MP_COMMIT_ID) ")",
#else
		"dev",
#endif
		Win32Helper::GetOSVersion().ToString<char>(),
		Win32Helper::IsProcessElevated() ? "是" : "否"
	));

#if _DEBUG
	OutputDebugString(fmt::format(L"可执行文件路径: {}\n窗口类: {}\n",
		Win32Helper::GetWindowPath(hwndSrc), Win32Helper::GetWindowClassName(hwndSrc)).c_str());
#endif

	_runtimeError = ScalingError::NoError;
	_isFirstFrame = true;
	_isResizingOrMoving = false;
	_isPreparingForResizing = false;
	_isMovingDueToSrcMoved = false;
	_shouldWaitForRender = false;
	_areResizeHelperWindowsVisible = false;
	_isSrcRepositioning = false;

	if (_options.IsWindowedMode()) {
		if (_options.Is3DGameMode()) {
			return ScalingError::Windowed3DGameMode;
		} else if (_options.captureMethod == CaptureMethod::DesktopDuplication) {
			return ScalingError::WindowedDesktopDuplication;
		}
	}

	if (FindWindow(CommonSharedConstants::SCALING_WINDOW_CLASS_NAME, nullptr)) {
		Logger::Get().Error("已存在缩放窗口");
		return ScalingError::ScalingAlreadyActive;
	}

	InitMessage();

	bool isSrcInvisibleOrMinimized = false;
	if (ScalingError error = _srcTracker.Set(hwndSrc, _options, isSrcInvisibleOrMinimized);
		error != ScalingError::NoError
	) {
		Logger::Get().Error("初始化 SrcTracker 失败");
		return error;
	}

	// Watch identity before capture/backend initialization. Rebuilds retain the
	// existing watch so a destroy notification cannot be lost between outputs.
	if (_sourceCoordinator.Source().capture != hwndSrc) {
		if (!_sourceCoordinator.Prepare(hwndSrc)) {
			Logger::Get().Win32Error("Prepare source window coordination failed");
			return ScalingError::ScalingWindowCreationFailed;
		}
	}
	if (!_sourceCoordinator.Ready()) {
		Logger::Get().Error("Source window identity changed before initialization");
		return ScalingError::SourceWindowClosed;
	}
	_srcTracker.UpdateForeground(Win32Helper::GetRealForegroundWindow());
	const auto& identity = _sourceCoordinator.Source();
	Logger::Get().Info(fmt::format("Window coordination preparing: run={} epoch={} capture={:#x} root={:#x} "
		"capturePid={} captureTid={} rootPid={} rootTid={} captureStyle={:#x} rootStyle={:#x}",
		RunId(), _sourceCoordinator.Epoch(), uintptr_t(identity.capture), uintptr_t(identity.root),
		identity.capturePid, identity.captureTid, identity.rootPid, identity.rootTid,
		uintptr_t(identity.captureStyle), uintptr_t(identity.rootStyle)));

	if (isSrcInvisibleOrMinimized || (_srcTracker.IsMoving() && !_options.IsWindowedMode())) {
		// 等待源窗口状态改变
		_isSrcRepositioning = true;
		return ScalingError::NoError;
	}

	if (_srcTracker.IsZoomed()) {
		if (_options.IsWindowedMode()) {
			Logger::Get().Info("已最大化的窗口不支持窗口模式缩放");
			return ScalingError::BannedInWindowedMode;
		} else if (!_options.RealIsAllowScalingMaximized()) {
			Logger::Get().Info("源窗口已最大化");
			return ScalingError::Maximized;
		}
	}

	if (ScalingError error = CheckCapturePrerequisites(_options.captureMethod);
		error != ScalingError::NoError) {
		return error;
	}
	if (const auto error = CheckHdrComponentPrerequisites(hwndSrc, _options); error != ScalingError::NoError) {
		const auto index = _options.hdrComponents.errorIndex;
		if (_options.hdrComponents.error != HdrComponentError::None) {
			Logger::Get().Error(fmt::format("HDR component preflight failed at effect {}: {}", index + 1,
				index < _options.effects.size() ? _options.effects[index].name : "unknown"));
		} else Logger::Get().Error("HDR display, capture or runtime prerequisites need attention");
		return error;
	}
	// Fullscreen layout calculation may move the source. Check responsiveness
	// before that operation as well as immediately before window creation.
	if (Win32Helper::IsWindowHung(hwndSrc)) return ScalingError::SourceWindowUnresponsive;

	[[maybe_unused]] static Ignore _ = []() {
		WNDCLASSEXW wcex{
			.cbSize = sizeof(wcex),
			.lpfnWndProc = _WndProc,
			.hInstance = wil::GetModuleInstanceHandle(),
			.hCursor = LoadCursor(nullptr, IDC_ARROW),
			.lpszClassName = CommonSharedConstants::SCALING_WINDOW_CLASS_NAME
		};
		RegisterClassEx(&wcex);

		wcex.lpfnWndProc = _RendererWndProc;
		wcex.lpszClassName = CommonSharedConstants::RENDERER_CHILD_WINDOW_CLASS_NAME;
		RegisterClassEx(&wcex);

		return Ignore();
	}();

	const SrcWindowKind srcWindowKind = _srcTracker.WindowKind();
	const bool isWin11 = Win32Helper::GetOSVersion().IsWin11();
	// 不存在非客户区，渲染无需创建在子窗口里
	const bool isAllClient = !isWin11 &&
		(srcWindowKind == SrcWindowKind::NoBorder || srcWindowKind == SrcWindowKind::NoNativeFrame);
	if (_options.IsWindowedMode()) {
		const RECT& srcWindowRect = _srcTracker.WindowRect();

		const POINT windowCenter{
			(srcWindowRect.left + srcWindowRect.right) / 2,
			(srcWindowRect.top + srcWindowRect.bottom) / 2
		};
		HMONITOR hMon = MonitorFromPoint(windowCenter, MONITOR_DEFAULTTONEAREST);

		if (isAllClient) {
			_topBorderThicknessInClient = 0;
			_nonTopBorderThicknessInClient = 0;
		} else {
			GetDpiForMonitor(hMon, MDT_EFFECTIVE_DPI, &_currentDpi, &_currentDpi);

			if (isWin11 && srcWindowKind == SrcWindowKind::NoNativeFrame) {
				// NoNativeFrame 在 Win11 中和 NoTitleBar 一样处理并禁用边框，优点是只需一个辅助窗口
				_topBorderThicknessInClient = 0;
			} else {
				_topBorderThicknessInClient = Win32Helper::GetNativeWindowBorderThickness(_currentDpi);
			}

			if (_srcTracker.WindowKind() == SrcWindowKind::NoBorder) {
				// Win11 中为客户区内的边框预留空间
				assert(isWin11);
				_nonTopBorderThicknessInClient = _topBorderThicknessInClient;
			} else {
				_nonTopBorderThicknessInClient = 0;
			}
		}

		const SIZE srcSize = Win32Helper::GetSizeOfRect(_srcTracker.SrcRect());
		// 填入渲染矩形尺寸
		int windowWidth = 0;
		int windowHeight = 0;
		if (_lastWindowedRendererWidth == 0) {
			if (_options.initialWindowedScaleFactor < 1.0f) {
				// 根据屏幕的工作区尺寸计算
				MONITORINFO mi{ .cbSize = sizeof(mi) };
				if (GetMonitorInfo(hMon, &mi)) {
					const SIZE monitorSize = Win32Helper::GetSizeOfRect(mi.rcWork);
					const float srcAspectRatio = (float)srcSize.cy / srcSize.cx;

					// 放大到显示器的 3/4，且最少放大 1/3 倍
					if ((float)monitorSize.cy / monitorSize.cx > srcAspectRatio) {
						windowWidth = std::max(monitorSize.cx * 3 / 4, srcSize.cx * 4 / 3);
					} else {
						windowHeight = std::max(monitorSize.cy * 3 / 4, srcSize.cy * 4 / 3);
					}
				} else {
					Logger::Get().Win32Error("GetMonitorInfo 失败");
					windowWidth = srcSize.cx;
				}
			} else {
				windowWidth = (LONG)std::lroundf(srcSize.cx * _options.initialWindowedScaleFactor);
			}
		} else {
			// 恢复上次窗口模式缩放尺寸
			windowWidth = _lastWindowedRendererWidth;
		}

		if (!_CalcWindowedScalingWindowSize(windowWidth, windowHeight, true)) {
			Logger::Get().Error("_CalcWindowedScalingWindowSize 失败");
			return ScalingError::SourceWindowGeometryFailed;
		}

		// 让缩放窗口中心点和源窗口中心点相同
		_windowRect.left = srcWindowRect.left -
			(windowWidth - (srcWindowRect.right - srcWindowRect.left)) / 2;
		_windowRect.top = srcWindowRect.top -
			(windowHeight - (srcWindowRect.bottom - srcWindowRect.top)) / 2;
		_windowRect.right = _windowRect.left + windowWidth;
		_windowRect.bottom = _windowRect.top + windowHeight;

		if (Win32Helper::IsWindowHung(_srcTracker.Handle())) {
			Logger::Get().Error("源窗口已挂起");
			return ScalingError::SourceWindowUnresponsive;
		}

		CreateWindowEx(
			WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP,
			CommonSharedConstants::SCALING_WINDOW_CLASS_NAME,
			nullptr,
			WS_OVERLAPPED | WS_CAPTION | WS_THICKFRAME,
			_windowRect.left,
			_windowRect.top,
			_windowRect.right - _windowRect.left,
			_windowRect.bottom - _windowRect.top,
			nullptr,
			NULL,
			wil::GetModuleInstanceHandle(),
			this
		);

		if (!Handle()) {
			_startupFailureSystemError = GetLastError();
			_startupFailureContext = "Create scaling output failed";
			Logger::Get().Error(_startupFailureContext);
			return ScalingError::ScalingWindowCreationFailed;
		}

		if (isAllClient) {
			_rendererRect = _windowRect;
			_hwndRenderer = Handle();
		} else {
			_rendererRect = _CalcWindowedRendererRect();

			// 由于边框的存在，渲染应使用子窗口。WS_EX_LAYERED | WS_EX_TRANSPARENT 使鼠标
			// 穿透子窗口，参见 https://learn.microsoft.com/en-us/windows/win32/winmsg/window-features#layered-windows
			_hwndRenderer = CreateWindowEx(
				WS_EX_NOREDIRECTIONBITMAP | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOPARENTNOTIFY,
				CommonSharedConstants::RENDERER_CHILD_WINDOW_CLASS_NAME,
				nullptr,
				WS_CHILD | WS_VISIBLE,
				_nonTopBorderThicknessInClient,
				_topBorderThicknessInClient,
				_rendererRect.right - _rendererRect.left,
				_rendererRect.bottom - _rendererRect.top,
				Handle(),
				NULL,
				wil::GetModuleInstanceHandle(),
				nullptr
			);
			if (!_hwndRenderer) {
				_startupFailureSystemError = GetLastError();
				_startupFailureContext = "Create renderer child failed";
				Logger::Get().Win32Error("Create renderer child window failed");
				return ScalingError::ScalingWindowCreationFailed;
			}
		}
	} else {
		uint32_t monitorCount;
		ScalingError error = _CalcFullscreenRendererRect(monitorCount);
		if (error != ScalingError::NoError) {
			Logger::Get().Error("_CalcFullscreenRendererRect 失败");
			return error;
		}

		_topBorderThicknessInClient = 0;
		_nonTopBorderThicknessInClient = 0;
		_windowRect = _rendererRect;

		if (Win32Helper::IsWindowHung(_srcTracker.Handle())) {
			Logger::Get().Error("源窗口已挂起");
			return ScalingError::SourceWindowUnresponsive;
		}

		CreateWindowEx(
			WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP,
			CommonSharedConstants::SCALING_WINDOW_CLASS_NAME,
			nullptr,
			WS_POPUP | (monitorCount == 1 ? WS_MAXIMIZE : 0),
			_windowRect.left,
			_windowRect.top,
			_windowRect.right - _windowRect.left,
			_windowRect.bottom - _windowRect.top,
			nullptr,
			NULL,
			wil::GetModuleInstanceHandle(),
			this
		);

		if (!Handle()) {
			_startupFailureSystemError = GetLastError();
			_startupFailureContext = "Create scaling output failed";
			Logger::Get().Error(_startupFailureContext);
			return ScalingError::ScalingWindowCreationFailed;
		}

		_hwndRenderer = Handle();
	}

	LogRects(_srcTracker.SrcRect(), _rendererRect, _windowRect);
	NvidiaForegroundCompatibility::Prepare(_sourceCoordinator.Source(), _hwndRenderer, RunId(),
		_options.isVRREnabled, _options.IsWindowedMode(), _options.mainUI);

	_renderer = std::make_unique<class Renderer>();
	ScalingError error = _renderer->Initialize(_hwndRenderer, _options.overlayOptions);
	if (error != ScalingError::NoError) {
		rendererInitializationFailed = true;
		Logger::Get().Error("初始化 Renderer 失败");
		return error;
	}

	_cursorManager = std::make_unique<class CursorManager>();

	if (_options.IsTouchSupportEnabled()) {
		// 应在 Renderer 初始化后调用。推迟到缩放窗口显示后再显示
		_UpdateTouchHoleWindows(true);
	}
	if (!_sourceCoordinator.Ready()) {
		_startupFailureContext = "Source identity retired after renderer initialization";
		_startupFailureSystemError = ERROR_INVALID_WINDOW_HANDLE;
		Logger::Get().Error(_startupFailureContext);
		return ScalingError::SourceWindowClosed;
	}
	if (Win32Helper::IsWindowHung(hwndSrc)) {
		_startupFailureContext = "Source window unresponsive after renderer initialization";
		Logger::Get().Error(_startupFailureContext);
		return ScalingError::SourceWindowUnresponsive;
	}
	// First-frame GPU/presenter preparation must also stay unowned. Admission
	// and the success notification happen in the common first-display path.
	return ScalingError::NoError;
}

bool ScalingWindow::_EstablishWindowCoordination() noexcept {
	const HWND source = _srcTracker.Handle();
	const HWND root = SourceRoot();
	ScalingError error = ScalingError::NoError;
	uint32_t systemError = 0;
	std::string context;
	if (!_sourceCoordinator.Ready()) {
		error = ScalingError::SourceWindowClosed;
		context = "Source identity retired before first display";
	} else if (Win32Helper::IsWindowHung(source) || Win32Helper::IsWindowHung(root)) {
		error = ScalingError::SourceWindowUnresponsive;
		context = "Source unresponsive before first display";
	} else {
		DWORD integrity = 0;
		if (!Win32Helper::GetWindowIntegrityLevel(root, integrity) || integrity > Win32Helper::GetCurrentProcessIntegrityLevel()) {
			error = ScalingError::LowIntegrityLevel;
			systemError = ERROR_ACCESS_DENIED;
			context = "Source coordination integrity validation failed";
		} else {
			const auto result = TrySetScalingWindowOwner(Handle(), root);
			context = DescribeScalingWindowOwner("first-display", Handle(), source, root, result);
			systemError = result.systemError;
			if (!_sourceCoordinator.Accept(Handle(), result, true)) {
				error = !_sourceCoordinator.Ready() || result.status == ScalingWindowOwnerStatus::InvalidOwner ?
					ScalingError::SourceWindowClosed : ScalingError::ScalingWindowCreationFailed;
			} else {
				Logger::Get().Info(context);
				if (_sourceCoordinator.Mode() == WindowCoordinationMode::ManagedUnowned &&
					!_sourceCoordinator.UpdateManagedOrder(_CalcTopmostState(), _srcTracker.IsSessionForeground(), _options.IsTopmostDisabled())) {
					systemError = GetLastError();
					context = "Managed coordination preparation failed before first display: " + context;
					error = ScalingError::ScalingWindowCreationFailed;
				} else {
					Logger::Get().Info(fmt::format("Window coordination admitted: run={} epoch={} mode={} "
						"reason={} identityVerified=true permissionVerified=true",
						RunId(), _sourceCoordinator.Epoch(), _sourceCoordinator.ModeName(),
						result.IsApplied() ? "native-owner" :
						result.CanContinueUnowned() ? "owner-rejected" : "uwp-owner-not-applied"));
					if (_sourceCoordinator.Mode() == WindowCoordinationMode::NativeOwned &&
						!AttachThreadInput(GetCurrentThreadId(), GetWindowThreadProcessId(root, nullptr), FALSE)) {
						Logger::Get().Win32Warn("Detach source input queue after setting window owner failed");
					}
					return true;
				}
			}
		}
	}
	Logger::Get().Error(context);
	if (const auto report = _options.reportErrorDetails) report(source, error, context, systemError);
	else ShowError(error);
	_DelayedStop();
	return false;
}

void ScalingWindow::Start(HWND hwndSrc, ScalingOptions&& options) noexcept {
	CompletePendingDestroy();
	if (_retirement.Pending() || _isDestroying) {
		Logger::Get().Error("New scaling session deferred until previous output retirement completes");
		return;
	}
	assert(!Handle());
	_stopRequested = false;
	_frontendRenderPending = false;
	_pendingSourceTransition = 0;
	_sourceStateCheckDeferred = false;
	_dlssFgFrameJobs.clear();
	_nextSourceStateCheck = {};

	assert(options.minFrameRate >= 0);
	assert(!options.maxFrameRate.has_value() || *options.maxFrameRate > 0);
	assert(options.cursorScaling >= 0);
	assert(!options.autoHideCursorDelay.has_value() || *options.autoHideCursorDelay > 0);
	assert(options.initialWindowedScaleFactor >= 0);
	assert(!options.screenshotsDir.empty());
	assert(options.showToast && options.showError && options.save);

	// Retired profile flags cannot enable HDR. Explicit conversion components
	// derive capture/output domains for this session and every parameter restart.
	options.hdrComponents = BuildHdrComponentPlan(options.effects, options.legacyHdrCaptureIntent);
	if (!IsValidFrameSyncMode(options.frameSyncMode)) options.frameSyncMode = FrameSyncMode::FrontEdge;
	options.IsHdrCompatibilityEnabled(options.hdrComponents.enabled && options.hdrComponents.outputHdr);
	options.Log();
	// 缩放结束后失效
	// Automatic and explicit parameter restarts pass our own options back in.
	// Self move-assignment may clear vectors/maps and lose the session.
	if (&options != &_options) _options = std::move(options);

	_startupFailureContext.clear();
	_startupFailureSystemError = 0;
	Logger::DiagnosticCapture startupDiagnostic;
	bool rendererInitializationFailed = false;
	ScalingError error = _StartImpl(hwndSrc, rendererInitializationFailed);
	if (error != ScalingError::NoError) {
		if (_options.reportErrorDetails) {
			_options.reportErrorDetails(hwndSrc, error,
				rendererInitializationFailed ? _renderer->InitializationContext() :
					(_startupFailureContext.empty() ? startupDiagnostic.Details() : _startupFailureContext),
				rendererInitializationFailed ? _renderer->InitializationSystemError() :
					(_startupFailureContext.empty() ? startupDiagnostic.SystemError() : _startupFailureSystemError));
		} else {
			_options.showError(hwndSrc, error);
		}
		// 清理
		Stop();
	}
}

void ScalingWindow::Stop() noexcept {
	if (_isDestroying) return;
	NvidiaForegroundCompatibility::Retire();
	if (HasHeldParameterInput()) {
		// Finish the host's press/release pair before destroying its input HWND.
		// Swallowing just a low-level release leaves Windows' async key state down.
		_stopRequested = true;
		return;
	}
	_stopRequested = false;
	_pendingSourceTransition = 0;
	_CancelParameterRestart();
	Destroy();
	// 为了简化逻辑和确保可靠清理，这里始终调用 CleanAfterSrcRepositioned
	if (!_retirement.Pending()) CleanAfterSrcRepositioned();
}

void ScalingWindow::Destroy() noexcept {
	if (_finalizingRetirement || (_isDestroying && _windowCallbackDepth)) return;
	if (!Handle()) { CompletePendingDestroy(); return; }
	_BeginDestroy(Handle());
	if (!DestroyWindow(Handle())) Logger::Get().Win32Error("Destroy scaling output failed; retain session resources");
	CompletePendingDestroy();
}

void ScalingWindow::_BeginDestroy(HWND window) noexcept {
	if (!_retirement.Begin(window, reinterpret_cast<LONG_PTR>(this))) return;
	_isDestroying = true;
	_stopRequested = false;
	_pendingSourceTransition = 0;
	_sourceStateCheckDeferred = false;
	NvidiaForegroundCompatibility::Retire();
	// Only atomic cancellation/event wakeups precede the owner cut.
	if (_cursorManager) _cursorManager->BeginShutdown();
	if (_renderer) _renderer->BeginShutdown();
	++_runId;
	const auto result = TrySetScalingWindowOwner(window, nullptr);
	const auto diagnostic = DescribeScalingWindowOwner("retire-before-destroy", window, _srcTracker.Handle(), nullptr, result);
	if (result.IsApplied()) {
		Logger::Get().Info(diagnostic);
		NvidiaForegroundCompatibility::ReleaseReferenceFocus();
		// Focus restoration and USER32 calls can wait. Do them only after the
		// relationship is verified absent; otherwise retain input resources too.
		if (_cursorManager) _cursorManager->ReleasePendingCursorReposition();
		if (_renderer) _renderer->ReleaseParameterInput();
	} else Logger::Get().Warn(diagnostic);
	_sourceCoordinator.RetireOutput();
}

bool ScalingWindow::CompletePendingDestroy() noexcept {
	if (_finalizingRetirement || !_retirement.Ready(_windowCallbackDepth)) return false;
	// Keep the input owner alive if USER32 temporarily rejects verified clip
	// restoration. Runtime retirement passes continue independently of rendering.
	if (_cursorManager && !_cursorManager->ReleasePendingCursorReposition()) return false;
	_finalizingRetirement = true;
	_FinishDestroy();
	_retirement.Complete();
	_isDestroying = false;
	_finalizingRetirement = false;
	return true;
}

void ScalingWindow::PollSourceState() noexcept {
	if (_isDestroying) return;
	NvidiaForegroundCompatibility::Poll();
	const bool dirty = _sourceCoordinator.TakeDirty();
	const auto now = std::chrono::steady_clock::now();
	if (!dirty && now < _nextSourceStateCheck) return;
	_nextSourceStateCheck = now + 250ms;
	if (_sourceCoordinator.Source().capture && !_sourceCoordinator.Ready()) {
		if (Handle()) { _HideOutputForSourceTransition(); _DelayedStop(); }
		else if (_isSrcRepositioning) CleanAfterSrcRepositioned();
		return;
	}
	if (!Handle() || !_renderer || !_cursorManager || HasPendingSourceTransition()) return;
	bool repositioning = false, focusedChanged = false;
	if (!_UpdateSrcState(repositioning, focusedChanged)) {
		// Retire visually even if a held parameter key delays resource cleanup.
		_HideOutputForSourceTransition();
		_DelayedStop(false, repositioning);
		return;
	}
	if (_sourceStateCheckDeferred) return;
	if (focusedChanged) _renderer->OnSourceFocusChanged();
	if (!_isFirstFrame && (focusedChanged || dirty || _sourceCoordinator.Mode() == WindowCoordinationMode::ManagedUnowned)) {
		_UpdateFocusStateAsync();
	}
}

void ScalingWindow::_HideOutputForSourceTransition() noexcept {
	NvidiaForegroundCompatibility::Retire();
	ShowWindow(Handle(), SW_HIDE);
	for (const auto& helper : _hwndResizeHelpers) if (helper) ShowWindow(helper.get(), SW_HIDE);
	// Touch holes are deliberately unowned; hiding/destroying the output
	// cannot retire these windows on our behalf.
	for (const auto& hole : _hwndTouchHoles) if (hole) ShowWindow(hole.get(), SW_HIDE);
}

void ScalingWindow::ToggleScaling(bool isWindowedMode) noexcept {
	assert(Handle());
	if (HasHeldParameterInput()) {
		_pendingWindowedMode = isWindowedMode;
		return;
	}

	if (_options.IsWindowedMode() == isWindowedMode || !_srcTracker.IsSessionForeground()) {
		Stop();
		return;
	}

	// 源窗口在前台时按快捷键可以切换全屏/窗口模式缩放
	SessionWindowedMode(isWindowedMode);
	_repositionOverlayState = _renderer->CaptureOverlayState();
	_isSrcRepositioning = true;
	if (_options.IsWindowedMode()) {
		_lastWindowedRendererWidth = _rendererRect.right - _rendererRect.left;
	}
	Destroy();
	_options.IsWindowedMode(isWindowedMode);
	RestartAfterSrcRepositioned();
}

void ScalingWindow::SwitchToolbarState() noexcept {
	if (_renderer) {
		_renderer->SwitchToolbarState();
	}
}

void ScalingWindow::Render() noexcept {
	FrameTrace::Scope traceTick(FrameTrace::Event::FrontendTick);
	_frontendRenderPending = false;
	if (!_PrepareFrontendRender()) {
		return;
	}

	_CompleteFrontendRender(
		_renderer->Render(false, _shouldWaitForRender || _isFirstFrame), false);
}

bool ScalingWindow::_PrepareFrontendRender() noexcept {
	FrameTrace::Scope tracePrepare(FrameTrace::Event::FrontendPrepare);
	if (HasPendingSourceTransition()) return false;
	bool isSrcRepositioning = false;
	bool srcFocusedChanged = false;
	if (!_UpdateSrcState(isSrcRepositioning, srcFocusedChanged)) {
		RECT current{};
		const HWND source = _srcTracker.Handle();
		const bool rectValid = GetWindowRect(source, &current);
		Logger::Get().Info(fmt::format(
			"Source transition requested: reason={} reposition={} foreground={:#x} inputHost={:#x} "
			"editing={} focusSettling={} visible={} iconic={} rectValid={} "
			"before=({},{},{},{}) current=({},{},{},{})",
			_sourceStateChangeReason, isSrcRepositioning, uintptr_t(Win32Helper::GetRealForegroundWindow()),
			uintptr_t(_renderer->ParameterInputHandle()), _renderer->IsEditingParameters(),
			_renderer->IsParameterFocusSettling(), IsWindowVisible(source), IsIconic(source), rectValid,
			_sourceRectBeforeCheck.left, _sourceRectBeforeCheck.top,
			_sourceRectBeforeCheck.right, _sourceRectBeforeCheck.bottom,
			current.left, current.top, current.right, current.bottom));
		_DelayedStop(false, isSrcRepositioning);
		return false;
	}
	if (_sourceStateCheckDeferred) return false;

	if (srcFocusedChanged) {
		_renderer->OnSourceFocusChanged();
		if (!_isFirstFrame) _UpdateFocusStateAsync();
	}

	// 虽然可以在第一帧渲染完成后再隐藏系统光标，但某些设备上显示窗口时光标状态会变成忙，
	// 提前隐藏光标可以提高观感。缩放窗口显示后再隐藏光标还可能造成光标闪烁两次，第一次是
	// 创建 D3D 设备后（可能是 OS bug），第二次是我们隐藏系统光标。
	_cursorManager->Update();
	return true;
}

void ScalingWindow::_CompleteFrontendRender(
	bool submitted,
	bool fromDLSSFGQueue
) noexcept {
	if (submitted && _isFirstFrame) {
		Logger::Get().Info(fromDLSSFGQueue ?
			"First frontend frame submitted by DLSSFG FIFO; preparing first display" :
			"First frontend frame submitted by regular renderer; preparing first display");
		// Both regular and DLSSFG first frames enter the same admission gate.
		if (!_Show()) return;
		_isFirstFrame = false;
		Logger::Get().Info(fmt::format("Scaling output shown: run={} epoch={} mode={}",
			RunId(), _sourceCoordinator.Epoch(), _sourceCoordinator.ModeName()));
		// 缩放已开始，给用户一个确认；失败路径已有错误提示
		ShowToast(GetLocalizedString(L"Message_ScalingStarted"));
		const auto& notice = _renderer->MotionConfigurationNotice();
		if (!notice.empty()) ShowToast(notice);
	}
}

void ScalingWindow::RestartAfterSrcRepositioned() noexcept {
	Start(_srcTracker.Handle(), std::move(_options));
	if (Handle() && _renderer && _repositionOverlayState) {
		_renderer->RestoreOverlayState(*_repositionOverlayState);
		_repositionOverlayState.reset();
	}
}

void ScalingWindow::RenderOverlay() noexcept {
	if (!_renderer || !_PrepareFrontendRender()) {
		return;
	}
	_CompleteFrontendRender(_renderer->RenderOverlay(), false);
}

bool ScalingWindow::RenderNextDLSSFGFrame() noexcept {
	if (!_renderer || _dlssFgFrameJobs.empty()) {
		return false;
	}
	if (!_PrepareFrontendRender()) {
		// Keep jobs and their slot ownership until the outer loop commits the
		// transition. Input release may still be pending, or focus may settle.
		return false;
	}

	DLSSFGFrameJob& job = _dlssFgFrameJobs.front();
	const DLSSFGFrameRenderResult result = _renderer->RenderDLSSFGFrame(
		job.sharedTextureSlot, job.sharedTextureGeneration, job.timing);
	if (result == DLSSFGFrameRenderResult::Retry) {
		return false;
	}
	_dlssFgFrameJobs.pop_front();
	_CompleteFrontendRender(result == DLSSFGFrameRenderResult::Presented, true);
	return true;
}

bool ScalingWindow::HasUrgentOverlayInput() const noexcept {
	return _renderer && _renderer->HasUrgentOverlayInput();
}

void ScalingWindow::RestartWithEffectParameters(
	std::vector<EffectOption>&& effects,
	FrameRefreshSettings frameRefresh
) noexcept {
	for (const auto& effect : effects) {
		if (!IsDLSSNRPerformanceOptionValid(effect)) {
			ShowToast(GetLocalizedString(L"EffectParam_DLSSNR_DLSSNR_AI_Filter_InvalidConfiguration"));
			return;
		}
	}
	const HWND hwndSource = _srcTracker.Handle();
	if (!Handle() || !IsWindow(hwndSource) || effects.empty()) {
		ShowToast(GetLocalizedString(
			L"Overlay_EffectParameters_SourceUnavailable"));
		return;
	}
	if (HasHeldParameterInput()) {
		_pendingManualParameterRestart.emplace(std::move(effects), frameRefresh);
		return;
	}

	// Preserve the complete current session options while performing one full
	// teardown/startup. WM_DESTROY must not clear _options in between.
	const auto overlayState = _renderer ? _renderer->CaptureOverlayState() : OverlaySessionState{};
	_CancelParameterRestart();
	_isSrcRepositioning = true;
	Destroy();
	_isSrcRepositioning = false;
	_options.effects = std::move(effects);
	// The backend has joined: update the immutable pacing snapshot only now.
	ApplyFrameRefreshSettings(_options, frameRefresh, _options.isSimpleFrameRefresh);
	_options.parameterSession->Desired(_options.effects);
	Start(hwndSource, std::move(_options));
	if (Handle() && _renderer) _renderer->RestoreOverlayState(overlayState);
}

void ScalingWindow::CleanAfterSrcRepositioned() noexcept {
	// Options are still read by the retained backend until retirement joins it.
	if (_retirement.Pending() && _renderer) return;
	_sourceCoordinator.Reset();
	_CancelParameterRestart();
	_repositionOverlayState.reset();
	_pendingManualParameterRestart.reset();
	_pendingWindowedMode.reset();
	if (_options.save) {
		_options = {};
	}
	_lastWindowedRendererWidth = 0;
	_isSrcRepositioning = false;
}

bool ScalingWindow::QueueEffectParameterRestart(
	uint32_t effectIdx, uint32_t parameterIdx, float value, bool waitForOverlaySave
) noexcept {
	if (!Handle() || !_renderer || !_options.parameterSession || !std::isfinite(value)) return false;
	const auto& descriptions = _renderer->ActiveEffectDescs();
	if (effectIdx >= descriptions.size() || parameterIdx >= descriptions[effectIdx]->params.size()) return false;
	const auto& parameter = descriptions[effectIdx]->params[parameterIdx];
	const auto applied = _options.parameterSession->Applied();
	if (effectIdx >= applied.size()) return false;
	const auto it = applied[effectIdx].parameters.find(parameter.name);
	const float previous = it != applied[effectIdx].parameters.end() ? it->second : std::visit(
		[](const auto& constant) { return static_cast<float>(constant.defaultValue); }, parameter.constant);
	const bool accepted = _parameterRestartQueue.Update(effectIdx, parameter.name,
		NormalizeEffectParameterValue(parameter, value), previous, EffectParameterRestartQueue::Clock::now());
	if (!_parameterRestartQueue.HasChanges()) {
		_parameterRestartSaveRevision = 0;
	} else if (accepted && waitForOverlaySave) {
		// The overlay submits one autosave after drawing all edited controls.
		_parameterRestartSaveRevision = _options.parameterSession->saveRevision + 1;
	}
	return accepted;
}

void ScalingWindow::_CancelParameterRestart() noexcept {
	_parameterRestartQueue.Cancel();
	_parameterRestartSaveRevision = 0;
	_restartParameters.clear();
	_restartOverlayState = {};
}

void ScalingWindow::UpdateWaitingEffectParameter(
	uint32_t effectIdx, const std::string& parameter, float value
) noexcept {
	if (!_parameterRestartQueue.IsWaiting() || !_options.parameterSession || !std::isfinite(value) ||
		effectIdx >= _restartParameters.size() || effectIdx >= _options.effects.size()) return;
	const auto& parameters = _restartParameters[effectIdx];
	const auto it = std::ranges::find_if(parameters, [&](const RestartParameter& p) {
		return p.description.name == parameter;
	});
	if (it == parameters.end()) return;
	const float normalized = NormalizeEffectParameterValue(it->description, value);
	_options.parameterSession->Desired(effectIdx, parameter, normalized);
	// Preserve manual Restart edits as desired only, just as during normal rendering.
	if (it->applyMode != EffectParameterApplyMode::Live) return;
	auto& values = _options.effects[effectIdx].parameters;
	const auto previous = values.find(parameter);
	if (previous != values.end() && std::abs(previous->second - normalized) <= 1e-6f) return;
	values[parameter] = normalized;
	_parameterRestartQueue.WaitAfterStop(EffectParameterRestartQueue::Clock::now());
}

void ScalingWindow::ProcessPendingParameterRestart() noexcept {
	using Clock = EffectParameterRestartQueue::Clock;
	if (HasHeldParameterInput()) return;
	if (_pendingWindowedMode && Handle()) {
		const bool windowed = *_pendingWindowedMode;
		_pendingWindowedMode.reset();
		ToggleScaling(windowed);
		return;
	}
	if (_pendingManualParameterRestart && Handle()) {
		auto request = std::move(*_pendingManualParameterRestart);
		_pendingManualParameterRestart.reset();
		RestartWithEffectParameters(std::move(request.first), request.second);
		return;
	}
	if (_parameterRestartQueue.IsWaiting()) {
		if (!_parameterRestartQueue.ReadyToStart(Clock::now()) || (GetAsyncKeyState(VK_LBUTTON) & 0x8000)) return;
		const HWND source = _srcTracker.Handle();
		if (!IsWindow(source) || !IsWindowVisible(source) || IsIconic(source)) {
			Logger::Get().Info("DLSSNR parameter restart cancelled: source is no longer available");
			Stop();
			return;
		}
		const auto overlayState = _restartOverlayState;
		_CancelParameterRestart();
		Logger::Get().Info("DLSSNR parameter restart: starting effect group after 500 ms pause");
		Start(source, std::move(_options));
		if (Handle() && _renderer) _renderer->RestoreOverlayState(overlayState);
		return;
	}

	// Called by the outer scaling loop, never from an ImGui or rendering callback.
	if (!_parameterRestartQueue.HasChanges() || !Handle() || !_renderer ||
		!_parameterRestartQueue.ReadyToStop(Clock::now(),
		_renderer->IsEffectParameterInputActive() || _renderer->HasPendingOverlayInput() ||
		(GetAsyncKeyState(VK_LBUTTON) & 0x8000))) return;
	const auto& session = _options.parameterSession;
	if (!session) { _CancelParameterRestart(); return; }
	if (_parameterRestartSaveRevision) {
		// Wait for the overlay edit's receipt. Settings-page edits save separately;
		// an unrelated old overlay failure must not block those edits indefinitely.
		const uint64_t receipt = session->saveState->result.load(std::memory_order_acquire);
		if ((receipt >> 3) < std::max(_parameterRestartSaveRevision, session->saveRevision)) return;
		if ((receipt & 7) != static_cast<uint64_t>(EffectParametersSaveError::None)) {
			Logger::Get().Warn("DLSSNR parameter restart cancelled: parameter save failed");
			_CancelParameterRestart();
			return;
		}
	}
	const auto& descriptions = _renderer->ActiveEffectDescs();
	const auto& infos = _renderer->EffectParameterRuntimeInfos();
	_restartParameters.clear();
	// ActiveEffectDescs may also include an automatically appended resize pass.
	_restartParameters.resize(std::min({ _options.effects.size(), descriptions.size(), infos.size() }));
	for (size_t effect = 0; effect < _restartParameters.size(); ++effect) {
		for (size_t parameter = 0; parameter < std::min(descriptions[effect]->params.size(), infos[effect].size()); ++parameter) {
			_restartParameters[effect].push_back({ descriptions[effect]->params[parameter],
				infos[effect][parameter].applyMode });
		}
	}
	_restartOverlayState = _renderer->CaptureOverlayState();
	auto changes = _parameterRestartQueue.TakeChanges();
	Logger::Get().Info(fmt::format("DLSSNR parameter restart: stopping effect group for {} edited parameter(s)", changes.size()));
	// Joining the backend in WM_DESTROY preserves its final Applied snapshot.
	// Merge only these deferred live edits; manual Restart targets remain pending.
	_isSrcRepositioning = true;
	Destroy();
	_isSrcRepositioning = false;
	for (const auto& change : changes) {
		if (change.effect < _options.effects.size()) {
			_options.effects[change.effect].parameters[change.parameter] = change.value;
		}
	}
	_parameterRestartQueue.WaitAfterStop(Clock::now());
}

winrt::hstring ScalingWindow::GetLocalizedString(std::wstring_view resName) const {
	return _resourceLoader.GetString(resName);
}

LRESULT ScalingWindow::_MessageHandler(UINT msg, WPARAM wParam, LPARAM lParam) noexcept {
	struct CallbackScope {
		unsigned& depth;
		explicit CallbackScope(unsigned& value) noexcept : depth(value) { ++depth; }
		~CallbackScope() noexcept { --depth; }
	} callbackScope(_windowCallbackDepth);
	if (msg == WM_DESTROY) _BeginDestroy(Handle());
	if (msg == WM_NCDESTROY) {
		const HWND window = _retirement.Window();
		_retirement.NonclientDestroyed();
		// Preserve our identity until USER32 removes the old HWND; a null userdata
		// during this callback would look like a recycled handle before destruction.
		return DefWindowProc(window, msg, wParam, lParam);
	}
	if (_isDestroying && msg != WM_DESTROY) {
		return DefWindowProc(Handle() ? Handle() : _retirement.Window(), msg, wParam, lParam);
	}
	if (_renderer && !_isDestroying) {
		_renderer->MessageHandler(msg, wParam, lParam);
	}

	switch (msg) {
	case WM_TIMER:
		if (wParam == NvidiaForegroundCompatibility::ReferenceTimer) {
			NvidiaForegroundCompatibility::Poll();
			return 0;
		}
		break;
	case WM_CREATE:
	{
		// 防止缩放 UWP 窗口时无法遮挡任务栏
		// https://github.com/dechamps/WindowInvestigator/issues/3
		SetProp(Handle(), L"TreatAsDesktopFullscreen", (HANDLE)TRUE);

		_currentDpi = GetDpiForWindow(Handle());

		// 设置窗口不透明。不完全透明时可关闭 DirectFlip
		if (!SetLayeredWindowAttributes(Handle(), 0, 255, LWA_ALPHA)) {
			Logger::Get().Win32Error("SetLayeredWindowAttributes 失败");
		}

		if (_options.IsWindowedMode()) {
			BOOL value = TRUE;
			DwmSetWindowAttribute(Handle(), DWMWA_TRANSITIONS_FORCEDISABLED, &value, sizeof(value));

			if (_IsBorderless()) {
				// 保留窗口阴影
				MARGINS margins{ 1,1,1,1 };
				DwmExtendFrameIntoClientArea(Handle(), &margins);
			}

			if (_srcTracker.WindowKind() == SrcWindowKind::NoNativeFrame && Win32Helper::GetOSVersion().IsWin11()) {
				// Win11 中禁用边框和圆角以模仿 NoNativeFrame 的样式
				COLORREF color = DWMWA_COLOR_NONE;
				DwmSetWindowAttribute(Handle(), DWMWA_BORDER_COLOR, &color, sizeof(color));

				DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_DONOTROUND;
				DwmSetWindowAttribute(Handle(), DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
			}

			_UpdateFrameMargins();

			// 创建用于在窗口外调整尺寸的辅助窗口
			_CreateBorderHelperWindows();
		}

		// 提高时钟精度，默认为 15.6ms。缩放结束时还原
		timeBeginPeriod(1);
		break;
	}
	case CommonSharedConstants::WM_FRONTEND_RENDER:
	{
		if (!_renderer) {
			return 0;
		}

		// Regular capture notifications may be coalesced. DLSSFG sends wParam=1
		// synchronously for every generated frame, and those must remain ordered.
		if (wParam == 0) {
			MSG msg1;
			while (PeekMessage(&msg1, Handle(), CommonSharedConstants::WM_FRONTEND_RENDER,
				CommonSharedConstants::WM_FRONTEND_RENDER, PM_REMOVE)
			) {
				// 不做处理
			}
		}
		_frontendRenderPending = true;
		return 0;
	}
	case CommonSharedConstants::WM_FRONTEND_RENDER_DLSSFG:
	{
		if (_renderer) {
			_dlssFgFrameJobs.push_back(DLSSFGFrameJob{
				.sharedTextureSlot = static_cast<uint32_t>(wParam),
				.sharedTextureGeneration = static_cast<uint32_t>(lParam)
			});
		}
		return 0;
	}
	case WM_ENTERSIZEMOVE:
	{
		_isResizingOrMoving = true;
		if (!_isPreparingForResizing) {
			_cursorManager->OnStartMove();
		}

		if (_options.IsTouchSupportEnabled()) {
			_UpdateTouchProps(_srcTracker.SrcRect());
		}

		// 广播用户开始调整缩放窗口大小或移动缩放窗口
		PostMessage(HWND_BROADCAST, WM_MAGPIE_SCALINGCHANGED, 3, (LPARAM)Handle());
		return 0;
	}
	case WM_EXITSIZEMOVE:
	{
		_isResizingOrMoving = false;
		_renderer->OnEndResize();
		_cursorManager->OnEndResizeMove();

		if (!_srcTracker.MoveOnEndResizeMove()) {
			Logger::Get().Error("SrcTracker::MoveOnEndResizeMove 失败");
			_DelayedStop();
			return 0;
		}

		_cursorManager->OnSrcRectChanged();

		if (_options.IsTouchSupportEnabled()) {
			_UpdateTouchProps(_srcTracker.SrcRect());
		}

		// 广播缩放窗口位置或大小改变
		PostMessage(HWND_BROADCAST, WM_MAGPIE_SCALINGCHANGED, 2, (LPARAM)Handle());
		return 0;
	}
	case WM_GETDPISCALEDSIZE:
	{
		// DPI 改变时保持渲染尺寸保持不变
		const uint32_t newDpi = (uint32_t)wParam;
		SIZE& newSize = *(SIZE*)lParam;

		if (_options.IsWindowedMode()) {
			// 边框宽度变化会导致窗口尺寸变化
			int width = _rendererRect.right - _rendererRect.left;
			int height = 0;
			if (_CalcWindowedScalingWindowSize(width, height, true, newDpi)) {
				newSize = SIZE{ width, height };
			} else {
				Logger::Get().Error("_CalcWindowedScalingWindowSize 失败");
				_DelayedStop();
			}
		} else {
			newSize = _AdjustFullscreenWindowSize(
				Win32Helper::GetSizeOfRect(_windowRect), newDpi);
		}

		return TRUE;
	}
	case WM_DPICHANGED:
	{
		_currentDpi = HIWORD(wParam);

		RECT* newRect = (RECT*)lParam;
		SetWindowPos(
			Handle(),
			NULL,
			newRect->left,
			newRect->top,
			newRect->right - newRect->left,
			newRect->bottom - newRect->top,
			SWP_NOZORDER | SWP_NOACTIVATE
		);

		return 0;
	}
	case WM_NCHITTEST:
	{
		if (!_options.IsWindowedMode()) {
			break;
		}

		// Toolbar buttons and background remain client input at source resize edges.
		if (_renderer->IsToolbarAt({ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) })) {
			return HTCLIENT;
		}

		const int16_t srcHitTest = _cursorManager->SrcHitTest();
		if (srcHitTest != HTNOWHERE) {
			return srcHitTest;
		}

		break;
	}
	case WM_LBUTTONDOWN:
	case WM_RBUTTONDOWN:
	case WM_NCLBUTTONDOWN:
	case WM_NCRBUTTONDOWN:
	{
		// 在以下情况下会收到光标消息:
		// 1、未捕获光标且缩放后的位置未被遮挡而缩放前的位置被遮挡
		// 2、光标位于叠加层或黑边上
		// 这时鼠标点击将激活源窗口
		const HWND hwndForground = Win32Helper::GetRealForegroundWindow();
		if (_renderer && _renderer->IsEditingParameters()) return 0;
		if (!IsSourceForeground()) {
			if (!_srcTracker.SetFocus(hwndForground)) {
				// 设置前台窗口失败，可能是因为前台窗口是开始菜单
				if (WindowHelper::IsStartMenu(hwndForground)) {
					using namespace std::chrono;

					// 限制触发频率
					static steady_clock::time_point prevTimePoint{};
					auto now = steady_clock::now();
					if (duration_cast<milliseconds>(now - prevTimePoint).count() >= 1000) {
						prevTimePoint = now;

						// 模拟按键关闭开始菜单
						INPUT inputs[]{
							INPUT{
								.type = INPUT_KEYBOARD,
								.ki = KEYBDINPUT{
									.wVk = VK_LWIN
								}
							},
							INPUT{
								.type = INPUT_KEYBOARD,
								.ki = KEYBDINPUT{
									.wVk = VK_LWIN,
									.dwFlags = KEYEVENTF_KEYUP
								}
							}
						};
						SendInput((UINT)std::size(inputs), inputs, sizeof(INPUT));

						// 等待系统处理
						Sleep(1);
					}

					_srcTracker.SetFocus();
				}
			}
		}

		if (msg == WM_NCLBUTTONDOWN &&
			(wParam == HTCAPTION || (wParam >= HTSIZEFIRST && wParam <= HTSIZELAST)))
		{
			if (Win32Helper::IsWindowHung(_srcTracker.Handle())) {
				Logger::Get().Error("源窗口已挂起");
				_DelayedStop(true);
				// 阻止拖拽和调整大小
				return 0;
			}
		}

		break;
	}
	case WM_NCACTIVATE:
	{
		wParam = _srcTracker.IsSessionForeground();
		break;
	}
	case WM_MOUSEACTIVATE:
	{
		// 使得点击缩放窗口后关闭开始菜单能激活源窗口
		return MA_NOACTIVATE;
	}
	// 调整大小时消息的顺序以及我们的处理如下:
	//
	// WM_SIZING: 在用户调整尺寸时确保等比例以及限制最小和最大尺寸。
	// ↓
	// WM_WINDOWPOSCHANGING: 也要确保等比例，因为被第三方程序使用 SetWindowPos 修改
	// 尺寸时不会收到 WM_SIZING。如果第三方程序指定了 SWP_NOSENDCHANGING 标志我们
	// 不会收到 WM_WINDOWPOSCHANGING。
	// ↓
	// WM_NCCALCSIZE: 更新数据和渲染器矩形，这使得调整大小的过程中不会出现闪烁。如果需
	// 要，在这里移除原生标题栏。
	// ↓
	// WM_WINDOWPOSCHANGED: 收尾工作，如更新辅助窗口位置、广播消息等。也要更新数据和渲
	// 染器矩形，因为移动时不会收到 WM_NCCALCSIZE。
	case WM_SIZING:
	{
		if (!_options.IsWindowedMode()) {
			return TRUE;
		}

		RECT& windowRect = *(RECT*)lParam;

		int windowWidth;
		int windowHeight;
		if (wParam == WMSZ_TOP || wParam == WMSZ_BOTTOM) {
			// 上下两边上调整尺寸时宽度随高度变化
			windowWidth = 0;
			windowHeight = windowRect.bottom - windowRect.top;
		} else {
			// 其他边上调整尺寸时使用高度随宽度变化
			windowWidth = windowRect.right - windowRect.left;
			windowHeight = 0;
		}

		if (!_CalcWindowedScalingWindowSize(windowWidth, windowHeight, false)) {
			Logger::Get().Error("_CalcWindowedScalingWindowSize 失败");
			_DelayedStop();
			return TRUE;
		}

		if (wParam == WMSZ_LEFT || wParam == WMSZ_TOPLEFT || wParam == WMSZ_BOTTOMLEFT) {
			windowRect.left = windowRect.right - windowWidth;
		} else {
			windowRect.right = windowRect.left + windowWidth;
		}

		if (wParam == WMSZ_TOP || wParam == WMSZ_TOPLEFT || wParam == WMSZ_TOPRIGHT) {
			windowRect.top = windowRect.bottom - windowHeight;
		} else {
			windowRect.bottom = windowRect.top + windowHeight;
		}

		return TRUE;
	}
	case WM_WINDOWPOSCHANGING:
	{
		WINDOWPOS& windowPos = *(WINDOWPOS*)lParam;

		// 阻止 OS 修改置顶状态。当源窗口中途置顶/取消置顶时，OS 会试图修改缩放窗口的置顶
		// 状态，这不是我们想要的。
		if (!(windowPos.flags & SWP_NOZORDER) && !_options.IsDebugMode() &&
			_sourceCoordinator.Mode() != WindowCoordinationMode::ManagedUnowned) {
			if (_CalcTopmostState()) {
				if (windowPos.hwndInsertAfter != HWND_TOP) {
					windowPos.hwndInsertAfter = HWND_TOPMOST;
				}
			} else if (windowPos.hwndInsertAfter == HWND_TOPMOST) {
				windowPos.hwndInsertAfter = HWND_NOTOPMOST;
			}
		}

		// 如果全屏模式缩放包含 WS_MAXIMIZE 样式，创建窗口时将收到 WM_WINDOWPOSCHANGING，
		// 应该忽略。
		if (!_renderer || (windowPos.flags & (SWP_NOSIZE | SWP_NOMOVE)) == (SWP_NOSIZE | SWP_NOMOVE)) {
			return 0;
		}

		if (!(windowPos.flags & SWP_NOSIZE)) {
			if (_options.IsWindowedMode()) {
				// 用户调整尺寸时 WM_SIZING 已经确保等比例
				if (!_isResizingOrMoving) {
					// cx 不为 0 时使用 cx 计算，否则使用 cy 计算
					if (windowPos.cx == 0) {
						if (windowPos.cy == 0) {
							// cx 和 cy 都为 0 则使用最小尺寸
							windowPos.cx = 1;
						}
					} else {
						windowPos.cy = 0;
					}

					if (!_CalcWindowedScalingWindowSize(windowPos.cx, windowPos.cy, false)) {
						Logger::Get().Error("_CalcWindowedScalingWindowSize 失败");
						_DelayedStop();
						return 0;
					}
				}
			} else {
				SIZE newSize = _AdjustFullscreenWindowSize({ windowPos.cx, windowPos.cy });
				windowPos.cx = newSize.cx;
				windowPos.cy = newSize.cy;
			}
		}

		// 在这里更新窗口矩形和渲染器矩形可以减少闪烁，和 WM_NCCALCSIZE 的目的类似。
		// 由于 WM_WINDOWPOSCHANGING 消息是可选的，仍应在 WM_WINDOWPOSCHANGED 中
		// 执行相同的操作。
		_UpdateWindowRectFromWindowPos(windowPos);
		_UpdateRendererRect();

		return 0;
	}
	case WM_NCCALCSIZE:
	{
		// 如果全屏模式缩放包含 WS_MAXIMIZE 样式，创建窗口时将收到 WM_NCCALCSIZE，
		// 应该忽略。
		if (!_renderer || !wParam) {
			return 0;
		}

		NCCALCSIZE_PARAMS& params = *(NCCALCSIZE_PARAMS*)lParam;
		// 此时第一个成员是新窗口矩形
		_windowRect = params.rgrc[0];

		if (_options.IsWindowedMode()) {
			_UpdateFrameMargins();

			if (!_IsBorderless()) {
				// 应用默认边框
				LRESULT ret = DefWindowProc(Handle(), WM_NCCALCSIZE, wParam, lParam);
				if (ret != 0) {
					return ret;
				}

				// 重新应用原始上边框，因此我们完全移除了默认边框中的上边框和标题栏，但保留了其他方向的边框
				params.rgrc[0].top = _windowRect.top;
			}
		}

		// WM_NCCALCSIZE 内更新渲染器尺寸可以避免闪烁
		_UpdateRendererRect();
		return 0;
	}
	case WM_WINDOWPOSCHANGED:
	{
		const WINDOWPOS& windowPos = *(WINDOWPOS*)lParam;

		if (_options.IsWindowedMode()) {
			// WS_EX_NOACTIVATE 和处理 WM_MOUSEACTIVATE 仍然无法完全阻止缩放窗口接收
			// 焦点。进行下面的操作：调整缩放窗口尺寸，打开开始菜单然后关闭，缩放窗口便
			// 得到焦点了。这应该是 OS 的 bug，下面的代码用于规避它。
			if (!(windowPos.flags & SWP_NOACTIVATE) && (!_renderer || _renderer->AllowAutomaticSourceFocus())) {
				_srcTracker.SetFocus(Handle());
			}
		}

		if ((windowPos.flags & (SWP_NOSIZE | SWP_NOMOVE)) == (SWP_NOSIZE | SWP_NOMOVE)) {
			return 0;
		}

		// 下面的代码用于防止在缩放窗口调整大小或移动中途源窗口挂起时我们卡在 DefWindowProc
		// 里。但它有个问题是要等待源窗口响应消息，因为有的窗口响应速度很慢，我们一直避免同步
		// 等待。目前我把它禁用了，希望能找到更好的解决方案。因为缩放窗口调整大小或移动的过程
		// 中不会捕获光标，因此即使触发了也不会造成严重后果。
		// if (_isResizingOrMoving && Win32Helper::IsWindowHung(_srcTracker.Handle())) {
		//     Logger::Get().Error("源窗口已挂起");
		//     _DelayedStop(true);
		// }

		// 更新窗口矩形和渲染器矩形，因为 WM_NCCALCSIZE 和 WM_WINDOWPOSCHANGING 都是可选的
		_UpdateWindowRectFromWindowPos(windowPos);
		_UpdateRendererRect();

		_RepostionBorderHelperWindows();

		if (_options.IsTouchSupportEnabled()) {
			_UpdateTouchHoleWindows(false);
		}

		// 即使不广播也更新窗口属性，第三方程序如果需要可以定期检索
		_UpdateWindowProps();

		// 拖拽缩放窗口时不广播
		if (!_isResizingOrMoving && !_srcTracker.IsMoving()) {
			// 广播缩放窗口位置或大小改变
			PostMessage(HWND_BROADCAST, WM_MAGPIE_SCALINGCHANGED, 2, (LPARAM)Handle());
		}

		// 不调用 DefWindowProc，因此不会收到 WM_SIZE 和 WM_MOVE
		return 0;
	}
	case WM_SYSCOMMAND:
	{
		// 使用 WM_SYSCOMMAND 区分接下来的 WM_ENTERSIZEMOVE 是调整大小还是移动
		_isPreparingForResizing = (wParam & 0xFFF0) == SC_SIZE;
		if (_isPreparingForResizing && (!_renderer || _renderer->AllowAutomaticSourceFocus())) {
			_srcTracker.SetFocus();
		}
		break;
	}
	case WM_DESTROY:
		// Keep renderer/capture/swap-chain/cursor/input resources alive through
		// every destruction callback. The outer pump or Destroy() finishes them.
		_RemoveWindowProps();
		break;
	}

	const LRESULT result = base_type::_MessageHandler(msg, wParam, lParam);
	return result;
}

void ScalingWindow::_FinishDestroy() noexcept {
	Logger::Get().Info(fmt::format("Window retirement complete before blocking cleanup: output={:#x} run={}",
		uintptr_t(_retirement.Window()), RunId()));
	if (_renderer) _renderer->ReleaseParameterInput();
	const bool ngxWasFaulted = NgxRuntimeGuard::IsFaulted();
	Logger::Get().Info("缩放结束");
	if (_renderer) {
		_renderer->BeginShutdown();
		_renderer->ClearOverlayStates();
	}
	_frontendRenderPending = false;
	_dlssFgFrameJobs.clear();

	if (_exclModeMutex) {
		_exclModeMutex.ReleaseMutex();
		_exclModeMutex.reset();
	}

	for (wil::unique_hwnd& hWnd : _hwndTouchHoles) {
		hWnd.reset(); // Unowned touch holes remain alive after output destruction.
	}

	_cursorManager.reset();
	Logger::Get().Info("CursorManager 已析构");

	_renderer.reset();
	NvidiaForegroundCompatibility::FinishRetire();
	Logger::Get().Info("Renderer 已析构");
	if (!ngxWasFaulted && NgxRuntimeGuard::IsFaulted()) {
		if (const auto report = _options.reportErrorDetails) {
			report(_srcTracker.Handle(), ScalingError::NgxRestartRequired,
				fmt::format("NGX teardown fault at {:#x}, thread={}; restart required",
					NgxRuntimeGuard::FaultAddress(), NgxRuntimeGuard::FaultThread()),
				NgxRuntimeGuard::FaultCode());
		} else {
			ShowError(ScalingError::NgxRestartRequired);
		}
	}
	// The backend has joined. Preserve its final applied snapshot for every
	// automatic restart, without applying queued restart-only target values.
	if (_isSrcRepositioning && _options.parameterSession) {
		_options.effects = _options.parameterSession->Applied();
	}

	if (!_isSrcRepositioning) {
		// 缩放结束时保存配置
		_options.save(_options, NULL);
		CleanAfterSrcRepositioned();
	}

	// 还原时钟精度
	timeEndPeriod(1);


	// 广播停止缩放
	PostMessage(HWND_BROADCAST, WM_MAGPIE_SCALINGCHANGED, 0, 0);
	_hwndRenderer = nullptr;
	for (auto& helper : _hwndResizeHelpers) helper.release(); // Destroyed with their owner.
}

LRESULT ScalingWindow::_RendererWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	if (msg == WM_WINDOWPOSCHANGING) {
		WINDOWPOS& windowPos = *(WINDOWPOS*)lParam;
		if (!(windowPos.flags & SWP_NOSIZE)) {
			// 为了平滑调整窗口尺寸，渲染所在窗口需要在 WM_WINDOWPOSCHANGING 中
			// 更新渲染尺寸。
			Get()._ResizeRenderer();
		} else if (!(windowPos.flags & SWP_NOMOVE)) {
			Get()._MoveRenderer();
		}

		return 0;
	}

	return DefWindowProc(hWnd, msg, wParam, lParam);
}

bool ScalingWindow::_CalcWindowedScalingWindowSize(int& width, int& height, bool isRendererSize, uint32_t dpi) const noexcept {
	assert((width == 0) != (height == 0));

	if (dpi == 0) {
		dpi = _currentDpi;
	}

	const RECT& srcRect = _srcTracker.SrcRect();
	const float srcAspectRatio = float(srcRect.bottom - srcRect.top) / (srcRect.right - srcRect.left);

	// 计算最小尺寸时使用源窗口包含窗口框架的矩形而不是被缩放区域。用户显式指定 1.0x 时
	// 不应放大画面，此时只要求渲染窗口不小于被缩放区域，从而做到逐像素对应。
	const bool noUpscaling = _options.initialWindowedScaleFactor == 1.0f;
	const RECT& minSrcRect = noUpscaling ? srcRect : _srcTracker.WindowFrameRect();
	const int spaceAround = noUpscaling ? 0 : (int)lroundf(WINDOWED_MODE_MIN_SPACE_AROUND *
		dpi / float(USER_DEFAULT_SCREEN_DPI));
	const int minRendererWidth = minSrcRect.right - minSrcRect.left + spaceAround;
	const int minRendererHeight = minSrcRect.bottom - minSrcRect.top + spaceAround;

	int xExtraSpace;
	int yExtraSpace;
	if (_IsBorderless()) {
		xExtraSpace = 2 * _nonTopBorderThicknessInClient;
		yExtraSpace = _topBorderThicknessInClient + _nonTopBorderThicknessInClient;
	} else {
		RECT rect{};
		AdjustWindowRectExForDpi(&rect, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
		xExtraSpace = rect.right - rect.left;
		yExtraSpace = _topBorderThicknessInClient + rect.bottom;
	}

	if (!isRendererSize) {
		if (width != 0) {
			width -= xExtraSpace;
		} else {
			height -= yExtraSpace;
		}
	}

	int rendererWidth;
	int rendererHeight;
	if (width != 0) {
		rendererWidth = width;
		rendererHeight = (int)std::lroundf(rendererWidth * srcAspectRatio);
	} else {
		assert(height != 0);
		rendererHeight = height;
		rendererWidth = (int)std::lroundf(rendererHeight / srcAspectRatio);
	}

	// 确保渲染窗口比源窗口稍大
	if (rendererWidth > rendererHeight) {
		if (rendererHeight < minRendererHeight) {
			rendererHeight = minRendererHeight;
			rendererWidth = (int)std::lroundf(rendererHeight / srcAspectRatio);
		}
	} else {
		if (rendererWidth < minRendererWidth) {
			rendererWidth = minRendererWidth;
			rendererHeight = (int)std::lroundf(rendererWidth * srcAspectRatio);
		}
	}

	width = rendererWidth + xExtraSpace;
	height = rendererHeight + yExtraSpace;

	// 确保缩放窗口尺寸不超过系统限制
	const int maxWidth = GetSystemMetricsForDpi(SM_CXMAXTRACK, dpi);
	const int maxHeight = GetSystemMetricsForDpi(SM_CYMAXTRACK, dpi);
	if (width > maxWidth || height > maxHeight) {
		// 尝试最大宽度，失败则使用最大高度
		int testHeight = (int)std::lroundf((maxWidth - xExtraSpace) * srcAspectRatio) + yExtraSpace;
		if (testHeight < maxHeight) {
			width = maxWidth;
			height = testHeight;
		} else {
			height = maxHeight;
			width = (int)std::lroundf((maxHeight - yExtraSpace) / srcAspectRatio) + xExtraSpace;
		}

		rendererWidth = width - xExtraSpace;
		rendererHeight = height - yExtraSpace;
		if (rendererWidth < minRendererWidth || rendererHeight < minRendererHeight) {
			// 源窗口太大
			return false;
		}
	}

	return true;
}

RECT ScalingWindow::_CalcWindowedRendererRect() const noexcept {
	if (_IsBorderless()) {
		return {
			_windowRect.left + (LONG)_nonTopBorderThicknessInClient,
			_windowRect.top + (LONG)_topBorderThicknessInClient,
			_windowRect.right - (LONG)_nonTopBorderThicknessInClient,
			_windowRect.bottom - (LONG)_nonTopBorderThicknessInClient
		};
	} else {
		RECT frameRect{};
		AdjustWindowRectExForDpi(&frameRect, WS_OVERLAPPEDWINDOW, FALSE, 0, _currentDpi);

		return {
			_windowRect.left - frameRect.left,
			_windowRect.top + (LONG)_topBorderThicknessInClient,
			_windowRect.right - frameRect.right,
			_windowRect.bottom - frameRect.bottom
		};
	}
}

// 返回缩放窗口跨越的屏幕数量，失败返回 0
ScalingError ScalingWindow::_CalcFullscreenRendererRect(uint32_t& monitorCount) noexcept {
	switch (_options.multiMonitorUsage) {
	// 使用距离源窗口最近的显示器
	case MultiMonitorUsage::Closest:
	{
		if (ScalingError error = _InitialMoveSrcWindowInFullscreen(); error != ScalingError::NoError) {
			return error;
		}

		monitorCount = 1;
		return ScalingError::NoError;
	}
	// 使用源窗口跨越的所有显示器
	case MultiMonitorUsage::Intersected:
	{
		if (_srcTracker.IsZoomed()) {
			// 最大化的窗口不能跨越屏幕
			HMONITOR hMon = MonitorFromWindow(_srcTracker.Handle(), MONITOR_DEFAULTTONULL);
			MONITORINFO mi{ .cbSize = sizeof(mi) };
			if (!GetMonitorInfo(hMon, &mi)) {
				Logger::Get().Win32Error("GetMonitorInfo 失败");
				return ScalingError::DisplayLayoutFailed;
			}

			_rendererRect = mi.rcMonitor;
			monitorCount = 1;
			return ScalingError::NoError;
		}

		// [0] 存储源窗口坐标，[1] 存储计算结果
		struct MonitorEnumParam {
			RECT srcRect;
			RECT destRect;
			uint32_t monitorCount;
		} param{};

		// 使用窗口框架矩形来计算和哪些屏幕相交，而不是被缩放区域
		if (!Win32Helper::GetWindowFrameRect(_srcTracker.Handle(), param.srcRect)) {
			Logger::Get().Error("GetWindowFrameRect 失败");
			return ScalingError::DisplayLayoutFailed;
		}

		MONITORENUMPROC monitorEnumProc = [](HMONITOR, HDC, LPRECT monitorRect, LPARAM data) {
			MonitorEnumParam* param = (MonitorEnumParam*)data;

			if (Win32Helper::IsRectOverlap(param->srcRect, *monitorRect)) {
				UnionRect(&param->destRect, monitorRect, &param->destRect);
				++param->monitorCount;
			}

			return TRUE;
		};

		if (!EnumDisplayMonitors(NULL, NULL, monitorEnumProc, (LPARAM)&param)) {
			Logger::Get().Win32Error("EnumDisplayMonitors 失败");
			return ScalingError::DisplayLayoutFailed;
		}

		_rendererRect = param.destRect;

		if (ScalingError error = _InitialMoveSrcWindowInFullscreen(); error != ScalingError::NoError) {
			return error;
		}

		monitorCount = param.monitorCount;
		return ScalingError::NoError;
	}
	// 使用所有显示器 (Virtual Screen)
	case MultiMonitorUsage::All:
	{
		int vsWidth = GetSystemMetrics(SM_CXVIRTUALSCREEN);
		int vsHeight = GetSystemMetrics(SM_CYVIRTUALSCREEN);
		int vsX = GetSystemMetrics(SM_XVIRTUALSCREEN);
		int vsY = GetSystemMetrics(SM_YVIRTUALSCREEN);
		_rendererRect = { vsX, vsY, vsX + vsWidth, vsY + vsHeight };

		if (ScalingError error = _InitialMoveSrcWindowInFullscreen(); error != ScalingError::NoError) {
			return error;
		}

		monitorCount = GetSystemMetrics(SM_CMONITORS);
		return ScalingError::NoError;
	}
	// 使用用户指定的单个显示器
	case MultiMonitorUsage::Specific:
	{
		bool found = false;
		for (const Win32Helper::DisplayMonitorInfo& monitor :
			Win32Helper::GetDisplayMonitors()) {
			if (CompareStringOrdinal(
				monitor.deviceId.c_str(), (int)monitor.deviceId.size(),
				_options.preferredMonitorId.c_str(),
				(int)_options.preferredMonitorId.size(), TRUE) == CSTR_EQUAL) {
				_rendererRect = monitor.rect;
				found = true;
				break;
			}
		}

		if (!found) {
			// 显示器可能已断开或设备 ID 已变化。回退到源窗口最近的显示器，
			// 保证旧配置仍能启动缩放。
			Logger::Get().Warn("找不到首选显示器，回退到距离源窗口最近的显示器");
			HMONITOR hMonitor = MonitorFromWindow(
				_srcTracker.Handle(), MONITOR_DEFAULTTONULL);
			MONITORINFO monitorInfo{ .cbSize = sizeof(monitorInfo) };
			if (!hMonitor || !GetMonitorInfo(hMonitor, &monitorInfo)) {
				Logger::Get().Win32Error("GetMonitorInfo 失败");
				return ScalingError::DisplayLayoutFailed;
			}
			_rendererRect = monitorInfo.rcMonitor;
		}

		if (ScalingError error = _InitialMoveSrcWindowInFullscreen();
			error != ScalingError::NoError) {
			return error;
		}

		monitorCount = 1;
		return ScalingError::NoError;
	}
	default:
		assert(false);
		return ScalingError::DisplayLayoutFailed;
	}
}

// 全屏模式缩放无需保持比例，但要限制最大尺寸
SIZE ScalingWindow::_AdjustFullscreenWindowSize(SIZE size, uint32_t dpi) const noexcept {
	if (dpi == 0) {
		dpi = _currentDpi;
	}

	const LONG maxWidth = GetSystemMetricsForDpi(SM_CXMAXTRACK, dpi);
	const LONG maxHeight = GetSystemMetricsForDpi(SM_CYMAXTRACK, dpi);
	return SIZE{ std::clamp(size.cx, 1l, maxWidth), std::clamp(size.cy, 1l, maxHeight) };
}

ScalingError ScalingWindow::_InitialMoveSrcWindowInFullscreen() noexcept {
	HMONITOR hMonitor = MonitorFromWindow(_srcTracker.Handle(), MONITOR_DEFAULTTONULL);
	assert(hMonitor);

	MONITORINFO mi{ .cbSize = sizeof(mi) };
	if (!GetMonitorInfo(hMonitor, &mi)) {
		Logger::Get().Win32Error("GetMonitorInfo 失败");
		return ScalingError::DisplayLayoutFailed;
	}

	if (_options.multiMonitorUsage == MultiMonitorUsage::Closest) {
		_rendererRect = mi.rcMonitor;
	}

	if (_srcTracker.IsZoomed()) {
		return ScalingError::NoError;
	}

	const RECT& srcRect = _srcTracker.SrcRect();
	const SIZE srcSize = Win32Helper::GetSizeOfRect(srcRect);
	const SIZE monitorSize = Win32Helper::GetSizeOfRect(mi.rcMonitor);

	if (srcSize.cx > monitorSize.cx || srcSize.cy > monitorSize.cy) {
		// 被缩放区域比屏幕更大时无法使用 DesktopDuplication 捕获
		if (_options.captureMethod == CaptureMethod::DesktopDuplication) {
			return ScalingError::CaptureFailed;
		} else {
			return ScalingError::NoError;
		}
	}

	// 作为优化，如果窗口有一部分不在屏幕上则将被缩放区域移动到屏幕中央。Desktop Duplication
	// 不能捕获屏幕外的内容，所以被缩放区域必须在屏幕内。
	//
	// 无需考虑被任务栏遮挡，缩放时任务栏将自动隐藏。
	bool shouldMove = false;
	if (_options.captureMethod == CaptureMethod::DesktopDuplication) {
		shouldMove = !PtInRect(&mi.rcMonitor, POINT{ srcRect.left,srcRect.top })
			|| !PtInRect(&mi.rcMonitor, POINT{ srcRect.left,srcRect.bottom })
			|| !PtInRect(&mi.rcMonitor, POINT{ srcRect.right,srcRect.top })
			|| !PtInRect(&mi.rcMonitor, POINT{ srcRect.right,srcRect.bottom });
	} else {
		shouldMove = !MonitorFromPoint(POINT{ srcRect.left,srcRect.top }, MONITOR_DEFAULTTONULL)
			|| !MonitorFromPoint(POINT{ srcRect.left,srcRect.bottom }, MONITOR_DEFAULTTONULL)
			|| !MonitorFromPoint(POINT{ srcRect.right,srcRect.top }, MONITOR_DEFAULTTONULL)
			|| !MonitorFromPoint(POINT{ srcRect.right,srcRect.bottom }, MONITOR_DEFAULTTONULL);
	}

	if (shouldMove) {
		// 不要跨屏幕移动，否则如果 DPI 缩放不同会造成源窗口尺寸改变
		int offsetX = mi.rcMonitor.left + (monitorSize.cx - srcSize.cx) / 2 - srcRect.left;
		int offsetY = mi.rcMonitor.top + (monitorSize.cy - srcSize.cy) / 2 - srcRect.top;
		if (!_srcTracker.Move(offsetX, offsetY, false)) {
			return ScalingError::SourceWindowGeometryFailed;
		}
	}

	return ScalingError::NoError;
}

bool ScalingWindow::_Show() noexcept {
	if (!_sourceCoordinator.Ready() || _isDestroying || HasPendingSourceTransition()) return false;
	if (!IsWindowVisible(SourceRoot()) || IsIconic(SourceRoot())) {
		_DelayedStop(false, true);
		return false;
	}
	if (!_EstablishWindowCoordination()) return false;
	// 显示前设置窗口属性，这样其他程序可以根据缩放窗口是否可见判断当前是否处于缩放状态
	_SetWindowProps();

	if (Win32Helper::IsWindowHung(_srcTracker.Handle())) {
		Logger::Get().Error("源窗口已挂起");
		_DelayedStop(true);
		return false;
	}

	// 缩放窗口可能有 WS_MAXIMIZE 样式，因此使用 SetWindowsPos 而不是 ShowWindow
	// 以避免 OS 更改窗口尺寸和位置。
	//
	// SWP_NOACTIVATE 可以避免干扰 OS 内部的前台窗口历史，否则关闭开始菜单时不会自
	// 动激活源窗口。
	if (!SetWindowPos(
		Handle(),
		NULL,
		0, 0, 0, 0,
		SWP_SHOWWINDOW | SWP_NO_ACTIVATE_MOVE_SIZE
			| (_sourceCoordinator.Mode() == WindowCoordinationMode::ManagedUnowned ? SWP_NOZORDER : 0)
	)) {
		const DWORD error = GetLastError();
		const std::string context = "First display SetWindowPos failed";
		Logger::Get().Win32Error(context);
		if (const auto report = _options.reportErrorDetails)
			report(_srcTracker.Handle(), ScalingError::ScalingWindowCreationFailed, context, error);
		else ShowError(ScalingError::ScalingWindowCreationFailed);
		_DelayedStop();
		return false;
	}

	if (_options.IsWindowedMode()) {
		if (!_EnsureCaptionVisibleOnScreen()) {
			_RepostionBorderHelperWindows();
		}
	}

	// 如果源窗口位于前台则将缩放窗口置顶
	if (_srcTracker.IsSessionForeground()) _UpdateFocusStateAsync();
	if (_isDestroying || HasPendingSourceTransition() || !_sourceCoordinator.Ready() || !IsWindowVisible(Handle())) return false;
	// Broadcast only after the real first display and coordination succeeded.
	PostMessage(HWND_BROADCAST, WM_MAGPIE_SCALINGCHANGED, 1, (LPARAM)Handle());

	if (_options.IsTouchSupportEnabled()) {
		// 显示触控辅助窗口
		for (const wil::unique_hwnd& hWnd : _hwndTouchHoles) {
			if (!hWnd) {
				continue;
			}

			SetWindowPos(hWnd.get(), Handle(), 0, 0, 0, 0,
				SWP_SHOWWINDOW | SWP_NOACTIVATE | SWP_NOREDRAW | SWP_NOMOVE | SWP_NOSIZE);
		}
	}

	// 模拟独占全屏
	if (_options.RealIsSimulateExclusiveFullscreen()) {
		// 延迟 1s 以避免干扰游戏的初始化，见 #495
		([]()->winrt::fire_and_forget {
			const uint32_t runId = RunId();

			co_await 1s;
			co_await Dispatcher();

			if (RunId() != runId) {
				co_return;
			}

			if (!Get()._exclModeMutex) {
				Get()._exclModeMutex = ExclModeHelper::EnterExclMode();
			}
		})();
	};
	NvidiaForegroundCompatibility::OutputShown();
	return !_isDestroying && !HasPendingSourceTransition();
}

void ScalingWindow::_ResizeRenderer() noexcept {
	if (!_renderer->OnResize()) {
		Logger::Get().Error("更改 Renderer 尺寸失败");
		return;
	}

	_cursorManager->OnScalingPosChanged();
	_frontendRenderPending = true;
}

void ScalingWindow::_MoveRenderer() noexcept {
	_renderer->OnMove();

	if (!_isMovingDueToSrcMoved) {
		_cursorManager->OnScalingPosChanged();
		_frontendRenderPending = true;
	}
}

bool ScalingWindow::_UpdateSrcState(
	bool& isSrcRepositioning,
	bool& srcFocusedChanged
) noexcept {
	_sourceStateCheckDeferred = false;
	if (!_sourceCoordinator.Ready()) {
		_sourceStateChangeReason = "source identity retired";
		return false;
	}
	_sourceRectBeforeCheck = _srcTracker.WindowRect();
	if (_renderer) _renderer->UpdateParameterInputHost();
	HWND hwndFore = Win32Helper::GetRealForegroundWindow();
	NvidiaForegroundCompatibility::FrontendWindows(_renderer ? _renderer->ParameterInputHandle() : nullptr, hwndFore);
	NvidiaForegroundCompatibility::FrontendSuspended(_renderer &&
		(_renderer->IsEditingParameters() || _renderer->IsParameterFocusSettling()));

	if (hwndFore == Handle() && !NvidiaForegroundCompatibility::ReferenceOwnsForeground() &&
		(!_renderer || _renderer->AllowAutomaticSourceFocus())) {
		// 缩放窗口不应该得到焦点，我们通过 WS_EX_NOACTIVATE 样式和处理 WM_MOUSEACTIVATE
		// 等消息来做到这一点。但如果由于某种我们尚未了解的机制这些手段都失败了，这里
		// 进行纠正。
		_srcTracker.SetFocus(Handle());
		hwndFore = Win32Helper::GetRealForegroundWindow();
	}

	// 在 3D 游戏模式下需检测前台窗口变化
	if (_options.Is3DGameMode() && !_CheckForegroundFor3DGameMode(hwndFore)) {
		_sourceStateChangeReason = "foreground rejected by 3D policy";
		return false;
	}
	// Activating/deactivating the parameter host may temporarily change the
	// source's placement. Do not mutate the tracker's baseline until this
	// bounded handoff has settled; a real source destruction is never delayed.
	if (_renderer && _renderer->IsParameterFocusSettling() && IsWindow(_srcTracker.Handle())) {
		RECT current{};
		if (!IsWindowVisible(_srcTracker.Handle()) || IsIconic(_srcTracker.Handle()) ||
			(GetWindowRect(_srcTracker.Handle(), &current) && current != _sourceRectBeforeCheck)) {
			_sourceStateCheckDeferred = true;
			return true;
		}
	}

	bool isSrcInvisibleOrMinimized = false;
	bool srcRectChanged = false;
	bool srcSizeChanged = false;
	bool srcMovingChanged = false;
	// Only the bounded reference preserves baseline ordering using source focus.
	// All normal business decisions continue to read the real foreground HWND.
	const HWND trackerForeground = NvidiaForegroundCompatibility::ReferenceOwnsForeground() ? _srcTracker.Handle() : hwndFore;
	if (!_srcTracker.UpdateState(trackerForeground, _options.IsWindowedMode(), _isResizingOrMoving,
		isSrcInvisibleOrMinimized, srcFocusedChanged, srcRectChanged, srcSizeChanged, srcMovingChanged)) {
		_sourceStateChangeReason = "source tracker rejected window";
		return false;
	}

	if (isSrcInvisibleOrMinimized || srcSizeChanged || (!_options.IsWindowedMode() && srcRectChanged)) {
		_sourceStateChangeReason = isSrcInvisibleOrMinimized ? "source hidden or minimized" :
			srcSizeChanged ? "source size changed" : "fullscreen source moved";
		// 不要立刻设置 _isSrcSizing，销毁窗口是异步的
		isSrcRepositioning = true;

		if (srcSizeChanged) {
			// 源窗口大小改变则清除记忆
			_lastWindowedRendererWidth = 0;
		} else if (isSrcInvisibleOrMinimized) {
			if (_options.IsWindowedMode()) {
				_lastWindowedRendererWidth = _rendererRect.right - _rendererRect.left;
			}
		}

		return false;
	}

	if (srcMovingChanged) {
		assert(_options.IsWindowedMode());

		if (_srcTracker.IsMoving()) {
			_cursorManager->OnSrcStartMove();
		} else {
			_cursorManager->OnSrcEndMove();
			_EnsureCaptionVisibleOnScreen();
		}

		if (_options.IsTouchSupportEnabled()) {
			_UpdateTouchProps(_srcTracker.SrcRect());
		}

		// 广播用户开始或结束移动缩放窗口
		PostMessage(HWND_BROADCAST, WM_MAGPIE_SCALINGCHANGED,
			_srcTracker.IsMoving() ? 3 : 2, (LPARAM)Handle());
	}

	if (srcRectChanged) {
		assert(_options.IsWindowedMode());

		_cursorManager->OnSrcRectChanged();

		// 窗口模式缩放时允许源窗口移动
		const RECT& srcRect = _srcTracker.WindowRect();
		const LONG newLeft = (srcRect.left + srcRect.right + _windowRect.left - _windowRect.right) / 2;
		const LONG newTop = (srcRect.top + srcRect.bottom + _windowRect.top - _windowRect.bottom) / 2;
		// _srcTracker.IsMoving() 只能反应源窗口的持续拖拽
		_isMovingDueToSrcMoved = true;
		SetWindowPos(Handle(), NULL, newLeft, newTop, 0, 0,
			SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOSIZE | SWP_NOSENDCHANGING);
		_isMovingDueToSrcMoved = false;
	}

	return true;
}

// 返回真表示应继续缩放
bool ScalingWindow::_CheckForegroundFor3DGameMode(HWND hwndFore) const noexcept {
	if (NvidiaForegroundCompatibility::ReferenceOwnsForeground()) return true;
	const auto kind = ClassifyForeground(hwndFore);
	if (kind == SourceForegroundKind::InvalidSource) return false;
	if (kind == SourceForegroundKind::None || IsSourceSessionForeground(kind,
		_renderer && _renderer->IsParameterFocusSettling())) {
		return true;
	}

	if (WindowHelper::IsForbiddenSystemWindow(hwndFore)) {
		return true;
	}

	RECT rectForground;
	if (!Win32Helper::GetWindowFrameRect(hwndFore, rectForground)) {
		Logger::Get().Error("DwmGetWindowAttribute 失败");
		return false;
	}

	if (!Win32Helper::IntersectRect(rectForground, rectForground, _rendererRect)) {
		// 没有重叠
		return true;
	}

	// 允许稍微重叠，减少意外停止缩放的机率
	SIZE rectSize = Win32Helper::GetSizeOfRect(rectForground);
	return rectSize.cx < 8 || rectSize.cy < 8;
}

SourceForegroundKind ScalingWindow::ClassifyForeground(HWND foreground) const noexcept {
	return ClassifySourceForeground(foreground, SourceIdentityValid(), SrcTracker().Handle(),
		SourceRoot(), Handle(), _renderer ? _renderer->ParameterInputHandle() : nullptr, _options.mainUI);
}

void ScalingWindow::UpdateMainUI(SessionWindowIdentity identity) noexcept {
	_options.mainUI = identity;
	NvidiaForegroundCompatibility::MainUIChanged(identity);
}

bool ScalingWindow::IsSessionForeground(HWND foreground) const noexcept {
	return IsSourceSessionForeground(ClassifyForeground(foreground), _renderer && _renderer->IsParameterFocusSettling());
}

bool ScalingWindow::IsParameterForeground(HWND foreground, bool outputTransition) const noexcept {
	return !_isDestroying && IsSourceSessionForeground(ClassifyForeground(foreground), outputTransition) &&
		IsWindowEnabled(SourceRoot()) && IsWindowEnabled(SrcTracker().Handle()) &&
		!EnabledSourcePopup(SourceRoot(), Handle());
}

HWND ScalingWindow::SourceActivationTarget() const noexcept {
	if (!SourceIdentityValid() || !IsWindowVisible(SourceRoot()) || IsIconic(SourceRoot())) return nullptr;
	if (const HWND popup = EnabledSourcePopup(SourceRoot(), Handle())) return popup;
	return IsWindowEnabled(SourceRoot()) && IsWindowEnabled(SrcTracker().Handle()) ? SourceRoot() : nullptr;
}

bool ScalingWindow::IsParameterInputWindow(HWND hwnd) const noexcept {
	return ClassifyForeground(hwnd) == SourceForegroundKind::ParameterHost;
}

bool ScalingWindow::HasHeldParameterInput() const noexcept {
	return _renderer && IsWindow(_srcTracker.Handle()) && _renderer->HasHeldParameterInput();
}

void ScalingWindow::UpdateToolbarShortcutLabels(ToolbarShortcutLabels labels) noexcept {
	if (_options.toolbarShortcutLabels == labels) return;
	_options.toolbarShortcutLabels = std::move(labels);
	if (_renderer) _renderer->RefreshOverlay();
}

// 用于和其他程序交互
void ScalingWindow::_SetWindowProps() const noexcept {
	const HWND hWnd = Handle();
	SetProp(hWnd, L"Magpie.Windowed", (HANDLE)_options.IsWindowedMode());
	SetProp(hWnd, L"Magpie.SrcHWND", _srcTracker.Handle());

	_UpdateWindowProps();
}

void ScalingWindow::_UpdateWindowProps() const noexcept {
	const HWND hWnd = Handle();

	const RECT& srcRect = _renderer->SrcRect();
	SetProp(hWnd, L"Magpie.SrcLeft", (HANDLE)(INT_PTR)srcRect.left);
	SetProp(hWnd, L"Magpie.SrcTop", (HANDLE)(INT_PTR)srcRect.top);
	SetProp(hWnd, L"Magpie.SrcRight", (HANDLE)(INT_PTR)srcRect.right);
	SetProp(hWnd, L"Magpie.SrcBottom", (HANDLE)(INT_PTR)srcRect.bottom);

	const RECT& destRect = _renderer->DestRect();
	SetProp(hWnd, L"Magpie.DestLeft", (HANDLE)(INT_PTR)destRect.left);
	SetProp(hWnd, L"Magpie.DestTop", (HANDLE)(INT_PTR)destRect.top);
	SetProp(hWnd, L"Magpie.DestRight", (HANDLE)(INT_PTR)destRect.right);
	SetProp(hWnd, L"Magpie.DestBottom", (HANDLE)(INT_PTR)destRect.bottom);
}

// 供 TouchHelper.exe 使用
void ScalingWindow::_UpdateTouchProps(const RECT& srcRect) const noexcept {
	assert(_options.IsTouchSupportEnabled());

	const HWND hWnd = Handle();

	if (_isResizingOrMoving) {
		// 调整大小时应禁用触控变换
		SetProp(hWnd, L"Magpie.SrcTouchLeft",
			(HANDLE)(INT_PTR)std::numeric_limits<LONG>::min());
		return;
	}

	RECT srcTouchRect = srcRect;
	RECT destTouchRect = _rendererRect;

	// 拖动源窗口时将源矩形和目标矩形都尽可能放大
	if (_srcTracker.IsMoving()) {
		assert(srcRect == _srcTracker.SrcRect());

		// 测试发现 MagSetInputTransform 存在坐标限制，太大的值是无效的
		static constexpr int MIN_COORD = -20000;
		static constexpr int MAX_COORD = 20000;

		// 计算四个方向中的最小放大倍数
		double destCenterX = (_rendererRect.left + _rendererRect.right) / 2.0;
		double destCenterY = (_rendererRect.top + _rendererRect.bottom) / 2.0;
		double factorLeft = (destCenterX - MIN_COORD) / (destCenterX - _rendererRect.left);
		double factorTop = (destCenterY - MIN_COORD) / (destCenterY - _rendererRect.top);
		double factorRight = (MAX_COORD - destCenterX) / (_rendererRect.right - destCenterX);
		double factorBottom = (MAX_COORD - destCenterY) / (_rendererRect.bottom - destCenterY);
		double minFactor = std::min(std::min(factorLeft, factorTop), std::min(factorRight, factorBottom));

		double srcCenterX = (srcRect.left + srcRect.right) / 2.0;
		double srcCenterY = (srcRect.top + srcRect.bottom) / 2.0;
		srcTouchRect.left = std::lround(srcCenterX - (srcCenterX - srcRect.left) * minFactor);
		srcTouchRect.top = std::lround(srcCenterY - (srcCenterY - srcRect.top) * minFactor);
		srcTouchRect.right = std::lround(srcCenterX + (srcRect.right - srcCenterX) * minFactor);
		srcTouchRect.bottom = std::lround(srcCenterY + (srcRect.bottom - srcCenterY) * minFactor);

		destTouchRect.left = std::lround(destCenterX - (destCenterX - _rendererRect.left) * minFactor);
		destTouchRect.top = std::lround(destCenterY - (destCenterY - _rendererRect.top) * minFactor);
		destTouchRect.right = std::lround(destCenterX + (_rendererRect.right - destCenterX) * minFactor);
		destTouchRect.bottom = std::lround(destCenterY + (_rendererRect.bottom - destCenterY) * minFactor);
	}

	SetProp(hWnd, L"Magpie.SrcTouchLeft", (HANDLE)(INT_PTR)srcTouchRect.left);
	SetProp(hWnd, L"Magpie.SrcTouchTop", (HANDLE)(INT_PTR)srcTouchRect.top);
	SetProp(hWnd, L"Magpie.SrcTouchRight", (HANDLE)(INT_PTR)srcTouchRect.right);
	SetProp(hWnd, L"Magpie.SrcTouchBottom", (HANDLE)(INT_PTR)srcTouchRect.bottom);

	SetProp(hWnd, L"Magpie.DestTouchLeft", (HANDLE)(INT_PTR)destTouchRect.left);
	SetProp(hWnd, L"Magpie.DestTouchTop", (HANDLE)(INT_PTR)destTouchRect.top);
	SetProp(hWnd, L"Magpie.DestTouchRight", (HANDLE)(INT_PTR)destTouchRect.right);
	SetProp(hWnd, L"Magpie.DestTouchBottom", (HANDLE)(INT_PTR)destTouchRect.bottom);
}

// 文档要求窗口被销毁前清理所有属性，但实际上这不是必须的，见
// https://devblogs.microsoft.com/oldnewthing/20231030-00/?p=108939
void ScalingWindow::_RemoveWindowProps() const noexcept {
	const HWND hWnd = Handle();
	RemoveProp(hWnd, L"Magpie.SrcHWND");
	RemoveProp(hWnd, L"Magpie.SrcLeft");
	RemoveProp(hWnd, L"Magpie.SrcTop");
	RemoveProp(hWnd, L"Magpie.SrcRight");
	RemoveProp(hWnd, L"Magpie.SrcBottom");
	RemoveProp(hWnd, L"Magpie.DestLeft");
	RemoveProp(hWnd, L"Magpie.DestTop");
	RemoveProp(hWnd, L"Magpie.DestRight");
	RemoveProp(hWnd, L"Magpie.DestBottom");

	if (_options.IsTouchSupportEnabled()) {
		RemoveProp(hWnd, L"Magpie.SrcTouchLeft");
		RemoveProp(hWnd, L"Magpie.SrcTouchTop");
		RemoveProp(hWnd, L"Magpie.SrcTouchRight");
		RemoveProp(hWnd, L"Magpie.SrcTouchBottom");
		RemoveProp(hWnd, L"Magpie.DestTouchLeft");
		RemoveProp(hWnd, L"Magpie.DestTouchTop");
		RemoveProp(hWnd, L"Magpie.DestTouchRight");
		RemoveProp(hWnd, L"Magpie.DestTouchBottom");
	}
}

LRESULT ScalingWindow::_BorderHelperWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	if (msg == WM_NCCREATE) {
		SetWindowLongPtr(hWnd, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCT*)lParam)->lpCreateParams);
	} else {
		switch (msg) {
#ifdef MP_DEBUG_BORDER
		case WM_ERASEBKGND:
		{
			// 用颜色标示辅助窗口
			int side = (int)GetWindowLongPtr(hWnd, GWLP_USERDATA);
			HBRUSH hBrush = CreateSolidBrush(side % 2 == 0 ? RGB(255, 0, 0) : RGB(0, 0, 255));
			RECT clientRect;
			GetClientRect(hWnd, &clientRect);
			FillRect((HDC)wParam, &clientRect, hBrush);
			DeleteBrush(hBrush);
			return TRUE;
		}
#endif
		case WM_NCHITTEST:
		{
			POINT cursorPos{ GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam) };
			ScreenToClient(hWnd, &cursorPos);

			RECT clientRect;
			GetClientRect(hWnd, &clientRect);
			if (!PtInRect(&clientRect, cursorPos)) {
				return HTNOWHERE;
			}

			switch (GetWindowLongPtr(hWnd, GWLP_USERDATA)) {
			case 0:
			{
				const int resizeHandleWidth = clientRect.right - clientRect.left;
				if (cursorPos.y < resizeHandleWidth) {
					return HTTOPLEFT;
				} else if (cursorPos.y + resizeHandleWidth >= clientRect.bottom) {
					return HTBOTTOMLEFT;
				} else {
					return HTLEFT;
				}
			}
			case 1:
			{
				const int resizeHandleHeight = clientRect.bottom - clientRect.top;
				if (cursorPos.x < 2 * resizeHandleHeight) {
					return HTTOPLEFT;
				} else if (cursorPos.x + 2 * resizeHandleHeight >= clientRect.right) {
					return HTTOPRIGHT;
				} else {
					return HTTOP;
				}
			}
			case 2:
			{
				const int resizeHandleWidth = clientRect.right - clientRect.left;
				if (cursorPos.y < resizeHandleWidth) {
					return HTTOPRIGHT;
				} else if (cursorPos.y + resizeHandleWidth >= clientRect.bottom) {
					return HTBOTTOMRIGHT;
				} else {
					return HTRIGHT;
				}
			}
			case 3:
			{
				const int resizeHandleHeight = clientRect.bottom - clientRect.top;
				if (cursorPos.x < 2 * resizeHandleHeight) {
					return HTBOTTOMLEFT;
				} else if (cursorPos.x + 2 * resizeHandleHeight >= clientRect.right) {
					return HTBOTTOMRIGHT;
				} else {
					return HTBOTTOM;
				}
			}
			}
			break;
		}
		case WM_NCLBUTTONDOWN:
		case WM_NCLBUTTONDBLCLK:
		case WM_NCMOUSEMOVE:
		case WM_NCLBUTTONUP:
		{
			if (wParam >= HTSIZEFIRST || wParam <= HTSIZELAST) {
				// 将这些消息传给主窗口才能调整窗口大小
				return Get()._MessageHandler(msg, wParam, lParam);
			}
			return 0;
		}
		}
	}

	return DefWindowProc(hWnd, msg, wParam, lParam);
}

void ScalingWindow::_CreateBorderHelperWindows() noexcept {
	[[maybe_unused]] static Ignore _ = [] {
		WNDCLASSEXW wcex{
			.cbSize = sizeof(wcex),
			.lpfnWndProc = _BorderHelperWndProc,
			.hInstance = wil::GetModuleInstanceHandle(),
			.lpszClassName = CommonSharedConstants::SCALING_BORDER_HELPER_WINDOW_CLASS_NAME
		};
		RegisterClassEx(&wcex);

		return Ignore();
	}();

	if (Win32Helper::IsWindowHung(_srcTracker.Handle())) {
		Logger::Get().Error("源窗口已挂起");
		_DelayedStop(true);
		return;
	}

	const bool isBorderless = _IsBorderless();
	for (int i = 0; i < 4; ++i) {
		// 启用触控支持时始终创建四个辅助窗口，因为缩放窗口是透明的
		if (i != 1 && !isBorderless && !_options.IsTouchSupportEnabled()) {
			continue;
		}

		_hwndResizeHelpers[i].reset(CreateWindowEx(
#ifdef MP_DEBUG_BORDER
			WS_EX_NOACTIVATE,
#else
			WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP,
#endif
			CommonSharedConstants::SCALING_BORDER_HELPER_WINDOW_CLASS_NAME,
			nullptr,
			WS_POPUP,
			0, 0, 0, 0,
			Handle(),
			NULL,
			wil::GetModuleInstanceHandle(),
			(void*)(intptr_t)i
		));
	}
}

void ScalingWindow::_RepostionBorderHelperWindows() noexcept {
	int resizeHandleLen =
		GetSystemMetricsForDpi(SM_CXPADDEDBORDER, _currentDpi) +
		GetSystemMetricsForDpi(SM_CYSIZEFRAME, _currentDpi);
	// 启用触控支持时扩大窗口边缘可调整尺寸的区域
	if (_options.IsTouchSupportEnabled()) {
		resizeHandleLen *= 2;
	}

	int flags = SWP_NOACTIVATE | SWP_NOCOPYBITS;

	if (_areResizeHelperWindowsVisible) {
		flags |= SWP_NOZORDER;
	} else {
		_areResizeHelperWindowsVisible = true;
		flags |= SWP_SHOWWINDOW;

		// 辅助窗口的所有者链包含源窗口，因此应检查源窗口是否挂起，否则 SetWindowPos
		// 可能卡死。
		if (Win32Helper::IsWindowHung(_srcTracker.Handle())) {
			Logger::Get().Error("源窗口已挂起");
			_DelayedStop(true);
			return;
		}
	}

#ifndef MP_DEBUG_BORDER
	flags |= SWP_NOREDRAW;
#endif

	// ┌────────────────────┐
	// │         1          │
	// ├───┬────────────┬───┤
	// │   │            │   │
	// │ 0 │            │ 2 │
	// │   │            │   │
	// ├───┴────────────┴───┤
	// │         3          │
	// └────────────────────┘
	if (const wil::unique_hwnd& hWnd = _hwndResizeHelpers[0]) {
		SetWindowPos(
			hWnd.get(),
			NULL,
			_rendererRect.left - resizeHandleLen,
			_rendererRect.top,
			resizeHandleLen,
			_rendererRect.bottom - _rendererRect.top,
			flags
		);
	}
	if (const wil::unique_hwnd& hWnd = _hwndResizeHelpers[1]) {
		SetWindowPos(
			hWnd.get(),
			NULL,
			_rendererRect.left - resizeHandleLen,
			_rendererRect.top - resizeHandleLen,
			_rendererRect.right - _rendererRect.left + 2 * resizeHandleLen,
			resizeHandleLen,
			flags
		);
	}
	if (const wil::unique_hwnd& hWnd = _hwndResizeHelpers[2]) {
		SetWindowPos(
			hWnd.get(),
			NULL,
			_rendererRect.right,
			_rendererRect.top,
			resizeHandleLen,
			_rendererRect.bottom - _rendererRect.top,
			flags
		);
	}
	if (const wil::unique_hwnd& hWnd = _hwndResizeHelpers[3]) {
		SetWindowPos(
			hWnd.get(),
			NULL,
			_rendererRect.left - resizeHandleLen,
			_rendererRect.bottom,
			_rendererRect.right - _rendererRect.left + 2 * resizeHandleLen,
			resizeHandleLen,
			flags
		);
	}
}

static LRESULT CALLBACK BkgWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	if (msg == WM_WINDOWPOSCHANGING) {
		// 确保始终在缩放窗口后
		((WINDOWPOS*)lParam)->hwndInsertAfter = ScalingWindow::Get().Handle();
	}

	return DefWindowProc(hWnd, msg, wParam, lParam);
}

// 将黑边映射到源窗口
RECT ScalingWindow::_CalcSrcTouchRect() const noexcept {
	const RECT& srcRect = _renderer->SrcRect();
	const RECT& destRect = _renderer->DestRect();

	const double scaleX = double(destRect.right - destRect.left) / (srcRect.right - srcRect.left);
	const double scaleY = double(destRect.bottom - destRect.top) / (srcRect.bottom - srcRect.top);

	RECT srcTouchRect = srcRect;

	if (destRect.left > _rendererRect.left) {
		srcTouchRect.left -= lround((destRect.left - _rendererRect.left) / scaleX);
	}
	if (destRect.top > _rendererRect.top) {
		srcTouchRect.top -= lround((destRect.top - _rendererRect.top) / scaleX);
	}
	if (destRect.right < _rendererRect.right) {
		srcTouchRect.right += lround((_rendererRect.right - destRect.right) / scaleY);
	}
	if (destRect.bottom < _rendererRect.bottom) {
		srcTouchRect.bottom += lround((_rendererRect.bottom - destRect.bottom) / scaleY);
	}

	return srcTouchRect;
}

// 在源窗口四周创建辅助窗口拦截黑边上的触控点击。
//
// 直接将 srcRect 映射到 destRect 是天真的想法。似乎可以创建一个全屏的背景窗口来屏
// 蔽黑边，该方案的问题是无法解决源窗口和黑边的重叠部分。作为黑边，本应拦截用户点击，
// 但这也拦截了对源窗口的操作；若是不拦截会导致在黑边上可以操作源窗口。
//
// 我们的方案是：将源窗口和其周围映射到整个缩放窗口，并在源窗口四周创建背景窗口拦截
// 对黑边的点击。注意这些背景窗口不能由 TouchHelper.exe 创建，因为它有 UIAccess
// 权限，创建的窗口会遮盖缩放窗口。
void ScalingWindow::_UpdateTouchHoleWindows(bool onInit) noexcept {
	if (_options.IsWindowedMode()) {
		// 窗口模式缩放不存在黑边，因此不需要创建辅助窗口
		_UpdateTouchProps(_srcTracker.SrcRect());
		return;
	}

	const RECT& srcRect = _srcTracker.SrcRect();
	const RECT srcTouchRect = _CalcSrcTouchRect();
	_UpdateTouchProps(srcTouchRect);

	[[maybe_unused]] static Ignore _ = [] {
		WNDCLASSEXW wcex{
			.cbSize = sizeof(wcex),
			.lpfnWndProc = BkgWndProc,
			.hInstance = wil::GetModuleInstanceHandle(),
			.lpszClassName = CommonSharedConstants::TOUCH_HELPER_HOLE_WINDOW_CLASS_NAME
		};
		RegisterClassEx(&wcex);

		return Ignore();
	}();

	const auto createOrUpdateHoleWindow = [&](uint32_t idx, LONG left, LONG top, LONG right, LONG bottom) noexcept {
		wil::unique_hwnd& hWnd = _hwndTouchHoles[idx];
		if (hWnd) {
			SetWindowPos(hWnd.get(), Handle(), left, top, right - left, bottom - top,
				SWP_SHOWWINDOW | SWP_NOACTIVATE | SWP_NOREDRAW | SWP_NOCOPYBITS);
		} else {
			hWnd.reset(CreateWindowEx(
				WS_EX_NOREDIRECTIONBITMAP | WS_EX_NOACTIVATE,
				CommonSharedConstants::TOUCH_HELPER_HOLE_WINDOW_CLASS_NAME,
				nullptr,
				WS_POPUP,
				left,
				top,
				right - left,
				bottom - top,
				NULL,
				NULL,
				wil::GetModuleInstanceHandle(),
				0
			));

			// 推迟到缩放窗口显示后再显示
			if (!onInit) {
				SetWindowPos(hWnd.get(), Handle(), 0, 0, 0, 0,
					SWP_SHOWWINDOW | SWP_NOACTIVATE | SWP_NOREDRAW | SWP_NOMOVE | SWP_NOSIZE);
			}
		}
	};

	//      srcTouchRect
	// ┌───┬───────────┬───┐
	// │   │     1     │   │
	// │   ├───────────┤   │
	// │   │           │   │
	// │ 0 │  srcRect  │ 2 │
	// │   │           │   │
	// │   ├───────────┤   │
	// │   │     3     │   │
	// └───┴───────────┴───┘

	if (srcRect.left > srcTouchRect.left) {
		createOrUpdateHoleWindow(0, srcTouchRect.left, srcTouchRect.top, srcRect.left, srcTouchRect.bottom);
	} else {
		_hwndTouchHoles[0].reset();
	}

	if (srcRect.top > srcTouchRect.top) {
		createOrUpdateHoleWindow(1, srcRect.left, srcTouchRect.top, srcRect.right, srcRect.top);
	} else {
		_hwndTouchHoles[1].reset();
	}

	if (srcRect.right < srcTouchRect.right) {
		createOrUpdateHoleWindow(2, srcRect.right, srcTouchRect.top, srcTouchRect.right, srcTouchRect.bottom);
	} else {
		_hwndTouchHoles[2].reset();
	}

	if (srcRect.bottom < srcTouchRect.bottom) {
		createOrUpdateHoleWindow(3, srcRect.left, srcRect.bottom, srcRect.right, srcTouchRect.bottom);
	} else {
		_hwndTouchHoles[3].reset();
	}
}

void ScalingWindow::_UpdateFrameMargins() const noexcept {
	assert(_options.IsWindowedMode());

	if (Win32Helper::GetOSVersion().IsWin11() || _IsBorderless()) {
		return;
	}

	// 由于缩放窗口有 WS_EX_NOREDIRECTIONBITMAP 样式，所以不需要绘制黑色实线。需要扩展到
	// 标题栏高度，原因见 XamlWindowT::_UpdateFrameMargins 中的注释。
	RECT frame{};
	AdjustWindowRectExForDpi(&frame, GetWindowStyle(Handle()), FALSE, 0, _currentDpi);

	MARGINS margins{ .cyTopHeight = -frame.top };
	DwmExtendFrameIntoClientArea(Handle(), &margins);
}

winrt::fire_and_forget ScalingWindow::_UpdateFocusStateAsync() const noexcept {
	if (_options.IsWindowedMode()) {
		// 根据源窗口状态绘制非客户区，我们必须自己控制非客户区是绘制成焦点状态还是非焦点
		// 状态，因为缩放窗口实际上永远不会得到焦点。
		DefWindowProc(Handle(), WM_NCACTIVATE, _srcTracker.IsSessionForeground(), 0);
	}

	if (_isDestroying || !_sourceCoordinator.Ready()) co_return;
	if (_sourceCoordinator.Mode() == WindowCoordinationMode::ManagedUnowned) {
		if (!_options.IsDebugMode() && !_sourceCoordinator.UpdateManagedOrder(
			_CalcTopmostState(), _srcTracker.IsSessionForeground(), _options.IsTopmostDisabled())) {
			Logger::Get().Win32Error("Managed window order update failed");
			_DelayedStop();
			co_return;
		}
		PostMessage(HWND_BROADCAST, WM_MAGPIE_SCALINGCHANGED,
			_srcTracker.IsSessionForeground() ? 1 : 0, _srcTracker.IsSessionForeground() ? LPARAM(Handle()) : 1);
		co_return;
	}

	if (!_options.IsDebugMode()) {
		if (Win32Helper::IsWindowHung(SourceRoot())) {
			Logger::Get().Error("源窗口已挂起");
			_DelayedStop();
			co_return;
		}

		// 这里搞得很复杂，是我反复实验得到的，若要修改应测试下列情形：
		// 1. 缩放 WindowCase 中的 TopmostWindow 和 PopupHostWindow
		// 2. 缩放常规窗口然后切换到管理员身份的窗口。测试这一条时应直接运行，不要调试，因
		// 为调试状态下 SetWindowPos 的行为有变化
		// 3. 缩放时将任意窗口最小化然后还原
		// 4. 缩放时拖动任意窗口

		const bool oldTopmost = IsTopmostWindow(Handle());
		const bool newTopmost = _CalcTopmostState();
		if (oldTopmost != newTopmost) {
			if (newTopmost) {
				for (int i = 0; i < 10; ++i) {
					SetWindowPos(Handle(), HWND_TOPMOST, 0, 0, 0, 0, SWP_NO_ACTIVATE_MOVE_SIZE);

					if (IsTopmostWindow(Handle()) == newTopmost) {
						break;
					}
				}

				// 全屏模式缩放时确保缩放窗口在所有置顶窗口之上，这使不支持 MPO 的显卡更容易激
				// 活 DirectFlip。
				if (!_options.IsWindowedMode()) {
					SetWindowPos(Handle(), HWND_TOP, 0, 0, 0, 0, SWP_NO_ACTIVATE_MOVE_SIZE);
				}
			} else {
				const uint32_t runId = ScalingWindow::RunId();
				bool isInBackground = false;

				HWND hwndFore = Win32Helper::GetRealForegroundWindow();
				if (!hwndFore) {
					// 切换窗口时有一个瞬间无前台窗口，这里等待切换完成
					co_await winrt::resume_after(1ms);
					isInBackground = true;
					hwndFore = Win32Helper::GetRealForegroundWindow();
				}

				bool isForeMovable = true;
				if (hwndFore) {
					DWORD windowIL;
					isForeMovable = Win32Helper::GetWindowIntegrityLevel(hwndFore, windowIL) &&
						windowIL <= Win32Helper::GetCurrentProcessIntegrityLevel();
				}

				if (!isForeMovable) {
					if (!isInBackground) {
						co_await winrt::resume_background();
						isInBackground = true;
					}

					// 等待 DWM 开始合成新帧以避免显示中间状态
					Win32Helper::WaitForDwmComposition();
				}

				if (isInBackground) {
					co_await ScalingWindow::Get().Dispatcher();

					// 等待时源窗口重新回到前台了应放弃后续操作
					if (runId != ScalingWindow::RunId() || _isDestroying || !_sourceCoordinator.Ready() || IsSessionForeground(Win32Helper::GetRealForegroundWindow())) {
						co_return;
					}
				}

				for (int i = 0; i < 10; ++i) {
					HDWP hDwp = BeginDeferWindowPos(isForeMovable ? 2 : 3);

					// 改变置顶状态
					hDwp = DeferWindowPos(hDwp, Handle(), HWND_NOTOPMOST,
						0, 0, 0, 0, SWP_NO_ACTIVATE_MOVE_SIZE);

					// 将缩放窗口恰好置于源窗口前
					if (isForeMovable) {
						// 这个方式没有中间状态，但会导致源窗口遮挡前台窗口，之后会手动将前台窗口移到顶部
						hDwp = DeferWindowPos(hDwp, Handle(), SourceRoot(),
							0, 0, 0, 0, SWP_NO_ACTIVATE_MOVE_SIZE);
					} else {
						// 这个方式不会移动源窗口，但有中间状态。如果前台窗口 IL 更高，这是唯一的办法
						hDwp = DeferWindowPos(hDwp, Handle(), SourceRoot(),
							0, 0, 0, 0, SWP_NO_ACTIVATE_MOVE_SIZE | SWP_NOOWNERZORDER);
						hDwp = DeferWindowPos(hDwp, SourceRoot(), Handle(),
							0, 0, 0, 0, SWP_NO_ACTIVATE_MOVE_SIZE | SWP_NOOWNERZORDER);
					}

					EndDeferWindowPos(hDwp);

					// 确保缩放窗口刚好在源窗口前
					if (IsTopmostWindow(Handle()) == newTopmost &&
						GetWindow(SourceRoot(), GW_HWNDPREV) == Handle()) {
						break;
					}
				}

				if (isForeMovable && hwndFore && Win32Helper::GetRealForegroundWindow() == hwndFore) {
					SetWindowPos(hwndFore, HWND_TOP, 0, 0, 0, 0, SWP_NO_ACTIVATE_MOVE_SIZE);
				}
			}
		}
	}

	if (_srcTracker.IsSessionForeground()) {
		PostMessage(HWND_BROADCAST, WM_MAGPIE_SCALINGCHANGED, 1, (LPARAM)Handle());
	} else {
		// lParam 传 1 表示转到后台而非结束缩放
		PostMessage(HWND_BROADCAST, WM_MAGPIE_SCALINGCHANGED, 0, 1);
	}
}

bool ScalingWindow::_CalcTopmostState() const noexcept {
	return _sourceCoordinator.NeedsTopmost(_srcTracker.IsSessionForeground(), _options.IsTopmostDisabled());
}

bool ScalingWindow::_IsBorderless() const noexcept {
	assert(_options.IsWindowedMode());

	const SrcWindowKind srcWindowKind = _srcTracker.WindowKind();
	// NoBorder: Win11 中这类窗口有着特殊的边框，因此和 Win10 的处理方式相同。
	// NoNativeFrame: Win11 中实现为无标题栏并隐藏边框。
	return srcWindowKind == SrcWindowKind::NoBorder ||
		(srcWindowKind == SrcWindowKind::NoNativeFrame && Win32Helper::GetOSVersion().IsWin10());
}

void ScalingWindow::_UpdateRendererRect() noexcept {
	RECT oldRendererRect = _rendererRect;

	if (_options.IsWindowedMode()) {
		_rendererRect = _CalcWindowedRendererRect();
	} else {
		_rendererRect = _windowRect;
	}

	if (_rendererRect == oldRendererRect) {
		return;
	}

	const bool resized = Win32Helper::GetSizeOfRect(_rendererRect) !=
		Win32Helper::GetSizeOfRect(oldRendererRect);

	// 全屏模式缩放时不移动源窗口，因为我们不限制最小尺寸，而且源窗口可能处于最大化或全屏状态
	if (_options.IsWindowedMode() && !_isMovingDueToSrcMoved && !_srcTracker.IsMoving()) {
		// 确保源窗口中心点和缩放窗口中心点相同。应先移动源窗口，因为之后需要调整光标位置
		const RECT& srcRect = _srcTracker.WindowRect();
		const int offsetX = (_windowRect.left + _windowRect.right - srcRect.left - srcRect.right) / 2;
		const int offsetY = (_windowRect.top + _windowRect.bottom - srcRect.top - srcRect.bottom) / 2;
		if (!_srcTracker.Move(offsetX, offsetY, _isResizingOrMoving)) {
			Logger::Get().Error("SrcTracker::Move 失败");
			_DelayedStop();
			return;
		}

		_cursorManager->OnSrcRectChanged();
	}

	if (_hwndRenderer == Handle()) {
		if (resized) {
			// 为了平滑调整窗口尺寸，渲染所在窗口需要在 WM_NCCALCSIZE 中
			// 更新渲染尺寸。
			_ResizeRenderer();
		} else {
			_MoveRenderer();
		}
	} else {
		// 渲染口过程将在 WM_NCCALCSIZE 中更新渲染尺寸
		SetWindowPos(
			_hwndRenderer,
			NULL,
			_nonTopBorderThicknessInClient,
			_topBorderThicknessInClient,
			_rendererRect.right - _rendererRect.left,
			_rendererRect.bottom - _rendererRect.top,
			SWP_NOACTIVATE | SWP_NOCOPYBITS | SWP_NOZORDER | (resized ? 0 : SWP_NOSIZE)
		);
	}
}

// OS 有类似的机制，但我们很少能触发，只能自己处理。参考自
// https://github.com/Blinue/nt5src/blob/daad8a087a4e75422ec96b7911f1df4669989611/Source/XPSP1/NT/windows/core/ntuser/kernel/movesize.c#L592
bool ScalingWindow::_EnsureCaptionVisibleOnScreen() noexcept {
	struct EnumData {
		RECT windowRect;
		bool captionVisible;
	} data{ _windowRect, false };

	static const auto enumMonitorProc = [](HMONITOR hMon, HDC, LPRECT, LPARAM lParam) {
		EnumData& data = *(EnumData*)lParam;

		// 使用工作矩形而不是屏幕矩形
		MONITORINFO mi{ .cbSize = sizeof(mi) };
		if (!GetMonitorInfo(hMon, &mi)) {
			return TRUE;
		}

		if (Win32Helper::IsRectOverlap(data.windowRect, mi.rcWork) && data.windowRect.top >= mi.rcWork.top) {
			data.captionVisible = true;
			return FALSE;
		} else {
			return TRUE;
		}
	};

	EnumDisplayMonitors(NULL, NULL, enumMonitorProc, (LPARAM)&data);

	if (data.captionVisible) {
		return false;
	}

	// 调整位置使得标题栏可见
	HMONITOR hMon = MonitorFromWindow(Handle(), MONITOR_DEFAULTTONEAREST);
	MONITORINFO mi{ .cbSize = sizeof(mi) };
	if (!GetMonitorInfo(hMon, &mi)) {
		Logger::Get().Win32Error("GetMonitorInfo 失败");
		return false;
	}

	// 为了避免光标位置跳跃应等待渲染而且不要使用 SWP_NOSENDCHANGING
	_shouldWaitForRender = true;
	bool result = SetWindowPos(
		Handle(),
		NULL,
		_windowRect.left, mi.rcWork.top, 0, 0,
		SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOREDRAW | SWP_NOSIZE
	);
	_shouldWaitForRender = false;
	return result;
}

void ScalingWindow::_UpdateWindowRectFromWindowPos(const WINDOWPOS& windowPos) noexcept {
	assert((windowPos.flags & (SWP_NOSIZE | SWP_NOMOVE)) != (SWP_NOSIZE | SWP_NOMOVE));

	if (windowPos.flags & SWP_NOSIZE) {
		const LONG offsetX = windowPos.x - _windowRect.left;
		const LONG offsetY = windowPos.y - _windowRect.top;
		_windowRect.left = windowPos.x;
		_windowRect.top = windowPos.y;
		_windowRect.right += offsetX;
		_windowRect.bottom += offsetY;
	} else if (windowPos.flags & SWP_NOMOVE) {
		_windowRect.right = _windowRect.left + windowPos.cx;
		_windowRect.bottom = _windowRect.top + windowPos.cy;
	} else {
		_windowRect.left = windowPos.x;
		_windowRect.top = windowPos.y;
		_windowRect.right = windowPos.x + windowPos.cx;
		_windowRect.bottom = windowPos.y + windowPos.cy;
	}
}

void ScalingWindow::_DelayedStop(bool onSrcHung, bool onSrcRepositioning) const noexcept {
	const uint8_t requested = onSrcRepositioning ? 1 : 2;
	if (_pendingSourceTransition >= requested) return;
	_pendingSourceTransition = requested;
	if (!onSrcHung) {
		const HWND hwndSrc = _srcTracker.Handle();
		if (IsTopmostWindow(Handle()) && !(IsWindow(hwndSrc) && Win32Helper::IsWindowHung(hwndSrc))) {
			// 提前取消置顶，这样销毁时出现问题不会影响和桌面环境交互
			SetWindowPos(Handle(), HWND_NOTOPMOST, 0, 0, 0, 0,
				SWP_NO_ACTIVATE_MOVE_SIZE | SWP_NOOWNERZORDER);
		}
	}

	// The outer loop owns teardown. Keep the request while a parameter press
	// is held instead of repeatedly enqueueing callbacks which abandon it.
}

bool ScalingWindow::ProcessPendingSourceTransition() noexcept {
	if (!HasPendingSourceTransition() || HasHeldParameterInput()) return false;
	const uint8_t transition = std::exchange(_pendingSourceTransition, uint8_t{});
	if (transition == 1) {
		if (_renderer) _repositionOverlayState = _renderer->CaptureOverlayState();
		_isSrcRepositioning = true;
		Destroy();
	} else {
		Stop();
	}
	return true;
}

}
