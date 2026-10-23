#pragma once
#include <imgui.h>
#include "SmallVector.h"
#include "ImGuiImpl.h"
#include "ScalingOptions.h"
#include "EffectParameterResetGesture.h"
#include "ProfilerHistory.h"
#include "PresentationFrameRate.h"

namespace Magpie {

struct EffectDesc;
struct EffectParameterDesc;
struct EffectsProfilerSnapshot;

class OverlayDrawer {
public:
	OverlayDrawer() = default;
	~OverlayDrawer() noexcept;
	OverlayDrawer(const OverlayDrawer&) = delete;
	OverlayDrawer(OverlayDrawer&&) = delete;

	bool Initialize(DeviceResources& deviceResources, OverlayOptions& overlayOptions) noexcept;

	void Draw(
		uint32_t fps,
		const EffectsProfilerSnapshot& profile,
		POINT drawOffset,
		float hdrWhiteScale
	) noexcept;
	void ClearStates() noexcept;
	void OnPresentSucceeded() noexcept;
	void OnPresentFailed() noexcept;

	ToolbarState ToolbarState() const noexcept;

	void ToolbarState(Magpie::ToolbarState value) noexcept;

	void InvokeAction(OverlayAction action) noexcept;
	OverlaySessionState CaptureSessionState() const noexcept;
	void RestoreSessionState(const OverlaySessionState& state) noexcept;

	bool AnyVisibleWindow() const noexcept;
	bool IsEffectParametersVisible() const noexcept { return _isEffectParametersVisible; }
	bool IsEditingParameters() const noexcept { return _parameterFocusSwitchingEnabled && _parameterPanelState == ParameterPanelState::Edit; }
	bool IsParameterPreviewAt(POINT point) const noexcept {
		return _HasParameterForeground() && _imguiImpl.IsParameterPreviewAt(point);
	}
	HWND ParameterInputHandle() const noexcept { return _hwndParameterInput; }
	bool IsParameterFocusSettling() const noexcept {
		return _HasParameterForeground() && (_parameterInputTransition ||
			std::chrono::steady_clock::now() < _parameterFocusSettlesAt);
	}
	void SuspendParameterInput() noexcept;
	void ReleaseParameterInput() noexcept;
	bool HasHeldParameterInput() const noexcept;
	bool AllowAutomaticSourceFocus() const noexcept { return !IsEditingParameters() && !_parameterFocusFailed; }
	void UpdateParameterInputHost() noexcept;
	bool HandleParameterPreviewEscape(WPARAM message, const KBDLLHOOKSTRUCT& key) noexcept;
	bool IsEffectParameterInputActive() const noexcept {
		return _isEffectParametersVisible && _isEffectParameterInputActive;
	}

	bool MessageHandler(UINT msg, WPARAM wParam, LPARAM lParam) noexcept;
	bool HasPendingInput() const noexcept;
	bool HasUrgentInput() const noexcept;
	bool HasCriticalInput() const noexcept { return _imguiImpl.HasCriticalInput(); }

	bool NeedRedraw(uint32_t fps) const noexcept;

	void UpdateAfterActiveEffectsChanged() noexcept;

	bool IsCursorOnCaptionArea() const noexcept {
		return _isCursorOnCaptionArea && !_toolbarPlacement.IsDragging();
	}
	bool IsToolbarAt(POINT screenPoint) const noexcept;
	bool IsToolbarMoveCursor() const noexcept { return _isToolbarDragHovered || _toolbarPlacement.IsDragging(); }

private:
	bool _parameterFocusSwitchingEnabled = false;
	void _SetParameterPanelState(ParameterPanelState state, bool returnFocus = true, const char* request = "user action") noexcept;
	void _ToggleParameterPanel() noexcept;
	bool _EnsureParameterInputHost() noexcept;
	void _UpdateParameterPreviewHost() noexcept;
	bool _HasParameterForeground() const noexcept;
	bool _OwnsParameterInput() const noexcept;
	bool _PositionParameterInputHost(const RECT& rect) noexcept;
	void _SyncInheritedParameterKeys() noexcept;
	std::optional<ImGuiInputResult> _HandleParameterInputMessage(
		HWND sourceWindow, UINT message, WPARAM wParam, LPARAM lParam) noexcept;
	bool _BeginParameterInput(const char* request) noexcept;
	void _EndParameterInput(bool returnFocus) noexcept;
	void _FinishParameterInput() noexcept;
	static LRESULT CALLBACK _ParameterInputWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) noexcept;
	HWND _hwndParameterInput = nullptr;
	ParameterPanelState _parameterPanelState = ParameterPanelState::Closed;
	ParameterPanelState _pendingParameterPanelState = ParameterPanelState::Edit;
	bool _parameterInputTransition = false;
	bool _parameterInputOwned = false;
	bool _parameterLayerFailed = false;
	bool _parameterHostCanceled = false;
	uint32_t _parameterInputRunId = 0;
	bool _parameterEditPending = false;
	uint32_t _parameterEditRunId = 0;
	std::chrono::steady_clock::time_point _parameterFocusSettlesAt{};
	bool _returnClickPending = false;
	bool _escapePending = false;
	bool _parameterFocusFailed = false;
	bool _parameterResumeClickPending = false;
	bool _previewEscapeOwned = false, _previewEscapeCanClose = false, _previewClosePending = false;
	std::array<bool, 256> _parameterHeldKeys{};
	std::array<bool, 256> _parameterInheritedKeys{};
	uint32_t _parameterHeldButtons = 0;
	bool _BuildFonts() noexcept;
	SmallVector<ImWchar> _BuildFontUI(std::wstring_view language, const std::vector<uint8_t>& fontData) noexcept;
	void _BuildFontIcons(const char* fontPath) noexcept;

