#include "pch.h"
#include "App.h"
#include "AppSettings.h"
#include "CommonSharedConstants.h"
#include "EffectsService.h"
#include "ErrorService.h"
#include "Logger.h"
#include "ProfileService.h"
#include "ScalingMode.h"
#include "ScalingModesService.h"
#include "ScalingService.h"
#include "StrHelper.h"
#include "ShortcutService.h"
#include "ToastService.h"
#include "TouchHelper.h"
#include "Win32Helper.h"
#include "WindowHelper.h"

using namespace winrt::Magpie::implementation;
using namespace winrt;

using winrt::Magpie::ShortcutAction;

namespace Magpie {

static ToolbarShortcutLabels GetToolbarShortcutLabels() {
	const auto& settings = AppSettings::Get();
	auto label = [&](ShortcutAction action) {
		return StrHelper::UTF16ToUTF8(settings.GetShortcut(action).ToString());
	};
	return {
		.profiler = label(ShortcutAction::Profiler),
		.parameters = label(ShortcutAction::EffectParameters),
		.screenshot = label(ShortcutAction::Screenshot),
		.pin = label(ShortcutAction::ToolbarPin),
		.comparison = label(ShortcutAction::Comparison),
		.fullscreen = label(ShortcutAction::Scale),
		.windowed = label(ShortcutAction::WindowedModeScale)
	};
}

ScalingService& ScalingService::Get() noexcept {
	static ScalingService instance;
	return instance;
}

ScalingService::~ScalingService() {}

void ScalingService::Initialize() {
	_scalingRuntime.emplace();
	_scalingRuntime->StateChanged(
		std::bind_front(&ScalingService::_ScalingRuntime_StateChanged, this));

	const DispatcherQueue& dispatcher = App::Get().Dispatcher();

	_countDownTimer = dispatcher.CreateTimer();
	_countDownTimer.Interval(25ms);
	_countDownTimer.Tick({ this, &ScalingService::_CountDownTimer_Tick });

	_checkForegroundTimer = dispatcher.CreateTimer();
	_checkForegroundTimer.Interval(50ms);
	_checkForegroundTimer.Tick({ this, &ScalingService::_CheckForegroundTimer_Tick });
	_checkForegroundTimer.Start();

	_shortcutActivatedRevoker = ShortcutService::Get().ShortcutActivated(
		auto_revoke, std::bind_front(&ScalingService::_ShortcutService_ShortcutPressed, this));
	_toolbarShortcutsChangedRevoker = AppSettings::Get().ShortcutChanged(auto_revoke, [this](ShortcutAction) {
		if (_scalingRuntime) _scalingRuntime->UpdateToolbarShortcutLabels(GetToolbarShortcutLabels());
	});
	_frameSyncChangedRevoker = AppSettings::Get().FrontEdgeSyncChanged(auto_revoke, [this] {
		const auto& settings = AppSettings::Get();
		if (_scalingRuntime) _scalingRuntime->UpdateFrameSyncSettings(
			{ settings.IsFrontEdgeSyncEnabled(), settings.FrontEdgeSyncFrameRate(), settings.GetFrameSyncMode() });
	});

	// 立即检查前台窗口
	_CheckForegroundTimer_Tick(nullptr, nullptr);
}

void ScalingService::Uninitialize() {
	if (!_scalingRuntime) {
		return;
	}

	_checkForegroundTimer.Stop();
	_countDownTimer.Stop();
	_frameSyncChangedRevoker.Revoke();
	_toolbarShortcutsChangedRevoker.Revoke();
	_scalingRuntime.reset();
	// The runtime destructor drains UI requests before this final flush.
	_FlushEffectParametersSaves(true);
	// Also wait for any snapshot already being written in the background. The
	// newest synchronous revision includes requests drained during shutdown.
	AppSettings::Get().Save();

	_shortcutActivatedRevoker.Revoke();
}

void ScalingService::StartTimer(bool windowedMode) {
	_curCountdownSeconds = AppSettings::Get().CountdownSeconds();
	_isCurCountdownWindowedMode = windowedMode;
	_timerStartTimePoint = std::chrono::steady_clock::now();
	// 如果计时器已经启动会被重置，这正是我们想要的
	_countDownTimer.Start();
	IsTimerOnChanged.Invoke(true, windowedMode);
}

void ScalingService::StopTimer() {
	if (_curCountdownSeconds == 0) {
		return;
	}

	_curCountdownSeconds = 0;
	_countDownTimer.Stop();
	IsTimerOnChanged.Invoke(false, _isCurCountdownWindowedMode);
}

double ScalingService::SecondsLeft() const noexcept {
	using namespace std::chrono;

	if (!IsTimerOn()) {
		return std::numeric_limits<double>::max();
	}

	// DispatcherTimer 误差很大，因此我们自己计算剩余时间
	auto now = steady_clock::now();
	int msLeft = (int)duration_cast<milliseconds>(_timerStartTimePoint + seconds(_curCountdownSeconds) - now).count();
	return msLeft / 1000.0;
}

bool ScalingService::IsScaling() const noexcept {
	// 等待状态视为未缩放
	return _scalingRuntime->State() == ScalingState::Scaling;
}

void ScalingService::CheckForeground() {
	_hwndChecked = NULL;
	_CheckForegroundTimer_Tick(nullptr, nullptr);
}

void ScalingService::OnTaskSwitch() {
	if (!AppSettings::Get().IsStopEffectsOnTaskSwitchEnabled()) return;
	if (!_scalingRuntime || !_scalingRuntime->StopForTaskSwitch()) return;
	_isAutoScaleSuspended = true;
	StopTimer();
	Logger::Get().Info("Task switch: stopping fullscreen effects; waiting for a manual start");
}

void ScalingService::_ShortcutService_ShortcutPressed(ShortcutAction action) {
	switch (action) {
	case ShortcutAction::Scale:
	case ShortcutAction::WindowedModeScale:
	{
		const bool isWindowdMode = action == ShortcutAction::WindowedModeScale;

		const ScalingState state = _scalingRuntime->State();
		if (state == ScalingState::Scaling) {
			_scalingRuntime->ToggleScaling(isWindowdMode);
		} else if (state == ScalingState::Starting) {
			// A repeated hotkey during initialization means cancel this run. The
			// runtime generation prevents the queued stop from affecting a later run.
			_scalingRuntime->Stop();
		} else if (state == ScalingState::Stopping) {
			return;
		} else {
			_ScaleForegroundWindow(isWindowdMode);
		}

		break;
	}
	case ShortcutAction::Profiler:
		_scalingRuntime->InvokeOverlayAction(OverlayAction::Profiler);
		break;
	case ShortcutAction::EffectParameters:
		_scalingRuntime->InvokeOverlayAction(OverlayAction::EffectParameters);
		break;
	case ShortcutAction::Screenshot:
		_scalingRuntime->InvokeOverlayAction(OverlayAction::Screenshot);
		break;
	case ShortcutAction::ToolbarPin:
		_scalingRuntime->InvokeOverlayAction(OverlayAction::ToolbarPin);
		break;
	case ShortcutAction::Comparison:
		_scalingRuntime->InvokeOverlayAction(OverlayAction::Comparison);
		break;
	case ShortcutAction::Toolbar:
	{
		_scalingRuntime->SwitchToolbarState();
		break;
	}
	default:
		break;
	}
}

void ScalingService::_CountDownTimer_Tick(winrt::DispatcherQueueTimer const&, winrt::IInspectable const&) {
	const double timeLeft = SecondsLeft();

	// 剩余时间在 10 ms 以内计时结束
	if (timeLeft < 0.01) {
		StopTimer();
		_ScaleForegroundWindow(_isCurCountdownWindowedMode);
		return;
	}

	TimerTick.Invoke(timeLeft);
}

static IssueContext MakeIssueContext(HWND window, const Profile& profile) {
	IssueContext result;
	result.hasProfile = true;
	result.profileName = profile.name;
	result.profilePath = profile.pathRule;
	result.profileClass = profile.classNameRule;
	result.captureMethod = profile.captureMethod;
	const auto& modes = AppSettings::Get().ScalingModes();
	if (profile.scalingMode >= 0 && static_cast<size_t>(profile.scalingMode) < modes.size())
		result.scalingModeName = modes[profile.scalingMode].name;
	wchar_t title[512]{};
	GetWindowTextW(window, title, static_cast<int>(std::size(title)));
	result.windowTitle = title;
	return result;
}

static bool IsPopupWindow(HWND hwndPopup, HWND hwndOwner) noexcept {
	// 检查所有者关系
	{
		HWND hwndCur = hwndPopup;
		while (bool(hwndCur = GetWindowOwner(hwndCur))) {
			if (hwndCur == hwndOwner) {
				return true;
			}
		}
	}

	// 有些游戏不用所有者关系来实现弹窗，而是将主窗口禁用，做到和模态弹窗差不多的效果。
	// 这不可能准确检测，只能尽可能增加限制以减少误判，我们检查三个条件：
	//
	// 1. 主窗口处于禁用状态
	// 2. 两个窗口位于同一个进程
	// 3. 主窗口没有传统意义的弹窗

	if (IsWindowEnabled(hwndOwner)) {
		return false;
	}

	DWORD pid1 = 0;
	DWORD pid2 = 0;
	GetWindowThreadProcessId(hwndPopup, &pid1);
	GetWindowThreadProcessId(hwndOwner, &pid2);
	if (pid1 != pid2) {
		return false;
	}

	return !GetWindow(hwndOwner, GW_ENABLEDPOPUP);
}

static bool IsReadyForScaling(HWND hwndFore) noexcept {
	// GH#1148
	// 有些游戏刚启动时将窗口创建在屏幕外，初始化完成后再移到屏幕内
	if (!MonitorFromWindow(hwndFore, MONITOR_DEFAULTTONULL)) {
		return false;
	}

	// GH#1200
	// 有些游戏加载时不响应消息，应等待加载完成
	return !Win32Helper::IsWindowHung(hwndFore);
}

void ScalingService::_CheckForegroundTimer_Tick(winrt::DispatcherQueueTimer const&, winrt::IInspectable const&) {
	if (_isAutoScaleSuspended) return;
	const HWND hwndFore = GetForegroundWindow();
	if (!hwndFore || hwndFore == _hwndChecked) {
		return;
	}

	if (hwndFore != _hwndCurSrc) {
		// 检查自动缩放
		const Profile* profile = ProfileService::Get().GetProfileForWindow(hwndFore, true);
		// 正在缩放窗口时禁止自动缩放它的弹窗
		if (profile && !(_hwndCurSrc && IsPopupWindow(hwndFore, _hwndCurSrc))) {
			// 如果窗口处于某种中间状态则跳过此次检查
			if (!IsReadyForScaling(hwndFore)) {
				return;
			}

			// 自动缩放可以终止当前缩放
			_StartScale(hwndFore, *profile, profile->autoScale == AutoScale::Windowed, true);
		}
	}

	// 避免重复检查
	_hwndChecked = hwndFore;
}

void ScalingService::_ScalingRuntime_StateChanged(ScalingState value) {
	App::Get().Dispatcher().TryEnqueue([this, value]() {
		bool shouldRestartForSmoothMotion = false;

		if (value == ScalingState::Scaling) {
			StopTimer();
		} else if (value == ScalingState::Idle) {
			_FlushEffectParametersSaves();
			shouldRestartForSmoothMotion = !_isAutoScaleSuspended && _hwndCurSrc &&
				AppSettings::Get().IsSmoothMotionCompatibilityMode();

			// 缩放结束后源窗口位于前台则不要检查自动缩放，用户可能刚通过快捷键或
			// 工具栏终止缩放。_CheckForegroundTimer_Tick 也实现了类似功能，但它
			// 的触发频率较低，容易错过时机。
			if (GetForegroundWindow() == _hwndCurSrc) {
				_hwndChecked = _hwndCurSrc;
			}

			// 缩放结束后清空 _hwndCurSrc，等待状态下则保留
			_hwndCurSrc = NULL;
		}

		IsScalingChanged.Invoke(value == ScalingState::Scaling);

		if (shouldRestartForSmoothMotion) {
			Logger::Get().Info("Smooth Motion 兼容模式：缩放结束后重启 Magpie");
			AppSettings::Get().Save();
			App::Get().RestartForSmoothMotion();
		}
	});
}

void ScalingService::_ScaleForegroundWindow(bool windowedMode) {
	const HWND hWnd = GetForegroundWindow();
	if (!hWnd) {
		return;
	}

	const Profile& profile = *ProfileService::Get().GetProfileForWindow(hWnd, false);
	_StartScale(hWnd, profile, windowedMode, false);
}

void ScalingService::_StartScale(HWND hWnd, const Profile& profile, bool windowedMode, bool force) {
	assert(hWnd);

	const ScalingError error = _StartScaleImpl(hWnd, profile, windowedMode, force);
	if (error != ScalingError::NoError) {
		std::string context;
		const auto& modes = AppSettings::Get().ScalingModes();
		if (profile.scalingMode >= 0 && static_cast<size_t>(profile.scalingMode) < modes.size()) {
			const auto& effects = modes[profile.scalingMode].effects;
			for (size_t i = 0; i < effects.size(); ++i) {
				if ((error == ScalingError::ScalingModeUnknownEffect &&
					(effects[i].isRecoveryInvalid || !EffectsService::Get().GetEffect(effects[i].name))) ||
					(error == ScalingError::ConflictingFrameGenerationEffects &&
						ClassifyFrameGenerationEffect(effects[i].name) != FrameGenerationEffectKind::None)) {
					if (!context.empty()) context += '\n';
					context += fmt::format("#{} {}", i + 1, StrHelper::UTF16ToUTF8(effects[i].name));
				}
			}
		}
		ErrorService::Get().Report(error, std::move(context), hWnd, 0, MakeIssueContext(hWnd, profile));
	}
}

ScalingError ScalingService::_StartScaleImpl(HWND hWnd, const Profile& profile, bool windowedMode, bool force) {
	// ScalingRuntime::Start 会检查是否正在缩放，这里提前检查以避免无效操作
	if (!force && _scalingRuntime->State() == ScalingState::Scaling) {
		return ScalingError::NoError;
	}

	if (WindowHelper::IsForbiddenSystemWindow(hWnd)) {
		return ScalingError::NoError;
	}

	if (profile.scalingMode < 0 || static_cast<uint32_t>(profile.scalingMode) >=
		ScalingModesService::Get().GetScalingModeCount()) {
		return ScalingError::ScalingModeNotSelected;
	}

	const ScalingMode& scalingMode =
		ScalingModesService::Get().GetScalingMode(profile.scalingMode);
	const std::vector<EffectItem>& effects = scalingMode.effects;
	if (effects.empty()) {
		return ScalingError::ScalingModeEmpty;
	} else {
		for (const EffectItem& effect : effects) {
			// 被禁用的效果不参与渲染，即使文件缺失也不应阻止启动，
			// 这样用户可以靠禁用来绕过损坏的效果器
			if (!effect.enabled) {
				continue;
			}
			if (effect.isRecoveryInvalid || !EffectsService::Get().GetEffect(effect.name)) {
				// 存在无法解析的效果
				return ScalingError::ScalingModeUnknownEffect;
			}
		}
	}

	const FrameGenerationChainValidation frameGeneration =
		ValidateFrameGenerationChain(effects);
	if (frameGeneration.HasConflict()) {
		Logger::Get().Error(fmt::format(
			"Scaling mode '{}' contains {} frame-generation effects",
			StrHelper::UTF16ToUTF8(scalingMode.name), frameGeneration.count));
		return ScalingError::ConflictingFrameGenerationEffects;
	}

	if (profile.Is3DGameMode() && windowedMode) {
		return ScalingError::Windowed3DGameMode;
	}
	if (windowedMode && profile.captureMethod == CaptureMethod::DesktopDuplication) {
		return ScalingError::WindowedDesktopDuplication;
	}

	ScalingOptions options;
	options.scalingModeIdx = static_cast<uint32_t>(profile.scalingMode);
	options.scalingModeName = scalingMode.name;

	options.effects.reserve(effects.size());
	for (const EffectItem& effectItem : effects) {
		// 被禁用的效果保留在配置里，但不进入渲染链
		if (!effectItem.enabled) {
			continue;
		}
		options.effects.push_back((EffectOption)effectItem);
	}
	if (options.effects.empty()) {
		// 全部效果都被禁用，等价于效果组为空
		return ScalingError::ScalingModeEmpty;
	}

	// 尝试启用触控支持
	bool isTouchSupportEnabled;
	if (!TouchHelper::TryLaunchTouchHelper(isTouchSupportEnabled)) {
		Logger::Get().Error("TryLaunchTouchHelper 失败");
		return ScalingError::TouchSupport;
	}

	options.graphicsCardId = profile.graphicsCardId;
	options.captureMethod = profile.captureMethod;
	if (profile.isFrameRateLimiterEnabled) {
		options.maxFrameRate = profile.maxFrameRate;
	}
	options.multiMonitorUsage = profile.multiMonitorUsage;
	options.preferredMonitorId = profile.preferredMonitorId;
	options.destAlignment = profile.destAlignment;
	options.cursorInterpolationMode = profile.cursorInterpolationMode;
	options.flags = profile.scalingFlags;

	options.IsWindowedMode(windowedMode);
	options.IsTouchSupportEnabled(isTouchSupportEnabled);

	switch (profile.initialWindowedScaleFactor) {
	case InitialWindowedScaleFactor::Auto:
		options.initialWindowedScaleFactor = 0.0f;
		break;
	case InitialWindowedScaleFactor::x1:
		options.initialWindowedScaleFactor = 1.0f;
		break;
	case InitialWindowedScaleFactor::x1_25:
		options.initialWindowedScaleFactor = 1.25f;
		break;
	case InitialWindowedScaleFactor::x1_5:
		options.initialWindowedScaleFactor = 1.5f;
		break;
	case InitialWindowedScaleFactor::x1_75:
		options.initialWindowedScaleFactor = 1.75f;
		break;
	case InitialWindowedScaleFactor::x2:
		options.initialWindowedScaleFactor = 2.0f;
		break;
	case InitialWindowedScaleFactor::x3:
		options.initialWindowedScaleFactor = 3.0f;
		break;
	case InitialWindowedScaleFactor::Custom:
		options.initialWindowedScaleFactor = profile.customInitialWindowedScaleFactor;
		break;
	default:
		options.initialWindowedScaleFactor = 0.0f;
		break;
	}

	if (profile.isCroppingEnabled) {
		options.cropping = profile.cropping;
	}

	switch (profile.cursorScaling) {
	case CursorScaling::x0_5:
		options.cursorScaling = 0.5f;
		break;
	case CursorScaling::x0_75:
		options.cursorScaling = 0.75f;
		break;
	case CursorScaling::NoScaling:
		options.cursorScaling = 1.0f;
		break;
	case CursorScaling::x1_25:
		options.cursorScaling = 1.25f;
		break;
	case CursorScaling::x1_5:
		options.cursorScaling = 1.5f;
		break;
	case CursorScaling::x2:
		options.cursorScaling = 2.0f;
		break;
	case CursorScaling::Source:
		// 0 或负值表示和源窗口缩放比例相同
		options.cursorScaling = 0.0f;
		break;
	case CursorScaling::Custom:
		options.cursorScaling = profile.customCursorScaling;
		break;
	default:
		options.cursorScaling = 1.0f;
		break;
	}

	if (profile.isAutoHideCursorEnabled) {
		options.autoHideCursorDelay = profile.autoHideCursorDelay;
	}

	options.isParameterFocusSwitchingEnabled = profile.isParameterFocusSwitchingEnabled;

	// 应用全局配置
	AppSettings& settings = AppSettings::Get();
	options.toolbarShortcutLabels = GetToolbarShortcutLabels();
	options.IsDeveloperMode(settings.IsDeveloperMode());
	options.IsDebugMode(settings.IsDebugMode());
	options.IsBenchmarkMode(settings.IsBenchmarkMode());
	options.IsTopmostDisabled(settings.IsTopmostDisabled());
	options.IsEffectCacheDisabled(settings.IsEffectCacheDisabled());
	options.IsFontCacheDisabled(settings.IsFontCacheDisabled());
	options.IsSaveEffectSources(settings.IsSaveEffectSources());
	options.IsWarningsAreErrors(settings.IsWarningsAreErrors());
	options.IsAllowScalingMaximized(settings.IsAllowScalingMaximized());
	options.IsSimulateExclusiveFullscreen(settings.IsSimulateExclusiveFullscreen());
	options.duplicateFrameDetectionMode = settings.DuplicateFrameDetectionMode();
	options.IsStatisticsForDynamicDetectionEnabled(settings.IsStatisticsForDynamicDetectionEnabled());
	options.IsInlineParams(settings.IsInlineParams());
	options.IsFP16Disabled(settings.IsFP16Disabled());
	options.isFrontEdgeSyncEnabled = settings.IsFrontEdgeSyncEnabled();
	// VRR is deferred while its settings card is hidden. Ignore an older
	// saved true value so no session silently enables tearing.
	options.isVRREnabled = false;
	options.frontEdgeSyncFrameRate = settings.FrontEdgeSyncFrameRate();
	options.frameSyncMode = settings.GetFrameSyncMode();

	if (options.maxFrameRate) {
		// 最小帧数不能大于最大帧数
		options.minFrameRate = std::min(settings.MinFrameRate(), *options.maxFrameRate);
	} else {
		options.minFrameRate = settings.MinFrameRate();
	}

	options.fullscreenInitialToolbarState = settings.FullscreenInitialToolbarState();
	options.windowedInitialToolbarState = settings.WindowedInitialToolbarState();
	options.screenshotsDir = settings.ScreenshotsDir();
	if (options.screenshotsDir.empty()) {
		// 回落到使用当前目录
		options.screenshotsDir = L".";
	}

	options.overlayOptions = settings.OverlayOptions();

	options.showToast = [](HWND hwndTarget, std::wstring_view msg) noexcept {
		ToastService::Get().ShowMessageOnWindow({}, msg, hwndTarget);
	};

	const IssueContext issueContext = MakeIssueContext(hWnd, profile);
	options.showError = [issueContext](HWND target, ScalingError error) noexcept {
		ErrorService::Get().Report(error, {}, target, 0, issueContext);
	};
	options.reportErrorDetails = [issueContext](HWND target, ScalingError error, std::string_view context, uint32_t systemError) noexcept {
		ErrorService::Get().Report(error, std::string(context), target, systemError, issueContext);
	};

	options.save = [](const ScalingOptions& options, HWND /*hwndScaling*/) noexcept {
		App::Get().Dispatcher().TryEnqueue(
			[overlayOptions(options.overlayOptions)]() {
				AppSettings::Get().OverlayOptions() = std::move(overlayOptions);
				AppSettings::Get().SaveAsync();
			}
		);
	};
	options.revertEffectParameter = [modeIdx = options.scalingModeIdx, modeName = options.scalingModeName](
		uint32_t effectIdx, const EffectOption& previousEffect, const std::string& parameter, float rejected, float previous) {
		App::Get().Dispatcher().TryEnqueue([modeIdx, modeName, effectIdx, previousEffect, parameter, rejected, previous] {
			auto& modes = AppSettings::Get().ScalingModes();
			if (modeIdx >= modes.size() || modes[modeIdx].name != modeName || effectIdx >= modes[modeIdx].effects.size()) return;
			auto& effect = modes[modeIdx].effects[effectIdx];
			if (StrHelper::UTF16ToUTF8(effect.name) != previousEffect.name ||
				effect.scale != previousEffect.scale || effect.scalingType != previousEffect.scalingType) return;
			// A later settings/toolbar edit wins over this older failed request.
			if (!RestoreRejectedEffectParameter(effect.parameters, StrHelper::UTF8ToUTF16(parameter), rejected, previous)) return;
			ScalingModesService::Get().EffectParametersChanged.Invoke(modeIdx, effectIdx);
			AppSettings::Get().SaveAsync();
		});
	};

	options.requestEffectParameters = [](
		const ScalingOptions& sessionOptions,
		EffectParametersRequest&& request
	) noexcept -> bool {
		// Do not discard edits while a previous request is queued. Each request
		// owns its snapshot; disk writes are coalesced on the UI thread.
		try {
			return App::Get().Dispatcher().TryEnqueue([
				sessionOptions = ScalingOptions(sessionOptions), request = std::move(request)
			]() mutable {
				const auto state = request.saveState;
				const uint64_t revision = request.revision;
				try {
					ScalingService::Get()._HandleEffectParametersRequest(
						std::move(sessionOptions), std::move(request));
				} catch (...) {
					Logger::Get().Error("Effect parameter request failed with an exception");
					state->Complete(revision, EffectParametersSaveError::WriteFailed);
				}
			});
		} catch (...) {
			Logger::Get().Error("Unable to enqueue effect parameter changes");
			return false;
		}
	};

	if (!_scalingRuntime->Start(hWnd, std::move(options), force)) {
		return ScalingError::ScalingFailedGeneral;
	}

	_hwndCurSrc = hWnd;
	if (!force) _isAutoScaleSuspended = false;
	return ScalingError::NoError;
}

void ScalingService::_ScheduleEffectParametersSave(const EffectParametersRequest& request) {
	if (!_effectParametersSaveTimer) {
		_effectParametersSaveTimer = App::Get().Dispatcher().CreateTimer();
		_effectParametersSaveTimer.IsRepeating(false);
		_effectParametersSaveTimer.Tick([this](auto const&, auto const&) {
			_FlushEffectParametersSaves();
		});
	}
	const auto now = std::chrono::steady_clock::now();
	if (_pendingEffectParametersSaves.empty()) _firstEffectParametersEdit = now;
	auto existing = std::ranges::find(_pendingEffectParametersSaves,
		request.saveState, &PendingEffectParametersSave::state);
	if (existing == _pendingEffectParametersSaves.end()) {
		_pendingEffectParametersSaves.push_back({ request.saveState, request.revision });
	} else {
		existing->revision = request.revision;
	}
	// Save after 300 ms of inactivity, and at least once per second during a drag.
	_effectParametersSaveTimer.Stop();
	if (now - _firstEffectParametersEdit >= std::chrono::seconds(1)) {
		_FlushEffectParametersSaves();
	} else {
		_effectParametersSaveTimer.Interval(std::chrono::milliseconds(300));
		_effectParametersSaveTimer.Start();
	}
}

void ScalingService::EffectParameterEdited(uint32_t modeIdx, uint32_t effectIdx,
	const std::string& parameter, float value) {
	if (!_scalingRuntime || (!IsScaling() && _scalingRuntime->State() != ScalingState::Starting)) return;
	const auto& modes = AppSettings::Get().ScalingModes();
	if (modeIdx >= modes.size() || effectIdx >= modes[modeIdx].effects.size()) return;
	_scalingRuntime->UpdateEffectParameterFromSettings(modeIdx, modes[modeIdx].name,
		effectIdx, static_cast<EffectOption>(modes[modeIdx].effects[effectIdx]), parameter, value);
}

void ScalingService::RetryConfigurationSave(std::function<void(bool)> completed) {
	// Capture only failed revisions. A newer edit/conflict must keep its own result.
	std::vector<std::pair<std::shared_ptr<EffectParametersSaveState>, uint64_t>> failed;
	for (const auto& weak : _parameterSaveStates) {
		if (auto state = weak.lock()) {
			const auto result = state->result.load(std::memory_order_acquire);
			if ((result & 7) == static_cast<uint64_t>(EffectParametersSaveError::WriteFailed))
				failed.emplace_back(std::move(state), result);
		}
	}
	AppSettings::Get().SaveAsync([failed = std::move(failed), completed = std::move(completed)](bool succeeded) {
		if (succeeded) {
			for (const auto& [state, result] : failed) {
				auto expected = result;
				state->result.compare_exchange_strong(expected, result & ~uint64_t(7), std::memory_order_acq_rel);
			}
		}
		if (completed) completed(succeeded);
	});
}

void ScalingService::_FlushEffectParametersSaves(bool synchronous) {
	if (_effectParametersSaveTimer) _effectParametersSaveTimer.Stop();
	if (_pendingEffectParametersSaves.empty()) return;
	auto pending = std::exchange(_pendingEffectParametersSaves, {});
	auto complete = [pending = std::move(pending)](bool succeeded) noexcept {
		for (const auto& item : pending) {
			item.state->Complete(item.revision, succeeded ?
				EffectParametersSaveError::None : EffectParametersSaveError::WriteFailed);
		}
	};
	if (synchronous) {
		complete(AppSettings::Get().Save());
	} else {
		AppSettings::Get().SaveAsync(std::move(complete));
	}
}

void ScalingService::_HandleEffectParametersRequest(
	ScalingOptions&& sessionOptions,
	EffectParametersRequest&& request
) {
	std::erase_if(_parameterSaveStates, [](const auto& state) { return state.expired(); });
	if (std::ranges::none_of(_parameterSaveStates, [&](const auto& state) { return state.lock() == request.saveState; }))
		_parameterSaveStates.push_back(request.saveState);
	auto fail = [&](EffectParametersSaveError error) noexcept {
		request.saveState->Complete(request.revision, error);
		if (error == EffectParametersSaveError::Conflict) {
			ErrorService::Get().Report(ScalingError::EffectParameterConflict,
				StrHelper::UTF16ToUTF8(sessionOptions.scalingModeName), request.hwndScaling);
		}
	};
	const bool restart = request.kind == EffectParametersRequestKind::SaveAndRestart;
	if ((request.saveState->result.load(std::memory_order_acquire) & 7) ==
		static_cast<uint64_t>(EffectParametersSaveError::Conflict)) {
		fail(EffectParametersSaveError::Conflict);
		return;
	}
	// Persist already submitted edits even if scaling has just stopped. Only
	// restarting needs live HWNDs and the same scaling run.
	if (restart) {
		if (!_scalingRuntime || _scalingRuntime->State() != ScalingState::Scaling ||
			request.scalingRunId != _scalingRuntime->RunId()) {
			fail(EffectParametersSaveError::SessionExpired);
			return;
		}
		if (!IsWindow(request.hwndSource) || !IsWindow(request.hwndScaling) ||
			request.hwndSource != _hwndCurSrc) {
			fail(EffectParametersSaveError::SourceUnavailable);
			return;
		}
	}

	auto& settings = AppSettings::Get();
	FrameSyncSettings mergedFrameSync{ settings.IsFrontEdgeSyncEnabled(), settings.FrontEdgeSyncFrameRate(), settings.GetFrameSyncMode() };
	if (!MergeFrameSyncSettings(mergedFrameSync, request.previousFrameSync, request.frameSync)) {
		fail(EffectParametersSaveError::Conflict);
		return;
	}
	std::vector<ScalingMode>& modes = settings.ScalingModes();
	if (sessionOptions.scalingModeIdx >= modes.size()) {
		fail(EffectParametersSaveError::Conflict);
		return;
	}
	ScalingMode& mode = modes[sessionOptions.scalingModeIdx];
	if (mode.name != sessionOptions.scalingModeName ||
		mode.effects.size() != sessionOptions.effects.size() ||
		request.effects.size() != mode.effects.size() ||
		request.previousEffects.size() != mode.effects.size()) {
		fail(EffectParametersSaveError::Conflict);
		return;
	}

	auto merged = mode.effects;
	for (size_t i = 0; i < merged.size(); ++i) {
		EffectItem& destination = merged[i];
		const EffectOption& session = sessionOptions.effects[i];
		if (StrHelper::UTF16ToUTF8(destination.name) != session.name ||
			request.effects[i].name != session.name ||
			request.previousEffects[i].name != session.name ||
			destination.scalingType != session.scalingType || destination.scale != session.scale) {
			fail(EffectParametersSaveError::Conflict);
			return;
		}
		phmap::flat_hash_map<std::wstring, float> before, after;
		for (const auto& [name, value] : request.previousEffects[i].parameters) {
			before[StrHelper::UTF8ToUTF16(name)] = value;
		}
		for (const auto& [name, value] : request.effects[i].parameters) {
			after[StrHelper::UTF8ToUTF16(name)] = value;
		}
		if (!MergeEffectParameterChanges(destination.parameters, before, after)) {
			fail(EffectParametersSaveError::Conflict);
			return;
		}
	}
	mode.effects = std::move(merged);
	settings.IsFrontEdgeSyncEnabled(mergedFrameSync.enabled);
	settings.FrontEdgeSyncFrameRate(mergedFrameSync.frameRate);
	settings.SetFrameSyncMode(mergedFrameSync.mode);
	if (sessionOptions.parameterSession) sessionOptions.parameterSession->DesiredFrameSync(mergedFrameSync);
	for (uint32_t i = 0; i < mode.effects.size(); ++i) {
		ScalingModesService::Get().EffectParametersChanged.Invoke(sessionOptions.scalingModeIdx, i);
	}
	if (!restart) {
		_ScheduleEffectParametersSave(request);
		return;
	}

	// Keep the edited settings on failure so the next edit can retry. Never
	// announce success through a throwing ResourceLoader/Toast callback.
	_FlushEffectParametersSaves(true);
	if (!AppSettings::Get().Save()) {
		fail(EffectParametersSaveError::WriteFailed);
		return;
	}
	request.saveState->Complete(request.revision, EffectParametersSaveError::None);
	std::vector<EffectOption> effects;
	for (const EffectItem& item : mode.effects) effects.push_back(static_cast<EffectOption>(item));
	if (!_scalingRuntime->RestartWithEffectParameters(request.hwndSource,
		request.hwndScaling, request.scalingRunId, std::move(effects), mergedFrameSync)) {
		fail(EffectParametersSaveError::SessionExpired);
	}
}

}
