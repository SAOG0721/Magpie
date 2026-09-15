#pragma once
#include <parallel_hashmap/phmap.h>
#include <string_view>
#include <memory>
#include "EffectParameterPersistence.h"
#include "FramePacingOptions.h"
#include "HdrComponents.h"
#include "OverlayWindowGeometry.h"
#include <mutex>
#include <functional>

namespace Magpie {

enum class OverlayAction { Profiler, EffectParameters, Screenshot, ToolbarPin, Comparison };

struct ToolbarShortcutLabels {
	std::string profiler;
	std::string parameters;
	std::string screenshot;
	std::string pin;
	std::string comparison;
	std::string fullscreen;
	std::string windowed;
	bool operator==(const ToolbarShortcutLabels&) const = default;
};

enum class CaptureMethod {
	GraphicsCapture,
	DesktopDuplication,
	GDI,
	DwmSharedSurface,
	COUNT
};

enum class MultiMonitorUsage {
	Closest,
	Intersected,
	All,
	Specific,
	COUNT
};

enum class CursorInterpolationMode {
	NearestNeighbor,
	Bilinear,
	COUNT
};

struct Cropping {
	float Left;
	float Top;
	float Right;
	float Bottom;
};

struct GraphicsCardId {
	// idx 为显卡索引，vendorId 和 deviceId 用于验证，如果不匹配则遍历显卡查找匹配。这可以处理显卡
	// 改变的情况，比如某些笔记本电脑可以在混合架构和独显直连之间切换。
	// idx 有两个作用，一是作为性能优化，二是用于区分同一型号的两个显卡。
	// idx 为 -1 表示使用默认显卡，如果此时 vendorId 和 deviceId 有值表示由于目前不存在该显卡因此
	// 使用默认显卡，如果以后该显卡再次可用将自动使用。
	int idx = -1;
	uint32_t vendorId = 0;
	uint32_t deviceId = 0;
};

enum class DestAlignment {
	LeftTop,
	Top,
	RightTop,
	Left,
	Center,
	Right,
	LeftBottom,
	Bottom,
	RightBottom,
	COUNT
};

enum class ScalingType {
	Normal,		// Scale 表示缩放倍数
	Fit,		// Scale 表示相对于屏幕能容纳的最大等比缩放的比例
	Absolute,	// Scale 表示目标大小（单位为像素）
	Fill		// 充满屏幕，此时不使用 Scale 参数
};

struct EffectOption {
	std::string name;
	phmap::flat_hash_map<std::string, float> parameters;
	ScalingType scalingType = ScalingType::Normal;
	std::pair<float, float> scale = { 1.0f,1.0f };

	bool HasScale() const noexcept {
		return scalingType != ScalingType::Normal ||
			!IsApprox(scale.first, 1.0f) || !IsApprox(scale.second, 1.0f);
	}
};

enum class FrameGenerationEffectKind : uint8_t {
	None,
	DLSS,
	XeSSX2,
	XeSSMultiFrame
};

inline FrameGenerationEffectKind ClassifyFrameGenerationEffect(
	std::string_view name
) noexcept {
	if (name == "DLSSFG\\DLSS_FrameGeneration") {
		return FrameGenerationEffectKind::DLSS;
	}
	if (name == "XeSSFG\\XeSS_FrameGeneration_x2_ZeroMV") {
		return FrameGenerationEffectKind::XeSSX2;
	}
	if (name == "XeSSFG\\XeSS_FrameGeneration" || name == "XeSSFG\\XeSS_MultiFrameGeneration_ZeroMV") {
		return FrameGenerationEffectKind::XeSSMultiFrame;
	}
	return FrameGenerationEffectKind::None;
}

inline FrameGenerationEffectKind ClassifyFrameGenerationEffect(
	std::wstring_view name
) noexcept {
	if (name == L"DLSSFG\\DLSS_FrameGeneration") {
		return FrameGenerationEffectKind::DLSS;
	}
	if (name == L"XeSSFG\\XeSS_FrameGeneration_x2_ZeroMV") {
		return FrameGenerationEffectKind::XeSSX2;
	}
	if (name == L"XeSSFG\\XeSS_FrameGeneration" || name == L"XeSSFG\\XeSS_MultiFrameGeneration_ZeroMV") {
		return FrameGenerationEffectKind::XeSSMultiFrame;
	}
	return FrameGenerationEffectKind::None;
}

struct FrameGenerationChainValidation {
	uint32_t count = 0;
	FrameGenerationEffectKind first = FrameGenerationEffectKind::None;
	FrameGenerationEffectKind second = FrameGenerationEffectKind::None;