	struct _EffectDrawInfo {
		std::string label;
		const EffectDesc* desc = nullptr;
		std::span<const float> passTimings;
		float totalTime = 0.0f;
	};

	bool _DrawTimingItem(
		int& itemId,
		const char* text,
		const ImColor* color,
		float time,
		bool isExpanded = false
	) const noexcept;

	int _DrawEffectTimings(
		int& itemId,
		const _EffectDrawInfo& drawInfo,
		bool showPasses,
		std::span<const ImColor> colors,
		bool singleEffect
	) const noexcept;

	void _DrawTimelineItem(
		int& itemId,
		ImU32 color,
		float dpiScale,
		std::string_view name,
		float time,
		float effectsTotalTime,
		bool selected = false
	);

	bool _DrawToolbar(uint32_t fps, int& itemId) noexcept;
	void _DrawToolbarDockHints(const ToolbarGeometry& geometry) noexcept;

	bool _DrawProfiler(const EffectsProfilerSnapshot& profile, uint32_t fps, int& itemId) noexcept;
	bool _BeginProfilerWindow() noexcept;
	bool _DrawProfilerGraphs(const EffectsProfilerSnapshot& profile) noexcept;
	void _DrawProfilerGraph(const ProfilerHistory::Plot& plot, int metric, bool available) noexcept;
	bool _DrawEffectParameters(int& itemId) noexcept;
	void _InitEffectParameterValues() noexcept;
	void _SyncEffectParameterValues() noexcept;
	std::vector<EffectOption> _BuildDraftEffectOptions(bool restoreStartupSizing = false) const noexcept;
	bool _RequestEffectParameters(EffectParametersRequestKind kind, bool restoreStartupSizing = false) noexcept;

	const std::string& _GetResourceString(const std::wstring_view& key) noexcept;

	std::string _FormatFrameRate(uint32_t fps) const noexcept;
	float _CalcToolbarAlpha() const noexcept;
	float _CalcComparisonStatusAlpha() const noexcept;
	void _ShowComparisonStatus(bool original) noexcept;

	void _ClearStatesIfNoVisibleWindow() noexcept;

	OverlayOptions* _overlayOptions = nullptr;
	float _dpiScale = 1.0f;

	ImFont* _fontUI = nullptr; // 普通 UI 文字
	ImFont* _fontMonoNumbers = nullptr; // 普通 UI 文字，但数字部分是等宽的，只支持 ASCII
	ImFont* _fontIcons = nullptr; // 图标字体

	std::chrono::steady_clock::time_point _lastUpdateTime{};
	std::chrono::steady_clock::time_point _lastProfilerDrawTime{};
	uint64_t _profilerGeneration = 0;
	std::array<float, 3> _profilerGraphMax{};
	PresentationProfilerSnapshot _profilerOutputProfile{};
	bool _profilerTimingKnown = false;
	bool _profilerWindowLayoutInitialized = false;
	bool _profilerWindowLayoutDirty = false;
	ImVec2 _profilerViewport{};
	OverlayWindowRect _profilerWindowRect{};
	// (总计时间, 帧数)
	SmallVector<std::pair<float, uint32_t>, 0> _effectTimingsStatistics;
	SmallVector<float> _lastestAvgEffectTimings;

	SmallVector<uint32_t> _timelineColors;
	std::vector<std::vector<EffectParameterDesc>> _localizedEffectParameters;
	std::vector<EffectOption> _startupEffectOptions;
	std::vector<std::vector<float>> _startupEffectParameterValues;
	std::vector<std::vector<float>> _appliedEffectParameterValues;
	EffectParameterSessionState::Snapshot _parameterSessionSnapshot;
	std::vector<std::vector<float>> _draftEffectParameterValues;
	std::vector<std::vector<float>> _submittedEffectParameterValues;
	std::vector<EffectOption> _submittedEffectOptions;
	FrameRefreshSettings _startupFrameRefresh;
	FrameRefreshSettings _draftFrameRefresh;
	FrameRefreshSettings _submittedFrameRefresh;
	std::shared_ptr<EffectParametersSaveState> _effectParametersSaveState;
	uint64_t _effectParametersRevision = 0;
	uint64_t _lastEffectParametersSaveResult = 0;

	struct {
		std::string gpuName;
	} _hardwareInfo;

	ImGuiImpl _imguiImpl;
	bool _isHdrOverlay = false;

	uint32_t _lastFPS = std::numeric_limits<uint32_t>::max();
	std::string _lastFrameRateText;
	float _lastToolbarAlpha = -1.0f;
	std::chrono::steady_clock::time_point _comparisonStatusStarted{};
	float _lastComparisonStatusAlpha = 0.0f;
	bool _comparisonStatusOriginal = false;

	bool _isToolbarVisible = false;
	bool _isFirstFrame = true;
	bool _isToolbarPinned = false;
	bool _isCursorOnCaptionArea = false;
	bool _isToolbarItemActive = false;
	ToolbarPlacement _toolbarPlacement;
	bool _isToolbarDragHovered = false;
	std::optional<ImVec4> _stagedToolbarRect, _presentedToolbarRect;
	std::vector<ImVec4> _stagedToolbarButtons, _presentedToolbarButtons;
	bool _isProfilerVisible = false;
	bool _isEffectParametersVisible = false;
	bool _isEffectParameterInputActive = false;
	bool _overlayDirty = true;
	bool _effectParametersInitialized = false;
	bool _effectParametersWindowLayoutInitialized = false;
	bool _effectParametersWindowLayoutDirty = false;
	ImVec2 _effectParametersViewport{};
	OverlayWindowRect _effectParametersWindowRect{};
	EffectParameterResetGesture _parameterResetGesture;
#ifdef _DEBUG
	bool _isDemoWindowVisible = false;
#endif
};

}
