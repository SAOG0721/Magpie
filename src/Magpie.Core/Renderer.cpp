#include "pch.h"
#include "FrameTrace.h"
#include "FramePacingOptions.h"
#include "FramePacingWait.h"
#include "CommonSharedConstants.h"
#include "CursorManager.h"
#include "DesktopDuplicationFrameSource.h"
#include "DeviceResources.h"
#include "DirectXHelper.h"
#include "DwmSharedSurfaceFrameSource.h"
#include "EffectCompiler.h"
#include "EffectHelper.h"
#include "EffectDrawer.h"
#include "GroupAHdrRoutes.h"
#include "GroupBHdrRoutes.h"
#include "EffectProtocolCatalogC.h"
#include "EffectParameterValue.h"
#include "EffectParameterRestart.h"
#include "EffectsProfiler.h"
#include "GDIFrameSource.h"
#include "GraphicsCaptureFrameSource.h"
#include "Logger.h"
#include "OverlayDrawer.h"
#include "Renderer.h"
#include "NgxRuntimeGuard.h"
#include "ScalingOptions.h"
#include "ScalingWindow.h"
#include "ScreenshotHelper.h"
#include "StrHelper.h"
#include "TextureHelper.h"
#include "Win32Helper.h"
#include "DLSSFrameGenerator.h"
#include "DLSSSRUpscaler.h"
#include "FSR2Upscaler.h"
#include "FSR3Upscaler.h"
#include "NativeEffectBackend.h"
#include "NativeEffectBackendFactory.h"
#include "NvidiaOpticalFlowProvider.h"
#include "AmdOpticalFlowProvider.h"
#include "XeSSFGPresenter.h"
#ifdef MP_USE_COMPSWAPCHAIN
#include "CompSwapchainPresenter.h"
#else
#include "AdaptivePresenter.h"
#endif
#include <dispatcherqueue.h>
#include <d3dkmthk.h>