	bool HasConflict() const noexcept { return count > 1; }
	bool HasFrameGeneration() const noexcept { return count != 0; }
};

// 配置层的 EffectItem 带 enabled 字段，运行期的 EffectOption 没有。
// 被禁用的效果不参与渲染，因此也不应参与补帧冲突校验。这里用概念做鸭子类型
// 判断，让所有调用点（含 UI 的 CanAddEffect 与运行期装配）都自动忽略禁用项，
// 不会因为漏过滤而误报「一组内存在多个补帧效果」。
template <typename T>
concept HasEnabledFlag = requires(T effect) {
	effect.enabled ? 0 : 1;
};

template <typename EffectRange>
FrameGenerationChainValidation ValidateFrameGenerationChain(
	const EffectRange& effects
) noexcept {
	FrameGenerationChainValidation result;
	for (const auto& effect : effects) {
		if constexpr (HasEnabledFlag<decltype(effect)>) {
			if (!effect.enabled) {
				continue;
			}
		}
		const FrameGenerationEffectKind kind =
			ClassifyFrameGenerationEffect(effect.name);
		if (kind == FrameGenerationEffectKind::None) {
			continue;
		}
		if (result.count == 0) {
			result.first = kind;
		} else if (result.count == 1) {
			result.second = kind;
		}
		++result.count;
	}
	return result;
}

enum class EffectParameterApplyMode : uint8_t {
	Unavailable,
	Live,
	RestartRequired
};

enum class EffectParameterRestartReason : uint8_t {
	None,
	InlineParameters,
	NativeBackend,
	ResourceRecreation,
	FrameGuidance,
	FrameGeneration
};

struct EffectParameterRuntimeInfo {
	std::string name;
	EffectParameterApplyMode applyMode = EffectParameterApplyMode::RestartRequired;
	EffectParameterRestartReason restartReason = EffectParameterRestartReason::NativeBackend;
	bool automaticRestart = false;
};

// One state per user-started session, retained across automatic rebuilds.
// Backend writes only applied values; UI edits write only desired values.
struct EffectParameterSessionState {
	struct Snapshot {
		std::vector<EffectOption> applied;
		std::vector<EffectOption> desired;
		FrameSyncSettings frameSync;
		uint64_t revision = 0;
		bool applying = false;
		bool applyFailed = false;
	};

	explicit EffectParameterSessionState(const std::vector<EffectOption>& effects,
		FrameSyncSettings frameSync = {}) : _snapshot{ effects, effects, frameSync, 1 } {}
	// Save receipts survive automatic rebuilds too. The revision is allocated
	// only by the scaling/UI thread; completion itself is atomic.
	std::shared_ptr<EffectParametersSaveState> saveState = std::make_shared<EffectParametersSaveState>();
	uint64_t saveRevision = 0;

	bool ReadIfChanged(Snapshot& result) const {
		std::scoped_lock lock(_mutex);
		if (result.revision == _snapshot.revision) return false;
		result = _snapshot;
		return true;
	}
	bool HasChanges(uint64_t revision) const {
		std::scoped_lock lock(_mutex);
		return revision != _snapshot.revision;
	}
	std::vector<EffectOption> Applied() const {
		std::scoped_lock lock(_mutex);
		return _snapshot.applied;
	}
	void Applied(const std::vector<EffectOption>& effects) {
		std::scoped_lock lock(_mutex);
		_snapshot.applied = effects;
		++_snapshot.revision;
	}
	void Desired(const std::vector<EffectOption>& effects) {
		std::scoped_lock lock(_mutex);
		_snapshot.desired = effects;
		++_snapshot.revision;
	}
	void Desired(uint32_t effect, const std::string& parameter, float value) {
		std::scoped_lock lock(_mutex);
		if (effect >= _snapshot.desired.size()) return;
		_snapshot.desired[effect].parameters[parameter] = value;
		++_snapshot.revision;
	}
	void DesiredFrameSync(FrameSyncSettings value) {
		std::scoped_lock lock(_mutex);
		if (_snapshot.frameSync == value) return;
		_snapshot.frameSync = value;
		++_snapshot.revision;
	}
	void Applying(bool applying, bool failed = false) {
		std::scoped_lock lock(_mutex);
		_snapshot.applying = applying;
		_snapshot.applyFailed = failed;
		++_snapshot.revision;
	}
	void RevertDesired(uint32_t effect, const std::string& parameter, float expected, float previous) {
		std::scoped_lock lock(_mutex);
		if (effect >= _snapshot.desired.size()) return;
		if (RestoreRejectedEffectParameter(_snapshot.desired[effect].parameters, parameter, expected, previous))
			++_snapshot.revision;
	}
private:
	mutable std::mutex _mutex;
	Snapshot _snapshot;
};

enum class EffectParametersRequestKind : uint8_t {
	AutoSave,
	SaveAndRestart
};

struct EffectParametersRequest {
	EffectParametersRequestKind kind = EffectParametersRequestKind::AutoSave;
	std::vector<EffectOption> effects;
	std::vector<EffectOption> previousEffects;
	FrameSyncSettings frameSync;
	FrameSyncSettings previousFrameSync;
	std::shared_ptr<EffectParametersSaveState> saveState;
	uint64_t revision = 0;
	HWND hwndSource = nullptr;
	HWND hwndScaling = nullptr;
	uint32_t scalingRunId = 0;
};

enum class DuplicateFrameDetectionMode {
	Always,
	Dynamic,
	Never
};

enum class ToolbarState {
	Off,
	AlwaysShow,
	AutoHide,
	COUNT
};

enum class ParameterPanelState : uint8_t { Closed, Edit, Preview };

struct OverlaySessionState {
	bool toolbarVisible = false;
	bool toolbarPinned = false;
	bool profilerVisible = false;
	bool effectParametersVisible = false;
	ParameterPanelState parameterPanelState = ParameterPanelState::Closed;
};

struct OverlayOptions {
	phmap::flat_hash_map<std::string, OverlayWindowOption> windows;
};

// Values are exposed as MP diagnostic codes. Append new values; do not reorder.
enum class ScalingError {
	NoError,

	/////////////////////////////////////
	//
	// 先决条件错误
	//
	/////////////////////////////////////

	// 未配置缩放模式或者缩放模式不合法
	InvalidScalingMode,
	// 启用触控支持失败
	TouchSupport,
	// 3D 游戏模式下不支持窗口模式缩放
	Windowed3DGameMode,
	// Desktop Duplication 不支持窗口模式缩放
	WindowedDesktopDuplication,
	// 通用的不支持缩放错误
	InvalidSourceWindow,
	// 因窗口已最大化或全屏而无法缩放，可通过更改设置强制缩放
	Maximized,
	// 因窗口的 IL 更高而无法缩放
	LowIntegrityLevel,
	// 应用自定义裁剪后尺寸太小或为负
	InvalidCropping,
	// 窗口不符合窗口模式缩放的条件，如已最大化
	BannedInWindowedMode,

	/////////////////////////////////////
	//
	// 初始化和缩放时错误
	//
	/////////////////////////////////////