namespace Magpie {

// AcquireSync returns WAIT_TIMEOUT / WAIT_ABANDONED as nonnegative values.
// Treat only S_OK as ownership; FAILED(hr) alone is not a valid test here.
static HRESULT AcquirePresentationTextures(
	const std::array<IDXGIKeyedMutex*, 3>& mutexes, uint64_t key, DWORD timeout,
	size_t* failedIndex = nullptr) noexcept {
	for (size_t i = 0; i < mutexes.size(); ++i) {
		if (!mutexes[i]) continue;
		const HRESULT hr = mutexes[i]->AcquireSync(key, timeout);
		if (hr != S_OK) {
			if (failedIndex) *failedIndex = i;
			for (size_t j = 0; j < i; ++j) if (mutexes[j]) mutexes[j]->ReleaseSync(key);
			return FAILED(hr) ? hr : HRESULT_FROM_WIN32(static_cast<DWORD>(hr));
		}
	}
	return S_OK;
}

static HRESULT ReleasePresentationTextures(
	const std::array<IDXGIKeyedMutex*, 3>& mutexes, uint64_t key) noexcept {
	HRESULT result = S_OK;
	for (auto mutex : mutexes) {
		if (!mutex) continue;
		const HRESULT hr = mutex->ReleaseSync(key);
		if (FAILED(hr) && SUCCEEDED(result)) result = hr;
	}
	return result;
}

static FrameGuidanceRequirements CollectFrameGuidanceRequirements(
	const std::vector<std::unique_ptr<NativeEffectBackend>>& backends,
	const DLSSFrameGenerator* frameGenerator,
	MotionVectorRequest xessRequest = {}
) noexcept {
	FrameGuidanceRequirements result;
	for (const auto& backend : backends) {
		if (backend) result.Merge(backend->GetFrameGuidanceRequirements());
	}
	if (frameGenerator) {
		result.Merge(frameGenerator->GetFrameGuidanceRequirements());
	}
	if (xessRequest.method != OpticalFlowMethod::None) {
		result.zero = true;
		result.Add(xessRequest);
	}
	return result.Resolved();
}

// 大多数时候会在最后添加 Bicubic 来降采样或升采样，因此缓存在内存中
static EffectDesc bicubicDesc;

static bool IsDLSSFrameGenerationEffect(std::string_view name) noexcept {
	return ClassifyFrameGenerationEffect(name) ==
		FrameGenerationEffectKind::DLSS;
}

static bool IsXeSSFrameGenerationEffect(std::string_view name) noexcept {
	const FrameGenerationEffectKind kind =
		ClassifyFrameGenerationEffect(name);
	return kind == FrameGenerationEffectKind::XeSSX2 ||
		kind == FrameGenerationEffectKind::XeSSMultiFrame;
}

static bool IsFrameGenerationEffect(std::string_view name) noexcept {
	return IsDLSSFrameGenerationEffect(name) || IsXeSSFrameGenerationEffect(name);
}

static HdrFormatRoutes GetHdrRoutesForEffect(
	const EffectOption& effect,
	bool hdrEnabled
) noexcept {
	// Route selection is a HDR-only contract. Keep this guard local so a
	// caller cannot accidentally turn saved HDR parameters into a live route
	// while the profile option is disabled.
	if (!hdrEnabled) {
		return {};
	}
	const size_t separator = effect.name.find('\\');
	const std::string_view group = separator == std::string::npos
		? std::string_view(effect.name)
		: std::string_view(effect.name).substr(0, separator);
	int casFormatOption = 0;
	if (group == "Anime4K" || group == "CAS" || group == "CRT" ||
		group == "CuNNy" || group == "CuNNy2" || group == "Diagnostics" ||
		group == "FSRCNNX" || group == "FXAA" || group == "MLAA") {
		if (const auto it = effect.parameters.find("hdrFormat"); it != effect.parameters.end()) {
			casFormatOption = static_cast<int>(std::lround(it->second));
		}
		return GetGroupAHdrRoutes(group, casFormatOption);
	}
	if (group == "DLSSNR") {
		const auto path = effect.parameters.find("experimentalHdrPath");
		return GetGroupBHdrRoutes(group,
			path != effect.parameters.end() && path->second >= 0.5f);
	}
	if (group == "DLSS" || group == "FSR" || group == "FSR2" ||
		group == "FSR3" || group == "FSR4" || group == "NIS") {
		return GetGroupBHdrRoutes(group);
	}
	if (group == "RTXVideo") {
		return effect.name.find("_VSR_") != std::string::npos
			? EffectProtocolC::RTXVideoVsr()
			: EffectProtocolC::RTXVideoDenoiser();
	}
	if (group == "DLSSFG" || group == "FSR3FG") {
		return EffectProtocolC::GetGroupCHdrRoutes(group);
	}
	return EffectProtocolC::GetGroupCHdrRoutes(group);
}

static void ConfigureHdrBackendProtocol(
	std::string_view effectName,
	NativeEffectBackend& backend,
	const HdrFrameMetadata& metadata,
	bool hdrEnabled
) noexcept {
	if (!hdrEnabled || !metadata.IsValid()) return;
	// Capture normalization has already applied source pre-exposure into the
	// canonical FP16 surface. Native SR backends therefore consume linear HDR
	// values with a neutral exposure contract for this frame.
	const FsrHdrProtocol protocol{
		.hdrColorInput = true,
		.transfer = GroupBTransfer::Linear,
		.preExposure = 1.0f,
		.exposure = 1.0f,
		.depthInverted = true,
		.depthInfinite = true,
		.useReactiveMask = true,
		.useTransparencyMask = true,
	};
	if (effectName == "DLSS\\DLSS_SR") {
		static_cast<DLSSSRUpscaler&>(backend).SetDlssHdrProtocol(protocol);
	} else if (effectName == "FSR2\\FSR2_SR") {
		static_cast<FSR2Upscaler&>(backend).SetFsrHdrProtocol(protocol);
	} else if (effectName == "FSR3\\FSR3_SR" || effectName == "FSR4\\FSR4_SR") {
		static_cast<FSR3Upscaler&>(backend).SetFsrHdrProtocol(protocol);
	}
}

static MotionVectorRequest GetMotionVectorRequest(
	const FrameGuidanceRequirements& requirements
) noexcept {
	return requirements.FirstMotion();
}

static int ReadIntegralEffectParameter(
	const EffectOption& effect,
	std::string_view parameterName,
	int minimum,
	int maximum,
	int fallback
) noexcept {
	const auto it = effect.parameters.find(std::string(parameterName));
	if (it == effect.parameters.end()) {
		return fallback;
	}

	const float rawValue = it->second;
	if (std::isfinite(rawValue)) {
		const float rounded = std::round(rawValue);
		if (rawValue == rounded && rounded >= minimum && rounded <= maximum) {
			return static_cast<int>(rounded);
		}
	}

	Logger::Get().Warn(fmt::format(
		"Invalid effect parameter: effect='{}', parameter='{}', value={}, "
		"fallback={}",
		effect.name, parameterName, rawValue, fallback));
	return fallback;
}

static double GetDisplayRefreshRate(HWND window) noexcept {
	MONITORINFOEXW monitorInfo{};
	monitorInfo.cbSize = sizeof(monitorInfo);
	const HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
	if (!monitor || !GetMonitorInfoW(monitor, &monitorInfo)) return 0.0;
	DEVMODEW mode{};
	mode.dmSize = sizeof(mode);
	if (!EnumDisplaySettingsW(
		monitorInfo.szDevice, ENUM_CURRENT_SETTINGS, &mode) ||
		mode.dmDisplayFrequency <= 1) return 0.0;
	return static_cast<double>(mode.dmDisplayFrequency);
}

Renderer::Renderer() noexcept :
	_sessionLifetime(std::make_shared<ScalingSessionLifetime>(ScalingWindow::RunId())) {}

void Renderer::BeginShutdown() noexcept {
	_sessionLifetime->RequestStop();
	// The backend can be waiting for a synchronous DLSSFG presentation while
	// the frontend thread is destroying this Renderer. Stop issuing new
	// synchronous sends before waiting for the backend thread to exit.
	_synchronousFramePresentationEnabled.store(false, std::memory_order_release);
}

Renderer::~Renderer() noexcept {
	BeginShutdown();

	_hKeyboardHook.reset();

	if (_backendThread.joinable()) {
		const HANDLE hThread = _backendThread.native_handle();

		if (!wil::handle_wait(hThread, 0)) {
			const DWORD threadId = GetThreadId(_backendThread.native_handle());

			// 持续尝试直到 _backendThread 创建了消息队列
			while (!PostThreadMessage(threadId, WM_QUIT, 0, 0)) {
				if (wil::handle_wait(hThread, 1)) {
					break;
				}
			}
		}

		_backendThread.join();
	}
	_ReleaseNgxConsumers();
	if (_frameTraceStarted) FrameTrace::Stop();
}

static DXGI_ADAPTER_DESC1 LogAdapter(IDXGIAdapter4* adapter) noexcept {
	DXGI_ADAPTER_DESC1 desc;
	if (FAILED(adapter->GetDesc1(&desc))) {
		desc = {};
	}

	Logger::Get().Info(fmt::format("当前图形适配器: \n\tVendorId: {:#x}\n\tDeviceId: {:#x}\n\tDescription: {}",
		desc.VendorId, desc.DeviceId, StrHelper::UTF16ToUTF8(desc.Description)));
	return desc;
}

static void SetGpuPriority() noexcept {
	// 来自 https://github.com/obsproject/obs-studio/blob/16cb051a57bb357fe866252c1360ce2c38e2deec/libobs-d3d11/d3d11-subsystem.cpp#L429
	// 使用 REALTIME 避免与游戏同时运行时 Magpie 的 GPU 工作被持续抢占。
	// 这里只更改 GPU 调度类，不会更改 Windows 的 CPU 进程优先级。
	NTSTATUS status = D3DKMTSetProcessSchedulingPriorityClass(
		GetCurrentProcess(), D3DKMT_SCHEDULINGPRIORITYCLASS_REALTIME);
	if (status != STATUS_SUCCESS) {
		Logger::Get().NTError("D3DKMTSetProcessSchedulingPriorityClass 失败", status);
	}
}

ScalingError Renderer::Initialize(HWND hwndAttach, OverlayOptions& overlayOptions) noexcept {
	_frameTraceStarted = FrameTrace::Start();
	_xessMotionRequest = {};
	_isXeSSFrameGenerationActive = false;
	_xessFrameGenerationKind = FrameGenerationEffectKind::None;

	const std::vector<EffectOption>& effects =
		ScalingWindow::Get().Options().effects;
	const FrameGenerationChainValidation frameGeneration =
		ValidateFrameGenerationChain(effects);
	if (frameGeneration.HasConflict()) {
		Logger::Get().Error(fmt::format(
			"Renderer rejected {} frame-generation effects in one chain",
			frameGeneration.count));
		return ScalingError::ConflictingFrameGenerationEffects;
	}

	_hasFrameGeneration = frameGeneration.HasFrameGeneration();
	_frontEdgeUsesSharedSlot = frameGeneration.first != FrameGenerationEffectKind::DLSS;
	std::optional<XeSSFGVariant> xessVariant;
	uint32_t xessFrameGenerationMultiplier = 2;
	for (const EffectOption& effect : effects) {
		const FrameGenerationEffectKind kind =
			ClassifyFrameGenerationEffect(effect.name);
		if (kind != FrameGenerationEffectKind::None) {
			_configuredFrameGenerationMultiplier = kind == FrameGenerationEffectKind::XeSSX2 ? 2u :
				static_cast<uint32_t>(ReadIntegralEffectParameter(effect, "multiplier", 2, 4,
					kind == FrameGenerationEffectKind::XeSSMultiFrame ? 3 : 2));
		}
		if (kind != FrameGenerationEffectKind::XeSSX2 &&
			kind != FrameGenerationEffectKind::XeSSMultiFrame) {
			continue;
		}

		xessVariant = kind == FrameGenerationEffectKind::XeSSX2 ?
			XeSSFGVariant::X2 : XeSSFGVariant::MultiFrame;
		_xessFrameGenerationKind = kind;
		xessFrameGenerationMultiplier = *xessVariant == XeSSFGVariant::X2 ? 2u :
			static_cast<uint32_t>(ReadIntegralEffectParameter(
				effect, "multiplier", 2, 4, 3));
		const int method = ReadIntegralEffectParameter(
			effect, "opticalFlowMethod", 0,
			*xessVariant == XeSSFGVariant::X2 ? 2 : 1, 0);
		if (method == static_cast<int>(OpticalFlowMethod::Amd)) {
			const int quality = ReadIntegralEffectParameter(
				effect, "amdOpticalFlowMode", 0, 1, 1);
			_xessMotionRequest = MotionVectorRequest::Amd(
				static_cast<AmdOpticalFlowMode>(quality));
		} else if (*xessVariant == XeSSFGVariant::X2 &&
			method == static_cast<int>(OpticalFlowMethod::Nvidia)) {
			const int quality = ReadIntegralEffectParameter(
				effect, "nvidiaOpticalFlowQuality", 1, NVIDIA_OPTICAL_FLOW_MAX_QUALITY, 2);
			_xessMotionRequest = MotionVectorRequest::Nvidia(
				static_cast<NvidiaOpticalFlowQuality>(quality));
		}
	}
	_isXeSSFrameGenerationActive = xessVariant.has_value();

	if (!_frontendResources.Initialize(true)) {
		Logger::Get().Error("初始化前端资源失败");
		return ScalingError::GraphicsDeviceInitFailed;
	}

	const DXGI_ADAPTER_DESC1 adapterDesc =
		LogAdapter(_frontendResources.GetGraphicsAdapter());

	// 每次创建 D3D 设备后尝试提高 GPU 优先级，OBS 也是这么做的
	SetGpuPriority();
	_UpdateOverlayRefreshRate();

	if (xessVariant) {
		Logger::Get().Info(fmt::format(
			"XeSSFG request: variant={}, requestedMultiplier={}x, "
			"adapterVendor={:#x}, adapterDevice={:#x}, opticalFlowMethod={}, "
			"opticalFlowQuality={}",
			*xessVariant == XeSSFGVariant::X2 ? "x2" : "MFG",
			xessFrameGenerationMultiplier, adapterDesc.VendorId,
			adapterDesc.DeviceId, static_cast<uint32_t>(_xessMotionRequest.method),
			static_cast<uint32_t>(_xessMotionRequest.quality)));
		if (xessFrameGenerationMultiplier > 2 &&
			adapterDesc.VendorId != 0x8086) {
			Logger::Get().Error(
				"XeSS Multi-Frame Generation x3/x4 requires an Intel adapter");
			return ScalingError::XeSSMfgRequiresIntel;
		}

		auto xessPresenter = std::make_unique<XeSSFGPresenter>(
			*xessVariant,
			xessFrameGenerationMultiplier,
			_xessMotionRequest.method != OpticalFlowMethod::None);
		if (!xessPresenter->Initialize(hwndAttach, _frontendResources)) {
			Logger::Get().Error("初始化 XeSSFGPresenter 失败");
			return xessPresenter->InitializationError();
		}
		_presenter = std::move(xessPresenter);
	} else {
#ifdef MP_USE_COMPSWAPCHAIN
	_presenter = std::make_unique<CompSwapchainPresenter>();
	if (!_presenter->Initialize(hwndAttach, _frontendResources)) {
		Logger::Get().Error("初始化 CompSwapchainPresenter 失败");
#else
	_presenter = std::make_unique<AdaptivePresenter>();
	if (!_presenter->Initialize(hwndAttach, _frontendResources)) {
		Logger::Get().Error("初始化 AdaptivePresenter 失败");
#endif
		return ScalingError::PresentationInitFailed;
	}
	}

	const auto& pacingOptions = ScalingWindow::Get().Options();
	_frontEdgeSyncEnabled = pacingOptions.isFrontEdgeSyncEnabled && !pacingOptions.IsBenchmarkMode();
#ifdef MP_USE_COMPSWAPCHAIN
	if (!_hasFrameGeneration) _frontEdgeSyncEnabled = false;
#else
	if (!_hasFrameGeneration && pacingOptions.IsDirectFlipDisabled()) _frontEdgeSyncEnabled = false;
#endif
	if (_frontEdgeSyncEnabled) {
		_frontEdgeConsumedEvent.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
		if (!_frontEdgeConsumedEvent) {
			Logger::Get().Win32Error("Create frame pacing event failed");
			return ScalingError::PresentationInitFailed;
		}
	}
	Logger::Get().Info(fmt::format("Front Edge Sync: requested={} active={} targetBaseFPS={} mode={}",
		pacingOptions.isFrontEdgeSyncEnabled, _frontEdgeSyncEnabled, pacingOptions.frontEdgeSyncFrameRate,
		_isXeSSFrameGenerationActive ? "XeLL input" : (_hasFrameGeneration ? "DLSSFG input" : "Present boundary")));
	_backendThread = std::thread(&Renderer::_BackendThreadProc, this);

	// 等待后端初始化完成
	_sharedTextureHandle.wait(NULL, std::memory_order_relaxed);
	const HANDLE sharedTextureHandle = _sharedTextureHandle.load(std::memory_order_acquire);
	if (sharedTextureHandle == INVALID_HANDLE_VALUE) {
		Logger::Get().Error("后端初始化失败");
		if (NgxRuntimeGuard::IsFaulted()) return ScalingError::NgxRestartRequired;
		// 一般的错误不会设置 _backendInitError
		return _backendInitError == ScalingError::NoError ? ScalingError::ScalingFailedGeneral : _backendInitError;
	}

	// The backend publishes this immutable configuration with the initialization handle.
	if (_motionConsumers.size() > 1 && std::ranges::any_of(_motionConsumers, [&](const auto& consumer) {
		return consumer.second != _motionConsumers.front().second;
	})) {
		const auto& window = ScalingWindow::Get();
		FrameGuidanceRequirements requested;
		std::wstring names;
		for (const auto& [name, request] : _motionConsumers) {
			requested.Add(request);
			if (!names.empty()) names += L", ";
			names += StrHelper::UTF8ToUTF16(name);
		}
		const auto selected = requested.PreferredMotion();
		std::wstring method;
		{
			const bool nvidia = selected.method == OpticalFlowMethod::Nvidia;
			const auto qualityKey = nvidia ? fmt::format(L"Motion_Nvidia_{}", selected.quality) :
				fmt::format(L"Motion_Amd_{}", selected.quality);
			method = fmt::format(L"{} / {}", nvidia ? L"NVOF" : L"AMD OF",
				std::wstring_view(window.GetLocalizedString(qualityKey)));
		}
		_motionConfigurationNotice = fmt::format(
			fmt::runtime(std::wstring_view(window.GetLocalizedString(L"Message_MixedOpticalFlow"))), names, method);
	}

	if (!_OpenFrontendSharedTextures()) {
		return ScalingError::SharedTextureOpenFailed;
	}

	_hasFrameGeneration = _dlssFrameGenerator != nullptr || _isXeSSFrameGenerationActive;

	_UpdateDestRect();

	Logger::Get().Info(fmt::format("目标矩形: {},{},{},{} ({}x{})",
		_destRect.left, _destRect.top, _destRect.right, _destRect.bottom,
		_destRect.right - _destRect.left, _destRect.bottom - _destRect.top));

	if (!_cursorDrawer.Initialize(_frontendResources)) {
		Logger::Get().Error("Initialize CursorDrawer failed");
		return ScalingError::OverlayInitFailed;
	}

	if (!_overlayDrawer.Initialize(_frontendResources, overlayOptions)) {
		Logger::Get().Error("初始化 OverlayDrawer 失败");
		return ScalingError::OverlayInitFailed;
	}

	_ResetDLSSFGSlotEvents();
	_presentationClock.Reset();
	_synchronousFramePresentationEnabled.store(true, std::memory_order_release);

	const ScalingOptions& options = ScalingWindow::Get().Options();
	if (!options.Is3DGameMode()) {
		_overlayDrawer.ToolbarState(options.IsWindowedMode() ?
			options.windowedInitialToolbarState : options.fullscreenInitialToolbarState);
	}

	_hKeyboardHook.reset(SetWindowsHookEx(WH_KEYBOARD_LL, _LowLevelKeyboardHook, NULL, 0));
	if (!_hKeyboardHook) {
		Logger::Get().Win32Warn("SetWindowsHookEx 失败");
	}

	return ScalingError::NoError;
}

void Renderer::OnCursorVisibilityChanged(bool isVisible, bool onDestory) {
	_backendThreadDispatcher.TryEnqueue([this, isVisible, onDestory]() {
		if (_frameSource && (onDestory || !_sessionLifetime->IsStopping())) {
			_frameSource->OnCursorVisibilityChanged(isVisible, onDestory);
			// Apply capture continuity resets on the first valid frame, not on a
			// forced redraw while a restarted session is still waiting for content.
		}
	});
}

void Renderer::OnSourceFocusChanged() noexcept {
	_backendThreadDispatcher.TryEnqueue([this]() {
		if (_frameGuidanceService.IsInitialized()) {
			_frameGuidanceService.ResetHistory(
				FrameGuidanceResetReason::CaptureInterrupted);
		}
		if (_dlssFrameGenerator) _dlssFrameGenerator->RequestHistoryReset();
	});
}

bool Renderer::MessageHandler(UINT msg, WPARAM wParam, LPARAM lParam) noexcept {
	// WndProc only records ImGui input and marks the Overlay dirty. A true return
	// value tells the outer message pump to end this batch after a queued
	// button/wheel edge so down and up cannot be consumed by the same UI frame.
	return _overlayDrawer.MessageHandler(msg, wParam, lParam);
}

void Renderer::ClearOverlayStates() noexcept {
	_overlayDrawer.ClearStates();
}

void Renderer::StartProfile() noexcept {
	_backendThreadDispatcher.TryEnqueue([this] {
		uint32_t passCount = 0;
		for (const EffectDesc* desc : _activeEffectDescs) {
			passCount += (uint32_t)desc->passes.size();
		}
		_effectsProfiler.Start(_backendResources.GetD3DDevice(), passCount);
	});
}

void Renderer::StopProfile() noexcept {
	_backendThreadDispatcher.TryEnqueue([this] {
		_effectsProfiler.Stop();
	});
}

winrt::fire_and_forget Renderer::TakeScreenshot(
	uint32_t effectIdx,
	uint32_t passIdx,
	uint32_t outputIdx
) noexcept {
	assert(effectIdx < _activeEffectDescs.size() || effectIdx == std::numeric_limits<uint32_t>::max());

	// All notifications use snapshots in the implementation, including after
	// the originating scaling window has closed.
	const auto& options = ScalingWindow::Get().Options();
	const auto report = options.reportErrorDetails;
	const HWND target = ScalingWindow::Get().SrcTracker().Handle();
	try {
		co_await _TakeScreenshotImpl(effectIdx, passIdx, outputIdx);
	} catch (const winrt::hresult_error& error) {
		if (report) report(target, ScalingError::ScreenshotReadbackFailed,
			"Screenshot task / dispatcher or GPU operation", static_cast<uint32_t>(error.code().value));
	} catch (...) {
		if (report) report(target, ScalingError::ScreenshotReadbackFailed, "Screenshot task", 0);
	}
}

bool Renderer::_OpenFrontendSharedTextures() noexcept {
	// Backend rendering may already be running. Hold all publication slots
	// while opening (or disabling) the optional reference branch.
	std::scoped_lock slotLocks(_sharedTextureAccessMutexes[0], _sharedTextureAccessMutexes[1],
		_sharedTextureAccessMutexes[2], _sharedTextureAccessMutexes[3]);
	if (!_passThroughFrames.OpenFrontend(_frontendResources, _sharedTextureSlotCount)) {
		const auto& window = ScalingWindow::Get();
		if (const auto& report = window.Options().reportErrorDetails) {
			report(window.SrcTracker().Handle(), ScalingError::PassThroughUnavailable,
				"Open original comparison resources / continuing effects", 0);
		}
	}
	_frontendBaseTexture = nullptr;
	_frontendPresentedBaseTexture = nullptr;
	_frontendMotionTexture = nullptr;
	_frontendMotionFrameId = 0;
	_frontendMotionValid = false;
	_frontendMotionReset = true;
	_frontendBaseValid = false;
	_frontendPresentedBaseValid = false;
	_frontendFrameMetadata = {};
	_frontendPresentedFrameMetadata = {};
	_frontendBaseNeedsPresent = false;
	for (uint32_t i = 0; i < MAX_SHARED_TEXTURE_SLOTS; ++i) {
		_frontendSharedTextureMutexes[i] = nullptr;
		_frontendSharedTextures[i] = nullptr;
		_frontendSharedMotionTextureMutexes[i] = nullptr;
		_frontendSharedMotionTextures[i] = nullptr;
		_lastAccessMutexKeys[i] = 0;
	}
	for (uint32_t i = 0; i < _sharedTextureSlotCount; ++i) {
		if (!_sharedTextureHandles[i]) {
			Logger::Get().Error("DLSSFG shared presentation slot has no handle");
			return false;
		}
		const HRESULT hr = _frontendResources.GetD3DDevice()->OpenSharedResource(
			_sharedTextureHandles[i],
			IID_PPV_ARGS(_frontendSharedTextures[i].put()));
		if (FAILED(hr)) {
			Logger::Get().ComError("Open shared presentation texture failed", hr);
			return false;
		}
		_frontendSharedTextureMutexes[i] =
			_frontendSharedTextures[i].try_as<IDXGIKeyedMutex>();
		if (!_frontendSharedTextureMutexes[i]) {
			Logger::Get().Error("Get shared presentation texture keyed mutex failed");
			return false;
		}
		if (_xessMotionRequest.method != OpticalFlowMethod::None) {
			if (!_sharedMotionTextureHandles[i]) {
				Logger::Get().Error("XeSSFG shared motion slot has no NT handle");
				return false;
			}
			const HRESULT motionHr = _frontendResources.GetD3DDevice()->OpenSharedResource1(
				_sharedMotionTextureHandles[i].get(),
				IID_PPV_ARGS(_frontendSharedMotionTextures[i].put()));
			if (FAILED(motionHr)) {
				Logger::Get().ComError("Open XeSSFG shared motion texture failed", motionHr);
				return false;
			}
			_frontendSharedMotionTextureMutexes[i] =
				_frontendSharedMotionTextures[i].try_as<IDXGIKeyedMutex>();
			if (!_frontendSharedMotionTextureMutexes[i]) {
				Logger::Get().Error("Get XeSSFG shared motion keyed mutex failed");
				return false;
			}
		}
	}

	return true;
}

void Renderer::_ResetDLSSFGSlotEvents() noexcept {
	for (uint32_t i = 0; i < _sharedTextureSlotCount; ++i) {
		if (_sharedTextureAvailableEvents[i]) {
			SetEvent(_sharedTextureAvailableEvents[i].get());
		}
	}
}

bool Renderer::_UpdateFrontendBase(uint32_t sharedTextureSlot) noexcept {
	FrameTrace::Scope traceBase(FrameTrace::Event::FrontendBase, sharedTextureSlot);
	if (sharedTextureSlot >= _sharedTextureSlotCount) {
		return false;
	}
	ID3D11Texture2D* source = _frontendSharedTextures[sharedTextureSlot].get();
	IDXGIKeyedMutex* keyedMutex = _frontendSharedTextureMutexes[sharedTextureSlot].get();
	if (!source || !keyedMutex) {
		return false;
	}

	std::unique_lock accessLock(
		_sharedTextureAccessMutexes[sharedTextureSlot], std::try_to_lock);
	if (!accessLock.owns_lock()) {
		FrameTrace::Mark(FrameTrace::Event::FrontendAcquireBusy, 0, sharedTextureSlot);
		return false;
	}

	const uint64_t currentKey =
		_sharedTextureMutexKeys[sharedTextureSlot].load(std::memory_order_acquire);
	if (_lastAccessMutexKeys[sharedTextureSlot] == currentKey && _frontendBaseValid) {
		return true;
	}

	const uint64_t releaseKey = currentKey + 1;
	std::array<IDXGIKeyedMutex*, 3> mutexes{ keyedMutex,
		_passThroughFrames.FrontendMutex(sharedTextureSlot),
		_frontendSharedMotionTextureMutexes[sharedTextureSlot].get() };
	size_t failedIndex = mutexes.size();
	FrameTrace::Scope traceAcquire(FrameTrace::Event::FrontendAcquire);
	HRESULT hr = AcquirePresentationTextures(mutexes, currentKey, 0, &failedIndex);
	traceAcquire.Data(hr, static_cast<int64_t>(failedIndex));
	traceAcquire.End();
	if (FAILED(hr)) FrameTrace::Mark(FrameTrace::Event::FrontendAcquireBusy, 1, hr);
	if (FAILED(hr) && failedIndex == 1 && hr != HRESULT_FROM_WIN32(WAIT_TIMEOUT)) {
		// A reference-only failure must not hold the effect pipeline hostage.
		// Keep objects alive for transactions already holding their mutexes.
		if (_passThroughFrames.DisableSharing()) {
			const auto& window = ScalingWindow::Get();
			if (const auto& report = window.Options().reportErrorDetails) {
				report(window.SrcTracker().Handle(), ScalingError::PassThroughUnavailable,
					"Acquire frontend reference / continuing effects", static_cast<uint32_t>(hr));
			}
		}
		mutexes[1] = nullptr;
		hr = AcquirePresentationTextures(mutexes, currentKey, 0);
	}
	if (FAILED(hr)) {
		return false;
	}
	const uint64_t slotSequence =
		_sharedTextureCaptureSequences[sharedTextureSlot].load(std::memory_order_acquire);
	const uint64_t activeSequence =
		_activeCaptureSequence.load(std::memory_order_acquire);
	const uint64_t slotGeneration = _sharedTextureResourceGenerations[sharedTextureSlot].load(
		std::memory_order_acquire);
	const uint64_t activeGeneration = _activeResourceGeneration.load(std::memory_order_acquire);
	if ((slotSequence != 0 && activeSequence != 0 && slotSequence != activeSequence) ||
		(slotGeneration != 0 && activeGeneration != 0 && slotGeneration != activeGeneration)) {
		const HRESULT staleRelease = ReleasePresentationTextures(mutexes, releaseKey);
		if (SUCCEEDED(staleRelease)) {
			_sharedTextureMutexKeys[sharedTextureSlot].store(releaseKey, std::memory_order_release);
			_lastAccessMutexKeys[sharedTextureSlot] = releaseKey;
		}
		Logger::Get().Info(fmt::format(
			"Dropped stale frontend slot={} sequence={}/{} resourceGeneration={}/{}",
			sharedTextureSlot, slotSequence, activeSequence, slotGeneration, activeGeneration));
		return false;
	}

	D3D11_TEXTURE2D_DESC sourceDesc{};
	source->GetDesc(&sourceDesc);
	bool recreateBase = !_frontendBaseTexture || !_frontendPresentedBaseTexture;
	if (_frontendBaseTexture && _frontendPresentedBaseTexture) {
		D3D11_TEXTURE2D_DESC baseDesc{};
		_frontendBaseTexture->GetDesc(&baseDesc);
		recreateBase = baseDesc.Width != sourceDesc.Width ||
			baseDesc.Height != sourceDesc.Height || baseDesc.Format != sourceDesc.Format;
	}
	if (recreateBase) {
		_frontendBaseValid = false;
		D3D11_TEXTURE2D_DESC baseDesc = sourceDesc;
		baseDesc.Usage = D3D11_USAGE_DEFAULT;
		baseDesc.BindFlags = 0;
		baseDesc.CPUAccessFlags = 0;
		baseDesc.MiscFlags = 0;
		_frontendBaseTexture = nullptr;
		_frontendPresentedBaseTexture = nullptr;
		hr = _frontendResources.GetD3DDevice()->CreateTexture2D(
			&baseDesc, nullptr, _frontendBaseTexture.put());
		if (SUCCEEDED(hr)) {
			hr = _frontendResources.GetD3DDevice()->CreateTexture2D(
				&baseDesc, nullptr, _frontendPresentedBaseTexture.put());
		}
		_frontendPresentedBaseValid = false;
	}
	if (SUCCEEDED(hr)) {
		_frontendResources.GetD3DDC()->CopyResource(_frontendBaseTexture.get(), source);
		_frontendCaptureFrameId = _sharedTextureFrameIds[sharedTextureSlot].load(std::memory_order_acquire);
		_frontendFrameMetadata = _sharedFrameMetadata[sharedTextureSlot];
		FrameTrace::SetFrame(_frontendCaptureFrameId);
		traceBase.FrameId(_frontendCaptureFrameId);
		if (!_passThroughFrames.Consume(sharedTextureSlot) && _isPassThroughActive) {
			SetPassThroughActive(false);
			const auto& window = ScalingWindow::Get();
			if (const auto& report = window.Options().reportErrorDetails) {
				report(window.SrcTracker().Handle(), ScalingError::PassThroughUnavailable,
					"Read pass-through reference frame / continuing processed output", 0);
			}
		}
		if (ID3D11Texture2D* motionSource =
			_frontendSharedMotionTextures[sharedTextureSlot].get()) {
			D3D11_TEXTURE2D_DESC motionDesc{};
			motionSource->GetDesc(&motionDesc);
			bool recreateMotion = !_frontendMotionTexture;
			if (_frontendMotionTexture) {
				D3D11_TEXTURE2D_DESC stableDesc{};
				_frontendMotionTexture->GetDesc(&stableDesc);
				recreateMotion = stableDesc.Width != motionDesc.Width ||
					stableDesc.Height != motionDesc.Height ||
					stableDesc.Format != motionDesc.Format;
			}
			if (recreateMotion) {
				motionDesc.Usage = D3D11_USAGE_DEFAULT;
				motionDesc.BindFlags = 0;
				motionDesc.CPUAccessFlags = 0;
				motionDesc.MiscFlags = 0;
				_frontendMotionTexture = nullptr;
				hr = _frontendResources.GetD3DDevice()->CreateTexture2D(
					&motionDesc, nullptr, _frontendMotionTexture.put());
			}
			if (SUCCEEDED(hr)) {
				_frontendResources.GetD3DDC()->CopyResource(
					_frontendMotionTexture.get(), motionSource);
				_frontendMotionFrameId = _sharedMotionFrameIds[sharedTextureSlot].load(
					std::memory_order_acquire);
				_frontendMotionValid = _sharedMotionValid[sharedTextureSlot].load(
					std::memory_order_acquire);
				_frontendMotionReset = _sharedMotionReset[sharedTextureSlot].load(
					std::memory_order_acquire);
			}
		}
	}
	const HRESULT releaseResult = ReleasePresentationTextures(mutexes, releaseKey);
	if (FAILED(releaseResult)) {
		Logger::Get().ComError("Release frontend shared texture failed", releaseResult);
		return false;
	}
	_sharedTextureMutexKeys[sharedTextureSlot].store(releaseKey, std::memory_order_release);
	_lastAccessMutexKeys[sharedTextureSlot] = releaseKey;
	if (FAILED(hr)) {
		Logger::Get().ComError("Create stable frontend base texture failed", hr);
		return false;
	}

	_frontendBaseValid = true;
	_frontendBaseNeedsPresent = true;
	return true;
}

void Renderer::_CopySceneToTarget(ID3D11Texture2D* scene, ID3D11Texture2D* target,
	ID3D11RenderTargetView* rtv, POINT drawOffset) noexcept {
	auto context = _frontendResources.GetD3DDC();
	const RECT& rendererRect = ScalingWindow::Get().RendererRect();
	static constexpr FLOAT BLACK[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	context->ClearRenderTargetView(rtv, BLACK);
	D3D11_TEXTURE2D_DESC targetDesc{}, sceneDesc{};
	target->GetDesc(&targetDesc);
	scene->GetDesc(&sceneDesc);
	if (targetDesc.Width == sceneDesc.Width && targetDesc.Height == sceneDesc.Height &&
		drawOffset.x == 0 && drawOffset.y == 0) {
		context->CopyResource(target, scene);
	} else {
		context->CopySubresourceRegion(target, 0,
			drawOffset.x + _destRect.left - rendererRect.left,
			drawOffset.y + _destRect.top - rendererRect.top, 0, scene, 0, nullptr);
	}
}

bool Renderer::_FrontendRender(
	bool waitForGpu,
	uint32_t sharedTextureSlot,
	FrontendRenderTimings* timings,
	bool stableBaseOnly
) noexcept {
	if (_pendingFrontendFrame) return _SubmitFrontendFrame();
	_frontendPacingDeadline.reset();
	const bool paced = _frontEdgeSyncEnabled && !_hasFrameGeneration &&
		!waitForGpu && _presenter->SupportsDeferredPresent() &&
		!ScalingWindow::Get().IsResizingOrMoving();
	if (paced) {
		_frontEdgeClock.SetInterval(std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::duration<double>(1.0 / _FrontEdgeFrameRate())));
		const auto now = std::chrono::steady_clock::now();
		const auto prepareAt = _frontEdgeClock.Due(now) - std::chrono::microseconds(1000);
		if (now < prepareAt) {
			_frontendPacingDeadline = prepareAt;
			return false;
		}
	} else {
		_frontEdgeClock.Reset();
	}
	if (stableBaseOnly && !paced && !_CanRenderOverlay()) return false;
	if (_frontEdgeSyncEnabled && _isXeSSFrameGenerationActive &&
		!_presenter->SetBaseFrameRateLimit(_FrontEdgeFrameRate())) {
		if (!_frontEdgeLimiterFailed) {
			_frontEdgeLimiterFailed = true;
			ScalingWindow::Dispatcher().TryEnqueue([session = _sessionLifetime] {
				auto& window = ScalingWindow::Get();
				if (!session->IsCurrent(ScalingWindow::RunId()) || !window) return;
				if (auto report = window.Options().reportErrorDetails) report(
					window.SrcTracker().Handle(), ScalingError::PresentationInitFailed,
					"XeLL frame-rate configuration failed; disable Front Edge Sync and re-enable the effect group", 0);
				window.Stop();
			});
		}
		return false;
	}
	const auto beginFrameStart = std::chrono::steady_clock::now();
	if (sharedTextureSlot >= _sharedTextureSlotCount) {
		sharedTextureSlot = _latestSharedTextureSlot.load(std::memory_order_acquire);
	}
	if (!stableBaseOnly &&
		(!_frontendBaseValid || _lastAccessMutexKeys[sharedTextureSlot] !=
			_sharedTextureMutexKeys[sharedTextureSlot].load(std::memory_order_acquire)) &&
		!_UpdateFrontendBase(sharedTextureSlot)) {
		return false;
	}
	ID3D11Texture2D* baseTexture = stableBaseOnly ?
		_frontendPresentedBaseTexture.get() : _frontendBaseTexture.get();
	if ((stableBaseOnly ? !_frontendPresentedBaseValid : !_frontendBaseValid) ||
		!baseTexture) {
		return false;
	}

	winrt::com_ptr<ID3D11Texture2D> frameTex;
	winrt::com_ptr<ID3D11RenderTargetView> frameRtv;
	POINT drawOffset{};
	const RECT& rendererRect = ScalingWindow::Get().RendererRect();
	const RECT guidanceDestination{
		_destRect.left - rendererRect.left,
		_destRect.top - rendererRect.top,
		_destRect.right - rendererRect.left,
		_destRect.bottom - rendererRect.top
	};
	_presenter->SetFrameGuidance(
		_frontendMotionValid ? _frontendMotionTexture.get() : nullptr,
		_frontendMotionFrameId,
		_frontendMotionReset || !_frontendMotionValid,
		guidanceDestination);
	FrameTrace::Scope traceBegin(FrameTrace::Event::BeginFrame);
	const bool traceBegan = _presenter->BeginFrame(frameTex, frameRtv, drawOffset);
	traceBegin.Data(traceBegan);
	traceBegin.End();
	if (!traceBegan) {
		if (timings) {
			timings->beginFrame = std::chrono::steady_clock::now() - beginFrameStart;
		}
		return false;
	}
	FrameTrace::Scope traceDraw(FrameTrace::Event::FrontendDraw, stableBaseOnly, _isPassThroughActive);
	const auto drawStart = std::chrono::steady_clock::now();
	if (timings) {
		timings->beginFrame = drawStart - beginFrameStart;
	}

	ID3D11DeviceContext4* d3dDC = _frontendResources.GetD3DDC();
	d3dDC->ClearState();
	d3dDC->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

	const bool uiInIndependentLayer = _presenter->HasIndependentOverlay();
	const uint64_t overlayActionRevision = _overlayActionRevision;
	// XeSS must keep receiving the processed stream, even behind the overlay.
	ID3D11Texture2D* scene = baseTexture;
	if (_isPassThroughActive && !uiInIndependentLayer) {
		if (auto reference = _passThroughFrames.FrontendTexture(stableBaseOnly)) scene = reference;
	}
	_CopySceneToTarget(scene, frameTex.get(), frameRtv.get(), drawOffset);
	if (!uiInIndependentLayer) {
		ID3D11RenderTargetView* target = frameRtv.get();
		d3dDC->OMSetRenderTargets(1, &target, nullptr);
		// Input is injected inside Draw immediately before ImGui::NewFrame. All
		// capacity and base-texture waits have already completed at this point.
		// Drain samples only when drawing the overlay. Front Edge Sync retries
		// above must leave them available for the frame that actually draws it.
		_overlayDrawer.Draw(_stepTimer.FPS(), _effectsProfiler.GetTimings(), drawOffset);
		_cursorDrawer.Draw(frameTex.get(), drawOffset);
	}

	traceDraw.End();
	const auto endFrameStart = std::chrono::steady_clock::now();
	if (timings) {
		timings->draw = endFrameStart - drawStart;
	}
	const bool contentFrame = !stableBaseOnly && _frontendBaseNeedsPresent;
	const bool generatedFrame = contentFrame &&
		_sharedTextureContainsGeneratedFrame[sharedTextureSlot].load(std::memory_order_acquire);
	_pendingFrontendFrame = PendingFrontendFrame{
		.stableBaseOnly = stableBaseOnly, .contentFrame = contentFrame,
		.generatedFrame = generatedFrame, .independentOverlay = uiInIndependentLayer,
		.waitForGpu = waitForGpu, .paced = paced,
		.overlayRevision = overlayActionRevision,
		.contentKey = _lastAccessMutexKeys[sharedTextureSlot]
	};
	if (paced) _frontendResources.GetD3DDC()->Flush();
	const bool submitted = _SubmitFrontendFrame();
	if (timings) timings->endFrame = std::chrono::steady_clock::now() - endFrameStart;
	return submitted;
}

bool Renderer::_SubmitFrontendFrame() noexcept {
	assert(_pendingFrontendFrame);
	const auto frame = *_pendingFrontendFrame;
	const auto submitStart = std::chrono::steady_clock::now();
	if (frame.paced && _presenter->SupportsDeferredPresent() &&
		!ScalingWindow::Get().IsResizingOrMoving()) {
		const auto due = _frontEdgeClock.Due(submitStart);
		if (submitStart < due) {
			_frontendPacingDeadline = due;
			return false;
		}
	}
	_frontendPacingDeadline.reset();
	const auto [stableBaseOnly, contentFrame, generatedFrame, uiInIndependentLayer,
		waitForGpu, paced, overlayActionRevision, contentKey] = frame;
	const bool submitted = _presenter->EndFrame(waitForGpu);
	_pendingFrontendFrame.reset();
	if (submitted && _frontEdgeSyncEnabled && !_hasFrameGeneration &&
		_presenter->SupportsDeferredPresent() && !ScalingWindow::Get().IsResizingOrMoving()) {
		_frontEdgeClock.SetInterval(std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::duration<double>(1.0 / _FrontEdgeFrameRate())));
		_frontEdgeClock.Submitted(_presenter->LastSubmissionTime());
	}
	auto* d3dDC = _frontendResources.GetD3DDC();
	if (submitted) FrameTrace::Mark(contentFrame ? FrameTrace::Event::ContentSubmit :
		FrameTrace::Event::OverlaySubmit, generatedFrame,
		_presenter->LastPresentedFrameCount() ? int64_t(*_presenter->LastPresentedFrameCount()) : -1);
	if (submitted && contentFrame && _hasFrameGeneration && _frontendCaptureFrameId != 0 &&
		(generatedFrame || _frontendCaptureFrameId != _lastCountedRealFrameId)) {
		const auto count = _presenter->LastPresentedFrameCount();
		_presentationRate.Record(count, !generatedFrame && (!count || *count > 0) ? 1u : 0u);
		if (!generatedFrame && (!count || *count > 0)) _lastCountedRealFrameId = _frontendCaptureFrameId;
	}
	if (submitted) {
		if (!stableBaseOnly && _frontendPresentedBaseTexture) {
			d3dDC->CopyResource(
				_frontendPresentedBaseTexture.get(), _frontendBaseTexture.get());
			_frontendPresentedBaseValid = true;
			_frontendPresentedFrameMetadata = _frontendFrameMetadata;
			_frontendPresentedFrameMetadata = _frontendFrameMetadata;
			_frontendPresentedFrameMetadata.stage = HdrFrameStage::PresentedOutput;
			_passThroughFrames.OnPresented();
		}
		if (!stableBaseOnly) _frontendBaseNeedsPresent = false;
		if (!uiInIndependentLayer) {
			_overlayDrawer.OnPresentSucceeded();
			_overlayPresentationClock.Presented(std::chrono::steady_clock::now());
			_presentedOverlayActionRevision = overlayActionRevision;
		}
	} else if (!uiInIndependentLayer) {
		_overlayDrawer.OnPresentFailed();
	}
	if (submitted && contentFrame && _frontEdgeSyncEnabled && _frontEdgeUsesSharedSlot) {
		_frontEdgeAcknowledgedKey.store(contentKey, std::memory_order_release);
		SetEvent(_frontEdgeConsumedEvent.get());
	}

	if (submitted && uiInIndependentLayer &&
		(_HasPendingOverlayAction() || _isPassThroughActive || _cursorDrawer.NeedRedraw() || _cursorDrawer.IsBackgroundDependent() ||
			_overlayDrawer.NeedRedraw(_stepTimer.FPS()))) {
		_FrontendOverlayRender(_isPassThroughActive || _cursorDrawer.IsBackgroundDependent());
	}
	return submitted;
}

bool Renderer::_FrontendOverlayRender(bool contentChanged) noexcept {
	if (!_presenter->HasIndependentOverlay()) {
		return false;
	}
	// A comparison image or XOR cursor background must follow new content even
	// when an overlay-only redraw would still be rate limited.
	if (!contentChanged && !_CanRenderOverlay()) return false;
	winrt::com_ptr<ID3D11Texture2D> frameTex;
	winrt::com_ptr<ID3D11RenderTargetView> frameRtv;
	POINT drawOffset{};
	if (!_presenter->BeginOverlayFrame(frameTex, frameRtv, drawOffset)) {
		return false;
	}

	ID3D11DeviceContext4* d3dDC = _frontendResources.GetD3DDC();
	d3dDC->ClearState();
	static constexpr FLOAT CLEAR_TRANSPARENT[4]{};
	const uint64_t overlayActionRevision = _overlayActionRevision;
	d3dDC->ClearRenderTargetView(frameRtv.get(), CLEAR_TRANSPARENT);
	ID3D11Texture2D* cursorBackground =
		_frontendPresentedBaseValid ? _frontendPresentedBaseTexture.get() : nullptr;
	if (_isPassThroughActive) {
		if (auto reference = _passThroughFrames.FrontendTexture(true)) {
			_CopySceneToTarget(reference, frameTex.get(), frameRtv.get(), drawOffset);
			cursorBackground = reference;
		}
	}
	ID3D11RenderTargetView* target = frameRtv.get();
	d3dDC->OMSetRenderTargets(1, &target, nullptr);
	// XeSS draws its UI in this independent layer, not in _FrontendRender.
	_overlayDrawer.Draw(_stepTimer.FPS(), _effectsProfiler.GetTimings(), drawOffset);
	_cursorDrawer.Draw(frameTex.get(), drawOffset,
		cursorBackground);
	const bool submitted = _presenter->EndOverlayFrame();
	if (submitted) {
		_overlayDrawer.OnPresentSucceeded();
		_overlayPresentationClock.Presented(std::chrono::steady_clock::now());
		_presentedOverlayActionRevision = overlayActionRevision;
	} else {
		_overlayDrawer.OnPresentFailed();
	}
	return submitted;
}

bool Renderer::Render(bool force, bool waitForGpu) noexcept {
	if (_pendingFrontendFrame) return _SubmitFrontendFrame();
	_frontendPacingDeadline.reset();
	// In synchronous DLSSFG mode regular capture work remains owned by the FIFO.
	// RenderOverlay uses the stable frontend copy and never consumes a ring slot.
	if (_dlssFrameGenerator &&
		_synchronousFramePresentationEnabled.load(std::memory_order_acquire)) {
		// Timed overlays must advance even when a static source produces no
		// FIFO work. RenderOverlay only redraws the last presented stable image.
		return RenderOverlay();
	}

	const uint32_t sharedTextureSlot = std::min(
		_latestSharedTextureSlot.load(std::memory_order_acquire),
		_sharedTextureSlotCount - 1);
	const bool hasNewBackendFrame =
		_lastAccessMutexKeys[sharedTextureSlot] !=
		_sharedTextureMutexKeys[sharedTextureSlot].load(std::memory_order_relaxed) ||
		(_frontEdgeSyncEnabled && _frontEdgeUsesSharedSlot &&
			_frontEdgeAcknowledgedKey.load(std::memory_order_acquire) !=
			_sharedTextureMutexKeys[sharedTextureSlot].load(std::memory_order_acquire));
	FrameTrace::Mark(FrameTrace::Event::RenderDecision,
		(force ? 1 : 0) | (hasNewBackendFrame ? 2 : 0) |
		(_frontendBaseNeedsPresent ? 4 : 0) | (_isPassThroughActive ? 8 : 0));
	if (!force && !hasNewBackendFrame && !_frontendBaseNeedsPresent) {
		if (_lastAccessMutexKeys[sharedTextureSlot] == 0) {
			// 第一帧尚未完成
			return false;
		}
		if (_isXeSSFrameGenerationActive) {
			return (_HasPendingOverlayAction() || _cursorDrawer.NeedRedraw() ||
				_overlayDrawer.NeedRedraw(_stepTimer.FPS())) &&
				_FrontendOverlayRender();
		}

		if (!_HasPendingOverlayAction() && !_cursorDrawer.NeedRedraw() &&
			!_overlayDrawer.NeedRedraw(_stepTimer.FPS())) {
			return false;
		}
	}

	return _FrontendRender(waitForGpu, sharedTextureSlot,
		nullptr, !hasNewBackendFrame && !_frontendBaseNeedsPresent);
}

bool Renderer::RenderOverlay() noexcept {
	// For regular/XeSS rendering, consume available content first. An input
	// message must not insert a replay of the old image ahead of the new one.
	if (!_dlssFrameGenerator ||
		!_synchronousFramePresentationEnabled.load(std::memory_order_acquire)) {
		return Render();
	}
	// DLSS uses the content swap chain for its overlay. Do not spend its next
	// presentation opportunity replaying an old frame while FIFO content waits.
	// The next FIFO frame draws/acknowledges the same queued input. The atomic
	// count also covers frame notifications not yet dispatched by the UI thread.
	if (_dlssFrameGenerator &&
		_synchronousFramePresentationEnabled.load(std::memory_order_acquire) &&
		_pendingDLSSFGFrontendFrames.load(std::memory_order_acquire) != 0) {
		return false;
	}
	if (!_HasPendingOverlayAction() && !_overlayDrawer.NeedRedraw(_stepTimer.FPS()) && !_cursorDrawer.NeedRedraw()) {
		return false;
	}
	if (_presenter->HasIndependentOverlay()) {
		return _FrontendOverlayRender();
	}
	if (!_frontendPresentedBaseValid) {
		return false;
	}
	return _FrontendRender(false,
		std::numeric_limits<uint32_t>::max(), nullptr, true);
}

bool Renderer::HasPendingOverlayInput() const noexcept {
	return _overlayDrawer.HasPendingInput();
}

bool Renderer::HasUrgentOverlayInput() const noexcept {
	return _overlayDrawer.HasUrgentInput();
}

bool Renderer::HasPendingContent() const noexcept {
	if (_pendingFrontendFrame || _frontendPacingDeadline) return true;
	if (_dlssFrameGenerator &&
		_synchronousFramePresentationEnabled.load(std::memory_order_acquire)) {
		return _pendingDLSSFGFrontendFrames.load(std::memory_order_acquire) != 0;
	}
	const uint32_t slot = std::min(_latestSharedTextureSlot.load(std::memory_order_acquire),
		_sharedTextureSlotCount - 1);
	return _frontendBaseNeedsPresent ||
		(_frontEdgeSyncEnabled && _frontEdgeUsesSharedSlot &&
			_frontEdgeAcknowledgedKey.load(std::memory_order_acquire) !=
			_sharedTextureMutexKeys[slot].load(std::memory_order_acquire)) || _lastAccessMutexKeys[slot] !=
		_sharedTextureMutexKeys[slot].load(std::memory_order_acquire);
}

bool Renderer::_CanRenderOverlay() const noexcept {
	// Button/wheel/cancel edges and explicit toolbar actions remain immediate.
	// Continuous dragging is urgent to the input queue, but can be coalesced
	// for presentation without dropping its latest position or button edges.
	const bool due = _overlayPresentationClock.IsDue(std::chrono::steady_clock::now(),
		_HasPendingOverlayAction() || _overlayDrawer.HasCriticalInput() ||
		ScalingWindow::Get().IsResizingOrMoving());
	if (!due) FrameTrace::Mark(FrameTrace::Event::OverlayDeferred);
	return due;
}

void Renderer::_UpdateOverlayRefreshRate() noexcept {
	const HWND window = ScalingWindow::Get().Handle();
	const HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
	if (monitor == _overlayMonitor && _overlayMonitor) return;
	_overlayMonitor = monitor;
	const double refreshRate = GetDisplayRefreshRate(window);
	_presentationRefreshRate.store(refreshRate, std::memory_order_release);
	_overlayPresentationClock.SetRefreshRate(refreshRate);
	_frontEdgeClock.Reset();
	if (_backendThreadDispatcher) {
		_backendThreadDispatcher.TryEnqueue([this] { _UpdateFrameRateLimits(); });
	}
	Logger::Get().Info(fmt::format("Overlay-only presentation interval: {:.3f} ms",
		std::chrono::duration<double, std::milli>(_overlayPresentationClock.Interval()).count()));
}


double Renderer::_FrontEdgeFrameRate() const noexcept {
	return ResolvePresentationFrameRate(ScalingWindow::Get().Options().frontEdgeSyncFrameRate,
		_existingBaseFrameRateLimit.load(std::memory_order_acquire),
		_presentationRefreshRate.load(std::memory_order_acquire), _configuredFrameGenerationMultiplier);
}

void Renderer::WaitForFrontendWork(std::chrono::nanoseconds maximumWait) noexcept {
	if (_frontendPacingDeadline) {
		const auto remaining = *_frontendPacingDeadline - std::chrono::steady_clock::now();
		if (remaining <= std::chrono::nanoseconds::zero()) return;
		WaitForFramePacing(std::min(remaining, std::max(maximumWait,
			std::chrono::nanoseconds(std::chrono::milliseconds(8)))), _frontendPacingTimer);
	} else {
		const DWORD ms = static_cast<DWORD>(std::max<int64_t>(0,
			(maximumWait.count() + 999'999) / 1'000'000));
		if (!_presenter->WaitForFrameCapacity(ms)) {
			MsgWaitForMultipleObjectsEx(0, nullptr, ms, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
		}
	}
}

void Renderer::_RecordDLSSFGFrontendTimings(
	bool usesFrameLatencyWaitableObject,
	std::chrono::nanoseconds pacingWait,
	const FrontendRenderTimings& timings
) noexcept {
	if (!_dlssFgFrontendTimingModeInitialized ||
		_dlssFgFrontendTimingUsesWaitableObject != usesFrameLatencyWaitableObject) {
		_dlssFgFrontendTimingModeInitialized = true;
		_dlssFgFrontendTimingUsesWaitableObject = usesFrameLatencyWaitableObject;
		_dlssFgFrontendTimingFrames = 0;
		_dlssFgFrontendPacingWait = {};
		_dlssFgFrontendBeginFrame = {};
		_dlssFgFrontendDraw = {};
		_dlssFgFrontendEndFrame = {};
		_dlssFgRingWaitNanoseconds.exchange(0, std::memory_order_relaxed);
		_dlssFgRingWaitSamples.exchange(0, std::memory_order_relaxed);
	}

	++_dlssFgFrontendTimingFrames;
	_dlssFgFrontendPacingWait += pacingWait;
	_dlssFgFrontendBeginFrame += timings.beginFrame;
	_dlssFgFrontendDraw += timings.draw;
	_dlssFgFrontendEndFrame += timings.endFrame;
	if (_dlssFgFrontendTimingFrames < 120) {
		return;
	}

	const uint64_t ringWaitNanoseconds =
		_dlssFgRingWaitNanoseconds.exchange(0, std::memory_order_relaxed);
	const uint64_t ringWaitSamples =
		_dlssFgRingWaitSamples.exchange(0, std::memory_order_relaxed);
	const auto averageMilliseconds = [frames = _dlssFgFrontendTimingFrames](
		std::chrono::nanoseconds total) noexcept {
		return std::chrono::duration<double, std::milli>(total).count() / frames;
	};
	const double averageRingWaitMilliseconds = ringWaitSamples ?
		double(ringWaitNanoseconds) / 1'000'000.0 / ringWaitSamples : 0.0;
	Logger::Get().Info(fmt::format(
		"DLSSFG frontend timing: mode={} frames={} paceWait={:.3f} ms "
		"beginFrame={:.3f} ms draw={:.3f} ms endFrame={:.3f} ms "
		"ringWait={:.3f} ms",
		usesFrameLatencyWaitableObject ? "deadline+DXGI" : "DWM",
		_dlssFgFrontendTimingFrames,
		averageMilliseconds(_dlssFgFrontendPacingWait),
		averageMilliseconds(_dlssFgFrontendBeginFrame),
		averageMilliseconds(_dlssFgFrontendDraw),
		averageMilliseconds(_dlssFgFrontendEndFrame),
		averageRingWaitMilliseconds));

	_dlssFgFrontendTimingFrames = 0;
	_dlssFgFrontendPacingWait = {};
	_dlssFgFrontendBeginFrame = {};
	_dlssFgFrontendDraw = {};
	_dlssFgFrontendEndFrame = {};
}

DLSSFGFrameRenderResult Renderer::RenderDLSSFGFrame(
	uint32_t sharedTextureSlot,
	uint32_t sharedTextureGeneration
) noexcept {
	auto consumePendingFrame = [this]() noexcept {
		uint32_t pending = _pendingDLSSFGFrontendFrames.load(std::memory_order_acquire);
		while (pending > 0 && !_pendingDLSSFGFrontendFrames.compare_exchange_weak(
			pending, pending - 1, std::memory_order_acq_rel)) {
		}
	};
	if (sharedTextureGeneration != _sharedTextureGeneration.load(std::memory_order_acquire)) {
		// An old notification must never release a slot or decrement a count
		// belonging to the new ring after resize/recovery.
		return DLSSFGFrameRenderResult::Dropped;
	}
	if (sharedTextureSlot >= _sharedTextureSlotCount) {
		consumePendingFrame();
		return DLSSFGFrameRenderResult::Dropped;
	}
	if (!_synchronousFramePresentationEnabled.load(std::memory_order_acquire)) {
		consumePendingFrame();
		if (_sharedTextureAvailableEvents[sharedTextureSlot]) {
			SetEvent(_sharedTextureAvailableEvents[sharedTextureSlot].get());
		}
		return DLSSFGFrameRenderResult::Dropped;
	}

	const bool usesFrameLatencyWaitableObject =
		_presenter->UsesFrameLatencyWaitableObject();
	const auto pacingStart = std::chrono::steady_clock::now();
	const std::chrono::nanoseconds presentInterval(
		_sharedPresentIntervalNs[sharedTextureSlot].load(std::memory_order_acquire));
	const auto targetTime = _presentationClock.Due(pacingStart, presentInterval);
	if (presentInterval.count() > 0 && pacingStart < targetTime) {
		// The scheduler will wake on either input or the next short deadline. Do
		// not sleep in a FIFO job and make already queued input wait behind it.
		return DLSSFGFrameRenderResult::Retry;
	}
	const auto pacingEnd = pacingStart;

	FrontendRenderTimings timings;
	const bool presented = _FrontendRender(
		false, sharedTextureSlot, &timings);
	if (!presented) {
		return DLSSFGFrameRenderResult::Retry;
	}
	const auto presentEnd = std::chrono::steady_clock::now();
	_presentationClock.Presented(presentEnd, targetTime, presentInterval);
	consumePendingFrame();
	if (_sharedTextureAvailableEvents[sharedTextureSlot]) {
		SetEvent(_sharedTextureAvailableEvents[sharedTextureSlot].get());
	}
	_RecordDLSSFGFrontendTimings(
		usesFrameLatencyWaitableObject, pacingEnd - pacingStart, timings);
	return DLSSFGFrameRenderResult::Presented;
}

bool Renderer::OnResize() noexcept {
	if (_pendingFrontendFrame) {
		// A deferred Present has not performed flip-model RTV unbinding yet.
		// Drop the context's indirect back-buffer references before ResizeBuffers.
		_frontendResources.GetD3DDC()->ClearState();
		_frontendResources.GetD3DDC()->Flush();
		_overlayDrawer.OnPresentFailed();
	}
	_pendingFrontendFrame.reset();
	_frontendPacingDeadline.reset();
	_frontEdgeClock.Reset();
	_UpdateOverlayRefreshRate();
	_synchronousFramePresentationEnabled.store(false, std::memory_order_release);

	if (!_presenter->OnResize()) {
		Logger::Get().Error("更改呈现器尺寸失败");
		return false;
	}

	_sharedTextureHandle.store(NULL, std::memory_order_relaxed);

	_backendThreadDispatcher.TryEnqueue([this]() {
		_pendingFrameGenerationInput = nullptr;
		_fgInputClock.Reset();
		_frontEdgeAcknowledgedKey.store(0, std::memory_order_release);
		ID3D11Texture2D* outputTexture = _ResizeEffects();
		if (!outputTexture) {
			Logger::Get().Win32Error("_ResizeEffects 失败");
			_sharedTextureHandle.store(INVALID_HANDLE_VALUE, std::memory_order_relaxed);
			_sharedTextureHandle.notify_one();
			return;
		}

		HANDLE sharedHandle = _CreateSharedTexture(outputTexture);
		if (!sharedHandle) {
			Logger::Get().Win32Error("_CreateSharedTexture 失败");
			_sharedTextureHandle.store(INVALID_HANDLE_VALUE, std::memory_order_relaxed);
			_sharedTextureHandle.notify_one();
			return;
		}

		// 渲染完成再通知前端防止黑屏。前端会自动执行渲染，因此无需发送 WM_FRONTEND_RENDER
		_BackendRender(outputTexture, false);

		_sharedTextureHandle.store(sharedHandle, std::memory_order_release);
		_sharedTextureHandle.notify_one();
	});

	// 等待后端更改分辨率和渲染
	_sharedTextureHandle.wait(NULL, std::memory_order_relaxed);
	// 将三个成员同步到前端线程
	const HANDLE sharedTextureHandle = _sharedTextureHandle.load(std::memory_order_acquire);
	if (sharedTextureHandle == INVALID_HANDLE_VALUE) {
		return false;
	}

	if (!_OpenFrontendSharedTextures()) {
		return false;
	}
	_ResetDLSSFGSlotEvents();
	_presentationClock.Reset();
	_synchronousFramePresentationEnabled.store(true, std::memory_order_release);

	_UpdateDestRect();
	return true;
}

void Renderer::OnEndResize() noexcept {
	bool shouldRedraw = false;
	_presenter->OnEndResize(shouldRedraw);

	if (shouldRedraw) {
		_FrontendRender();
	}
}

void Renderer::OnMove() noexcept {
	_UpdateOverlayRefreshRate();
	_UpdateDestRect();
}

void Renderer::InvokeOverlayAction(OverlayAction action) noexcept {
    const ScalingWindow& window = ScalingWindow::Get();
    if (action == OverlayAction::Screenshot) {
        TakeDisplayedScreenshot();
        return;
    }
    if (window.Options().Is3DGameMode()) {
        window.ShowToast(window.GetLocalizedString(L"Message_ToolbarIn3DGameMode"));
        return;
    }
    ++_overlayActionRevision;
    _overlayDrawer.InvokeAction(action);
    if (action == OverlayAction::Comparison) {
        // A hotkey has no mouse edge. Present the selected stable image even
        // when a static source has no new capture or DLSSFG FIFO work ready.
        ScalingWindow::Get().RenderOverlay();
    } else {
        Render();
    }
}

void Renderer::RestoreOverlayState(const OverlaySessionState& state) noexcept {
	++_overlayActionRevision;
	_overlayDrawer.RestoreSessionState(state);
	Render();
}

bool Renderer::SetPassThroughActive(bool value) noexcept {
	if (value && !_passThroughFrames.FrontendTexture(true)) {
		const auto& window = ScalingWindow::Get();
		if (const auto& report = window.Options().reportErrorDetails) {
			report(window.SrcTracker().Handle(), ScalingError::PassThroughUnavailable,
				"Enable original comparison / no current reference texture", 0);
		}
		return false;
	}
	if (_isPassThroughActive == value) return true;
	_isPassThroughActive = value;
	++_overlayActionRevision;
	_frontendBaseNeedsPresent = true;
	Logger::Get().Info(fmt::format(
		"Pass through {}: referenceCapture={} independentOverlay={} (processing remains active)",
		value ? "enabled" : "disabled", _passThroughFrames.PresentedCaptureFrameId(),
		_presenter->HasIndependentOverlay()));
	return true;
}

void Renderer::SwitchToolbarState() noexcept {
	const ScalingWindow& scalingWindow = ScalingWindow::Get();

	if (scalingWindow.Options().Is3DGameMode()) {
		scalingWindow.ShowToast(scalingWindow.GetLocalizedString(L"Message_ToolbarIn3DGameMode"));
		return;
	}

	const ToolbarState newState = ToolbarState(
		((uint32_t)_overlayDrawer.ToolbarState() + 1) % (uint32_t)ToolbarState::COUNT);
	_overlayDrawer.ToolbarState(newState);
	++_overlayActionRevision;

	// 显示状态切换消息
	const wchar_t* stateResName = nullptr;
	if (newState == ToolbarState::Off) {
		stateResName = L"Home_Toolbar_InitialState_Off/Content";
	} else if (newState == ToolbarState::AlwaysShow) {
		stateResName = L"Home_Toolbar_InitialState_AlwaysShow/Content";
	} else {
		stateResName = L"Home_Toolbar_InitialState_AutoHide/Content";
	}

	winrt::hstring newStateMsg = scalingWindow.GetLocalizedString(L"Message_ToolbarNewState");
	scalingWindow.ShowToast(fmt::format(
		fmt::runtime(std::wstring_view(newStateMsg)),
		std::wstring_view(scalingWindow.GetLocalizedString(stateResName))
	));

	// 由统一 Render 路径决定何时呈现；DLSSFG 模式下等待下一帧 FIFO。
	Render();
}

const RECT& Renderer::SrcRect() const noexcept {
	return ScalingWindow::Get().SrcTracker().SrcRect();
}

bool Renderer::_InitFrameSource() noexcept {
	switch (ScalingWindow::Get().Options().captureMethod) {
	case CaptureMethod::GraphicsCapture:
		_frameSource = std::make_unique<GraphicsCaptureFrameSource>();
		break;
	case CaptureMethod::DesktopDuplication:
		_frameSource = std::make_unique<DesktopDuplicationFrameSource>();
		break;
	case CaptureMethod::GDI:
		_frameSource = std::make_unique<GDIFrameSource>();
		break;
	case CaptureMethod::DwmSharedSurface:
		_frameSource = std::make_unique<DwmSharedSurfaceFrameSource>();
		break;
	default:
		Logger::Get().Error("未知的捕获模式");
		return false;
	}

	Logger::Get().Info(StrHelper::Concat("当前捕获模式: ", _frameSource->Name()));

	const bool forceDuplicateFrameDetection = std::ranges::any_of(
		ScalingWindow::Get().Options().effects,
		[](const EffectOption& effect) { return IsFrameGenerationEffect(effect.name); });
	_frameSource->ForceDuplicateFrameDetection(forceDuplicateFrameDetection);
	if (forceDuplicateFrameDetection) {
		Logger::Get().Info(
			"Frame Generation: exact duplicate-frame filtering forced for captured input");
	}

	if (!_frameSource->Initialize(_backendResources, _backendDescriptorStore)) {
		Logger::Get().Error("初始化 FrameSource 失败");
		_backendInitError = ScalingError::CaptureFailed;
		return false;
	}
	if (ScalingWindow::Get().Options().IsHdrCompatibilityEnabled() &&
		!_hdrPresentationAdapter.Initialize(_backendResources, _backendDescriptorStore)) {
		Logger::Get().Error("初始化 HDR 发布适配器失败");
		_backendInitError = ScalingError::GraphicsDeviceInitFailed;
		return false;
	}

	// 由于 DPI 缩放，捕获尺寸和边界矩形尺寸不一定相同
	D3D11_TEXTURE2D_DESC desc;
	_frameSource->GetPipelineTexture()->GetDesc(&desc);
	Logger::Get().Info(fmt::format("捕获尺寸: {}x{}", desc.Width, desc.Height));

	return true;
}

static std::optional<EffectDesc> CompileEffect(
	const EffectOption& effectOption,
	bool noFP16,
	bool forceInlineParams = false,
	DXGI_FORMAT routeInputFormat = DXGI_FORMAT_UNKNOWN,
	DXGI_FORMAT routeOutputFormat = DXGI_FORMAT_UNKNOWN
) noexcept {
	// 指定效果名
	EffectDesc result{ .name = effectOption.name };

	uint32_t compileFlag = 0;
	const ScalingOptions& scalingOptions = ScalingWindow::Get().Options();
	if (scalingOptions.IsEffectCacheDisabled()) {
		compileFlag |= EffectCompilerFlags::NoCache;
	}
	if (scalingOptions.IsSaveEffectSources()) {
		compileFlag |= EffectCompilerFlags::SaveSources;
	}
	if (scalingOptions.IsWarningsAreErrors()) {
		compileFlag |= EffectCompilerFlags::WarningsAreErrors;
	}
	if (scalingOptions.IsInlineParams() || forceInlineParams) {
		compileFlag |= EffectCompilerFlags::InlineParams;
	}
	if (noFP16) {
		compileFlag |= EffectCompilerFlags::NoFP16;
	}
	if (scalingOptions.IsHdrCompatibilityEnabled()) {
		compileFlag |= EffectCompilerFlags::HdrCompatibility;
	}
	const auto encodeFormat = [](DXGI_FORMAT format, uint32_t shift) {
		if (format == DXGI_FORMAT_UNKNOWN) return uint32_t(0);
		for (uint32_t i = 0; i < std::size(EffectHelper::FORMAT_DESCS) - 1; ++i) {
			if (EffectHelper::FORMAT_DESCS[i].dxgiFormat == format)
				return ((i + 1) & EffectCompilerFlags::SurfaceFormatMask) << shift;
		}
		return uint32_t(0);
	};
	compileFlag |= encodeFormat(routeInputFormat, EffectCompilerFlags::InputFormatShift);
	compileFlag |= encodeFormat(routeOutputFormat, EffectCompilerFlags::OutputFormatShift);

	bool success = true;
	uint32_t duration = Measure([&]() {
		success = !EffectCompiler::Compile(result, compileFlag, &effectOption.parameters);
	});

	if (success) {
		Logger::Get().Info(fmt::format("编译 {}.hlsl 用时 {} 毫秒",
			effectOption.name, duration / 1000.0f));
		return result;
	} else {
		Logger::Get().Error(StrHelper::Concat("编译 ",
			effectOption.name, ".hlsl 失败"));
		return std::nullopt;
	}
}

ID3D11Texture2D* Renderer::_BuildEffects() noexcept {
	_backendInitError = ScalingError::ScalingFailedGeneral;
	_backendInitContext.clear();
	const ScalingOptions& options = ScalingWindow::Get().Options();
	const bool noFP16 = !_backendResources.IsFP16Supported() || options.IsFP16Disabled();

	const std::vector<EffectOption>& effects = _runtimeEffectOptions;
	assert(!effects.empty());
	const uint32_t effectCount = (uint32_t)effects.size();

	// 并行编译所有效果
	_effectDescs.resize(effects.size());
	bool anyFailure = false;
	wil::srwlock writeLock;

	int duration = Measure([&]() {
		Win32Helper::RunParallel([&](uint32_t id) {
			DXGI_FORMAT routeInput = DXGI_FORMAT_UNKNOWN;
			DXGI_FORMAT routeOutput = DXGI_FORMAT_UNKNOWN;
			if (options.IsHdrCompatibilityEnabled()) {
				const HdrFormatRoutes routes = GetHdrRoutesForEffect(
					effects[id], options.IsHdrCompatibilityEnabled());
				if (!routes.empty() && routes.front().inputFormat != DXGI_FORMAT_UNKNOWN) {
					routeInput = routes.front().inputFormat;
					routeOutput = routes.front().outputFormat;
				}
			}
			std::optional<EffectDesc> desc = CompileEffect(
				effects[id], noFP16, false, routeInput, routeOutput);

			auto lk = writeLock.lock_exclusive();
			if (desc) {
				_effectDescs[id] = std::move(*desc);
			} else {
				anyFailure = true;
				_backendInitError = ScalingError::EffectCompileFailed;
				if (!_backendInitContext.empty()) _backendInitContext += "; ";
				_backendInitContext += effects[id].name;
			}
		}, effectCount);
	});

	if (anyFailure) {
		return nullptr;
	}

	if (effectCount > 1) {
		Logger::Get().Info(fmt::format("编译着色器总计用时 {} 毫秒", duration / 1000.0f));
	}

	_ReleaseNgxConsumers();
	_effectDrawers.resize(effectCount);
	_nativeEffectBackends.resize(effectCount);
	_dlssFgConsecutiveFailures = 0;
	_dlssFgRecoveryAttempts = 0;
	std::optional<DLSSFrameGenerationSettings> dlssFrameGenerationSettings;

	ID3D11Texture2D* inOutTexture = _frameSource->GetPipelineTexture();
	if (!_frameSource->PrepareHdrOutputForResize()) {
		Logger::Get().Error("准备 HDR 输出尺寸失败");
		return nullptr;
	}
	inOutTexture = _frameSource->GetPipelineTexture();
	HdrFrame initialHdrFrame{};
	if (options.IsHdrCompatibilityEnabled()) {
		initialHdrFrame.texture = _frameSource->GetPipelineTexture();
		initialHdrFrame.metadata = _frameSource->GetHdrFrameMetadata();
		initialHdrFrame.workingFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
	}
	for (uint32_t i = 0; i < effectCount; ++i) {
		HdrEffectBoundaryContext initialHdrBoundary{};
		if (options.IsHdrCompatibilityEnabled()) {
			const EffectOption& effectOption = effects[i];
			initialHdrBoundary = HdrEffectBoundary::Prepare(
				true, initialHdrFrame,
				GetHdrRoutesForEffect(effectOption, options.IsHdrCompatibilityEnabled()),
				initialHdrFrame.metadata.color);
			_effectDrawers[i].SetHdrBoundary(initialHdrBoundary);
		}
		if (!_effectDrawers[i].Initialize(
			_effectDescs[i],
			effects[i],
			_backendResources,
			_backendDescriptorStore,
			&inOutTexture
		)) {
			Logger::Get().Error(fmt::format("初始化效果#{} ({}) 失败", i, effects[i].name));
			_backendInitError = ScalingError::EffectResourceFailed;
			_backendInitContext = effects[i].name;
			return nullptr;
		}

		NativeEffectBackendResult nativeBackend = CreateNativeEffectBackend(
			effects[i].name,
			effects[i],
			_backendResources,
			_ngxD3D12Core,
			_effectDrawers[i].GetTexture(0),
			_effectDrawers[i].GetOutputTexture());
		if (nativeBackend.recognized && !nativeBackend.backend) {
			_backendInitError = nativeBackend.error == ScalingError::NoError
				? ScalingError::NativeEffectInitFailed : nativeBackend.error;
			_backendInitContext = effects[i].name;
			return nullptr;
		}
		_nativeEffectBackends[i] = std::move(nativeBackend.backend);
		if (_nativeEffectBackends[i] && options.IsHdrCompatibilityEnabled()) {
			_nativeEffectBackends[i]->SetHdrBoundary(std::move(initialHdrBoundary));
			ConfigureHdrBackendProtocol(effects[i].name, *_nativeEffectBackends[i],
				initialHdrFrame.metadata, true);
		}
		if (effects[i].name == "DLSSNR\\DLSSNR_AI_Filter" && !_nativeEffectBackends[i] &&
			options.reportErrorDetails) {
			options.reportErrorDetails(ScalingWindow::Get().SrcTracker().Handle(),
				ScalingError::DlssNrUnavailable, effects[i].name, 0);
		}

		if (IsDLSSFrameGenerationEffect(effects[i].name)) {
			if (dlssFrameGenerationSettings) {
				Logger::Get().Error("Only one DLSS Frame Generation effect is allowed");
				return nullptr;
			}
			auto getParameter = [&](std::string_view name, float defaultValue) {
				auto it = effects[i].parameters.find(std::string(name));
				return it == effects[i].parameters.end() ? defaultValue : it->second;
			};
			dlssFrameGenerationSettings = DLSSFrameGenerationSettings{
				.multiplier = std::clamp(
					(uint32_t)std::lround(getParameter("multiplier", 2.0f)),
					2u, 4u),
				.motionVectorQuality = static_cast<NvidiaOpticalFlowQuality>(
					std::clamp(static_cast<int>(std::lround(
						getParameter("motionVectorQuality", 2.0f))), 0, int(NVIDIA_OPTICAL_FLOW_MAX_QUALITY)))
			};
		}

		// 释放 CSO 内存，不再需要它们
		for (EffectPassDesc& passDesc : _effectDescs[i].passes) {
			passDesc.cso = nullptr;
		}
	}

	_BuildEffectParameterRuntimeInfos();
	_effectInputRevisions.assign(effectCount, 0);

	if (_ShouldAppendBicubic(inOutTexture)) {
		if (!_AppendBicubic(&inOutTexture)) {
			Logger::Get().Error("_AppendBicubic 失败");
			return nullptr;
		}
	}

	if (dlssFrameGenerationSettings &&
		!_InitializeDLSSFrameGenerator(
			inOutTexture, *dlssFrameGenerationSettings)) {
		return nullptr;
	}

	_UpdateActiveEffectDescs();
	_UpdateHdrEffectBoundaryContexts();

	// 初始化所有效果共用的动态常量缓冲区
	for (const EffectDesc& effectDesc : _effectDescs) {
		if (effectDesc.flags & EffectFlags::UseDynamic) {
			D3D11_BUFFER_DESC bd{
				.ByteWidth = 16,	// 只用 4 个字节
				.Usage = D3D11_USAGE_DYNAMIC,
				.BindFlags = D3D11_BIND_CONSTANT_BUFFER,
				.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE
			};

			HRESULT hr = _backendResources.GetD3DDevice()->CreateBuffer(&bd, nullptr, _dynamicCB.put());
			if (FAILED(hr)) {
				Logger::Get().ComError("CreateBuffer 失败", hr);
				return nullptr;
			}

			break;
		}
	}

	return inOutTexture;
}

void Renderer::_UpdateHdrEffectBoundaryContexts() noexcept {
	const ScalingOptions& options = ScalingWindow::Get().Options();
	if (!options.IsHdrCompatibilityEnabled()) {
		for (size_t i = 0; i < _effectDrawers.size(); ++i) {
			_effectDrawers[i].SetHdrBoundary({});
			if (i < _nativeEffectBackends.size() && _nativeEffectBackends[i]) {
				_nativeEffectBackends[i]->SetHdrBoundary({});
			}
		}
		return;
	}
	if (!_frameSource) {
		return;
	}

	HdrFrame inputFrame{};
	inputFrame.texture = _frameSource->GetPipelineTexture();
	inputFrame.metadata = _frameSource->GetHdrFrameMetadata();
	inputFrame.workingFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

	for (size_t i = 0; i < _effectDrawers.size(); ++i) {
		const EffectOption& effectOption = _runtimeEffectOptions[i];
		const HdrFormatRoutes routes = GetHdrRoutesForEffect(effectOption, options.IsHdrCompatibilityEnabled());
		HdrEffectBoundaryContext context = HdrEffectBoundary::Prepare(
			true, inputFrame, routes, inputFrame.metadata.color);
		if (effectOption.name == "DLSSNR\\DLSSNR_AI_Filter") {
			D3D11_TEXTURE2D_DESC sourceDesc{};
			inputFrame.texture->GetDesc(&sourceDesc);
			const HdrTransformParameters transform = HdrColorTransform::ForFrame(
				inputFrame.metadata.color);
			const HdrColorTransform::ShoulderCurve boundedCurve =
				HdrColorTransform::BuildShoulderCurve(
					transform, HdrColorTransform::BoundedRouteHighlightTarget);
			Logger::Get().Info(fmt::format(
				"DLSSNR HDR boundary: enabled={} route={} profile={} requiresBounded={} "
				"normalizationScale={} sourceFormat={} source={}x{} "
				"sdrWhite={:.1f} peak={:.1f} curvePeak={:.3f} curveTarget={:.3f} "
				"curveK={:.4f} curveTailSlope={:.4f} colorInferred={}",
				options.IsHdrCompatibilityEnabled(),
				context.SelectedRoute() ? context.SelectedRoute()->Id() : "(none)",
				ToString(context.plan.profile), context.plan.requiresBoundedMapping,
				context.plan.normalizationScale, static_cast<uint32_t>(sourceDesc.Format),
				sourceDesc.Width, sourceDesc.Height,
				transform.sdrWhiteNits, transform.hdrPeakNits,
				boundedCurve.peak, boundedCurve.target, boundedCurve.k,
				boundedCurve.tailSlope,
				inputFrame.metadata.color.isInferred));
		}
		_effectDrawers[i].SetHdrBoundary(context);
		if (i < _nativeEffectBackends.size() && _nativeEffectBackends[i]) {
			_nativeEffectBackends[i]->SetHdrBoundary(std::move(context));
			ConfigureHdrBackendProtocol(_runtimeEffectOptions[i].name,
				*_nativeEffectBackends[i], inputFrame.metadata, true);
		}
	}
}

void Renderer::_BuildEffectParameterRuntimeInfos() noexcept {
	_effectParameterRuntimeInfos.clear();
	_effectParameterRuntimeInfos.resize(_effectDescs.size());
	const bool hdrEnabled = ScalingWindow::Get().Options().IsHdrCompatibilityEnabled();

	for (uint32_t effectIdx = 0; effectIdx < _effectDescs.size(); ++effectIdx) {
		const EffectDesc& desc = _effectDescs[effectIdx];
		const EffectOption& option = _runtimeEffectOptions[effectIdx];
		std::vector<EffectParameterRuntimeInfo>& infos =
			_effectParameterRuntimeInfos[effectIdx];
		infos.reserve(desc.params.size());

		for (const EffectParameterDesc& parameter : desc.params) {
			EffectParameterRuntimeInfo info{ .name = parameter.name };
			const bool isHdrOnlyParameter =
				(((option.name == "CAS\\CAS" || option.name == "CAS\\CAS_Scaling") &&
					parameter.name == "hdrFormat") ||
				 (option.name == "DLSSNR\\DLSSNR_AI_Filter" &&
					parameter.name == "experimentalHdrPath"));
			if (!hdrEnabled && isHdrOnlyParameter) {
				info.applyMode = EffectParameterApplyMode::Unavailable;
				info.restartReason = EffectParameterRestartReason::None;
			} else if (isHdrOnlyParameter) {
				info.applyMode = EffectParameterApplyMode::RestartRequired;
				info.restartReason = EffectParameterRestartReason::ResourceRecreation;
			} else if (IsFrameRateFilterEffect(option.name)) {
				info.applyMode = EffectParameterApplyMode::Live;
				info.restartReason = EffectParameterRestartReason::None;
			} else if (IsDLSSFrameGenerationEffect(option.name)) {
				info.restartReason = parameter.name == "motionVectorQuality"
					? EffectParameterRestartReason::FrameGuidance
					: EffectParameterRestartReason::FrameGeneration;
			} else if (IsXeSSFrameGenerationEffect(option.name)) {
				info.restartReason = EffectParameterRestartReason::FrameGeneration;
			} else if (_nativeEffectBackends[effectIdx]) {
				const NativeEffectBackend& backend = *_nativeEffectBackends[effectIdx];
				info.applyMode = backend.GetParameterApplyMode(parameter.name);
				info.restartReason = info.applyMode == EffectParameterApplyMode::Live
					? EffectParameterRestartReason::None
					: backend.GetParameterRestartReason(parameter.name);
			} else if (option.name == "DLSSNR\\DLSSNR_AI_Filter") {
				info.applyMode = EffectParameterApplyMode::Unavailable;
			} else if (desc.flags & EffectFlags::InlineParams) {
				info.restartReason = EffectParameterRestartReason::InlineParameters;
			} else {
				info.applyMode = EffectParameterApplyMode::Live;
				info.restartReason = EffectParameterRestartReason::None;
			}
			// The running backend owns live support and history reset semantics.
			// DLSSNR observes inputRevision without tearing down the effect group.
			infos.push_back(std::move(info));
		}
	}
}

bool Renderer::QueueEffectParameterUpdate(
	uint32_t effectIdx,
	uint32_t parameterIdx,
	float value,
	bool waitForOverlaySave
) noexcept {
	(void)waitForOverlaySave;
	if (!std::isfinite(value) || !_backendThreadDispatcher ||
		effectIdx >= _effectParameterRuntimeInfos.size() ||
		parameterIdx >= _effectParameterRuntimeInfos[effectIdx].size()) {
		return false;
	}
	const EffectParameterRuntimeInfo& info =
		_effectParameterRuntimeInfos[effectIdx][parameterIdx];
	if (info.applyMode != EffectParameterApplyMode::Live) {
		return false;
	}
	const uint64_t key = (uint64_t(effectIdx) << 32) | parameterIdx;
	bool enqueueWake = false;
	{
		std::scoped_lock lock(_effectParameterMailboxMutex);
		_effectParameterMailbox[key] = PendingEffectParameterUpdate{
			.effectIdx = effectIdx,
			.parameterIdx = parameterIdx,
			.value = value
		};
		if (!_effectParameterWakeQueued) {
			_effectParameterWakeQueued = true;
			enqueueWake = true;
		}
	}

	if (!enqueueWake) {
		return true;
	}

	if (_backendThreadDispatcher.TryEnqueue([this]() {
		std::unordered_map<uint64_t, PendingEffectParameterUpdate> updates;
		{
			std::scoped_lock lock(_effectParameterMailboxMutex);
			updates.swap(_effectParameterMailbox);
			_effectParameterWakeQueued = false;
		}
		for (auto& [key, update] : updates) {
			auto existing = std::ranges::find_if(
				_pendingEffectParameterUpdates,
				[key](const PendingEffectParameterUpdate& item) {
					return ((uint64_t(item.effectIdx) << 32) | item.parameterIdx) == key;
				});
			if (existing == _pendingEffectParameterUpdates.end()) {
				_pendingEffectParameterUpdates.push_back(std::move(update));
			} else {
				*existing = std::move(update);
			}
		}
	})) {
		return true;
	}

	std::scoped_lock lock(_effectParameterMailboxMutex);
	_effectParameterWakeQueued = false;
	return false;
}

void Renderer::_ApplyPendingEffectParameters() noexcept {
	std::vector<PendingEffectParameterUpdate> updates;
	updates.swap(_pendingEffectParameterUpdates);
	if (updates.empty()) {
		return;
	}

	std::vector<std::vector<PendingEffectParameterUpdate>> byEffect(
		_runtimeEffectOptions.size());
	for (PendingEffectParameterUpdate& update : updates) {
		if (update.effectIdx < byEffect.size()) {
			byEffect[update.effectIdx].push_back(std::move(update));
		}
	}

	bool anySucceeded = false;
	for (uint32_t effectIdx = 0; effectIdx < byEffect.size(); ++effectIdx) {
		std::vector<PendingEffectParameterUpdate>& effectUpdates = byEffect[effectIdx];
		if (effectUpdates.empty()) {
			continue;
		}

		EffectOption candidate = _runtimeEffectOptions[effectIdx];
		std::vector<std::string> changedNames;
		changedNames.reserve(effectUpdates.size());
		bool valid = effectIdx < _effectDescs.size() &&
			effectIdx < _effectParameterRuntimeInfos.size();
		for (PendingEffectParameterUpdate& update : effectUpdates) {
			if (!valid || update.parameterIdx >= _effectDescs[effectIdx].params.size() ||
				update.parameterIdx >= _effectParameterRuntimeInfos[effectIdx].size()) {
				valid = false;
				break;
			}
			const EffectParameterDesc& parameter =
				_effectDescs[effectIdx].params[update.parameterIdx];
			const EffectParameterRuntimeInfo& info =
				_effectParameterRuntimeInfos[effectIdx][update.parameterIdx];
			if (info.name != parameter.name ||
				info.applyMode != EffectParameterApplyMode::Live ||
				!std::isfinite(update.value)) {
				valid = false;
				break;
			}
			update.value = NormalizeEffectParameterValue(parameter, update.value);
			candidate.parameters[parameter.name] = update.value;
			changedNames.push_back(parameter.name);
		}

		bool succeeded = false;
		const bool isFrameRateFilter = IsFrameRateFilterEffect(candidate.name);
		if (valid && isFrameRateFilter) {
			_runtimeEffectOptions[effectIdx] = std::move(candidate);
			_UpdateFrameRateLimits();
			succeeded = true;
		} else if (valid && _nativeEffectBackends[effectIdx]) {
			succeeded = _nativeEffectBackends[effectIdx]->ApplyLiveParameters(
				candidate, changedNames);
			if (succeeded) {
				_runtimeEffectOptions[effectIdx] = std::move(candidate);
			}
		} else if (valid &&
			!(_effectDescs[effectIdx].flags & EffectFlags::InlineParams)) {
			succeeded = _effectDrawers[effectIdx].UpdateParameters(
				_effectDescs[effectIdx], candidate, _backendResources);
			if (succeeded) {
				_runtimeEffectOptions[effectIdx] = std::move(candidate);
			}
		}

		if (!succeeded) {
			Logger::Get().Error(fmt::format(
				"Live effect parameter update rejected: effect#{} ({})",
				effectIdx, _runtimeEffectOptions[effectIdx].name));
			const auto& options = ScalingWindow::Get().Options();
			if (options.reportErrorDetails) {
				std::string context = _runtimeEffectOptions[effectIdx].name;
				for (const auto& name : changedNames) context += " / " + name;
				options.reportErrorDetails(ScalingWindow::Get().SrcTracker().Handle(),
					ScalingError::EffectParameterLiveFailed, context, 0);
			}
		}
		anySucceeded |= succeeded;
		if (succeeded && !isFrameRateFilter) {
			// A forced render can reuse the same captured frame ID. Tell every
			// later native effect that its actual input changed so a duplicate-
			// frame cache cannot hide an upstream live shader/backend update.
			for (size_t i = effectIdx + 1; i < _effectInputRevisions.size(); ++i) {
				++_effectInputRevisions[i];
			}
		}
	}

	if (anySucceeded) {
		ScalingWindow::Get().Options().parameterSession->Applied(_runtimeEffectOptions);
		_forceNextRender = true;
	}
}

void Renderer::_UpdateActiveEffectDescs() noexcept {
	const uint32_t effectCount = (uint32_t)_effectDescs.size();
	const uint32_t drawerCount = (uint32_t)_effectDrawers.size();

	_activeEffectDescs.resize(drawerCount);

	for (uint32_t i = 0; i < effectCount; ++i) {
		_activeEffectDescs[i] = &_effectDescs[i];
	}

	if (drawerCount > effectCount) {
		// 已追加 Bicubic
		assert(drawerCount == effectCount + 1);
		_activeEffectDescs[effectCount] = &bicubicDesc;
	}
}

bool Renderer::_ShouldAppendBicubic(ID3D11Texture2D* outTexture) noexcept {
	const ScalingOptions& options = ScalingWindow::Get().Options();

	D3D11_TEXTURE2D_DESC texDesc;
	outTexture->GetDesc(&texDesc);
	const SIZE lastOutputSize = { (LONG)texDesc.Width, (LONG)texDesc.Height };
	const SIZE rendererSize = Win32Helper::GetSizeOfRect(ScalingWindow::Get().RendererRect());

	if (options.IsWindowedMode()) {
		// 窗口模式缩放时使用 Bicubic 放大。Bicubic (B=0, C=0.5) 的锐利度和 Lanczos 相差无几
		return lastOutputSize != rendererSize;
	} else {
		// 输出尺寸大于交换链尺寸则需要降采样
		return lastOutputSize.cx > rendererSize.cx || lastOutputSize.cy > rendererSize.cy;
	}
}

bool Renderer::_AppendBicubic(ID3D11Texture2D** inOutTexture) noexcept {
	const ScalingOptions& options = ScalingWindow::Get().Options();

	const EffectOption bicubicOption{
		.name = "Bicubic",
		.parameters{
			{"paramB", 0.0f},
			{"paramC", 0.5f}
		},
		.scalingType = options.IsWindowedMode() ? ScalingType::Fill : ScalingType::Fit
	};

	if (bicubicDesc.name.empty()) {
		// 参数不会改变，因此可以内联
		std::optional<EffectDesc> desc = CompileEffect(bicubicOption, true, true);
		if (!desc) {
			Logger::Get().Error("编译降采样效果失败");
			return false;
		}

		bicubicDesc = std::move(*desc);
	}

	EffectDrawer& bicubicDrawer = _effectDrawers.emplace_back();
	if (!bicubicDrawer.Initialize(
		bicubicDesc,
		bicubicOption,
		_backendResources,
		_backendDescriptorStore,
		inOutTexture
	)) {
		Logger::Get().Error("初始化降采样效果失败");
		return false;
	}

	return true;
}

ID3D11Texture2D* Renderer::_ResizeEffects() noexcept {
	const std::vector<EffectOption>& effects = _runtimeEffectOptions;
	assert(!effects.empty());
	const uint32_t effectCount = (uint32_t)effects.size();
	if (!_DrainNgxConsumers()) {
		Logger::Get().Error("Drain NGX consumers before resize failed");
		return nullptr;
	}

	ID3D11Texture2D* inOutTexture = _frameSource->GetPipelineTexture();
	D3D11_TEXTURE2D_DESC sourceDesc{};
	if (!_frameSource->PrepareHdrOutputForResize()) {
		Logger::Get().Error("准备 HDR 输出尺寸失败");
		return nullptr;
	}
	inOutTexture = _frameSource->GetPipelineTexture();
	inOutTexture->GetDesc(&sourceDesc);
	FrameGuidanceRequirements guidanceRequirements =
		CollectFrameGuidanceRequirements(
			_nativeEffectBackends, _dlssFrameGenerator.get(), _xessMotionRequest);
	if (_frameGuidanceService.IsInitialized()) {
		const FrameGuidanceExtent sourceExtent{
			sourceDesc.Width, sourceDesc.Height
		};
		if (!_frameGuidanceService.Resize(sourceExtent, guidanceRequirements)) {
			Logger::Get().Error("Resize Frame Guidance service failed");
			return nullptr;
		}
		// Resize only reallocates provider resources. Re-seed them from the
		// last real capture instead of manufacturing a color-less pseudo-frame.
		if (_capturedFrameId != 0 && !_frameGuidanceService.BeginFrame(
			_capturedFrameId, inOutTexture, guidanceRequirements
		).IsValidFor(_capturedFrameId, sourceExtent)) {
			Logger::Get().Error("Produce Frame Guidance after resize failed");
			return nullptr;
		}
	}
	for (uint32_t i = 0; i < effectCount; ++i) {
		if (!_effectDrawers[i].ResizeTextures(
			_effectDescs[i],
			effects[i],
			_backendResources,
			&inOutTexture
		)) {
			Logger::Get().Error(fmt::format("更改效果#{} ({}) 尺寸失败", i, effects[i].name));
			return nullptr;
		}

		if (_nativeEffectBackends[i] && !_nativeEffectBackends[i]->Resize(
			_backendResources, _effectDrawers[i].GetTexture(0), _effectDrawers[i].GetOutputTexture())) {
			if (effects[i].name == "DLSSNR\\DLSSNR_AI_Filter") {
				const char status[] =
					"DLSSNR STATUS: Feature=18 created=false stage=resize "
					"fallback=pass-through";
				Logger::Get().Warn(status);
				OutputDebugStringA(status);
				_nativeEffectBackends[i].reset();
				continue;
			}
			Logger::Get().Error(fmt::format("Resize native effect {} failed", effects[i].name));
			return nullptr;
		}
	}

	// 处理追加的 Bicubic
	bool changed = false;
	if (_ShouldAppendBicubic(inOutTexture)) {
		if (_effectDrawers.size() > effectCount) {
			const EffectOption bicubicOption{
				.name = "Bicubic",
				.parameters{
					{"paramB", 0.0f},
					{"paramC", 0.5f}
				},
				.scalingType = ScalingWindow::Get().Options().IsWindowedMode()
					? ScalingType::Fill : ScalingType::Fit
			};

			if (!_effectDrawers.back().ResizeTextures(
				bicubicDesc,
				bicubicOption,
				_backendResources,
				&inOutTexture
			)) {
				Logger::Get().Error("更改效果 Bicubic 尺寸失败");
				return nullptr;
			}
		} else {
			_AppendBicubic(&inOutTexture);
			changed = true;
		}
	} else {
		if (_effectDrawers.size() > effectCount) {
			_effectDrawers.resize(effectCount);
			changed = true;
		}
	}

	if (changed) {
		_UpdateActiveEffectDescs();
		_overlayDrawer.UpdateAfterActiveEffectsChanged();

		if (_effectsProfiler.IsProfiling()) {
			uint32_t passCount = 0;
			for (const EffectDesc* desc : _activeEffectDescs) {
				passCount += (uint32_t)desc->passes.size();
			}
			_effectsProfiler.SetPassCount(_backendResources.GetD3DDevice(), passCount);
		}
	}
	_UpdateHdrEffectBoundaryContexts();

	if (_dlssFrameGenerator) {
		const DLSSFrameGenerationSettings settings =
			_dlssFrameGenerator->Settings();
		if (!_InitializeDLSSFrameGenerator(inOutTexture, settings)) {
			return nullptr;
		}
	}

	return inOutTexture;
}

bool Renderer::_InitializeDLSSFrameGenerator(
	ID3D11Texture2D* input,
	const DLSSFrameGenerationSettings& settings
) noexcept {
	if (_dlssFrameGenerator) {
		if (!_dlssFrameGenerator->Drain()) {
			Logger::Get().Warn("Drain old DLSSFG feature before rebuild failed");
			return false;
		}
		_dlssFrameGenerator.reset();
	}
	D3D11_TEXTURE2D_DESC sourceDesc{};
	_frameSource->GetPipelineTexture()->GetDesc(&sourceDesc);
	auto frameGenerator = std::make_unique<DLSSFrameGenerator>();
	if (!frameGenerator->Initialize(
		_backendResources, _ngxD3D12Core, input,
		{ sourceDesc.Width, sourceDesc.Height }, settings)) {
		_backendInitError = NgxRuntimeGuard::IsFaulted() ?
			ScalingError::NgxRestartRequired : ScalingError::FrameGenerationInitFailed;
		_backendInitContext = "DLSSFG\\DLSS_FrameGeneration";
		return false;
	}
	_captureCadence.Reset();
	_captureSequence = 0;
	_captureCadenceQueueWait = {};
	_synchronousPresentInterval = _captureCadence.Interval(frameGenerator->Multiplier(), _baseFrameRateLimit);
	const double refreshRate = GetDisplayRefreshRate(ScalingWindow::Get().Handle());
	const double theoreticalOutput = _frameRateFilterTarget > 0.0f ?
		double(_frameRateFilterTarget) * frameGenerator->Multiplier() : 0.0;
	Logger::Get().Info(fmt::format(
		"DLSSFG capacity: displayRefresh={:.3f} Hz baseTarget={} requested={}x "
		"sdkMax={}x active={}x theoreticalOutput={}",
		refreshRate,
		_frameRateFilterTarget > 0.0f ?
			fmt::format("{:.3f} FPS", _frameRateFilterTarget) : "unlimited",
		settings.multiplier, frameGenerator->MaxSupportedMultiplier(),
		frameGenerator->Multiplier(),
		theoreticalOutput > 0.0 ?
			fmt::format("{:.3f} FPS", theoreticalOutput) : "capture-driven"));
	if (refreshRate > 0.0 && theoreticalOutput > refreshRate * 1.05) {
		Logger::Get().Warn(fmt::format(
			"DLSSFG requested output {:.1f} FPS exceeds the {:.1f} Hz display; "
			"higher multipliers may reduce real-frame rate without increasing "
			"visible presentation rate",
			theoreticalOutput, refreshRate));
	}
	_dlssFgDiagnosticsStart = {};
	_dlssFgCapturedFrameCount = 0;
	_dlssFgPresentedFrameCount = 0;
	_dlssFgGeneratedPublishSuccess = 0;
	_dlssFgGeneratedPublishFailure = 0;
	_dlssFgRealPublishSuccess = 0;
	_dlssFgRealPublishFailure = 0;
	_dlssFrameGenerator = std::move(frameGenerator);
	return true;
}

void Renderer::_HandleDLSSFrameGenerationFailure(ID3D11Texture2D* input) noexcept {
	if (!_dlssFrameGenerator) {
		return;
	}

	++_dlssFgConsecutiveFailures;
	if (_dlssFgConsecutiveFailures == 1) {
		Logger::Get().Warn(
			"DLSS Frame Generation failed; resetting history and presenting real frames");
		_dlssFrameGenerator->RequestHistoryReset();
		return;
	}

	if (_dlssFgRecoveryAttempts == 0) {
		++_dlssFgRecoveryAttempts;
		const DLSSFrameGenerationSettings settings =
			_dlssFrameGenerator->Settings();
		Logger::Get().Warn("DLSS Frame Generation failed again; recreating the feature once");
		if (_InitializeDLSSFrameGenerator(input, settings)) {
			_dlssFgConsecutiveFailures = 0;
			return;
		}
	}

	_DisableDLSSFrameGenerationForSession();
}

void Renderer::_DisableDLSSFrameGenerationForSession() noexcept {
	if (_dlssFrameGenerator && !_dlssFrameGenerator->Drain()) {
		Logger::Get().Warn("Drain DLSSFG queue before disabling failed");
	}
	_dlssFrameGenerator.reset();
	_synchronousPresentInterval = {};
	if (_frontEdgeSyncEnabled) {
		// A failed FG session no longer reaches the FG input gate.
		_stepTimer.Initialize(0, static_cast<float>(_FrontEdgeFrameRate()));
	}
	Logger::Get().Error(
		"DLSS Frame Generation was disabled for this scaling session after repeated failures");
	const auto& options = ScalingWindow::Get().Options();
	if (options.reportErrorDetails) options.reportErrorDetails(
		ScalingWindow::Get().SrcTracker().Handle(), ScalingError::FrameGenerationDisabled,
		"DLSSFG\\DLSS_FrameGeneration", 0);
}

bool Renderer::_DrainNgxConsumers() noexcept {
	bool succeeded = true;
	if (_dlssFrameGenerator && !_dlssFrameGenerator->Drain()) {
		Logger::Get().Warn("Drain DLSSFG queue failed");
		succeeded = false;
	}
	for (const auto& backend : _nativeEffectBackends) {
		if (backend && !backend->Drain()) {
			Logger::Get().Warn("Drain native NGX backend queue failed");
			succeeded = false;
		}
	}
	return succeeded;
}

void Renderer::_ReleaseNgxConsumers() noexcept {
	_DrainNgxConsumers();
	_dlssFrameGenerator.reset();
	_nativeEffectBackends.clear();
}

void Renderer::_UpdateDestRect() noexcept {
	const RECT& rendererRect = ScalingWindow::Get().RendererRect();
	DestAlignment alignment = ScalingWindow::Get().Options().destAlignment;

	LONG destWidth;
	LONG destHeight;
	{
		D3D11_TEXTURE2D_DESC desc;
		_frontendSharedTextures[0]->GetDesc(&desc);
		destWidth = (LONG)desc.Width;
		destHeight = (LONG)desc.Height;
	}

	using enum DestAlignment;

	if (alignment == LeftTop || alignment == Left || alignment == LeftBottom) {
		_destRect.left = 0;
		_destRect.right = destWidth;
	} else if (alignment == Top || alignment == Center || alignment == Bottom) {
		_destRect.left = (rendererRect.left + rendererRect.right - destWidth) / 2;
		_destRect.right = _destRect.left + destWidth;
	} else {
		_destRect.left = rendererRect.right - destWidth;
		_destRect.right = rendererRect.right;
	}

	if (alignment == LeftTop || alignment == Top || alignment == RightTop) {
		_destRect.top = 0;
		_destRect.bottom = destHeight;
	} else if (alignment == Left || alignment == Center || alignment == Right) {
		_destRect.top = (rendererRect.top + rendererRect.bottom - destHeight) / 2;
		_destRect.bottom = _destRect.top + destHeight;
	} else {
		_destRect.top = rendererRect.bottom - destHeight;
		_destRect.bottom = rendererRect.bottom;
	}

	assert(_destRect.left + destWidth == _destRect.right);
	assert(_destRect.top + destHeight == _destRect.bottom);
}

HANDLE Renderer::_CreateSharedTexture(ID3D11Texture2D* effectsOutput) noexcept {
	D3D11_TEXTURE2D_DESC desc;
	effectsOutput->GetDesc(&desc);
	if (_hdrPresentationTexture) {
		_backendDescriptorStore.RemoveCache(_hdrPresentationTexture.get());
		_hdrPresentationTexture = nullptr;
	}
	if (_dlssFgNormalizedInput) {
		_backendDescriptorStore.RemoveCache(_dlssFgNormalizedInput.get());
		_dlssFgNormalizedInput = nullptr;
	}
	if (_dlssFgCanonicalGenerated) {
		_backendDescriptorStore.RemoveCache(_dlssFgCanonicalGenerated.get());
		_dlssFgCanonicalGenerated = nullptr;
	}
	const bool hdrEnabled = ScalingWindow::Get().Options().IsHdrCompatibilityEnabled();
	const bool xessFgHdrTerminal = hdrEnabled && _isXeSSFrameGenerationActive;
	const DXGI_FORMAT presentationFormat = hdrEnabled
		? (xessFgHdrTerminal ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_R16G16B16A16_FLOAT)
		: DXGI_FORMAT_R8G8B8A8_UNORM;
	if (hdrEnabled && desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT) {
		Logger::Get().Error("HDR 发布要求 canonical FP16 输出");
		return NULL;
	}
	SIZE textureSize = { (LONG)desc.Width, (LONG)desc.Height };
	_dlssFgHdrNormalizationScale = 1.0f;
	if (hdrEnabled && _dlssFrameGenerator && _frameSource) {
		const auto color = _frameSource->GetHdrFrameMetadata().color;
		if (color.IsValid() && color.sdrWhiteNits > 80.0f) {
			_dlssFgHdrNormalizationScale = 80.0f / color.sdrWhiteNits;
			_dlssFgNormalizedInput = DirectXHelper::CreateTexture2D(
				_backendResources.GetD3DDevice(), DXGI_FORMAT_R16G16B16A16_FLOAT,
				textureSize.cx, textureSize.cy,
				D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
			_dlssFgCanonicalGenerated = DirectXHelper::CreateTexture2D(
				_backendResources.GetD3DDevice(), DXGI_FORMAT_R16G16B16A16_FLOAT,
				textureSize.cx, textureSize.cy,
				D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
			if (!_dlssFgNormalizedInput || !_dlssFgCanonicalGenerated) {
				Logger::Get().Error("Create DLSSFG HDR normalization textures failed");
				return NULL;
			}
			Logger::Get().Info(fmt::format(
				"DLSSFG HDR bounded bridge enabled: sdrWhiteNits={:.3f} inputScale={:.6f}",
				color.sdrWhiteNits, _dlssFgHdrNormalizationScale));
		}
	}
	_sharedTextureSlotCount = _dlssFrameGenerator ?
		std::clamp(_dlssFrameGenerator->Multiplier(), 2u, MAX_SHARED_TEXTURE_SLOTS) : 1u;
	_sharedTextureGeneration.fetch_add(1, std::memory_order_release);
	_pendingDLSSFGFrontendFrames.store(0, std::memory_order_release);
	_nextBackendSharedTextureSlot = 0;
	_latestSharedTextureSlot.store(0, std::memory_order_relaxed);
	for (uint32_t i = 0; i < MAX_SHARED_TEXTURE_SLOTS; ++i) {
		_backendSharedTextureMutexes[i] = nullptr;
		_backendSharedTextures[i] = nullptr;
		_backendSharedMotionTextureMutexes[i] = nullptr;
		_backendSharedMotionTextures[i] = nullptr;
		_sharedTextureHandles[i] = nullptr;
		_sharedMotionTextureHandles[i].reset();
		_sharedTextureAvailableEvents[i].reset();
		_sharedTextureMutexKeys[i].store(0, std::memory_order_relaxed);
		_sharedMotionFrameIds[i].store(0, std::memory_order_relaxed);
		_sharedMotionValid[i].store(false, std::memory_order_relaxed);
		_sharedMotionReset[i].store(true, std::memory_order_relaxed);
		_sharedTextureCaptureSequences[i].store(0, std::memory_order_relaxed);
		_sharedTextureFrameIds[i].store(0, std::memory_order_relaxed);
		_sharedTextureResourceGenerations[i].store(
			_sharedTextureGeneration.load(std::memory_order_relaxed), std::memory_order_relaxed);
		_sharedTextureTimestamps[i].store(0, std::memory_order_relaxed);
		_sharedFrameMetadata[i] = {};
	}

	for (uint32_t i = 0; i < _sharedTextureSlotCount; ++i) {
		_backendSharedTextures[i] = DirectXHelper::CreateTexture2D(
			_backendResources.GetD3DDevice(),
			presentationFormat,
			textureSize.cx,
			textureSize.cy,
			D3D11_BIND_SHADER_RESOURCE,
			D3D11_USAGE_DEFAULT,
			D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX);
		if (!_backendSharedTextures[i]) {
			Logger::Get().Error("Create shared presentation texture failed");
			return NULL;
		}
		_backendSharedTextureMutexes[i] =
			_backendSharedTextures[i].try_as<IDXGIKeyedMutex>();
		if (!_backendSharedTextureMutexes[i]) {
			Logger::Get().Error("Get shared presentation keyed mutex failed");
			return NULL;
		}

		winrt::com_ptr<IDXGIResource> sharedDxgiRes =
			_backendSharedTextures[i].try_as<IDXGIResource>();
		const HRESULT hr = sharedDxgiRes->GetSharedHandle(
			&_sharedTextureHandles[i]);
		if (FAILED(hr) || !_sharedTextureHandles[i]) {
			Logger::Get().ComError("Get shared presentation handle failed", hr);
			return NULL;
		}

		if (_xessMotionRequest.method != OpticalFlowMethod::None) {
			D3D11_TEXTURE2D_DESC motionDesc{};
			motionDesc.Width = desc.Width;
			motionDesc.Height = desc.Height;
			motionDesc.MipLevels = 1;
			motionDesc.ArraySize = 1;
			motionDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
			motionDesc.SampleDesc.Count = 1;
			motionDesc.Usage = D3D11_USAGE_DEFAULT;
			motionDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			motionDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX |
				D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
			HRESULT motionHr = _backendResources.GetD3DDevice()->CreateTexture2D(
				&motionDesc, nullptr, _backendSharedMotionTextures[i].put());
			if (FAILED(motionHr)) {
				Logger::Get().ComError("Create XeSSFG shared motion texture failed", motionHr);
				return NULL;
			}
			_backendSharedMotionTextureMutexes[i] =
				_backendSharedMotionTextures[i].try_as<IDXGIKeyedMutex>();
			winrt::com_ptr<IDXGIResource1> motionDxgiResource =
				_backendSharedMotionTextures[i].try_as<IDXGIResource1>();
			if (!_backendSharedMotionTextureMutexes[i] || !motionDxgiResource) {
				Logger::Get().Error("Create XeSSFG shared motion synchronization failed");
				return NULL;
			}
			HANDLE motionHandle = nullptr;
			motionHr = motionDxgiResource->CreateSharedHandle(
				nullptr, GENERIC_ALL, nullptr, &motionHandle);
			if (FAILED(motionHr) || !motionHandle) {
				Logger::Get().ComError("Create XeSSFG motion NT handle failed", motionHr);
				return NULL;
			}
			_sharedMotionTextureHandles[i].reset(motionHandle);
		}
		_sharedTextureAvailableEvents[i].reset(
			CreateEventW(nullptr, FALSE, TRUE, nullptr));
		if (!_sharedTextureAvailableEvents[i]) {
			Logger::Get().Win32Error(
				"Create shared presentation slot event failed");
			return NULL;
		}
	}
	if (_sharedTextureSlotCount > 1) {
		Logger::Get().Info(fmt::format(
			"DLSSFG bounded presentation ring initialized: slots={}",
			_sharedTextureSlotCount));
	}
	if (xessFgHdrTerminal) {
		_hdrPresentationTexture = DirectXHelper::CreateTexture2D(
			_backendResources.GetD3DDevice(), DXGI_FORMAT_R10G10B10A2_UNORM,
			textureSize.cx, textureSize.cy,
			D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
		if (!_hdrPresentationTexture) {
			Logger::Get().Error("Create XeSSFG HDR10 presentation texture failed");
			return NULL;
		}
	}
	if (!_passThroughFrames.InitializeBackend(_backendResources,
		_frameSource->GetPipelineTexture(), effectsOutput, _sharedTextureSlotCount,
		ScalingWindow::Get().Options().IsHdrCompatibilityEnabled(),
		HdrColorTransform::ForFrame(_frameSource->GetHdrFrameMetadata().color),
		_frameSource->GetHdrFrameMetadata())) {
		const auto& window = ScalingWindow::Get();
		if (const auto& report = window.Options().reportErrorDetails) {
			report(window.SrcTracker().Handle(), ScalingError::PassThroughUnavailable,
				"Initialize original comparison resources / continuing effects", 0);
		}
	}
	return _sharedTextureHandles[0];
}

void Renderer::_BackendThreadProc() noexcept {
	FrameTrace::BindBackend();
#ifdef _DEBUG
	SetThreadDescription(GetCurrentThread(), L"Magpie-缩放后端线程");
#endif

	winrt::init_apartment(winrt::apartment_type::single_threaded);

	if (const HANDLE sharedHandle = _InitBackend()) {
		_sharedTextureHandle.store(sharedHandle, std::memory_order_release);
		_sharedTextureHandle.notify_one();
	} else {
		_frameSource.reset();
		// 通知前端初始化失败
		_sharedTextureHandle.store(INVALID_HANDLE_VALUE, std::memory_order_release);
		_sharedTextureHandle.notify_one();

		// 即使失败也要创建消息循环，否则前端线程将一直等待
		MSG msg;
		while (GetMessage(&msg, NULL, 0, 0)) {
			DispatchMessage(&msg);
		}
		return;
	}

	StepTimerStatus stepTimerStatus = StepTimerStatus::WaitForNewFrame;
	const bool waitForNewFrame =
		_frameSource->WaitType() == FrameSourceWaitType::WaitForMessage ||
		_frameSource->WaitType() == FrameSourceWaitType::WaitForEvent;

	MSG msg;
	while (true) {
		if (NgxRuntimeGuard::IsFaulted() && _ngxD3D12Core.Device() && !_sessionLifetime->IsStopping()) {
			ScalingWindow::Dispatcher().TryEnqueue([session = _sessionLifetime]() {
				auto& window = ScalingWindow::Get();
				if (!session->IsCurrent(ScalingWindow::RunId()) || !window) return;
				if (const auto report = window.Options().reportErrorDetails) {
					report(window.SrcTracker().Handle(), ScalingError::NgxRestartRequired,
						fmt::format("NGX runtime fault at {:#x}, thread={}; restart required",
							NgxRuntimeGuard::FaultAddress(), NgxRuntimeGuard::FaultThread()),
						NgxRuntimeGuard::FaultCode());
				} else {
					window.ShowError(ScalingError::NgxRestartRequired);
				}
				window.Stop();
			});
			_frameSource.reset();
			return;
		}
		if (_sessionLifetime->IsStopping()) {
			if (GetMessage(&msg, NULL, 0, 0) <= 0) {
				_frameSource.reset();
				return;
			}
			DispatchMessage(&msg);
			continue;
		}
		bool fpsUpdated = false;
		bool waitedForContent = false;
		FrameTrace::Scope traceWait(FrameTrace::Event::BackendWait);
		if (_pendingFrameGenerationInput) {
			_fgInputClock.SetInterval(std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::duration<double>(1.0 / _FrontEdgeFrameRate())));
			const auto now = std::chrono::steady_clock::now();
			WaitForFramePacing(_fgInputClock.Due(now) - now, _fgInputTimer);
		} else if (_frontEdgeSyncEnabled && _frontEdgeUsesSharedSlot &&
			_frontEdgeAcknowledgedKey.load(std::memory_order_acquire) !=
			_sharedTextureMutexKeys[0].load(std::memory_order_acquire)) {
			waitedForContent = true;
			HANDLE consumed = _frontEdgeConsumedEvent.get();
			MsgWaitForMultipleObjectsEx(1, &consumed, 50, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
		} else {
			stepTimerStatus = _stepTimer.WaitForNextFrame(
				waitForNewFrame && stepTimerStatus != StepTimerStatus::WaitForFPSLimiter,
				fpsUpdated, _frameSource->FrameArrivedEvent());
		}

		traceWait.End();
		FrameTrace::Scope traceMessages(FrameTrace::Event::BackendMessages);
		while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
			if (msg.message == WM_QUIT) {
				// 不能在前端线程释放
				_frameSource.reset();
				return;
			}

			DispatchMessage(&msg);
		}

		traceMessages.End();
		if (_sessionLifetime->IsStopping()) continue;
		if (_pendingFrameGenerationInput) {
			const auto now = std::chrono::steady_clock::now();
			if (now < _fgInputClock.Due(now)) continue;
			// Do not replace colour/reference/motion before this input enters FG.
			auto input = std::move(_pendingFrameGenerationInput);
			_fgInputClock.Submitted(now);
			_CompleteBackendFrame(input.get(), true, _captureSequence);
			if (!_dlssFrameGenerator || !_synchronousFramePresentationEnabled.load(std::memory_order_acquire)) {
				PostMessage(ScalingWindow::Get().Handle(), CommonSharedConstants::WM_FRONTEND_RENDER, 0, 0);
			}
			continue;
		}
		if (_frontEdgeSyncEnabled && _frontEdgeUsesSharedSlot &&
			_frontEdgeAcknowledgedKey.load(std::memory_order_acquire) !=
			_sharedTextureMutexKeys[0].load(std::memory_order_acquire)) continue;
		// Refresh StepTimer's frame-start timestamp before accepting new input.
		if (waitedForContent) continue;
		if (!_pendingEffectParameterUpdates.empty()) {
			_ApplyPendingEffectParameters();
		}

		if (stepTimerStatus == StepTimerStatus::WaitForFPSLimiter) {
			// 新帧消息可能已被处理，之后的 WaitForNextFrame 不要等待消息，直到状态变化
			continue;
		}

		FrameTrace::SetFrame(_capturedFrameId + 1); // Candidate id until CaptureAccepted.
		FrameTrace::Scope traceCapture(FrameTrace::Event::CaptureUpdate);
		const FrameSourceState frameSourceState = _frameSource->Update();
		traceCapture.Data(static_cast<int64_t>(frameSourceState));
		traceCapture.End();
		FrameTrace::Mark(FrameTrace::Event::CaptureResult, static_cast<int64_t>(frameSourceState),
			_frameSource->CaptureTimestamp100ns());
		if (_sessionLifetime->IsStopping()) continue;
		switch (frameSourceState) {
		case FrameSourceState::Waiting:
			if (_frameSource->IsCaptureInterrupted()) {
				// Keep the last published frame; even live parameter edits must wait
				// for valid input before re-entering temporal effects. The timeout
				// also avoids busy spinning after the minimum-FPS deadline expires.
				if (fpsUpdated) PostMessage(ScalingWindow::Get().Handle(),
					CommonSharedConstants::WM_FRONTEND_RENDER, 0, 0);
				const HANDLE arrived = _frameSource->FrameArrivedEvent();
				MsgWaitForMultipleObjectsEx(arrived ? 1 : 0, arrived ? &arrived : nullptr,
					50, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
				break;
			}
			if (stepTimerStatus != StepTimerStatus::ForceNewFrame && !_forceNextRender) {
				if (fpsUpdated) {
					// FPS 变化则要求前端重新渲染以更新叠加层，调整大小时这个操作十分必要
					PostMessage(ScalingWindow::Get().Handle(),
						CommonSharedConstants::WM_FRONTEND_RENDER, 0, 0);
				}
				break;
			}

			// 强制帧
			if (_capturedFrameId == 0) {
				// The output texture has no defined contents before first capture.
				break;
			}
			[[fallthrough]];
		case FrameSourceState::NewFrame:
			_forceNextRender = false;
			_backendMayDeferFG = true;
			_BackendRender(
				_effectDrawers.back().GetExternalOutputTexture(),
				frameSourceState == FrameSourceState::NewFrame);
			_backendMayDeferFG = false;
			// DLSSFG uses synchronous, individually paced presentation so generated
			// frames cannot be coalesced into the following real frame.
			if (!_dlssFrameGenerator ||
				!_synchronousFramePresentationEnabled.load(std::memory_order_acquire)) {
				PostMessage(ScalingWindow::Get().Handle(),
					CommonSharedConstants::WM_FRONTEND_RENDER, 0, 0);
			}
			break;
		case FrameSourceState::Error:
			// 捕获出错，退出缩放
			ScalingWindow::Dispatcher().TryEnqueue([
				session = _sessionLifetime,
				context = std::string(_frameSource->CaptureErrorContext()),
				code = _frameSource->CaptureErrorCode()]() {
				ScalingWindow& scalingWindow = ScalingWindow::Get();
				if (!session->IsCurrent(ScalingWindow::RunId()) || !scalingWindow) return;
				if (auto report = scalingWindow.Options().reportErrorDetails) {
					report(scalingWindow.SrcTracker().Handle(), ScalingError::CaptureFailed,
						context, code);
				} else {
					scalingWindow.ShowError(ScalingError::CaptureFailed);
				}
				scalingWindow.Stop();
			});

			while (GetMessage(&msg, NULL, 0, 0)) {
				DispatchMessage(&msg);
			}

			_frameSource.reset();
			return;
		}
	}
}

void Renderer::_UpdateFrameRateLimits() noexcept {
	const ScalingOptions& options = ScalingWindow::Get().Options();
	std::optional<float> maxFrameRate = _captureMaxFrameRate;

	_frameRateFilterTarget = 0.0f;
	for (const EffectOption& effect : _runtimeEffectOptions) {
		if (!IsFrameRateFilterEffect(effect.name)) {
			continue;
		}
		const auto mode = effect.parameters.find("frameRateMode");
		const float modeValue = mode == effect.parameters.end() ? 0.0f : mode->second;
		const auto custom = effect.parameters.find("targetFrameRate");
		const float targetFrameRate = ResolveFrameRateFilterTarget(
			options.isFrontEdgeSyncEnabled, modeValue,
			custom == effect.parameters.end() ? 60.0f : custom->second,
			options.frontEdgeSyncFrameRate, _presentationRefreshRate.load(std::memory_order_acquire),
			_configuredFrameGenerationMultiplier);
		// Active Front Edge Sync already owns this target. Do not feed a resolved
		// auto target back as an independent cap, which would stick on monitor changes.
		if (!_frontEdgeSyncEnabled && (!maxFrameRate || targetFrameRate < *maxFrameRate)) {
			maxFrameRate = targetFrameRate;
		}
		_frameRateFilterTarget = _frameRateFilterTarget == 0.0f
			? targetFrameRate : std::min(_frameRateFilterTarget, targetFrameRate);
		Logger::Get().Info(fmt::format(
			"Frame Rate Filter enabled: {} FPS ({})", targetFrameRate,
			UsesFrontEdgeSyncFrameRate(options.isFrontEdgeSyncEnabled, modeValue)
				? "Front Edge Sync" : "Custom"));
	}

	if (options.maxFrameRate &&
		(!maxFrameRate || *options.maxFrameRate < *maxFrameRate)) {
		maxFrameRate = options.maxFrameRate;
	}
	const bool useFrameGeneration = std::ranges::any_of(
		_runtimeEffectOptions,
		[](const EffectOption& effect) { return IsFrameGenerationEffect(effect.name); });
	_existingBaseFrameRateLimit.store(maxFrameRate.value_or(0.0f), std::memory_order_release);
	const float minFrameRate = useFrameGeneration ? 0.0f :
		(options.IsBenchmarkMode() ? std::numeric_limits<float>::max() :
			std::min(options.minFrameRate, _frontEdgeSyncEnabled
				? float(_FrontEdgeFrameRate()) : maxFrameRate.value_or(options.minFrameRate)));
	if (_frontEdgeSyncEnabled) Logger::Get().Info(fmt::format(
		"Front Edge Sync effective base target: {:.3f} FPS (existingLimit={:.3f})",
		_FrontEdgeFrameRate(), maxFrameRate.value_or(0.0f)));
	// The consumer boundary owns this limit; avoid an independent capture gate.
	const bool consumerPacing = _frontEdgeSyncEnabled &&
		(_frontEdgeUsesSharedSlot || _dlssFrameGenerator || _effectDrawers.empty());
	const std::optional<float> fallbackLimit = _frontEdgeSyncEnabled ?
		std::optional<float>(float(_FrontEdgeFrameRate())) : maxFrameRate;
	_stepTimer.Initialize(minFrameRate, consumerPacing ? std::nullopt : fallbackLimit);

	_baseFrameRateLimit = _frontEdgeSyncEnabled ? _FrontEdgeFrameRate() : maxFrameRate.value_or(0.0f);
	if (_dlssFrameGenerator) _synchronousPresentInterval =
		_captureCadence.Interval(_dlssFrameGenerator->Multiplier(), _baseFrameRateLimit);
}

HANDLE Renderer::_InitBackend() noexcept {
	// 创建 DispatcherQueue
	{
		winrt::Windows::System::DispatcherQueueController dqc{ nullptr };
		HRESULT hr = CreateDispatcherQueueController(
			DispatcherQueueOptions{
				.dwSize = sizeof(DispatcherQueueOptions),
				.threadType = DQTYPE_THREAD_CURRENT
			},
			(PDISPATCHERQUEUECONTROLLER*)winrt::put_abi(dqc)
		);
		if (FAILED(hr)) {
			Logger::Get().ComError("CreateDispatcherQueueController 失败", hr);
			return NULL;
		}

		_backendThreadDispatcher = dqc.DispatcherQueue();
	}

	_runtimeEffectOptions = ScalingWindow::Get().Options().effects;
	ScalingWindow::Get().Options().parameterSession->Applied(_runtimeEffectOptions);

	if (!_backendResources.Initialize(false)) {
		_backendInitError = ScalingError::GraphicsDeviceInitFailed;
		return NULL;
	}

	ID3D11Device5* d3dDevice = _backendResources.GetD3DDevice();
	_backendDescriptorStore.Initialize(d3dDevice);

	if (!_InitFrameSource()) {
		return NULL;
	}
	_activeResourceGeneration.store(_frameSource->ResourceGeneration(), std::memory_order_release);
	{
		if (_frameSource->WaitType() == FrameSourceWaitType::NoWait) {
			// 某些捕获方式不会限制捕获帧率，因此将捕获帧率限制为屏幕刷新率
			const HWND hwndSrc = ScalingWindow::Get().SrcTracker().Handle();
			if (HMONITOR hMon = MonitorFromWindow(hwndSrc, MONITOR_DEFAULTTONEAREST)) {
				MONITORINFOEX mi{ { sizeof(MONITORINFOEX) } };
				GetMonitorInfo(hMon, &mi);

				DEVMODE dm{ .dmSize = sizeof(DEVMODE) };
				EnumDisplaySettings(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm);

				if (dm.dmDisplayFrequency > 0) {
					Logger::Get().Info(fmt::format("屏幕刷新率: {}", dm.dmDisplayFrequency));
					_captureMaxFrameRate = float(dm.dmDisplayFrequency);
				}
			}
		}
		_UpdateFrameRateLimits();
	}

	ID3D11Texture2D* outputTexture = _BuildEffects();
	if (!outputTexture) {
		return NULL;
	}

	FrameGuidanceRequirements guidanceRequirements =
		CollectFrameGuidanceRequirements(
			_nativeEffectBackends, _dlssFrameGenerator.get(), _xessMotionRequest);
	_motionConsumers.clear();
	if (ScalingWindow::Get().Options().IsHdrCompatibilityEnabled() && guidanceRequirements.HasMotion()) {
		Logger::Get().Info("HDR mode: optical-flow providers receive the canonical frame and perform provider-local format adaptation");
	}
	for (size_t i = 0; i < _runtimeEffectOptions.size(); ++i) {
		const auto& effect = _runtimeEffectOptions[i];
		const MotionVectorRequest request = IsDLSSFrameGenerationEffect(effect.name) && _dlssFrameGenerator ?
			_dlssFrameGenerator->GetFrameGuidanceRequirements().FirstMotion() :
			IsXeSSFrameGenerationEffect(effect.name) ? _xessMotionRequest :
			i < _nativeEffectBackends.size() && _nativeEffectBackends[i] ?
			_nativeEffectBackends[i]->GetFrameGuidanceRequirements().FirstMotion() : MotionVectorRequest{};
		if (request.method != OpticalFlowMethod::None)
			_motionConsumers.emplace_back(fmt::format("{}. {}", i + 1,
				_effectDescs[i].sortName.empty() ? effect.name : _effectDescs[i].sortName), request);
	}

	if (guidanceRequirements.Any()) {
		guidanceRequirements.ForEachMotion([&]([[maybe_unused]] MotionVectorRequest request) {
#ifdef MP_ENABLE_NVIDIA_OPTICAL_FLOW
			if (request.method == OpticalFlowMethod::Nvidia)
				_frameGuidanceService.SetMotionVectorProvider(request,
					std::make_unique<NvidiaOpticalFlowProvider>(static_cast<NvidiaOpticalFlowQuality>(request.quality)));
#endif
#ifdef MP_ENABLE_AMD_OPTICAL_FLOW
			if (request.method == OpticalFlowMethod::Amd)
				_frameGuidanceService.SetMotionVectorProvider(request,
					std::make_unique<AmdOpticalFlowProvider>(static_cast<AmdOpticalFlowMode>(request.quality)));
#endif
		});
		if (!_frameGuidanceService.Initialize(
			_backendResources, _frameSource->GetPipelineTexture(), guidanceRequirements)) {
			const OpticalFlowMethod method =
				_frameGuidanceService.InitializationFailedMethod();
			const OpticalFlowInitializationError error =
				_frameGuidanceService.InitializationError();
			if (method == OpticalFlowMethod::Nvidia) {
				_backendInitError =
					error == OpticalFlowInitializationError::QualityUnsupported ?
					ScalingError::NvidiaOpticalFlowQualityUnsupported :
					error == OpticalFlowInitializationError::InteropFailed ?
					ScalingError::OpticalFlowInteropFailed :
					ScalingError::NvidiaOpticalFlowUnsupported;
			} else if (method == OpticalFlowMethod::Amd) {
				_backendInitError =
					error == OpticalFlowInitializationError::InteropFailed ?
					ScalingError::OpticalFlowInteropFailed :
					ScalingError::AmdOpticalFlowUnsupported;
			} else {
				_backendInitError = ScalingError::OpticalFlowProviderUnavailable;
			}
			return NULL;
		}
	}

	HRESULT hr = d3dDevice->CreateFence(
		_fenceValue, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&_d3dFence));
	if (FAILED(hr)) {
		// GH#979
		// 这个错误会在某些很旧的显卡上出现，似乎是驱动的 bug。文档中提到 ID3D11Device5::CreateFence
		// 和 ID3D12Device::CreateFence 等价，但支持 DX12 的显卡也有失败的可能，如 GH#1013
		Logger::Get().ComError("CreateFence 失败", hr);
		_backendInitError = ScalingError::CreateFenceFailed;
		return NULL;
	}

	if (!_fenceEvent.try_create(wil::EventOptions::None, nullptr)) {
		Logger::Get().Win32Error("CreateEvent 失败");
		return NULL;
	}

	HANDLE sharedHandle = _CreateSharedTexture(outputTexture);
	if (!sharedHandle) {
		Logger::Get().Error("_CreateSharedTexture 失败");
		return NULL;
	}

	// 最后启动捕获以尽可能推迟显示黄色边框 (Win10) 或禁用圆角 (Win11)
	if (!_frameSource->Start()) {
		Logger::Get().Error("启动捕获失败");
		_backendInitError = ScalingError::CaptureFailed;
		_backendInitContext = fmt::format("{} (0x{:08X})",
			_frameSource->CaptureErrorContext(), uint32_t(_frameSource->CaptureErrorCode()));
		return NULL;
	}

	return sharedHandle;
}

void Renderer::_BackendRender(
	ID3D11Texture2D* effectsOutput,
	bool isNewCaptureFrame
) noexcept {
	FrameTrace::Scope traceRender(FrameTrace::Event::BackendRender, isNewCaptureFrame);
	_stepTimer.PrepareForRender();
	if (isNewCaptureFrame) {
		const auto captureTime = std::chrono::steady_clock::now();
		const uint64_t sequence = _frameSource->CaptureSequence();
		if (sequence != _captureSequence) {
			_captureSequence = sequence;
			_activeCaptureSequence.store(sequence, std::memory_order_release);
			_activeResourceGeneration.store(
				_frameSource->ResourceGeneration(), std::memory_order_release);
			_captureCadence.RestartSequence();
			_captureCadenceQueueWait = {};
			if (_capturedFrameId) {
				if (_frameGuidanceService.IsInitialized()) {
					_frameGuidanceService.ResetHistory(FrameGuidanceResetReason::CaptureInterrupted);
				}
				if (_dlssFrameGenerator) _dlssFrameGenerator->RequestHistoryReset();
			}
			Logger::Get().Info(fmt::format(
				"Capture sequence accepted: sequence={} frameId={} timestamp100ns={} historyResetRequested={}",
				sequence, _capturedFrameId + 1, _frameSource->CaptureTimestamp100ns(),
				_capturedFrameId != 0));
		}
		const auto downstreamWait = std::exchange(_captureCadenceQueueWait,
			std::chrono::steady_clock::duration::zero());
		if (_captureCadence.Observe(captureTime, downstreamWait)) {
			// A delayed WGC notification is a delivery-time observation, not a
			// capture interruption. Resetting DLSSFG history here drops the first
			// generated frame after an otherwise valid static-window gap and causes
			// visible flashing. Actual interruptions already advance CaptureSequence
			// and reset temporal consumers in the branch above.
			Logger::Get().Info("Capture cadence gap observed; preserving temporal history until capture sequence changes");
		}
		if (_dlssFrameGenerator) _synchronousPresentInterval =
			_captureCadence.Interval(_dlssFrameGenerator->Multiplier(), _baseFrameRateLimit);
		_lastCapturedFrameTime = captureTime;
		++_capturedFrameId;
		FrameTrace::SetFrame(_capturedFrameId);
		FrameTrace::Mark(FrameTrace::Event::CaptureAccepted, _frameSource->CaptureTimestamp100ns(), sequence);
		const FrameGuidanceRequirements guidanceRequirements =
			CollectFrameGuidanceRequirements(
				_nativeEffectBackends, _dlssFrameGenerator.get(), _xessMotionRequest);
		if (_frameGuidanceService.IsInitialized()) {
			FrameTrace::Scope traceGuidance(FrameTrace::Event::Guidance);
			_frameGuidanceService.BeginFrame(
			_capturedFrameId, _frameSource->GetPipelineTexture(), guidanceRequirements,
			_frameSource->CaptureSequence(), _frameSource->ResourceGeneration(),
			_frameSource->CaptureTimestamp100ns(),
			_frameSource->GetHdrFrameMetadata().color);
		}
	}
	if (_dlssFrameGenerator && isNewCaptureFrame) {
		++_dlssFgCapturedFrameCount;
	}

	FrameTrace::SetFrame(_capturedFrameId);
	traceRender.FrameId(_capturedFrameId);
	if (ScalingWindow::Get().Options().IsHdrCompatibilityEnabled() && isNewCaptureFrame && _capturedFrameId <= 2) {
		_LogHdrTextureStats(_frameSource->GetPipelineTexture(), "capture-canonical");
	}
	if (ScalingWindow::Get().Options().IsHdrCompatibilityEnabled()) {
		HdrFrame captureFrame = _frameSource->GetCanonicalFrame();
		captureFrame.metadata.frameId = _capturedFrameId;
		captureFrame.metadata.captureSequence = _captureSequence;
		_passThroughFrames.UpdateBackend(captureFrame, isNewCaptureFrame);
	} else {
		_passThroughFrames.UpdateBackend(_capturedFrameId, isNewCaptureFrame);
	}

	ID3D11DeviceContext4* d3dDC = _backendResources.GetD3DDC();
	d3dDC->ClearState();

	if (ID3D11Buffer* t = _dynamicCB.get()) {
		_UpdateDynamicConstants();
		d3dDC->CSSetConstantBuffers(1, 1, &t);
	}

	_effectsProfiler.OnBeginEffects(d3dDC);
	if (isNewCaptureFrame) {
		_UpdateHdrEffectBoundaryContexts();
	}

	for (uint32_t i = 0; i < _effectDrawers.size(); ++i) {
		const EffectDrawer& effectDrawer = _effectDrawers[i];
		if (ScalingWindow::Get().Options().IsHdrCompatibilityEnabled()) {
			// Rebind every boundary to the actual upstream canonical handoff for
			// this frame. Initialization-time pointers become stale after the
			// first capture and after any resize/rebuild.
			ID3D11Texture2D* upstream = i == 0
				? _frameSource->GetPipelineTexture()
				: _effectDrawers[i - 1].GetExternalOutputTexture();
			_effectDrawers[i].SetHdrInputSource(upstream);
		}
		if (i < _nativeEffectBackends.size() && _nativeEffectBackends[i]) {
			if (!effectDrawer.PrepareHdrInput()) {
				Logger::Get().Error("准备 native HDR 效果输入失败");
				continue;
			}
			D3D11_TEXTURE2D_DESC inputDesc{};
			effectDrawer.GetTexture(0)->GetDesc(&inputDesc);
			const FrameGuidanceConsumerViews guidance =
				_frameGuidanceService.GetConsumerViews(
					_capturedFrameId, { inputDesc.Width, inputDesc.Height },
					GetMotionVectorRequest(
						_nativeEffectBackends[i]->GetFrameGuidanceRequirements()));
			const NativeEffectDrawContext drawContext{
				.input = effectDrawer.GetTexture(0),
				.output = effectDrawer.GetOutputTexture(),
				.inputMetadata = ScalingWindow::Get().Options().IsHdrCompatibilityEnabled()
					? effectDrawer.GetHdrBoundary().inputFrame.metadata : HdrFrameMetadata{},
				// The backend writes the route-local output; downstream canonical
				// metadata is rebuilt by CompleteHdrOutput from the same color.
				.outputMetadata = ScalingWindow::Get().Options().IsHdrCompatibilityEnabled()
					? effectDrawer.GetHdrBoundary().inputFrame.metadata : HdrFrameMetadata{},
				.frameId = _capturedFrameId,
				.inputRevision = i < _effectInputRevisions.size()
					? _effectInputRevisions[i] : 0,
				.frameGuidance = guidance.produced,
				.zeroFrameGuidance = guidance.zero
			};
			FrameTrace::Scope traceNative(FrameTrace::Event::NativeEffect, i);
			const bool nativeDrawSucceeded = _nativeEffectBackends[i]->Draw(drawContext);
			if (ScalingWindow::Get().Options().IsHdrCompatibilityEnabled() && isNewCaptureFrame && _capturedFrameId <= 2) {
				_LogHdrTextureStats(drawContext.input, fmt::format("effect-{}-input", i));
				_LogHdrTextureStats(drawContext.output, fmt::format("effect-{}-backend-output", i));
			}
			if (!nativeDrawSucceeded) {
				const HdrEffectBoundaryContext& boundary = effectDrawer.GetHdrBoundary();
				D3D11_TEXTURE2D_DESC outputDesc{};
				if (effectDrawer.GetOutputTexture()) {
					effectDrawer.GetOutputTexture()->GetDesc(&outputDesc);
				}
				Logger::Get().Error(fmt::format(
					"Native effect Draw failed: effect={} route={} profile={} "
					"inputFormat={} outputFormat={} fallback=marker-pass",
					_runtimeEffectOptions[i].name,
					boundary.SelectedRoute() ? boundary.SelectedRoute()->Id() : "(none)",
					ToString(boundary.plan.profile),
					static_cast<uint32_t>(inputDesc.Format),
					static_cast<uint32_t>(outputDesc.Format)));
				// A runtime SDK failure must not publish the cleared route output.
				// Execute the production marker pass through the same drawer so the
				// chain remains visible at the requested output size.
				effectDrawer.Draw(_effectsProfiler);
			} else if (!effectDrawer.CompleteHdrOutput()) {
				Logger::Get().Error("完成 native HDR 效果输出失败");
			}
			_effectsProfiler.OnEndPass(d3dDC);
		} else {
			effectDrawer.Draw(_effectsProfiler);
			if (ScalingWindow::Get().Options().IsHdrCompatibilityEnabled() && isNewCaptureFrame && _capturedFrameId <= 2) {
				_LogHdrTextureStats(effectDrawer.GetTexture(0), fmt::format("effect-{}-adapter-input", i));
				_LogHdrTextureStats(effectDrawer.GetExternalOutputTexture(), fmt::format("effect-{}-canonical-output", i));
			}
		}
	}

	_effectsProfiler.OnEndEffects(d3dDC);
	// Keep the pacing/queue optimization, while avoiding duplicate real-frame
	// submissions during a capture gap. FG must advance from a new canonical
	// capture or its own generated frame, never from a repeated stale input.
	if (!isNewCaptureFrame && (_dlssFrameGenerator || _isXeSSFrameGenerationActive)) {
		d3dDC->Flush();
		return;
	}

	if (_frontEdgeSyncEnabled && _dlssFrameGenerator && isNewCaptureFrame &&
		_backendMayDeferFG && _synchronousFramePresentationEnabled.load(std::memory_order_acquire)) {
		_pendingFrameGenerationInput.copy_from(effectsOutput);
		d3dDC->Flush();
		return;
	}
	_CompleteBackendFrame(effectsOutput, isNewCaptureFrame, _captureSequence);
}

void Renderer::_LogHdrTextureStats(ID3D11Texture2D* texture, std::string_view label) noexcept {
	if (!texture) {
		Logger::Get().Warn(fmt::format("HDR texture stats: label={} texture=null", label));
		return;
	}
	D3D11_TEXTURE2D_DESC sourceDesc{};
	texture->GetDesc(&sourceDesc);
	if (sourceDesc.ArraySize != 1 || sourceDesc.MipLevels != 1) return;
	D3D11_TEXTURE2D_DESC staging = sourceDesc;
	staging.Usage = D3D11_USAGE_STAGING;
	staging.BindFlags = 0;
	staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	staging.MiscFlags = 0;
	winrt::com_ptr<ID3D11Texture2D> readback;
	if (FAILED(_backendResources.GetD3DDevice()->CreateTexture2D(&staging, nullptr, readback.put()))) {
		Logger::Get().Warn(fmt::format("HDR texture stats: label={} staging-create-failed format={}", label, static_cast<uint32_t>(sourceDesc.Format)));
		return;
	}
	_backendResources.GetD3DDC()->CopyResource(readback.get(), texture);
	_backendResources.GetD3DDC()->Flush();
	D3D11_MAPPED_SUBRESOURCE mapped{};
	if (FAILED(_backendResources.GetD3DDC()->Map(readback.get(), 0, D3D11_MAP_READ, 0, &mapped))) {
		Logger::Get().Warn(fmt::format("HDR texture stats: label={} map-failed format={}", label, static_cast<uint32_t>(sourceDesc.Format)));
		return;
	}
	double sum[4]{}; float minimum[4]{ FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX };
	float maximum[4]{ -FLT_MAX, -FLT_MAX, -FLT_MAX, -FLT_MAX }; uint64_t finite = 0, invalid = 0;
	for (UINT y = 0; y < sourceDesc.Height; ++y) {
		const auto* row = static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch;
		for (UINT x = 0; x < sourceDesc.Width; ++x) {
			float values[4]{};
			if (sourceDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM) {
				const auto* p = row + size_t(x) * 4; for (int c = 0; c < 4; ++c) values[c] = p[c] / 255.0f;
			} else if (sourceDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
				const auto* p = reinterpret_cast<const uint16_t*>(row) + size_t(x) * 4;
				for (int c = 0; c < 4; ++c) { const uint16_t h = p[c]; const uint32_t e = (h >> 10) & 31u; const uint32_t f = h & 1023u; const bool s = (h & 0x8000u) != 0; values[c] = e == 0 ? std::ldexp(float(f), -24) : e == 31 ? (f ? NAN : (s ? -INFINITY : INFINITY)) : std::ldexp(float(1024 + f), int(e) - 25) * (s ? -1.0f : 1.0f); }
			} else { continue; }
			for (int c = 0; c < 4; ++c) { if (std::isfinite(values[c])) { sum[c] += values[c]; minimum[c] = (std::min)(minimum[c], values[c]); maximum[c] = (std::max)(maximum[c], values[c]); ++finite; } else ++invalid; }
		}
	}
	_backendResources.GetD3DDC()->Unmap(readback.get(), 0);
	const double pixels = double(sourceDesc.Width) * double(sourceDesc.Height);
	Logger::Get().Info(fmt::format("HDR texture stats: label={} format={} size={}x{} finite={} invalid={} R=[{:.6g},{:.6g},{:.6g}] G=[{:.6g},{:.6g},{:.6g}] B=[{:.6g},{:.6g},{:.6g}] A=[{:.6g},{:.6g},{:.6g}]", label, static_cast<uint32_t>(sourceDesc.Format), sourceDesc.Width, sourceDesc.Height, finite, invalid, minimum[0], maximum[0], sum[0] / pixels, minimum[1], maximum[1], sum[1] / pixels, minimum[2], maximum[2], sum[2] / pixels, minimum[3], maximum[3], sum[3] / pixels));
}

void Renderer::_CompleteBackendFrame(
	ID3D11Texture2D* effectsOutput,
	bool isNewCaptureFrame,
	uint64_t captureSequence
) noexcept {
	auto* d3dDC = _backendResources.GetD3DDC();
	if (_frontEdgeSyncEnabled) _baseFrameRateLimit = _FrontEdgeFrameRate();
	if (_dlssFrameGenerator) _synchronousPresentInterval =
		_captureCadence.Interval(_dlssFrameGenerator->Multiplier(), _baseFrameRateLimit);
	if (_dlssFrameGenerator && isNewCaptureFrame) {
		D3D11_TEXTURE2D_DESC sourceDesc{};
		_frameSource->GetPipelineTexture()->GetDesc(&sourceDesc);
		const FrameGuidanceConsumerViews guidance =
			_frameGuidanceService.GetConsumerViews(
				_capturedFrameId, { sourceDesc.Width, sourceDesc.Height },
				GetMotionVectorRequest(
					_dlssFrameGenerator->GetFrameGuidanceRequirements()));
		_dlssFgPresentationStopping = false;
		ID3D11Texture2D* dlssInput = effectsOutput;
		if (_dlssFgNormalizedInput && _dlssFgHdrNormalizationScale != 1.0f) {
			if (!_hdrPresentationAdapter.ConvertHdrToBounded(
				effectsOutput, _dlssFgNormalizedInput.get(),
				HdrColorTransform::ForFrame(_frameSource->GetHdrFrameMetadata().color),
				_dlssFgHdrNormalizationScale)) {
				Logger::Get().Error("DLSSFG canonical-to-bounded input conversion failed");
				return;
			}
			dlssInput = _dlssFgNormalizedInput.get();
		}
		const bool generated = _dlssFrameGenerator->Draw(
			dlssInput,
			_capturedFrameId,
			guidance.produced,
			guidance.zero,
		[this, captureSequence](ID3D11Texture2D* generatedFrame) {
				ID3D11Texture2D* canonicalGenerated = generatedFrame;
				if (_dlssFgCanonicalGenerated && _dlssFgHdrNormalizationScale != 1.0f) {
					if (!_hdrPresentationAdapter.ConvertBoundedToHdr(
						generatedFrame, _dlssFgCanonicalGenerated.get(),
						HdrColorTransform::ForFrame(_frameSource->GetHdrFrameMetadata().color),
						_dlssFgHdrNormalizationScale)) {
						Logger::Get().Error("DLSSFG bounded-to-canonical output conversion failed");
						return false;
					}
					canonicalGenerated = _dlssFgCanonicalGenerated.get();
				}
				return _PublishBackendTexture(canonicalGenerated, true, true, captureSequence,
					_frameSource ? _frameSource->ResourceGeneration() : 0);
			}
		);
		if (!generated && !_dlssFgPresentationStopping) {
			_HandleDLSSFrameGenerationFailure(effectsOutput);
		} else if (!generated) {
			Logger::Get().Info(
				"DLSSFG presentation interrupted by normal window shutdown");
		} else {
			_dlssFgConsecutiveFailures = 0;
		}
	}

	const bool synchronous = _dlssFrameGenerator &&
		_synchronousFramePresentationEnabled.load(std::memory_order_acquire);
	if (!_PublishBackendTexture(effectsOutput, synchronous, false, captureSequence,
		_frameSource ? _frameSource->ResourceGeneration() : 0)) {
		return;
	}

	// 查询效果的渲染时间
	_effectsProfiler.QueryTimings(d3dDC);
}

bool Renderer::_PublishBackendTexture(
	ID3D11Texture2D* texture,
	bool synchronous,
	bool generatedFrame,
	uint64_t captureSequence,
	uint64_t resourceGeneration
) noexcept {
	const uint64_t currentGeneration = _frameSource ? _frameSource->ResourceGeneration() :
		_sharedTextureGeneration.load(std::memory_order_acquire);
	if (captureSequence != 0 && captureSequence != _captureSequence) {
		Logger::Get().Warn(fmt::format(
			"Dropping stale {} frame: generation={} currentGeneration={}",
			generatedFrame ? "generated" : "real", captureSequence, _captureSequence));
		// The SDK submission itself completed; treat the stale publication as
		// consumed so a recovery does not disable an otherwise healthy FG path.
		return true;
	}
	if (resourceGeneration != 0 && resourceGeneration != currentGeneration) {
		Logger::Get().Warn(fmt::format(
			"Dropping stale {} frame: resourceGeneration={} currentResourceGeneration={}",
			generatedFrame ? "generated" : "real", resourceGeneration, currentGeneration));
		return true;
	}
	if (!texture) {
		Logger::Get().Error("Dropping frame publication with null texture");
		return false;
	}
	ID3D11DeviceContext4* d3dDC = _backendResources.GetD3DDC();
	ID3D11Texture2D* publicationTexture = texture;
	HdrFrameMetadata publicationMetadata{};
	if (ScalingWindow::Get().Options().IsHdrCompatibilityEnabled()) {
		publicationMetadata = _frameSource->GetHdrFrameMetadata();
		publicationMetadata.frameId = _capturedFrameId;
		publicationMetadata.captureSequence = captureSequence;
		publicationMetadata.resourceGeneration = _frameSource->ResourceGeneration();
		publicationMetadata.timestamp100ns = _frameSource->CaptureTimestamp100ns();
		publicationMetadata.stage = generatedFrame
			? HdrFrameStage::GeneratedOutput : HdrFrameStage::CanonicalOutput;
		publicationMetadata.generated = generatedFrame;
		D3D11_TEXTURE2D_DESC textureDesc{};
		texture->GetDesc(&textureDesc);
		if (textureDesc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT) {
			Logger::Get().Error("HDR 发布收到非 canonical FP16 纹理");
			return false;
		}
		if (_isXeSSFrameGenerationActive) {
			if (!_hdrPresentationTexture ||
				!_hdrPresentationAdapter.ConvertCanonicalToHdr10(
					texture, _hdrPresentationTexture.get(),
					HdrColorTransform::ForFrame(_frameSource->GetHdrFrameMetadata().color))) {
				Logger::Get().Error("XeSSFG canonical-to-HDR10 conversion failed");
				return false;
			}
			publicationTexture = _hdrPresentationTexture.get();
			publicationMetadata.stage = HdrFrameStage::PublishedOutput;
			publicationMetadata.color.transfer = HdrTransferFunction::PQ;
			publicationMetadata.sourceFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
		}
	}
	const bool queuedPresentation = synchronous &&
		_synchronousFramePresentationEnabled.load(std::memory_order_acquire);
	const uint32_t sharedTextureSlot =
		_nextBackendSharedTextureSlot++ % _sharedTextureSlotCount;
	if (ScalingWindow::Get().Options().IsHdrCompatibilityEnabled() &&
		_capturedFrameId <= 2) {
		_LogHdrTextureStats(publicationTexture, fmt::format(
			"publication-source-slot-{}", sharedTextureSlot));
	}
	bool slotReserved = false;
	if (queuedPresentation) {
		const auto ringWaitStart = std::chrono::steady_clock::now();
		while (true) {
			const DWORD waitResult = WaitForSingleObject(
				_sharedTextureAvailableEvents[sharedTextureSlot].get(), 50);
			if (waitResult == WAIT_OBJECT_0) {
				slotReserved = true;
				break;
			}
			if (waitResult != WAIT_TIMEOUT ||
				!_synchronousFramePresentationEnabled.load(std::memory_order_acquire)) {
				_dlssFgPresentationStopping =
					!_synchronousFramePresentationEnabled.load(std::memory_order_acquire);
				return false;
			}
		}
		const auto ringWait = std::chrono::steady_clock::now() - ringWaitStart;
		_captureCadenceQueueWait += ringWait;
		_dlssFgRingWaitNanoseconds.fetch_add(
			(uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(ringWait).count(),
			std::memory_order_relaxed);
		_dlssFgRingWaitSamples.fetch_add(1, std::memory_order_relaxed);
	}
	auto releaseReservedSlot = [&]() noexcept {
		if (slotReserved && _sharedTextureAvailableEvents[sharedTextureSlot]) {
			SetEvent(_sharedTextureAvailableEvents[sharedTextureSlot].get());
			slotReserved = false;
		}
	};

	ID3D11Texture2D* xessMotion = nullptr;
	FrameGuidanceFrameId xessMotionFrameId = _capturedFrameId;
	bool xessMotionValid = false;
	bool xessMotionReset = true;
	if (!generatedFrame && _xessMotionRequest.method != OpticalFlowMethod::None) {
		D3D11_TEXTURE2D_DESC textureDesc{};
		texture->GetDesc(&textureDesc);
		const FrameGuidanceExtent extent{ textureDesc.Width, textureDesc.Height };
		const FrameGuidanceConsumerViews guidance =
			_frameGuidanceService.GetConsumerViews(
				_capturedFrameId, extent, _xessMotionRequest);
		if (guidance.produced.IsValidFor(_capturedFrameId, extent) &&
			!guidance.produced.motion.metadata.isZero) {
			xessMotion = guidance.produced.motion.texture;
			xessMotionReset = guidance.produced.requiresHistoryReset;
			const FrameGuidanceSyncPoint sync = guidance.produced.motion.metadata.sync;
			if (sync.fence && sync.value && FAILED(d3dDC->Wait(sync.fence, sync.value))) {
				Logger::Get().Warn("XeSSFG motion synchronization failed; using Zero Motion");
				xessMotion = nullptr;
				xessMotionReset = true;
			}
			xessMotionValid = xessMotion != nullptr;
		}
	}

	HRESULT hr = S_OK;
	FrameTrace::Scope tracePublication(FrameTrace::Event::Publication, generatedFrame, sharedTextureSlot);
	FrameTrace::Scope traceTransaction(FrameTrace::Event::PublicationTransaction);
	const auto transactionStart = std::chrono::steady_clock::now();
	{
		// One CPU transaction and one matching GPU key for all slot images.
		std::lock_guard accessLock(_sharedTextureAccessMutexes[sharedTextureSlot]);
		const uint64_t currentKey =
			_sharedTextureMutexKeys[sharedTextureSlot].load(std::memory_order_acquire);
		const uint64_t key = currentKey + 1;
		std::array<IDXGIKeyedMutex*, 3> mutexes{
			_backendSharedTextureMutexes[sharedTextureSlot].get(),
			_passThroughFrames.BackendMutex(sharedTextureSlot),
			_backendSharedMotionTextureMutexes[sharedTextureSlot].get() };
		size_t failedIndex = mutexes.size();
		hr = AcquirePresentationTextures(mutexes, currentKey, 250, &failedIndex);
		if (FAILED(hr) && failedIndex == 1) {
			if (_passThroughFrames.DisableSharing()) {
				const auto& window = ScalingWindow::Get();
				if (const auto& report = window.Options().reportErrorDetails) {
					report(window.SrcTracker().Handle(), ScalingError::PassThroughUnavailable,
						"Acquire backend reference / continuing effects", static_cast<uint32_t>(hr));
				}
			}
			mutexes[1] = nullptr;
			hr = AcquirePresentationTextures(mutexes, currentKey, 250);
		}
		if (SUCCEEDED(hr)) {
			HdrFrameMetadata frameMetadata = _frameSource ?
				_frameSource->GetHdrFrameMetadata() : HdrFrameMetadata{};
			frameMetadata.frameId = _capturedFrameId;
			frameMetadata.captureSequence = captureSequence;
			frameMetadata.resourceGeneration = resourceGeneration != 0 ? resourceGeneration : currentGeneration;
			frameMetadata.timestamp100ns = _frameSource ? _frameSource->CaptureTimestamp100ns() : 0;
			frameMetadata.generated = generatedFrame;
			frameMetadata.stage = generatedFrame ? HdrFrameStage::GeneratedOutput : HdrFrameStage::PublishedOutput;
			frameMetadata.valid = true;
			_sharedFrameMetadata[sharedTextureSlot] = frameMetadata;
			d3dDC->CopyResource(_backendSharedTextures[sharedTextureSlot].get(), publicationTexture);
			if (ScalingWindow::Get().Options().IsHdrCompatibilityEnabled() &&
				_capturedFrameId <= 2) {
				_LogHdrTextureStats(_backendSharedTextures[sharedTextureSlot].get(),
					fmt::format("publication-shared-slot-{}", sharedTextureSlot));
			}
			_passThroughFrames.Publish(sharedTextureSlot, generatedFrame);
			if (mutexes[2] && xessMotionValid) {
				d3dDC->CopyResource(_backendSharedMotionTextures[sharedTextureSlot].get(), xessMotion);
			}
			_sharedMotionFrameIds[sharedTextureSlot].store(xessMotionFrameId, std::memory_order_release);
			_sharedMotionValid[sharedTextureSlot].store(xessMotionValid, std::memory_order_release);
			_sharedMotionReset[sharedTextureSlot].store(xessMotionReset || !xessMotionValid,
				std::memory_order_release);
			_sharedTextureCaptureSequences[sharedTextureSlot].store(
				captureSequence, std::memory_order_release);
			_sharedTextureFrameIds[sharedTextureSlot].store(
				publicationMetadata.frameId, std::memory_order_release);
			_sharedTextureResourceGenerations[sharedTextureSlot].store(
				publicationMetadata.resourceGeneration, std::memory_order_release);
			_sharedTextureTimestamps[sharedTextureSlot].store(
				_frameSource->CaptureTimestamp100ns(), std::memory_order_release);
			_sharedFrameMetadata[sharedTextureSlot] = publicationMetadata;
			hr = ReleasePresentationTextures(mutexes, key);
			if (SUCCEEDED(hr)) _sharedTextureMutexKeys[sharedTextureSlot].store(key, std::memory_order_release);
		}
	}
	if (FAILED(hr)) {
		releaseReservedSlot();
		Logger::Get().ComError("Shared presentation slot transaction failed", hr);
		return false;
	}

	traceTransaction.End();
	FrameTrace::Scope traceFence(FrameTrace::Event::PublicationFence);
	const auto transactionEnd = std::chrono::steady_clock::now();
	// Signal after the copy so the D3D12 generated texture cannot be reused
	// until its D3D11 copy into the bounded presentation ring has completed.
	hr = d3dDC->Signal(_d3dFence.get(), ++_fenceValue);
	if (SUCCEEDED(hr)) {
		hr = _d3dFence->SetEventOnCompletion(_fenceValue, _fenceEvent.get());
	}
	if (FAILED(hr)) {
		releaseReservedSlot();
		Logger::Get().ComError("Signal shared presentation copy failed", hr);
		return false;
	}
	d3dDC->Flush();
	_fenceEvent.wait();
	traceFence.End();
	const double transactionMs = std::chrono::duration<double, std::milli>(
		transactionEnd - transactionStart).count();
	const double fenceMs = std::chrono::duration<double, std::milli>(
		std::chrono::steady_clock::now() - transactionEnd).count();
	_publicationTransactionTotalMs += transactionMs;
	_publicationTransactionMaxMs = std::max(_publicationTransactionMaxMs, transactionMs);
	_publicationFenceTotalMs += fenceMs;
	_publicationFenceMaxMs = std::max(_publicationFenceMaxMs, fenceMs);
	if (++_publicationTimingSamples == 120) {
		Logger::Get().Info(fmt::format(
			"Backend publication CPU timing: samples=120 transactionAvgMs={:.3f} transactionMaxMs={:.3f} fenceAvgMs={:.3f} fenceMaxMs={:.3f} profiling={}",
			_publicationTransactionTotalMs / 120, _publicationTransactionMaxMs,
			_publicationFenceTotalMs / 120, _publicationFenceMaxMs, _effectsProfiler.IsProfiling()));
		_publicationTimingSamples = 0;
		_publicationTransactionTotalMs = _publicationTransactionMaxMs = 0;
		_publicationFenceTotalMs = _publicationFenceMaxMs = 0;
	}
	_sharedPresentIntervalNs[sharedTextureSlot].store(
		_synchronousPresentInterval.count(), std::memory_order_release);
	_sharedTextureContainsGeneratedFrame[sharedTextureSlot].store(
		generatedFrame, std::memory_order_release);
	_latestSharedTextureSlot.store(sharedTextureSlot, std::memory_order_release);

	if (queuedPresentation) {
		_pendingDLSSFGFrontendFrames.fetch_add(1, std::memory_order_release);
		if (!PostMessage(
			ScalingWindow::Get().Handle(),
			CommonSharedConstants::WM_FRONTEND_RENDER_DLSSFG,
			sharedTextureSlot,
			_sharedTextureGeneration.load(std::memory_order_acquire)
		)) {
			_pendingDLSSFGFrontendFrames.fetch_sub(1, std::memory_order_acq_rel);
			releaseReservedSlot();
			const bool stopping =
				!_synchronousFramePresentationEnabled.load(std::memory_order_acquire) ||
				!IsWindow(ScalingWindow::Get().Handle());
			_dlssFgPresentationStopping = stopping;
			if (generatedFrame) {
				++_dlssFgGeneratedPublishFailure;
			} else {
				++_dlssFgRealPublishFailure;
			}
			if (!stopping) {
				Logger::Get().Warn("DLSSFG synchronous frontend presentation failed");
			}
			return false;
		}
		slotReserved = false;
		++_dlssFgPresentedFrameCount;
		if (generatedFrame) {
			++_dlssFgGeneratedPublishSuccess;
		} else {
			++_dlssFgRealPublishSuccess;
		}
		const auto now = std::chrono::steady_clock::now();
		if (_dlssFgDiagnosticsStart.time_since_epoch().count() == 0) {
			_dlssFgDiagnosticsStart = now;
		} else {
			const double elapsed = std::chrono::duration<double>(
				now - _dlssFgDiagnosticsStart).count();
			if (elapsed >= 1.0) {
				Logger::Get().Info(fmt::format(
					"DLSSFG presentation ring: captured={:.1f} FPS, queued={:.1f} FPS "
					"generatedPublish={}/{} realPublish={}/{} pacing={:.3f} ms excludedRingWait={:.3f} ms",
					_dlssFgCapturedFrameCount / elapsed,
					_dlssFgPresentedFrameCount / elapsed,
					_dlssFgGeneratedPublishSuccess,
					_dlssFgGeneratedPublishFailure,
					_dlssFgRealPublishSuccess,
					_dlssFgRealPublishFailure,
					std::chrono::duration<double, std::milli>(_synchronousPresentInterval).count(),
					std::chrono::duration<double, std::milli>(_captureCadence.LastDownstreamWait()).count()));
				_dlssFgDiagnosticsStart = now;
				_dlssFgCapturedFrameCount = 0;
				_dlssFgPresentedFrameCount = 0;
				_dlssFgGeneratedPublishSuccess = 0;
				_dlssFgGeneratedPublishFailure = 0;
				_dlssFgRealPublishSuccess = 0;
				_dlssFgRealPublishFailure = 0;
			}
		}
	}

	return true;
}

bool Renderer::_UpdateDynamicConstants() const noexcept {
	// cbuffer __CB2 : register(b1) { uint __frameCount; };

	ID3D11DeviceContext4* d3dDC = _backendResources.GetD3DDC();

	D3D11_MAPPED_SUBRESOURCE ms;
	HRESULT hr = d3dDC->Map(_dynamicCB.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
	if (SUCCEEDED(hr)) {
		// 避免使用 *(uint32_t*)ms.pData，见
		// https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-map
		const uint32_t frameCount = _stepTimer.FrameCount();
		std::memcpy(ms.pData, &frameCount, 4);
		d3dDC->Unmap(_dynamicCB.get(), 0);
	} else {
		Logger::Get().ComError("Map 失败", hr);
		return false;
	}

	return true;
}

winrt::IAsyncOperation<bool> Renderer::_TakeScreenshotImpl(
	uint32_t effectIdx,
	uint32_t passIdx,
	uint32_t outputIdx
) noexcept {
	const bool displayed = effectIdx == std::numeric_limits<uint32_t>::max();
	const bool original = displayed && _isPassThroughActive;
	// Snapshot session data before the first suspension. No ScalingWindow
	// state is read during encoding or notification on a background thread.
	const auto& options = ScalingWindow::Get().Options();
	const auto screenshotsDir = options.screenshotsDir;
	const auto report = options.reportErrorDetails;
	const auto toast = options.showToast;
	const HWND target = ScalingWindow::Get().SrcTracker().Handle();
	const auto successMsg = ScalingWindow::Get().GetLocalizedString(L"Message_ScreenshotSaved");
	const std::string effectName = displayed ? (original ? "Original image" : "Displayed effects")
		: _activeEffectDescs[effectIdx]->name;
	const auto dispatcher = displayed ? ScalingWindow::Dispatcher() : _backendThreadDispatcher;
	auto fail = [&](ScalingError error, std::string_view stage, uint32_t code = 0) {
		if (report) report(target, error, effectName + " / " +
			StrHelper::UTF16ToUTF8(screenshotsDir.native()) + " / " + std::string(stage), code);
	};
	if (!displayed) co_await dispatcher;

	// 最后一个通道的输出即 OUTPUT 不会被覆盖，可以直接使用。
	// 倒数第二个通道的输出也不会被覆盖，因为最后一个通道只会写入 OUTPUT。
	// 从倒数第三个通道开始需要检查输出是否被后面的通道覆盖。
	bool isOverwritten = false;
	ID3D11Texture2D* sourceTex;
	EffectIntermediateTextureFormat format;
	// 效果输出保存为 png，中间结果保存为 dds
	const wchar_t* imgFormat;

	if (displayed) {
		sourceTex = original ? _passThroughFrames.FrontendTexture(true)
			: (_frontendPresentedBaseValid ? _frontendPresentedBaseTexture.get() : nullptr);
		if (!sourceTex) {
			fail(ScalingError::ScreenshotReadbackFailed, "No presented scene available");
			co_return false;
		}
		format = EffectIntermediateTextureFormat::R8G8B8A8_UNORM;
		imgFormat = L"png";
	} else if (passIdx == std::numeric_limits<uint32_t>::max()) {
		sourceTex = _effectDrawers[effectIdx].GetOutputTexture();
		format = _activeEffectDescs[effectIdx]->textures[1].format;
		imgFormat = L"png";
	} else {
		const std::vector<EffectPassDesc>& passes = _activeEffectDescs[effectIdx]->passes;
		const uint32_t passCount = (uint32_t)passes.size();

		const SmallVector<uint32_t>& outputs = passes[passIdx].outputs;
		// 只有一个输出时才允许不提供 outputIdx
		assert(outputIdx != std::numeric_limits<uint32_t>::max() || outputs.size() == 1);
		const uint32_t targetOutput =
			outputIdx == std::numeric_limits<uint32_t>::max() ? outputs[0] : outputs[outputIdx];

		sourceTex = _effectDrawers[effectIdx].GetTexture(targetOutput);
		format = _activeEffectDescs[effectIdx]->textures[targetOutput].format;
		imgFormat = targetOutput == 1 ? L"png" : L"dds";

		if (passIdx + 3 <= passCount) {
			// 检查 targetOutput 是否被后面的通道修改
			for (uint32_t i = passIdx + 1, end = passCount - 1; i < end; ++i) {
				const SmallVector<uint32_t>& curOutputs = passes[i].outputs;
				if (std::find(curOutputs.begin(), curOutputs.end(), targetOutput) != curOutputs.end()) {
					isOverwritten = true;
					break;
				}
			}
		}
	}

	// Retain device interfaces across the GPU wait. After CopyResource the
	// coroutine no longer needs Renderer or its textures and can outlive it.
	winrt::com_ptr<ID3D11Device5> device;
	device.copy_from(displayed ? _frontendResources.GetD3DDevice() : _backendResources.GetD3DDevice());
	winrt::com_ptr<ID3D11DeviceContext4> context;
	context.copy_from(displayed ? _frontendResources.GetD3DDC() : _backendResources.GetD3DDC());
	auto* d3dDevice = device.get();
	auto* d3dDC = context.get();

	if (isOverwritten) {
		// 重新渲染
		d3dDC->ClearState();

		if (ID3D11Buffer* t = _dynamicCB.get()) {
			d3dDC->CSSetConstantBuffers(1, 1, &t);
		}

		_effectDrawers[effectIdx].DrawForExport(*_activeEffectDescs[effectIdx], passIdx);
	}

	// 创建 staging 纹理
	D3D11_TEXTURE2D_DESC desc;
	sourceTex->GetDesc(&desc);
	desc.Usage = D3D11_USAGE_STAGING;
	desc.BindFlags = 0;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	desc.MiscFlags = 0;

	winrt::com_ptr<ID3D11Texture2D> stagingTex;
	HRESULT hr = d3dDevice->CreateTexture2D(
		&desc, nullptr, stagingTex.put());
	if (FAILED(hr)) {
		fail(ScalingError::ScreenshotReadbackFailed, "CreateTexture2D 失败", static_cast<uint32_t>(hr));
		Logger::Get().ComError("CreateTexture2D 失败", hr);
		co_return false;
	}

	d3dDC->CopyResource(stagingTex.get(), sourceTex);

	// 如果要导出的纹理不会被覆盖则转到后台等待 GPU 以防止卡顿
	if (!isOverwritten) {
		// 为避免混乱，使用独立的栅栏
		winrt::com_ptr<ID3D11Fence> localFence;
		wil::unique_event_nothrow localFenceEvent;

		hr = d3dDevice->CreateFence(
			0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&localFence));
		if (FAILED(hr)) {
			fail(ScalingError::ScreenshotReadbackFailed, "CreateFence 失败", static_cast<uint32_t>(hr));
			Logger::Get().ComError("CreateFence 失败", hr);
			co_return false;
		}

		if (!localFenceEvent.try_create(wil::EventOptions::None, nullptr)) {
			fail(ScalingError::ScreenshotReadbackFailed, "CreateEvent", GetLastError());
			Logger::Get().Win32Error("CreateEvent 失败");
			co_return false;
		}

		hr = d3dDC->Signal(localFence.get(), 1);
		if (FAILED(hr)) {
			fail(ScalingError::ScreenshotReadbackFailed, "Signal 失败", static_cast<uint32_t>(hr));
			Logger::Get().ComError("Signal 失败", hr);
			co_return false;
		}

		hr = localFence->SetEventOnCompletion(1, localFenceEvent.get());
		if (FAILED(hr)) {
			fail(ScalingError::ScreenshotReadbackFailed, "SetEventOnCompletion 失败", static_cast<uint32_t>(hr));
			Logger::Get().ComError("SetEventOnCompletion 失败", hr);
			co_return false;
		}

		d3dDC->Flush();

		co_await winrt::resume_background();
		if (!localFenceEvent.wait(10000)) {
			fail(ScalingError::ScreenshotReadbackFailed, "GPU fence timeout", WAIT_TIMEOUT);
			co_return false;
		}
		co_await dispatcher;
	}

	// 读取纹理数据到内存。isOverwritten 为真时这个调用将阻塞 CPU
	D3D11_MAPPED_SUBRESOURCE mapped;
	hr = d3dDC->Map(stagingTex.get(), 0, D3D11_MAP_READ, 0, &mapped);
	if (FAILED(hr)) {
		fail(ScalingError::ScreenshotReadbackFailed, "Map 失败", static_cast<uint32_t>(hr));
		Logger::Get().ComError("Map 失败", hr);
		co_return false;
	}

	std::vector<uint8_t> pixelData(size_t(mapped.RowPitch) * desc.Height);
	std::memcpy(pixelData.data(), mapped.pData, pixelData.size());

	d3dDC->Unmap(stagingTex.get(), 0);

	co_await winrt::resume_background();

	// Serialize disk naming and writing, not rendering. This also avoids
	// holding a reference to a destroyed Renderer while scanning the folder.
	static std::mutex screenshotSaveMutex;
	std::lock_guard saveLock(screenshotSaveMutex);
	if (!Win32Helper::CreateDir(screenshotsDir.c_str(), true)) {
		fail(ScalingError::ScreenshotDirectoryFailed, "CreateDir", GetLastError());
		co_return false;
	}
	const uint32_t screenshotNum = ScreenshotHelper::FindUnusedScreenshotNum(screenshotsDir);
	if (!screenshotNum) {
		fail(ScalingError::ScreenshotDirectoryFailed, "Enumerate screenshot directory");
		co_return false;
	}
	const std::wstring fileName = fmt::format(L"Magpie_{:03}.{}", screenshotNum, imgFormat);
	const auto fullPath = screenshotsDir / fileName;
	TextureSaveError saveError;
	if (!TextureHelper::SaveTexture(fullPath.c_str(), desc.Width, desc.Height,
		format, pixelData, mapped.RowPitch, &saveError)) {
		if (report) report(target, saveError.fileWriteFailed ? ScalingError::ScreenshotWriteFailed
			: ScalingError::ScreenshotEncodeFailed,
			effectName + " / " + StrHelper::UTF16ToUTF8(fullPath.native()),
			static_cast<uint32_t>(saveError.code));
		co_return false;
	}
	if (toast) toast(target, fmt::format(fmt::runtime(std::wstring_view(successMsg)), fileName));
	co_return true;
}

// 监听 PrintScreen 实现截屏时隐藏光标
LRESULT CALLBACK Renderer::_LowLevelKeyboardHook(int nCode, WPARAM wParam, LPARAM lParam) {
	if (nCode != HC_ACTION ||
		(wParam != WM_KEYDOWN && wParam != WM_SYSKEYDOWN)) {
		return CallNextHookEx(NULL, nCode, wParam, lParam);
	}

	KBDLLHOOKSTRUCT* info = (KBDLLHOOKSTRUCT*)lParam;
	if (info->vkCode == VK_LWIN || info->vkCode == VK_RWIN ||
		(info->flags & LLKHF_ALTDOWN)) {
		// System UI/focus transitions terminate an Overlay drag as one ordered
		// cancel transaction. The hook only posts; ImGui remains single-threaded.
		PostMessage(ScalingWindow::Get().Handle(), WM_CANCELMODE, 0, 0);
	}
	if (info->vkCode == VK_SNAPSHOT) {
		// 为了缩短钩子处理时间，异步执行所有逻辑
		ScalingWindow::Dispatcher().TryEnqueue([]() -> winrt::fire_and_forget {
			// 暂时隐藏光标
			Renderer& renderer = ScalingWindow::Get().Renderer();
			renderer._cursorDrawer.IsCursorVisible(false);
			renderer.Render();

			const uint32_t runId = ScalingWindow::RunId();

			winrt::DispatcherQueue dispatcher = ScalingWindow::Dispatcher();
			co_await 200ms;
			co_await dispatcher;

			if (ScalingWindow::RunId() == runId &&
				!renderer._cursorDrawer.IsCursorVisible()
			) {
				renderer._cursorDrawer.IsCursorVisible(true);
				renderer.Render();
			}
		});
	}

	return CallNextHookEx(NULL, nCode, wParam, lParam);
}

}