	// 通用的缩放失败错误
	ScalingFailedGeneral,
	// FrameSource 初始化失败
	CaptureFailed,
	// ID3D11Device5::CreateFence 失败
	CreateFenceFailed,
	// NVIDIA VSR 运行库无法从当前程序路径完成初始化
	NvidiaVsrPathUnsupported,
	OpticalFlowProviderUnavailable,
	NvidiaOpticalFlowUnsupported,
	NvidiaOpticalFlowQualityUnsupported,
	AmdOpticalFlowUnsupported,
	OpticalFlowInteropFailed,
	ConflictingFrameGenerationEffects,
	XeSSMfgOpticalFlowUnsupported,
	XeSSMfgCompatibilityUnavailable,
	XeSSMfgUnsupported,
	XeSSMfgMultiplierUnsupported,
	ScalingModeNotSelected,
	ScalingModeEmpty,
	ScalingModeUnknownEffect,
	GraphicsDeviceInitFailed,
	PresentationInitFailed,
	EffectCompileFailed,
	EffectResourceFailed,
	NativeEffectInitFailed,
	FrameGenerationInitFailed,
	OverlayInitFailed,
	SharedTextureOpenFailed,
	DlssNrUnavailable,
	FrameGenerationDisabled,
	ConfigurationWriteFailed,
	EffectParameterConflict,
	EffectParameterLiveFailed,
	ScreenshotDirectoryFailed,
	ScreenshotEncodeFailed,
	ScreenshotReadbackFailed,
	ScreenshotWriteFailed,
	SourceWindowClosed,
	SourceWindowUnresponsive,
	SourceWindowTooSmall,
	SourceWindowOffscreen,
	SourceWindowUnsupported,
	SourceWindowGeometryFailed,
	ScalingAlreadyActive,
	ScalingWindowCreationFailed,
	DisplayLayoutFailed,
	ImportReadFailed,
	ImportEmpty,
	ImportInvalidJson,
	ImportWrongFileType,
	ImportIncompatible,
	ExportWriteFailed,
	FileDialogFailed,
	PassThroughUnavailable,
	NgxRestartRequired,
	ScreenshotIntermediateEncodeFailed,
	ConfigurationRecoveredBackup,
	ConfigurationRecoveredPartial,
	ConfigurationRepaired,
	ConfigurationResetDefaults,
	CaptureMethodUnavailable,
	HdrComponentExpectedHdr,
	HdrComponentExpectedSdr,
	HdrComponentMissingPair,
	HdrComponentInvalidParameters,
	HdrCaptureRequired,
	HdrDisplayRequired,
	HdrCaptureMethodRequired,
	RtxHdrUnavailable,
	DuplicateScalingModeNames
};

struct ScalingFlags {
	static constexpr uint32_t WindowedMode = 1;
	static constexpr uint32_t DebugMode = 1 << 1;
	static constexpr uint32_t DisableEffectCache = 1 << 2;
	static constexpr uint32_t SaveEffectSources = 1 << 3;
	static constexpr uint32_t WarningsAreErrors = 1 << 4;
	static constexpr uint32_t SimulateExclusiveFullscreen = 1 << 5;
	static constexpr uint32_t Is3DGameMode = 1 << 6;
	static constexpr uint32_t CaptureTitleBar = 1 << 10;
	static constexpr uint32_t AdjustCursorSpeed = 1 << 11;
	static constexpr uint32_t DisableDirectFlip = 1 << 13;
	static constexpr uint32_t DisableFontCache = 1 << 14;
	static constexpr uint32_t AllowScalingMaximized = 1 << 15;
	static constexpr uint32_t EnableStatisticsForDynamicDetection = 1 << 16;
	// 只影响缩放行为，Magpie.Core 不负责启动 TouchHelper.exe
	static constexpr uint32_t TouchSupportEnabled = 1 << 17;
	static constexpr uint32_t InlineParams = 1 << 18;
	static constexpr uint32_t DisableFP16 = 1 << 19;
	static constexpr uint32_t BenchmarkMode = 1 << 20;
	static constexpr uint32_t DeveloperMode = 1 << 21;
	static constexpr uint32_t DisableTopmost = 1 << 22;
	static constexpr uint32_t EnableHdrCompatibility = 1 << 23;
};

struct ScalingOptions {
	DEFINE_FLAG_ACCESSOR(IsWindowedMode, ScalingFlags::WindowedMode, flags)
	DEFINE_FLAG_ACCESSOR(IsDeveloperMode, ScalingFlags::DeveloperMode, flags)
	DEFINE_FLAG_ACCESSOR(IsDebugMode, ScalingFlags::DebugMode, flags)
	DEFINE_FLAG_ACCESSOR(IsBenchmarkMode, ScalingFlags::BenchmarkMode, flags)
	DEFINE_FLAG_ACCESSOR(IsTopmostDisabled, ScalingFlags::DisableTopmost, flags)
	DEFINE_FLAG_ACCESSOR(IsFP16Disabled, ScalingFlags::DisableFP16, flags)
	DEFINE_FLAG_ACCESSOR(IsEffectCacheDisabled, ScalingFlags::DisableEffectCache, flags)
	DEFINE_FLAG_ACCESSOR(IsFontCacheDisabled, ScalingFlags::DisableFontCache, flags)
	DEFINE_FLAG_ACCESSOR(IsSaveEffectSources, ScalingFlags::SaveEffectSources, flags)
	DEFINE_FLAG_ACCESSOR(IsWarningsAreErrors, ScalingFlags::WarningsAreErrors, flags)
	DEFINE_FLAG_ACCESSOR(IsStatisticsForDynamicDetectionEnabled, ScalingFlags::EnableStatisticsForDynamicDetection, flags)
	DEFINE_FLAG_ACCESSOR(IsInlineParams, ScalingFlags::InlineParams, flags)
	DEFINE_FLAG_ACCESSOR(IsTouchSupportEnabled, ScalingFlags::TouchSupportEnabled, flags)
	DEFINE_FLAG_ACCESSOR(IsAllowScalingMaximized, ScalingFlags::AllowScalingMaximized, flags)
	DEFINE_FLAG_ACCESSOR(IsSimulateExclusiveFullscreen, ScalingFlags::SimulateExclusiveFullscreen, flags)
	DEFINE_FLAG_ACCESSOR(Is3DGameMode, ScalingFlags::Is3DGameMode, flags)
	DEFINE_FLAG_ACCESSOR(IsCaptureTitleBar, ScalingFlags::CaptureTitleBar, flags)
	DEFINE_FLAG_ACCESSOR(IsAdjustCursorSpeed, ScalingFlags::AdjustCursorSpeed, flags)
	DEFINE_FLAG_ACCESSOR(IsDirectFlipDisabled, ScalingFlags::DisableDirectFlip, flags)
	DEFINE_FLAG_ACCESSOR(IsHdrCompatibilityEnabled, ScalingFlags::EnableHdrCompatibility, flags)

	std::vector<EffectOption> effects;
	// Session-only plan. Persisted HDR flags remain retired; explicit components
	// select capture and output independently before creating graphics resources.
	HdrComponentPlan hdrComponents;
	bool IsHdrCaptureEnabled() const noexcept {
		return hdrComponents.enabled ? hdrComponents.captureHdr : IsHdrCompatibilityEnabled();
	}
	bool IsEffectHdrEnabled(size_t index) const noexcept {
		return hdrComponents.enabled && index < hdrComponents.stages.size()
			? hdrComponents.stages[index].inputHdr : IsHdrCompatibilityEnabled();
	}
	uint32_t scalingModeIdx = 0;
	std::shared_ptr<EffectParameterSessionState> parameterSession;
	std::wstring scalingModeName;
	uint32_t flags = ScalingFlags::AdjustCursorSpeed;
	Cropping cropping{};
	GraphicsCardId graphicsCardId;
	float minFrameRate = 0.0f;
	std::optional<float> maxFrameRate;
	bool isFrontEdgeSyncEnabled = true;
	bool isParameterFocusSwitchingEnabled = false;
	bool isVRREnabled = false;
	// 0 targets the display refresh rate, divided by FG multiplier for base FPS.
	float frontEdgeSyncFrameRate = 60.0f;
	FrameSyncMode frameSyncMode = FrameSyncMode::FrontEdge;
	float cursorScaling = 1.0f;
	CaptureMethod captureMethod = CaptureMethod::GraphicsCapture;
	MultiMonitorUsage multiMonitorUsage = MultiMonitorUsage::Closest;
	std::wstring preferredMonitorId;
	DestAlignment destAlignment = DestAlignment::Center;
	CursorInterpolationMode cursorInterpolationMode = CursorInterpolationMode::NearestNeighbor;
	std::optional<float> autoHideCursorDelay;
	DuplicateFrameDetectionMode duplicateFrameDetectionMode = DuplicateFrameDetectionMode::Dynamic;
	ToolbarState fullscreenInitialToolbarState = ToolbarState::AutoHide;
	ToolbarState windowedInitialToolbarState = ToolbarState::AutoHide;
	float initialWindowedScaleFactor = 0.0f;
	std::filesystem::path screenshotsDir;

	// 下面的成员支持在缩放时修改
	OverlayOptions overlayOptions;
	ToolbarShortcutLabels toolbarShortcutLabels;

	void (*showToast)(HWND hwndTarget, std::wstring_view msg) noexcept = nullptr;
	std::function<void(HWND hwndTarget, ScalingError error)> showError;
	std::function<void(HWND hwndTarget, ScalingError error,
		std::string_view context, uint32_t systemError)> reportErrorDetails;
	std::function<void(uint32_t, const EffectOption&, const std::string&, float, float)> revertEffectParameter;
	void (*save)(const ScalingOptions& options, HWND hwndScaling) noexcept = nullptr;
	bool (*requestEffectParameters)(
		const ScalingOptions& sessionOptions,
		EffectParametersRequest&& request
	) noexcept = nullptr;

	void Log() const noexcept;

	bool RealIsCaptureTitleBar() const noexcept {
		// GDI 和 DwmSharedSurface 不支持捕获标题栏
		return IsCaptureTitleBar() &&
			captureMethod != CaptureMethod::GDI && captureMethod != CaptureMethod::DwmSharedSurface;
	}

	bool RealIsAllowScalingMaximized() const noexcept {
		return IsAllowScalingMaximized() && !IsWindowedMode();
	}

	bool RealIsSimulateExclusiveFullscreen() const noexcept {
		return IsSimulateExclusiveFullscreen() && !IsWindowedMode();
	}
};

}
