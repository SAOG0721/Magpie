#include "pch.h"
#include "HdrParameterPolicy.h"
#include "NvidiaVfxParameterPolicy.h"
#include "OverlayDrawer.h"
#include "CursorManager.h"
#include "DeviceResources.h"
#include "EffectDesc.h"
#include "EffectParameterValue.h"
#include "EffectParameterRules.h"
#include "EffectParameterLocalization.h"
#include "FrameSourceBase.h"
#include "ImGuiFontsCacheManager.h"
#include "Logger.h"
#include "OverlayHelper.h"
#include "Renderer.h"
#include "ScalingOptions.h"
#include "ScalingWindow.h"
#include "StrHelper.h"
#include "Win32Helper.h"
#include <ShlObj.h>
#include <imgui_internal.h>

using namespace std::chrono;

namespace Magpie {

static const char* COLOR_INDICATOR = "■";
static const wchar_t COLOR_INDICATOR_W = L'■';

static const float CORNER_ROUNDING = 6;

static const char* TOOLBAR_WINDOW_ID = "toolbar";
static const char* PROFILER_WINDOW_ID = "profiler";
static const char* EFFECT_PARAMETERS_WINDOW_ID = "effectParameters";

static constexpr float EFFECT_PARAMETERS_MIN_WIDTH = 360.0f;
static constexpr float EFFECT_PARAMETERS_MIN_HEIGHT = 400.0f;

static void SetDefaultWindowOptions(
	phmap::flat_hash_map<std::string, OverlayWindowOption>& windowOptions
) noexcept {
	if (!windowOptions.contains(PROFILER_WINDOW_ID)) {
		// 右侧竖直居中
		windowOptions.emplace(PROFILER_WINDOW_ID, OverlayWindowOption{
			.hArea = 2,
			.vArea = 1,
			.hPos = 60.0f,
			.vPos = 0.5f
		});
	}
	if (!windowOptions.contains(EFFECT_PARAMETERS_WINDOW_ID)) {
		windowOptions.emplace(EFFECT_PARAMETERS_WINDOW_ID, OverlayWindowOption{
			.hArea = 0,
			.vArea = 1,
			.hPos = 60.0f,
			.vPos = 0.5f
		});
	}
}

bool OverlayDrawer::Initialize(DeviceResources& deviceResources, OverlayOptions& overlayOptions) noexcept {
	_overlayOptions = &overlayOptions;
	_parameterFocusSwitchingEnabled = ScalingWindow::Get().Options().isParameterFocusSwitchingEnabled;
	_toolbarPlacement.state.docks = ScalingWindow::Get().Options().toolbarDocks;
	_imguiImpl.ParameterFocusSwitchingEnabled(_parameterFocusSwitchingEnabled);
	SetDefaultWindowOptions(overlayOptions.windows);

	if (!_imguiImpl.Initialize(deviceResources)) {
		Logger::Get().Error("初始化 ImGuiImpl 失败");
		return false;
	}

	_dpiScale = GetDpiForWindow(ScalingWindow::Get().Handle()) / float(USER_DEFAULT_SCREEN_DPI);

	ImGui::StyleColorsDark();
	ImGuiStyle& style = ImGui::GetStyle();
	style.PopupRounding = style.WindowRounding = CORNER_ROUNDING;
	// 由于我们是按需渲染，显示 tooltip 时不要有延迟
	style.HoverFlagsForTooltipMouse = ImGuiHoveredFlags_DelayNone;
	style.FrameBorderSize = 1;
	style.FrameRounding = 2;
	style.WindowMinSize = ImVec2(10, 10);
	style.ScaleAllSizes(_dpiScale);

	if (!_BuildFonts()) {
		Logger::Get().Error("_BuildFonts 失败");
		return false;
	}

	// 将 _fontUI 设为默认字体
	ImGui::GetIO().FontDefault = _fontUI;

	// 获取硬件信息
	DXGI_ADAPTER_DESC desc{};
	HRESULT hr = deviceResources.GetGraphicsAdapter()->GetDesc(&desc);
	_hardwareInfo.gpuName = SUCCEEDED(hr) ? StrHelper::UTF16ToUTF8(desc.Description) : "UNAVAILABLE";

	UpdateAfterActiveEffectsChanged();
	return true;
}

void OverlayDrawer::Draw(
	uint32_t fps,
	const EffectsProfilerSnapshot& profile,
	POINT drawOffset,
	float hdrWhiteScale
) noexcept {
	// This method is called only after Presenter::BeginFrame has provided a valid
	// back buffer. Build and draw one complete ImGui frame in the same frontend
	// render that will present it.
	_overlayDirty = false;
	const float dpiScale = GetDpiForWindow(ScalingWindow::Get().Handle()) / float(USER_DEFAULT_SCREEN_DPI);
	if (dpiScale > 0 && dpiScale != _dpiScale) {
		ClearStates();
		const float ratio = dpiScale / _dpiScale;
		ImGui::GetStyle().ScaleAllSizes(ratio);
		ImGui::GetIO().FontGlobalScale *= ratio;
		_dpiScale = dpiScale;
		_effectParametersWindowLayoutInitialized = false;
		_profilerWindowLayoutInitialized = false;
	}
	if (!AnyVisibleWindow()) {
		_lastComparisonStatusAlpha = 0.0f;
		return;
	}

	_lastFPS = fps;
	_lastFrameRateText = _FormatFrameRate(fps);
	const bool needsLayoutFollowUp = std::exchange(_isFirstFrame, false);

	const bool oldProfilerVisible = _isProfilerVisible;

	// Shift edge input inward for both docks. Free dragging uses the real pointer.
	float fittsLawAdjustment = 0;
	const char* hoveredWindowId = _imguiImpl.GetHoveredWindowId();
	if (!_toolbarPlacement.IsDragging() && hoveredWindowId &&
		hoveredWindowId == std::string_view(TOOLBAR_WINDOW_ID)) {
		const SIZE viewport = Win32Helper::GetSizeOfRect(ScalingWindow::Get().Renderer().DestRect());
		const ToolbarGeometry geometry(float(viewport.cx), float(viewport.cy), _dpiScale);
		fittsLawAdjustment = 4 * geometry.scale *
			(_toolbarPlacement.Dock(ScalingWindow::Get().Options().IsWindowedMode()) == ToolbarDock::Top ? 1 : -1);
	}

	_imguiImpl.NewFrame(_overlayOptions->windows, fittsLawAdjustment, _dpiScale);
	_isHdrOverlay = hdrWhiteScale > 0;
	if (_isHdrOverlay) {
		// Block HDR highlights without changing glyph coverage or fade animation.
		for (ImGuiCol color : { ImGuiCol_WindowBg, ImGuiCol_PopupBg }) {
			ImVec4 background = ImGui::GetStyle().Colors[color];
			background.w = 1;
			ImGui::PushStyleColor(color, background);
		}
	}
	if (!_isEffectParametersVisible || _imguiImpl.FrameInputCanceled()) {
		_parameterResetGesture.Clear();
	}

	bool needRedraw = false;
	// 防止 ID 冲突
	int itemId = 0;

	if (_isToolbarVisible && _DrawToolbar(fps, itemId)) {
		needRedraw = true;
	}

	if (_isProfilerVisible && _DrawProfiler(profile, fps, itemId)) {
		needRedraw = true;
	}

	if (_isEffectParametersVisible && _DrawEffectParameters(itemId)) {
		needRedraw = true;
	}
	if (!_isEffectParametersVisible && _parameterPanelState != ParameterPanelState::Closed) {
		_SetParameterPanelState(ParameterPanelState::Closed);
		_isEffectParametersVisible = _parameterPanelState != ParameterPanelState::Closed;
	}
	if ((_effectParametersWindowLayoutDirty || _profilerWindowLayoutDirty) && !ImGui::IsAnyMouseDown()) {
		const ScalingWindow& scalingWindow = ScalingWindow::Get();
		const ScalingOptions& options = scalingWindow.Options();
		if (options.save) options.save(options, scalingWindow.Handle());
		_effectParametersWindowLayoutDirty = false;
		_profilerWindowLayoutDirty = false;
	}
	_isEffectParameterInputActive = _isEffectParametersVisible && ImGui::IsAnyItemActive();
	const float comparisonAlpha = _CalcComparisonStatusAlpha();
	_lastComparisonStatusAlpha = comparisonAlpha;
	if (comparisonAlpha > 0.0f) {
		const std::string& statusText = _GetResourceString(_comparisonStatusOriginal ?
			L"Overlay_Comparison_Original" : L"Overlay_Comparison_Processed");
		const ImVec2 textSize = ImGui::CalcTextSize(statusText.c_str());
		const ImVec2 padding = ImGui::GetStyle().WindowPadding;
		const float margin = 12.0f * _dpiScale;
		// Size from this label now, avoiding an invisible auto-fit frame or
		// clipping when switching between labels on a static source.
		ImGui::SetNextWindowSize({ textSize.x + padding.x * 2, textSize.y + padding.y * 2 });
		ImGui::SetNextWindowPos({ ImGui::GetIO().DisplaySize.x - margin,
			ImGui::GetIO().DisplaySize.y - margin }, ImGuiCond_Always, { 1.0f, 1.0f });
		// The explicit background alpha overrides ImGui's style alpha.
		ImGui::SetNextWindowBgAlpha((_isHdrOverlay ? 1.0f : 0.72f) * comparisonAlpha);
		ImGui::PushStyleVar(ImGuiStyleVar_Alpha, comparisonAlpha);
		if (ImGui::Begin("##passThroughStatus", nullptr,
			ImGuiWindowFlags_NoDecoration |
			ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
			ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove)) {
			ImGui::TextUnformatted(statusText.c_str());
		}
		ImGui::End();
		ImGui::PopStyleVar();
	}

#ifdef _DEBUG
	if (_isDemoWindowVisible) {
		ImGui::ShowDemoWindow(&_isDemoWindowVisible);
	}
#endif

	// ImGui validates the style stack at EndFrame (inside Draw/Render).
	if (_isHdrOverlay) ImGui::PopStyleColor(2);
	_imguiImpl.Draw(drawOffset, hdrWhiteScale);
	_overlayDirty = needRedraw || needsLayoutFollowUp ||
		ImGui::IsAnyMouseDown() || ImGui::IsAnyItemActive();

	if (_isProfilerVisible != oldProfilerVisible) {
		Renderer& renderer = ScalingWindow::Get().Renderer();
		if (_isProfilerVisible) {
			renderer.StartProfile();
		} else {
			renderer.StopProfile();
		}
	}

	_ClearStatesIfNoVisibleWindow();
}

void OverlayDrawer::ClearStates() noexcept {
	_toolbarPlacement.Cancel();
	_isToolbarDragHovered = false;
	_stagedToolbarRect.reset();
	_stagedToolbarButtons.clear();
	_presentedToolbarRect.reset();
	_presentedToolbarButtons.clear();
	_isEffectParameterInputActive = false;
	_parameterResetGesture.Clear();
	_imguiImpl.ClearStates();
	_isCursorOnCaptionArea = false;
	_isToolbarItemActive = false;
	_overlayDirty = true;
}

void OverlayDrawer::OnPresentSucceeded() noexcept {
	_imguiImpl.OnPresentSucceeded();
	_presentedToolbarRect = _stagedToolbarRect;
	_presentedToolbarButtons = _stagedToolbarButtons;
	_UpdateParameterPreviewHost();
}

void OverlayDrawer::OnPresentFailed() noexcept {
	_overlayDirty = true;
}

ToolbarState OverlayDrawer::ToolbarState() const noexcept {
	if (!_isToolbarVisible) {
		return ToolbarState::Off;
	} else {
		return _isToolbarPinned ? ToolbarState::AlwaysShow : ToolbarState::AutoHide;
	}
}

void OverlayDrawer::ToolbarState(Magpie::ToolbarState value) noexcept {
	if (ToolbarState() == value) {
		return;
	}

	if (value == ToolbarState::Off) {
		_toolbarPlacement.Cancel();
		_isToolbarDragHovered = false;
		_stagedToolbarRect.reset();
	_stagedToolbarButtons.clear();
		_presentedToolbarRect.reset();
	_presentedToolbarButtons.clear();
		_isToolbarVisible = false;
		_ClearStatesIfNoVisibleWindow();
	} else if (value == ToolbarState::AlwaysShow) {
		_isToolbarVisible = true;
		_isToolbarPinned = true;
	} else {
		_isToolbarVisible = true;
		_isToolbarPinned = false;
	}

	_overlayDirty = true;
}

OverlaySessionState OverlayDrawer::CaptureSessionState() const noexcept {
	OverlaySessionState result{ _isToolbarVisible, _isToolbarPinned, _isProfilerVisible, _isEffectParametersVisible };
	if (_parameterFocusSwitchingEnabled) {
		result.parameterPanelState = _parameterEditPending ? ParameterPanelState::Edit :
			IsEditingParameters() ? _pendingParameterPanelState : _parameterPanelState;
		result.effectParametersVisible = result.parameterPanelState != ParameterPanelState::Closed;
	}
	result.toolbarPosition = _toolbarPlacement.state;
	return result;
}

void OverlayDrawer::RestoreSessionState(const OverlaySessionState& state) noexcept {
	_toolbarPlacement.Cancel();
	_toolbarPlacement.state = state.toolbarPosition;
	_isToolbarVisible = state.toolbarVisible;
	_isToolbarPinned = state.toolbarPinned;
	const auto parameterState = state.parameterPanelState == ParameterPanelState::Closed && state.effectParametersVisible
		? ParameterPanelState::Preview : state.parameterPanelState;
	// A restart can restore intent before the first valid frame admits/shows
	// the output. Input ownership is always acquired anew after that boundary.
	const bool waitForOutput = _parameterFocusSwitchingEnabled && parameterState == ParameterPanelState::Edit &&
		!IsWindowVisible(ScalingWindow::Get().Handle()) && _HasParameterForeground();
	_SetParameterPanelState(waitForOutput ? ParameterPanelState::Preview : parameterState, false, "session restore");
	_parameterEditPending = waitForOutput;
	_parameterEditRunId = waitForOutput ? ScalingWindow::RunId() : 0;
	if (_isProfilerVisible != state.profilerVisible) InvokeAction(OverlayAction::Profiler);
	_overlayDirty = true;
	_ClearStatesIfNoVisibleWindow();
}

void OverlayDrawer::InvokeAction(OverlayAction action) noexcept {
    switch (action) {
    case OverlayAction::Profiler:
        _isProfilerVisible = !_isProfilerVisible;
        if (_isProfilerVisible) ScalingWindow::Get().Renderer().StartProfile();
        else ScalingWindow::Get().Renderer().StopProfile();
        break;
    case OverlayAction::EffectParameters:
        _ToggleParameterPanel();
        break;
    case OverlayAction::ToolbarPin:
        _isToolbarPinned = !_isToolbarPinned;
        _isToolbarVisible = true;
        break;
    case OverlayAction::Comparison:
        {
            auto& renderer = ScalingWindow::Get().Renderer();
            const bool original = !renderer.IsPassThroughActive();
            if (renderer.SetPassThroughActive(original)) _ShowComparisonStatus(original);
        }
        break;
    default:
        return;
    }
    _overlayDirty = true;
    _ClearStatesIfNoVisibleWindow();
}

bool OverlayDrawer::AnyVisibleWindow() const noexcept {
	bool result = _isToolbarVisible || _isProfilerVisible ||
		_isEffectParametersVisible || _CalcComparisonStatusAlpha() > 0.0f;
#ifdef _DEBUG
	result = result || _isDemoWindowVisible;
#endif
	return result;
}

bool OverlayDrawer::MessageHandler(UINT msg, WPARAM wParam, LPARAM lParam) noexcept {
	if (const auto handled = _HandleParameterInputMessage(ScalingWindow::Get().Handle(), msg, wParam, lParam))
		return *handled == ImGuiInputResult::Urgent;
	if (!AnyVisibleWindow()) {
		return false;
	}

	const ImGuiInputResult result = _imguiImpl.MessageHandler(msg, wParam, lParam);
	if (result != ImGuiInputResult::None) {
		_overlayDirty = true;
	}
	_ClearStatesIfNoVisibleWindow();
	return result == ImGuiInputResult::Urgent;
}

bool OverlayDrawer::HasPendingInput() const noexcept {
	return _imguiImpl.HasPendingInput();
}

bool OverlayDrawer::HasUrgentInput() const noexcept {
	return _imguiImpl.HasUrgentInput();
}

bool OverlayDrawer::NeedRedraw(uint32_t fps) const noexcept {
	if (_overlayDirty || _imguiImpl.HasPendingInput() ||
		_CalcComparisonStatusAlpha() != _lastComparisonStatusAlpha) {
		return true;
	}
	if (!AnyVisibleWindow()) {
		return false;
	}
	// Independent overlays also need timing refreshes when the FPS text is
	// unchanged. Bound them to 2 Hz, including when no new sample is available.
	if (_isProfilerVisible && steady_clock::now() - _lastProfilerDrawTime >= 500ms) return true;
	if (_isEffectParametersVisible && (!_effectParametersInitialized ||
		_lastEffectParametersSaveResult != _effectParametersSaveState->result.load(std::memory_order_acquire) ||
		ScalingWindow::Get().Options().parameterSession->HasChanges(_parameterSessionSnapshot.revision))) return true;
	if (_CalcToolbarAlpha() != _lastToolbarAlpha) {
		return true;
	}
	return _lastFrameRateText != _FormatFrameRate(fps) &&
		(_lastToolbarAlpha > FLOAT_EPSILON<float> ||
			_isProfilerVisible || _isEffectParametersVisible);
}

std::string OverlayDrawer::_FormatFrameRate(uint32_t fps) const noexcept {
	const auto& renderer = ScalingWindow::Get().Renderer();
	if (!renderer.HasFrameGeneration()) return fmt::format("{} FPS", fps);
	const auto rate = renderer.PresentationRate();
	std::string text = rate.totalKnown ? fmt::format("{}/{} FPS", rate.total, rate.real) :
		fmt::format("—/{} FPS", rate.real);
	if (rate.idleSubmissions) text += fmt::format(" +{} R/s", rate.idleSubmissions);
	return text;
}

void OverlayDrawer::_ShowComparisonStatus(bool original) noexcept {
	_comparisonStatusOriginal = original;
	_comparisonStatusStarted = steady_clock::now();
	_overlayDirty = true;
}

float OverlayDrawer::_CalcComparisonStatusAlpha() const noexcept {
	if (_comparisonStatusStarted == steady_clock::time_point{}) return 0.0f;
	const auto elapsed = steady_clock::now() - _comparisonStatusStarted;
	if (elapsed <= 2s) return 1.0f;
	if (elapsed >= 2500ms) return 0.0f;
	return 1.0f - duration<float>(elapsed - 2s).count() / 0.5f;
}

void OverlayDrawer::UpdateAfterActiveEffectsChanged() noexcept {
	_parameterResetGesture.Clear();
	const std::vector<const EffectDesc*>& effectDescs =
		ScalingWindow::Get().Renderer().ActiveEffectDescs();
	_timelineColors = OverlayHelper::GenerateTimelineColors(effectDescs);

	uint32_t passCount = 0;
	for (const EffectDesc* info : effectDescs) {
		passCount += (uint32_t)info->passes.size();
	}

	// 清空旧时间数据
	_effectTimingsStatistics.clear();
	_effectTimingsStatistics.resize(passCount);
	_lastestAvgEffectTimings.clear();
	_lastestAvgEffectTimings.resize(passCount);
	_lastUpdateTime = {};
	_overlayDirty = true;
}

static const std::wstring& GetAppLanguage() noexcept {
	static std::wstring language;
	if (language.empty()) {
		winrt::ResourceContext resourceContext = winrt::ResourceContext::GetForViewIndependentUse();
		language = resourceContext.QualifierValues().Lookup(L"Language");
		StrHelper::ToLowerCase(language);
	}
	return language;
}

static const std::wstring& GetSystemFontsFolder() noexcept {
	static std::wstring result;

	if (result.empty()) {
		wil::unique_cotaskmem_string fontsFolder;
		HRESULT hr = SHGetKnownFolderPath(FOLDERID_Fonts, 0, NULL, fontsFolder.put());
		if (FAILED(hr)) {
			Logger::Get().ComError("SHGetKnownFolderPath 失败", hr);
			return result;
		}

		result = fontsFolder.get();
	}

	return result;
}

bool OverlayDrawer::_BuildFonts() noexcept {
	const std::wstring& language = GetAppLanguage();
	ImFontAtlas& fontAtlas = *ImGui::GetIO().Fonts;

	auto buildFontAtlas = [&]() {
		fontAtlas.Flags |= ImFontAtlasFlags_NoPowerOfTwoHeight | ImFontAtlasFlags_NoMouseCursors;

		std::wstring uiFontPath = GetSystemFontsFolder();
		std::string iconFontPath = StrHelper::UTF16ToUTF8(uiFontPath);
		if (Win32Helper::GetOSVersion().IsWin11()) {
			uiFontPath += L"\\SegUIVar.ttf";
			iconFontPath += "\\SegoeIcons.ttf";
		} else {
			uiFontPath += L"\\segoeui.ttf";
			iconFontPath += "\\segmdl2.ttf";
		}

		std::vector<uint8_t> uiFontData;
		if (!Win32Helper::ReadFile(uiFontPath.c_str(), uiFontData)) {
			Logger::Get().Error("读取字体文件失败");
			return false;
		}

		// 构建 ImFontAtlas 前 ranges 不能析构，因为 ImGui 只保存了指针
		SmallVector<ImWchar> uiRanges = _BuildFontUI(language, uiFontData);
		_BuildFontIcons(iconFontPath.c_str());

		if (!fontAtlas.Build()) {
			Logger::Get().Error("构建 ImFontAtlas 失败");
			return false;
		}

		return true;
	};

	if (ScalingWindow::Get().Options().IsFontCacheDisabled()) {
		if (!buildFontAtlas()) {
			return false;
		}
	} else {
		const uint32_t dpi = (uint32_t)std::lroundf(_dpiScale * USER_DEFAULT_SCREEN_DPI);
		if (ImGuiFontsCacheManager::Get().Load(language, dpi, fontAtlas)) {
			_fontUI = fontAtlas.Fonts[0];
			_fontMonoNumbers = fontAtlas.Fonts[1];
			_fontIcons = fontAtlas.Fonts[2];
		} else {
			if (!buildFontAtlas()) {
				return false;
			}

			ImGuiFontsCacheManager::Get().Save(language, dpi, fontAtlas);
		}
	}

	if (!_imguiImpl.BuildFonts()) {
		Logger::Get().Error("构建字体失败");
		return false;
	}

	return true;
}

template <size_t SIZE>
static void SetGlyphRanges(SmallVector<ImWchar>& uiRanges, const ImWchar (&ranges)[SIZE]) noexcept {
	uiRanges.assign(std::begin(ranges), std::end(ranges));
}

// 指针重载，但不能直接使用指针
template <typename T, typename = std::enable_if_t<std::is_same_v<T, const ImWchar*>>>
static void SetGlyphRanges(SmallVector<ImWchar>& uiRanges, T ranges) noexcept {
	// 删除末尾的 0
	for (const ImWchar* range = ranges; *range; ++range) {
		uiRanges.push_back(*range);
	}
}

SmallVector<ImWchar> OverlayDrawer::_BuildFontUI(
	std::wstring_view language,
	const std::vector<uint8_t>& fontData
) noexcept {
	ImFontAtlas& fontAtlas = *ImGui::GetIO().Fonts;

	std::string extraFontPath;
	const ImWchar* extraRanges = nullptr;
	int extraFontNo = 0;

	SmallVector<ImWchar> ranges;
	if (language == L"en-us") {
		SetGlyphRanges(ranges, OverlayHelper::BASIC_LATIN_RANGES);
	} else if (language == L"ru" || language == L"uk") {
		SetGlyphRanges(ranges, fontAtlas.GetGlyphRangesCyrillic());
	} else if (language == L"tr" || language == L"pl" || language == L"fi") {
		SetGlyphRanges(ranges, OverlayHelper::EXTENDED_LATIN_RANGES);
	} else if (language == L"vi") {
		SetGlyphRanges(ranges, fontAtlas.GetGlyphRangesVietnamese());
	} else if (language == L"fr") {
		SetGlyphRanges(ranges, OverlayHelper::FRENCH_RANGES);
	} else {
		// Basic Latin 使用默认字体
		SetGlyphRanges(ranges, OverlayHelper::BASIC_LATIN_RANGES);

		// 一些语言需要加载额外的字体:
		// 简体中文 -> Microsoft YaHei UI
		// 繁体中文 -> Microsoft JhengHei UI
		// 日语 -> Yu Gothic UI
		// 韩语/朝鲜语 -> Malgun Gothic
		// 泰米尔语 -> Nirmala UI
		// 参见 https://learn.microsoft.com/en-us/windows/apps/design/style/typography#fonts-for-non-latin-languages
		if (language == L"zh-hans") {
			// msyh.ttc: 0 是微软雅黑，1 是 Microsoft YaHei UI
			extraFontPath = StrHelper::Concat(StrHelper::UTF16ToUTF8(GetSystemFontsFolder()), "\\msyh.ttc");
			extraFontNo = 1;
			extraRanges = OverlayHelper::GetGlyphRangesChineseSimplifiedOfficial();
		} else if (language == L"zh-hant") {
			// msjh.ttc: 0 是 Microsoft JhengHei，1 是 Microsoft JhengHei UI
			extraFontPath = StrHelper::Concat(StrHelper::UTF16ToUTF8(GetSystemFontsFolder()), "\\msjh.ttc");
			extraFontNo = 1;
			extraRanges = OverlayHelper::GetGlyphRangesChineseTraditionalOfficial();
		} else if (language == L"ja") {
			// YuGothM.ttc: 0 是 Yu Gothic Medium，1 是 Yu Gothic UI
			extraFontPath = StrHelper::Concat(StrHelper::UTF16ToUTF8(GetSystemFontsFolder()), "\\YuGothM.ttc");
			extraFontNo = 1;
			extraRanges = fontAtlas.GetGlyphRangesJapanese();
		} else if (language == L"ko") {
			extraFontPath = StrHelper::Concat(StrHelper::UTF16ToUTF8(GetSystemFontsFolder()), "\\malgun.ttf");
			extraRanges = fontAtlas.GetGlyphRangesKorean();
		} else if (language == L"ta") {
			extraFontPath = StrHelper::Concat(StrHelper::UTF16ToUTF8(GetSystemFontsFolder()), "\\Nirmala.ttf");
			extraRanges = OverlayHelper::EXTRA_TAMIL_RANGES;
		}
	}

	ranges.push_back(COLOR_INDICATOR_W);
	ranges.push_back(COLOR_INDICATOR_W);
	ranges.push_back(0);

	ImFontConfig config;
	// fontData 需要多次使用，我们自己读取并管理生命周期
	config.FontDataOwnedByAtlas = false;

	const float fontSize = 18 * _dpiScale;

	//////////////////////////////////////////////////////////
	//
	// ranges (+ extraRanges) -> _fontUI
	//
	//////////////////////////////////////////////////////////


#ifdef _DEBUG
	std::char_traits<char>::copy(config.Name, "_fontUI", std::size(config.Name));
#endif

	_fontUI = fontAtlas.AddFontFromMemoryTTF(
		(void*)fontData.data(), (int)fontData.size(), fontSize, &config, ranges.data());

	if (extraRanges) {
		assert(Win32Helper::FileExists(StrHelper::UTF8ToUTF16(extraFontPath).c_str()));

		// 在 MergeMode 下已有字符会跳过而不是覆盖
		config.MergeMode = true;
		config.FontNo = extraFontNo;
		// 额外字体数据由 ImGui 管理，初始化完成后释放
		config.FontDataOwnedByAtlas = true;
		fontAtlas.AddFontFromFileTTF(extraFontPath.c_str(), fontSize, &config, extraRanges);
		config.FontDataOwnedByAtlas = false;
		config.FontNo = 0;
		config.MergeMode = false;
	}

	//////////////////////////////////////////////////////////
	//
	// NUMBER_RANGES + NOT_NUMBER_RANGES -> _fontMonoNumbers
	//
	//////////////////////////////////////////////////////////

#ifdef _DEBUG
	std::char_traits<char>::copy(config.Name, "_fontMonoNumbers", std::size(config.Name));
#endif

	// 等宽的数字字符
	config.GlyphMinAdvanceX = config.GlyphMaxAdvanceX = fontSize * 0.42f;
	_fontMonoNumbers = fontAtlas.AddFontFromMemoryTTF(
		(void*)fontData.data(), (int)fontData.size(), fontSize, &config, OverlayHelper::NUMBER_RANGES);

	// 其他不等宽的字符
	config.MergeMode = true;
	config.GlyphMinAdvanceX = 0;
	config.GlyphMaxAdvanceX = std::numeric_limits<float>::max();
	fontAtlas.AddFontFromMemoryTTF(
		(void*)fontData.data(), (int)fontData.size(), fontSize, &config, OverlayHelper::NOT_NUMBER_RANGES);

	return ranges;
}

void OverlayDrawer::_BuildFontIcons(const char* fontPath) noexcept {
	ImFontConfig config;
#ifdef _DEBUG
	std::char_traits<char>::copy(config.Name, "_fontIcons", std::size(config.Name));
#endif

	const float fontSize = 16 * _dpiScale;
	_fontIcons = ImGui::GetIO().Fonts->AddFontFromFileTTF(
		fontPath, fontSize, &config, OverlayHelper::ICON_RANGES);
}

static std::string_view GetEffectDisplayName(const EffectDesc& effectDesc) noexcept {
	// Match the picker aliases without changing saved IDs or effect filenames.
	if (effectDesc.name == "XeSSFG\\XeSS_FrameGeneration") return "XeSS_FrameGeneration";
	if (!effectDesc.sortName.empty()) {
		return effectDesc.sortName;
	}
	auto delimPos = effectDesc.name.find_last_of('\\');
	if (delimPos == std::string::npos) {
		return effectDesc.name;
	} else {
		return std::string_view(effectDesc.name.begin() + delimPos + 1, effectDesc.name.end());
	}
}

bool OverlayDrawer::_DrawTimingItem(
	int& itemId,
	const char* text,
	const ImColor* color,
	float time,
	bool isExpanded
) const noexcept {
	ImGui::TableNextRow();
	ImGui::TableNextColumn();

	const std::string timeStr = _profilerTimingKnown ? fmt::format("{:.3f} ms", time) : "-- ms";
	const float timeWidth = _fontMonoNumbers->CalcTextSizeA(
		ImGui::GetFontSize(), FLT_MAX, 0.0f, timeStr.c_str()).x;

	// 计算布局
	static constexpr float spacingBeforeText = 3;
	static constexpr float spacingAfterText = 8;
	const float descWrapPos = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - timeWidth - spacingAfterText;
	const float descHeight = ImGui::CalcTextSize(
		text, nullptr, false, descWrapPos - ImGui::GetCursorPosX() - (color ? ImGui::CalcTextSize(COLOR_INDICATOR).x + spacingBeforeText : 0)).y;

	const float fontHeight = ImGui::GetFont()->FontSize;

	ImGui::PushID(itemId++);

	bool isHovered = false;
	if (color) {
		ImGui::Selectable("", false, 0, ImVec2(0, descHeight));
		if (ImGui::IsItemHovered()) {
			isHovered = true;
		}
		ImGui::SameLine(0, 0);

		ImGui::PushStyleColor(ImGuiCol_Text, (ImU32)*color);

		if (descHeight >= fontHeight * 2) {
			// 不知为何 SetCursorPos 不起作用
			// 所以这里使用占位竖直居中颜色框
			ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2());
			ImGui::BeginGroup();
			ImGui::Dummy(ImVec2(0, (descHeight - fontHeight) / 2));
			ImGui::TextUnformatted(COLOR_INDICATOR);
			ImGui::EndGroup();
			ImGui::PopStyleVar();
		} else {
			ImGui::TextUnformatted(COLOR_INDICATOR);
		}
		ImGui::PopStyleColor();

		ImGui::SameLine(0, spacingBeforeText);
	}

	ImGui::PushTextWrapPos(descWrapPos);
	ImGui::TextUnformatted(text);
	ImGui::PopTextWrapPos();
	ImGui::SameLine(0, 0);

	// 描述过长导致换行时竖直居中时间
	if (color && descHeight >= fontHeight * 2) {
		ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (descHeight - fontHeight) / 2);
	}

	if (isExpanded) {
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 0.5f));
	}

	ImGui::PushFont(_fontMonoNumbers);
	ImGui::SetCursorPosX(descWrapPos + spacingAfterText);
	ImGui::TextUnformatted(timeStr.c_str());
	ImGui::PopFont();

	if (isExpanded) {
		ImGui::PopStyleColor();
	}

	ImGui::PopID();

	return isHovered;
}

// 返回鼠标悬停的项的序号，未悬停于任何项返回 -1
int OverlayDrawer::_DrawEffectTimings(
	int& itemId,
	const _EffectDrawInfo& drawInfo,
	bool showPasses,
	std::span<const ImColor> colors,
	bool singleEffect
) const noexcept {
	int result = -1;

	showPasses &= drawInfo.passTimings.size() > 1;

	std::string effectName;
	if (ScalingWindow::Get().Options().IsDeveloperMode() && drawInfo.desc->flags & EffectFlags::FP16) {
		// 开发者选项开启时显示效果是否使用 FP16
		effectName = StrHelper::Concat(drawInfo.label, " (FP16)");
	} else {
		effectName = drawInfo.label;
	}

	if (_DrawTimingItem(
		itemId,
		effectName.c_str(),
		(!singleEffect && !showPasses) ? &colors[0] : nullptr,
		drawInfo.totalTime,
		showPasses
	)) {
		result = 0;
	}

	if (showPasses) {
		for (size_t j = 0; j < drawInfo.passTimings.size(); ++j) {
			ImGui::Indent(16);

			if (_DrawTimingItem(
				itemId,
				drawInfo.desc->passes[j].desc.c_str(),
				&colors[j],
				drawInfo.passTimings[j]
			)) {
				result = (int)j;
			}

			ImGui::Unindent(16);
		}
	}

	return result;
}

void OverlayDrawer::_DrawTimelineItem(
	int& itemId,
	ImU32 color,
	float dpiScale,
	std::string_view name,
	float time,
	float effectsTotalTime,
	bool selected
) {
	ImGui::PushID(itemId++);

	ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, color);
	ImGui::PushStyleColor(ImGuiCol_HeaderActive, color);
	ImGui::PushStyleColor(ImGuiCol_HeaderHovered, color);
	ImGui::PushStyleColor(ImGuiCol_Header, color);
	ImGui::Selectable("", selected);
	ImGui::PopStyleColor(3);

	if (ImGui::IsItemHovered() || ImGui::IsItemClicked()) {
		std::string content = fmt::format("{}\n{:.3f} ms\n{}%", name, time, std::lroundf(time / effectsTotalTime * 100));
		ImGui::PushFont(_fontMonoNumbers);
		_imguiImpl.Tooltip(content.c_str(), _dpiScale, nullptr, 500 * dpiScale);
		ImGui::PopFont();
	}

	// 空间足够时显示文字
	std::string text;
	if (selected) {
		text = fmt::format("{}%", std::lroundf(time / effectsTotalTime * 100));
	} else {
		text.assign(name);
	}

	float textWidth = ImGui::CalcTextSize(text.c_str()).x;
	float itemWidth = ImGui::GetItemRectSize().x;
	float itemSpacing = ImGui::GetStyle().ItemSpacing.x;
	if (itemWidth - (selected ? 0 : itemSpacing) > textWidth + 4 * _dpiScale) {
		ImGui::SameLine(0, 0);
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (itemWidth - textWidth - itemSpacing) / 2);
		// 竖直方向居中
		ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 0.5f * _dpiScale);
		ImGui::TextUnformatted(text.c_str());
	}

	ImGui::PopID();
}

static std::string IconLabel(ImWchar iconChar) noexcept {
	const wchar_t text[] = { iconChar, L'\0' };
	return StrHelper::UTF16ToUTF8(text);
}

static std::string FormatToolbarTooltip(std::string_view name, std::string_view shortcut) {
	std::string result(name);
	if (!shortcut.empty()) result.append("（").append(shortcut).append("）");
	return result;
}

bool OverlayDrawer::_DrawToolbar(uint32_t fps, int& itemId) noexcept {
	bool needRedraw = false;

	const auto& options = ScalingWindow::Get().Options();
	const bool windowed = options.IsWindowedMode();
	const ImVec2 viewport = ImGui::GetIO().DisplaySize;
	const ToolbarGeometry geometry(viewport.x, viewport.y, _dpiScale);
	const float toolbarScale = geometry.scale;
	const ImVec2 mouse = _imguiImpl.FrameMousePosition();
	if (_imguiImpl.FrameInputCanceled()) _toolbarPlacement.Cancel();
	if (_toolbarPlacement.IsDragging()) {
		_toolbarPlacement.Update(geometry, mouse.x, mouse.y);
		if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
			if (const auto changed = _toolbarPlacement.Release(geometry, mouse.x, mouse.y);
				changed && options.saveToolbarDock) {
				options.saveToolbarDock(windowed, *changed, ScalingWindow::RunId());
			}
			needRedraw = true;
		} else if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
			_toolbarPlacement.Cancel();
		}
	}
	const bool bottom = _toolbarPlacement.Target().value_or(_toolbarPlacement.Dock(windowed)) == ToolbarDock::Bottom;
	const float contentTop = bottom ? 0.0f : CORNER_ROUNDING;
	const auto rect = _toolbarPlacement.Layout(windowed, geometry);
	ImGui::SetNextWindowSize({ rect.width, rect.height });
	ImGui::SetNextWindowPos({ rect.x, rect.y });

	_lastToolbarAlpha = _CalcToolbarAlpha();
	ImGui::PushStyleVar(ImGuiStyleVar_Alpha, _lastToolbarAlpha);
	ImGui::PushStyleColor(ImGuiCol_WindowBg, (ImU32)ImColor(15, 15, 15, _isHdrOverlay ? 255 : 180));
	const ImVec2 originalWindowPadding = ImGui::GetStyle().WindowPadding;
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 6 * toolbarScale,0.0f });
	ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, { 1.0f, 1.0f });
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, geometry.rounding);

	_isToolbarItemActive = false;
	_isToolbarDragHovered = false;
	_stagedToolbarRect.reset();
	_stagedToolbarButtons.clear();

	if (ImGui::Begin(StrHelper::Concat("##", TOOLBAR_WINDOW_ID).c_str(), nullptr,
		ImGuiWindowFlags_NoTitleBar |
		ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoResize |
		ImGuiWindowFlags_NoScrollbar |
		ImGuiWindowFlags_NoScrollWithMouse |
		ImGuiWindowFlags_NoSavedSettings))
	{
		ImGui::SetWindowFontScale(toolbarScale / _dpiScale);
		// All toolbar input is client input; background dragging moves this bar.
		_isCursorOnCaptionArea = false;
		_stagedToolbarRect = ImVec4(rect.x, std::max(0.f, rect.y),
			rect.x + rect.width, std::min(viewport.y, rect.y + rect.height));
		auto recordButton = [&] {
			const auto min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
			_stagedToolbarButtons.emplace_back(min.x, min.y, max.x, max.y);
		};

		ImGui::SetCursorPosY((contentTop + 3) * toolbarScale);

		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, originalWindowPadding);
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, { 4 * toolbarScale,4 * toolbarScale });
		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4 * toolbarScale);
		ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0);
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, { 4 * toolbarScale, 0.0f });
		// 禁用仅为阻止交互，不应有视觉改变
		ImGui::PushStyleVar(ImGuiStyleVar_DisabledAlpha, 1.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, { 0.5f, 0.5f });
		ImGui::PushStyleColor(ImGuiCol_Button, { 0,0,0,0 });
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, { 0.118f, 0.533f, 0.894f, 1.0f });
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, { 0.118f, 0.533f, 0.894f, 0.8f });

		auto drawToggleButton = [&](bool& value, ImWchar icon, const char* tooltip) {
			ImGui::SetCursorPosY((contentTop + 3) * toolbarScale);
			bool stylePushed = value;
			if (stylePushed) {
				ImGui::PushStyleColor(ImGuiCol_Button, { 0.118f, 0.533f, 0.894f, 0.8f });
			}

			ImGui::PushFont(_fontIcons);
			if (ImGui::Button(IconLabel(icon).c_str(), { 24.0f * toolbarScale, 24.0f * toolbarScale })) {
				value = !value;
				needRedraw = true;
			}
			recordButton();
			if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
				_isCursorOnCaptionArea = false;
				_isToolbarItemActive = true;
			}
			ImGui::PopFont();
			if (ImGui::IsItemHovered() || ImGui::IsItemClicked()) {
				_imguiImpl.Tooltip(tooltip, toolbarScale);
			}

			if (stylePushed) {
				ImGui::PopStyleColor();
			}
		};

		auto drawButton = [&](ImWchar icon, const char* tooltip) {
			ImGui::SetCursorPosY((contentTop + 3) * toolbarScale);
			ImGui::PushFont(_fontIcons);
			const bool clicked = ImGui::Button(IconLabel(icon).c_str(), { 24.0f * toolbarScale, 24.0f * toolbarScale });
			recordButton();
			if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
				_isCursorOnCaptionArea = false;
				_isToolbarItemActive = true;
			}
			ImGui::PopFont();
			if (ImGui::IsItemHovered()) {
				_imguiImpl.Tooltip(tooltip, toolbarScale);
			}
			return clicked;
		};

		// 光标不在缩放窗口上时阻止交互
		ImGui::BeginDisabled(!IsEditingParameters() && !ScalingWindow::Get().CursorManager().CursorHandle());

		const auto& shortcuts = ScalingWindow::Get().Options().toolbarShortcutLabels;
		auto tooltip = [&](std::wstring_view name, std::string_view shortcut) {
			return FormatToolbarTooltip(_GetResourceString(name), shortcut);
		};
		const std::string pinStr = tooltip(L"Overlay_Toolbar_Pin", shortcuts.pin);
		drawToggleButton(_isToolbarPinned, OverlayHelper::SegoeIcons::Pinned, pinStr.c_str());
		ImGui::SameLine();
		const std::string profilerStr = tooltip(L"Overlay_Toolbar_Profiler", shortcuts.profiler);
		drawToggleButton(_isProfilerVisible, OverlayHelper::SegoeIcons::Diagnostic, profilerStr.c_str());
		ImGui::SameLine();
		const std::string parametersStr = tooltip(L"Overlay_Toolbar_EffectParameters", shortcuts.parameters);
		bool parametersVisible = _isEffectParametersVisible;
		drawToggleButton(parametersVisible, OverlayHelper::SegoeIcons::Parameters, parametersStr.c_str());
		if (parametersVisible != _isEffectParametersVisible) InvokeAction(OverlayAction::EffectParameters);
		ImGui::SameLine();
		Renderer& renderer = ScalingWindow::Get().Renderer();
		bool passThrough = renderer.IsPassThroughActive();
		const std::string comparisonTip = tooltip(L"Overlay_Toolbar_PassThrough", shortcuts.comparison);
		drawToggleButton(passThrough, OverlayHelper::SegoeIcons::View, comparisonTip.c_str());
		if (passThrough != renderer.IsPassThroughActive() && renderer.SetPassThroughActive(passThrough)) {
			_ShowComparisonStatus(passThrough);
		}
#ifdef _DEBUG
		ImGui::SameLine();
		const std::string& demoStr = _GetResourceString(L"Overlay_Toolbar_Demo");
		drawToggleButton(_isDemoWindowVisible, OverlayHelper::SegoeIcons::Design, demoStr.c_str());
#endif
		ImGui::SameLine();
		const std::string screenshotStr = tooltip(L"Overlay_Toolbar_TakeScreenshot", shortcuts.screenshot);
		if (drawButton(OverlayHelper::SegoeIcons::Camera, screenshotStr.c_str())) {
			ScalingWindow::Get().Renderer().TakeDisplayedScreenshot();
		}
		const float leftControlsEnd = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x;
		// Dear ImGui's normal popup placement can prefer the space below the
		// mouse. Anchor a bottom toolbar's context menu above its visible edge.
		if (bottom) {
			ImGui::SetNextWindowPos({ ImGui::GetItemRectMin().x, rect.y }, ImGuiCond_Appearing, { 0.0f, 1.0f });
		}
		// 截图按钮右键菜单
		if (ImGui::BeginPopupContextItem()) {
			_isCursorOnCaptionArea = false;
			_isToolbarItemActive = true;

			ImGui::SeparatorText(_GetResourceString(L"Overlay_Toolbar_TakeScreenshot_PopupTitle").c_str());
			ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4.0f * toolbarScale);
			ImGui::PushStyleVarY(ImGuiStyleVar_ItemSpacing, 6.0f * toolbarScale);

			const std::vector<const EffectDesc*>& effectDescs =
				ScalingWindow::Get().Renderer().ActiveEffectDescs();
			const uint32_t effectCount = (uint32_t)effectDescs.size();

			const bool isDeveloperMode = ScalingWindow::Get().Options().IsDeveloperMode();
			for (uint32_t i = 0; i < effectCount; ++i) {
				const EffectDesc& effectDesc = *effectDescs[i];
				std::string_view effectName = GetEffectDisplayName(effectDesc);

				if (isDeveloperMode && effectDesc.passes.size() > 1) {
					// 开发者模式允许保存任意通道的输出
					ImGui::PushID(itemId++);
					if (ImGui::BeginMenu(effectName.data())) {
						const uint32_t passCount = (uint32_t)effectDesc.passes.size();
						for (uint32_t j = 0; j < passCount; ++j) {
							const EffectPassDesc& passDesc = effectDesc.passes[j];
							const uint32_t outputCount = (uint32_t)passDesc.outputs.size();

							if (outputCount == 1) {
								ImGui::PushID(itemId++);
								if (ImGui::MenuItem(passDesc.desc.c_str())) {
									ScalingWindow::Get().Renderer().TakeScreenshot(i, j);
								}
								ImGui::PopID();
							} else {
								ImGui::PushID(itemId++);
								if (ImGui::BeginMenu(passDesc.desc.c_str())) {
									for (uint32_t k = 0; k < outputCount; ++k) {
										ImGui::PushID(itemId++);
										if (ImGui::MenuItem(effectDesc.textures[passDesc.outputs[k]].name.c_str())) {
											ScalingWindow::Get().Renderer().TakeScreenshot(i, j, k);
										}
										ImGui::PopID();
									}

									ImGui::EndMenu();
								}
								ImGui::PopID();
							}
						}

						ImGui::EndMenu();
					}
					ImGui::PopID();
				} else {
					ImGui::PushID(itemId++);
					if (ImGui::MenuItem(effectName.data())) {
						ScalingWindow::Get().Renderer().TakeScreenshot(i);
					}
					ImGui::PopID();
				}
			}

			ImGui::PopStyleVar();
			ImGui::EndPopup();
		}

		// 居中绘制 FPS
		// Reset the button line's text baseline before positioning FPS explicitly.
		ImGui::NewLine();
		const std::string fpsText = _FormatFrameRate(fps);
		const HWND hwndSrc = ScalingWindow::Get().SrcTracker().Handle();
		const bool canSrcMinimized = GetWindowStyle(hwndSrc) & WS_MINIMIZEBOX;
		const float rightControlsStart = ImGui::GetContentRegionMax().x -
			((canSrcMinimized ? 3 : 2) * 28 - 4) * toolbarScale;
		ImGui::PushFont(_fontMonoNumbers);
		const float textWidth = ImGui::CalcTextSize(fpsText.c_str()).x;
		const float textMinX = leftControlsEnd + 4.0f * toolbarScale;
		const float center = rect.width / 2.f;
		const float textRoom = std::max(1.f, 2.f * std::min(center - textMinX,
			rightControlsStart - center - 4.f * toolbarScale));
		const float textScale = std::min(1.f, textRoom / std::max(1.f, textWidth));
		ImGui::SetWindowFontScale(toolbarScale / _dpiScale * textScale);
		ImGui::SetCursorPosX((rect.width - textWidth * textScale) / 2.f);
		ImGui::SetCursorPosY((contentTop + 15) * toolbarScale - ImGui::GetFontSize() / 2);
		ImGui::TextUnformatted(fpsText.c_str());
		ImGui::PopFont();
		if (ScalingWindow::Get().Renderer().HasFrameGeneration() && ImGui::IsItemHovered())
			ImGui::SetTooltip("%s", _GetResourceString(ScalingWindow::Get().Renderer().PresentationRate().source == ContentFrameCountSource::Submitted ?
				L"Overlay_FG_SubmittedRateHelp" : L"Overlay_FG_FrameRateHelp").c_str());
		ImGui::SetWindowFontScale(toolbarScale / _dpiScale);

		ImGui::SameLine();
		ImGui::SetCursorPosY((contentTop + 3) * toolbarScale);

		// 源窗口支持最小化时才显示最小化按钮
		ImGui::SetCursorPosX(rightControlsStart);

		if (canSrcMinimized) {
			const std::string& minimizeStr = _GetResourceString(L"Overlay_Toolbar_Minimize");
			const ImWchar icon = Win32Helper::GetOSVersion().IsWin11() ?
				OverlayHelper::SegoeIcons::CheckboxIndeterminate : OverlayHelper::SegoeIcons::Remove;
			if (drawButton(icon, minimizeStr.c_str())) {
				// 模拟通过标题栏最小化，失败则回落到 ShowWindow
				if (!PostMessage(hwndSrc, WM_SYSCOMMAND, SC_MINIMIZE, 0)) {
					ShowWindowAsync(hwndSrc, SW_SHOWMINIMIZED);
				}
			}
			ImGui::SameLine();
		}

		{
			const bool isWindowedMode = ScalingWindow::Get().Options().IsWindowedMode();
			const ImWchar icon = isWindowedMode ?
				OverlayHelper::SegoeIcons::FullScreen : OverlayHelper::SegoeIcons::Favicon;
			const std::string switchScalingStr = tooltip(
				isWindowedMode ? L"Overlay_Toolbar_SwitchToFullscreen" : L"Overlay_Toolbar_SwitchToWindowed",
				isWindowedMode ? shortcuts.fullscreen : shortcuts.windowed);
			if (drawButton(icon, switchScalingStr.c_str())) {
				ScalingWindow::Dispatcher().TryEnqueue([]() {
					ScalingWindow::Get().ToggleScaling(!ScalingWindow::Get().Options().IsWindowedMode());
				});
			}
		}
		ImGui::SameLine();

		// 和主窗口保持一致 (#C42B1C)
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, { 0.769f, 0.169f, 0.11f, 1.0f });
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, { 0.769f, 0.169f, 0.11f, 0.8f });

		const std::string closeStr = tooltip(L"Overlay_Toolbar_Close",
			ScalingWindow::Get().Options().IsWindowedMode() ? shortcuts.windowed : shortcuts.fullscreen);
		if (drawButton(OverlayHelper::SegoeIcons::Cancel, closeStr.c_str())) {
			ScalingWindow::Get().RequestStop(ScalingWindow::RunId());
		}
		if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
			ScalingWindow::Dispatcher().TryEnqueue([this, runId(ScalingWindow::RunId())]() {
				if (runId == ScalingWindow::RunId()) {
					ToolbarState(ToolbarState::Off);
					ScalingWindow::Get().Renderer().Render(true);
				}
			});
		}

		// Register the background only outside successfully presented buttons.
		// An active background item keeps ownership until release/cancellation.
		const auto contains = [&](const ImVec4& r) {
			return mouse.x >= r.x && mouse.x < r.z && mouse.y >= r.y && mouse.y < r.w;
		};
		const bool presentedBackground = _presentedToolbarRect && contains(*_presentedToolbarRect) &&
			std::none_of(_presentedToolbarButtons.begin(), _presentedToolbarButtons.end(), contains);
		const char* hovered = _imguiImpl.GetHoveredWindowId();
		const bool toolbarHovered = hovered && std::string_view(hovered) == TOOLBAR_WINDOW_ID;
		if (_toolbarPlacement.IsDragging() || (presentedBackground && toolbarHovered)) {
			ImGui::SetCursorPos({ 0.f, 0.f });
			ImGui::InvisibleButton("##toolbarBackgroundDrag", { rect.width, rect.height });
			_isToolbarDragHovered = ImGui::IsItemHovered();
			if (_isToolbarDragHovered || _toolbarPlacement.IsDragging()) {
				_isToolbarItemActive = true;
				ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
			}
			if (!_imguiImpl.FrameInputCanceled() && ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
				_toolbarPlacement.Begin(windowed, geometry, mouse.x, mouse.y);
				needRedraw = true;
			}
		}

		ImGui::EndDisabled();
		ImGui::SetWindowFontScale(1.0f);

		ImGui::PopStyleColor(5);
		ImGui::PopStyleVar(7);
	} else {
		_isCursorOnCaptionArea = false;
	}
	ImGui::End();

	ImGui::PopStyleColor();
	ImGui::PopStyleVar(4);
	if (_toolbarPlacement.IsDragging()) _DrawToolbarDockHints(geometry);

	return needRedraw;
}

void OverlayDrawer::_DrawToolbarDockHints(const ToolbarGeometry& geometry) noexcept {
	// Draw lists have no window/input region, so the strips cannot intercept a
	// click in the game. The existing paired pointer capture owns this drag.
	ImDrawList* drawList = ImGui::GetBackgroundDrawList();
	const auto target = _toolbarPlacement.Target();
	for (const ToolbarDock dock : { ToolbarDock::Top, ToolbarDock::Bottom }) {
		const float y = dock == ToolbarDock::Top ? 0.0f : geometry.viewportHeight - geometry.zone;
		drawList->AddRectFilled({ 0.0f, y }, { geometry.viewportWidth, y + geometry.zone },
			IM_COL32(30, 136, 228, target == dock ? 115 : 55));
	}
	if (_toolbarPlacement.IsCenterSnapped()) {
		// Integer-aligned filled rectangle: exactly two physical output pixels.
		const float x = std::floor(geometry.viewportWidth / 2.0f) - 1.0f;
		drawList->AddRectFilled({ x, 0.0f }, { x + 2.0f, geometry.viewportHeight },
			IM_COL32(0, 255, 255, 255));
	}

	if (target) {
		const auto preview = _toolbarPlacement.Preview(geometry);
		drawList->AddRect({ preview.x, preview.y },
			{ preview.x + preview.width, preview.y + preview.height },
			IM_COL32(90, 180, 255, 255), geometry.rounding, 0, 2.0f * geometry.scale);
	}
}

bool OverlayDrawer::IsToolbarAt(POINT screenPoint) const noexcept {
	if (!_isToolbarVisible || !_presentedToolbarRect) return false;
	const auto& dest = ScalingWindow::Get().Renderer().DestRect();
	const float x = float(screenPoint.x - dest.left), y = float(screenPoint.y - dest.top);
	const auto& rect = *_presentedToolbarRect;
	return x >= rect.x && x < rect.z && y >= rect.y && y < rect.w;
}

#ifdef MP_DEBUG_INFO_ON_OVERLAY
static std::string RectToStr(const RECT& rect) noexcept {
	return fmt::format("{},{},{},{} ({}x{})",
		rect.left, rect.top, rect.right, rect.bottom,
		rect.right - rect.left, rect.bottom - rect.top);
}
#endif

static bool ParameterValuesEqual(float left, float right) noexcept {
	return std::abs(left - right) <= 1e-5f;
}

static bool IsEffectParameterDescriptorValid(
	const EffectParameterDesc& parameter
) noexcept {
	if (parameter.name.empty()) {
		return false;
	}

	if (IsChoiceEffectParameter(parameter)) {
		const int defaultValue = std::get<1>(parameter.constant).defaultValue;
		bool hasDefault = false;
		for (size_t i = 0; i < parameter.choices.size(); ++i) {
			const EffectParameterChoice& choice = parameter.choices[i];
			if (choice.label.empty()) return false;
			hasDefault = hasDefault || choice.value == defaultValue;
			for (size_t j = 0; j < i; ++j) {
				if (parameter.choices[j].value == choice.value) return false;
			}
		}
		return hasDefault;
	}

	if (parameter.constant.index() == 0) {
		const EffectConstant<float>& value = std::get<0>(parameter.constant);
		if (!std::isfinite(value.defaultValue) || !std::isfinite(value.minValue) ||
			!std::isfinite(value.maxValue) || !std::isfinite(value.step) ||
			value.minValue > value.maxValue || value.step <= 0.0f ||
			value.defaultValue < value.minValue || value.defaultValue > value.maxValue) {
			return false;
		}
	} else {
		const EffectConstant<int>& value = std::get<1>(parameter.constant);
		if (value.minValue > value.maxValue || value.step <= 0 ||
			value.defaultValue < value.minValue || value.defaultValue > value.maxValue) {
			return false;
		}
	}

	int currentTick = 0;
	int maximumTick = 0;
	return GetEffectParameterTicks(
		parameter,
		parameter.constant.index() == 0
			? std::get<0>(parameter.constant).defaultValue
			: static_cast<float>(std::get<1>(parameter.constant).defaultValue),
		currentTick,
		maximumTick);
}

void OverlayDrawer::_InitEffectParameterValues() noexcept {
	const ScalingOptions& options = ScalingWindow::Get().Options();
	_effectParametersSaveState = options.parameterSession->saveState;
	_effectParametersRevision = options.parameterSession->saveRevision;
	const std::vector<const EffectDesc*>& descriptions =
		ScalingWindow::Get().Renderer().ActiveEffectDescs();
	const size_t effectCount = std::min(options.effects.size(), descriptions.size());
	if (descriptions.size() < options.effects.size()) {
		Logger::Get().Error(fmt::format(
			"Overlay parameter initialization is missing {} active effect description(s)",
			options.effects.size() - descriptions.size()));
	} else if (descriptions.size() > options.effects.size() + 1) {
		Logger::Get().Warn(fmt::format(
			"Overlay parameter initialization found {} unexpected appended effect(s)",
			descriptions.size() - options.effects.size()));
	}

	_localizedEffectParameters.clear();
	_localizedEffectParameters.resize(effectCount);
	_startupEffectParameterValues.clear();
	_startupEffectParameterValues.resize(effectCount);
	for (size_t effectIdx = 0; effectIdx < effectCount; ++effectIdx) {
		const EffectDesc& description = *descriptions[effectIdx];
		_localizedEffectParameters[effectIdx] = description.params;
		EffectParameterLocalization::Localize(options.effects[effectIdx].name,
			_localizedEffectParameters[effectIdx]);
		std::vector<float>& values = _startupEffectParameterValues[effectIdx];
		values.reserve(description.params.size());
		for (const EffectParameterDesc& parameter : description.params) {
			float value = parameter.constant.index() == 0
				? std::get<0>(parameter.constant).defaultValue
				: static_cast<float>(std::get<1>(parameter.constant).defaultValue);
			if (description.name == "Color\\SDR_to_HDR" && parameter.name == "nativeWhiteNits")
				value = NativeHdrWhiteValue(options.effects[effectIdx].parameters, std::string("nativeWhiteNits"), std::string("whiteNits"), std::string("exposure"));
			if (auto it = options.effects[effectIdx].parameters.find(parameter.name);
				it != options.effects[effectIdx].parameters.end()) {
				value = it->second;
			}
			values.push_back(NormalizeEffectParameterValue(parameter, value));
		}
	}
	_appliedEffectParameterValues = _startupEffectParameterValues;
	_draftEffectParameterValues = _startupEffectParameterValues;
	_submittedEffectParameterValues = _startupEffectParameterValues;
	_submittedEffectOptions = options.effects;
	_startupEffectOptions = options.effects;
	_startupFrameRefresh = options.frameRefresh;
	_draftFrameRefresh = _submittedFrameRefresh = _startupFrameRefresh;
	_effectParametersInitialized = true;
}


void OverlayDrawer::_SyncEffectParameterValues() noexcept {
	const auto& session = ScalingWindow::Get().Options().parameterSession;
	if (!session || !session->ReadIfChanged(_parameterSessionSnapshot)) return;
	const auto& refresh = _parameterSessionSnapshot.frameRefresh;
	// Bring only external changes into the draft. Unsubmitted local edits to
	// other fields remain available for the transactional save below.
	auto sync = [](auto& draft, auto& submitted, const auto& desired) {
		if (desired != submitted) draft = submitted = desired;
	};
	sync(_draftFrameRefresh.contentMode, _submittedFrameRefresh.contentMode, refresh.contentMode);
	sync(_draftFrameRefresh.contentRate, _submittedFrameRefresh.contentRate, refresh.contentRate);
	sync(_draftFrameRefresh.pacing, _submittedFrameRefresh.pacing, refresh.pacing);
	sync(_draftFrameRefresh.cursorMode, _submittedFrameRefresh.cursorMode, refresh.cursorMode);
	sync(_draftFrameRefresh.cursorSupplement, _submittedFrameRefresh.cursorSupplement, refresh.cursorSupplement);
	sync(_draftFrameRefresh.cursorRate, _submittedFrameRefresh.cursorRate, refresh.cursorRate);
	sync(_draftFrameRefresh.idleEnabled, _submittedFrameRefresh.idleEnabled, refresh.idleEnabled);
	sync(_draftFrameRefresh.idleRate, _submittedFrameRefresh.idleRate, refresh.idleRate);
	sync(_draftFrameRefresh.legacyContentLimit, _submittedFrameRefresh.legacyContentLimit, refresh.legacyContentLimit);
	sync(_draftFrameRefresh.legacySourceTarget, _submittedFrameRefresh.legacySourceTarget, refresh.legacySourceTarget);
	sync(_draftFrameRefresh.legacyLimiterOnly, _submittedFrameRefresh.legacyLimiterOnly, refresh.legacyLimiterOnly);
	sync(_draftFrameRefresh.legacyResponsiveMinimum, _submittedFrameRefresh.legacyResponsiveMinimum, refresh.legacyResponsiveMinimum);
	const auto& descriptions = ScalingWindow::Get().Renderer().ActiveEffectDescs();
	for (size_t i = 0; i < _draftEffectParameterValues.size(); ++i) {
		if (i >= _parameterSessionSnapshot.applied.size() ||
			i >= _parameterSessionSnapshot.desired.size()) break;
		const auto& desired = _parameterSessionSnapshot.desired[i];
		const auto& applied = _parameterSessionSnapshot.applied[i];
		for (size_t j = 0; j < descriptions[i]->params.size(); ++j) {
			const auto& parameter = descriptions[i]->params[j];
			float fallback = parameter.constant.index() == 0 ?
				std::get<0>(parameter.constant).defaultValue : float(std::get<1>(parameter.constant).defaultValue);
			auto value = [&](const EffectOption& option) {
				if (option.name == "Color\\SDR_to_HDR" && parameter.name == "nativeWhiteNits")
					return NormalizeEffectParameterValue(parameter, NativeHdrWhiteValue(option.parameters, std::string("nativeWhiteNits"), std::string("whiteNits"), std::string("exposure")));
				const auto it = option.parameters.find(parameter.name);
				return NormalizeEffectParameterValue(parameter, it == option.parameters.end() ? fallback : it->second);
			};
			_appliedEffectParameterValues[i][j] = value(applied);
			const float target = value(desired);
			if (!ParameterValuesEqual(target, _submittedEffectParameterValues[i][j])) {
				_draftEffectParameterValues[i][j] = target;
				_submittedEffectParameterValues[i][j] = target;
			}
		}
	}
	_submittedEffectOptions = _parameterSessionSnapshot.desired;
}

std::vector<EffectOption> OverlayDrawer::_BuildDraftEffectOptions(bool restoreStartupSizing) const noexcept {
	std::vector<EffectOption> effects = _submittedEffectOptions;
	const std::vector<const EffectDesc*>& descriptions =
		ScalingWindow::Get().Renderer().ActiveEffectDescs();
	for (size_t effectIdx = 0;
		effectIdx < _draftEffectParameterValues.size() &&
			effectIdx < effects.size() && effectIdx < descriptions.size();
		++effectIdx) {
		const EffectDesc& description = *descriptions[effectIdx];
		if (effects[effectIdx].name == NVIDIA_VFX_VSR_ID) {
			for (size_t p = 0; p < description.params.size(); ++p)
				if (description.params[p].name == "category")
					ChangeNvidiaVfxVsrCategory(effects[effectIdx], NvidiaVfxCategory(_draftEffectParameterValues[effectIdx][p]));
			if (restoreStartupSizing && effectIdx < _startupEffectOptions.size()) {
				effects[effectIdx].scalingType = _startupEffectOptions[effectIdx].scalingType;
				effects[effectIdx].scale = _startupEffectOptions[effectIdx].scale;
			}
		}
		for (size_t parameterIdx = 0;
			parameterIdx < description.params.size() &&
				parameterIdx < _draftEffectParameterValues[effectIdx].size();
			++parameterIdx) {
			// Category restoration may replace the tier even when it equals the
			// last submitted value. Always apply the complete visible VSR draft.
			if (effects[effectIdx].name != NVIDIA_VFX_VSR_ID &&
				ParameterValuesEqual(_draftEffectParameterValues[effectIdx][parameterIdx],
					_submittedEffectParameterValues[effectIdx][parameterIdx])) continue;
			effects[effectIdx].parameters[description.params[parameterIdx].name] =
				_draftEffectParameterValues[effectIdx][parameterIdx];
		}
	}
	return effects;
}

bool OverlayDrawer::_RequestEffectParameters(EffectParametersRequestKind kind, bool restoreStartupSizing) noexcept {
	const ScalingOptions& options = ScalingWindow::Get().Options();
	const uint64_t revision = ++options.parameterSession->saveRevision;
	_effectParametersRevision = revision;
	try {
		auto effects = _BuildDraftEffectOptions(restoreStartupSizing);
		EffectParametersRequest request{
			.kind = kind,
			.effects = effects,
			.previousEffects = _submittedEffectOptions,
			.frameRefresh = _draftFrameRefresh,
			.previousFrameRefresh = _submittedFrameRefresh,
			.saveState = _effectParametersSaveState,
			.revision = revision,
			.hwndSource = ScalingWindow::Get().SrcTracker().Handle(),
			.hwndScaling = ScalingWindow::Get().Handle(),
			.scalingRunId = ScalingWindow::RunId()
		};
		if (options.requestEffectParameters &&
			options.requestEffectParameters(options, std::move(request))) {
			options.parameterSession->Desired(effects);
			options.parameterSession->DesiredFrameRefresh(_draftFrameRefresh);
			_submittedFrameRefresh = _draftFrameRefresh;
			_submittedEffectOptions = std::move(effects);
			_submittedEffectParameterValues = _draftEffectParameterValues;
			return true;
		}
	} catch (...) {
		Logger::Get().Error("Unable to submit effect parameter snapshot");
	}
	_effectParametersSaveState->Complete(revision, EffectParametersSaveError::WriteFailed);
	return false;
}

static bool DrawEffectParameterSlider(const char* label, const EffectParameterDesc& parameter,
	float& value, int& currentTick, int maximumTick, const char* displayValue) noexcept {
	const ImGuiID id = ImGui::GetID(label);
	const ImVec2 position = ImGui::GetCursorScreenPos();
	const ImVec2 end(position.x + ImGui::CalcItemWidth(), position.y + ImGui::GetFrameHeight());
	const bool textInput = ImGui::TempInputIsActive(id) ||
		(ImGui::GetIO().KeyCtrl && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
			ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(position, end));
	if (textInput) {
		// The slider's integer is a STEP index, not the parameter's value. Use
		// actual units for text input, then let the caller normalize to the grid.
		const float minimum = GetEffectParameterValueFromTick(parameter, 0, maximumTick);
		const float maximum = GetEffectParameterValueFromTick(parameter, maximumTick, maximumTick);
		const std::string format = "%." + std::to_string(GetEffectParameterDisplayPrecision(parameter)) + "f";
		return ImGui::SliderFloat(label, &value, minimum, maximum, format.c_str(),
			ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_NoRoundToFormat);
	}
	const float previous = value;
	bool changed = ImGui::SliderInt(label, &currentTick, 0, maximumTick,
		displayValue, ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_NoInput);
	if (changed) value = GetEffectParameterValueFromTick(parameter, currentTick, maximumTick);
	if (parameter.inputStep > 0) {
		const bool up = ImGui::IsItemFocused() && (ImGui::IsKeyPressed(ImGuiKey_RightArrow) || ImGui::IsKeyPressed(ImGuiKey_UpArrow));
		const bool down = ImGui::IsItemFocused() && (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_DownArrow));
		const float wheel = ImGui::IsItemHovered() ? ImGui::GetIO().MouseWheel : 0;
		if (up || down || wheel != 0) {
			value = StepEffectParameter(parameter, previous, up || wheel > 0, ImGui::GetIO().KeyShift);
			changed = value != previous;
		}
	}
	return changed;
}

bool OverlayDrawer::_DrawEffectParameters(int& itemId) noexcept {
	const ImGuiIO& resetInput = ImGui::GetIO();
	_parameterResetGesture.Move(resetInput.MousePos.x, resetInput.MousePos.y, 4.0f * _dpiScale);
	if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
		_parameterResetGesture.Release(_imguiImpl.LeftReleaseWasDrag());
	}
	bool resetPressClaimed = false;
	Renderer& renderer = ScalingWindow::Get().Renderer();
	const std::vector<const EffectDesc*>& descriptions = renderer.ActiveEffectDescs();
	const auto& runtimeInfos = renderer.EffectParameterRuntimeInfos();
	const auto nrSnapshot = ScalingWindow::Get().Options().dlssNrSession->Read();
	if (!_effectParametersInitialized) {
		_InitEffectParameterValues();
	}
	_SyncEffectParameterValues();

	const size_t configuredEffectCount = std::min(
		ScalingWindow::Get().Options().effects.size(), descriptions.size());
	auto frameSyncChangeCount = [&]() noexcept -> uint32_t {
		const auto& options = ScalingWindow::Get().Options();
		return uint32_t(_draftFrameRefresh != options.frameRefresh ||
			_parameterSessionSnapshot.simpleFrameRefresh != options.isSimpleFrameRefresh ||
			_parameterSessionSnapshot.refreshPolicyChanged);
	};
	uint32_t restartChangeCount = frameSyncChangeCount();
	for (size_t effectIdx = 0; effectIdx < configuredEffectCount; ++effectIdx) {
		for (size_t parameterIdx = 0;
			parameterIdx < _draftEffectParameterValues[effectIdx].size();
			++parameterIdx) {
			const bool changed = !ParameterValuesEqual(
				_draftEffectParameterValues[effectIdx][parameterIdx],
				_appliedEffectParameterValues[effectIdx][parameterIdx]);
			if (changed && effectIdx < runtimeInfos.size() &&
				parameterIdx < runtimeInfos[effectIdx].size() &&
				runtimeInfos[effectIdx][parameterIdx].applyMode ==
					EffectParameterApplyMode::RestartRequired) {
				++restartChangeCount;
			}
		}
	}

	const ImVec2 displaySize = ImGui::GetIO().DisplaySize;
	const float viewportMargin = 16.0f * _dpiScale;
	const ImVec2 maxWindowSize{
		std::max(1.0f, displaySize.x - viewportMargin),
		std::max(1.0f, displaySize.y - viewportMargin)
	};
	const ImVec2 minWindowSize{
		std::min(EFFECT_PARAMETERS_MIN_WIDTH * _dpiScale, maxWindowSize.x),
		std::min(EFFECT_PARAMETERS_MIN_HEIGHT * _dpiScale, maxWindowSize.y)
	};
	ImGui::SetNextWindowSizeConstraints(minWindowSize, maxWindowSize);
	OverlayWindowOption& windowOption = _overlayOptions->windows.at(EFFECT_PARAMETERS_WINDOW_ID);
	const bool restoreLayout = !_effectParametersWindowLayoutInitialized ||
		_effectParametersViewport.x != displaySize.x || _effectParametersViewport.y != displaySize.y;
	if (restoreLayout) {
		const auto rect = RestoreEffectParametersWindow(
			windowOption, displaySize.x, displaySize.y, _dpiScale);
		ImGui::SetNextWindowSize({ rect.width, rect.height });
		ImGui::SetNextWindowPos({ rect.x, rect.y });
		_effectParametersWindowLayoutInitialized = true;
		_effectParametersViewport = displaySize;
	}

	const std::string title = !_parameterFocusSwitchingEnabled
		? StrHelper::Concat(_GetResourceString(L"Overlay_EffectParameters"), "##", EFFECT_PARAMETERS_WINDOW_ID)
		: StrHelper::Concat(
		_GetResourceString(L"Overlay_EffectParameters"),
		" - ", _GetResourceString(IsEditingParameters() ? L"Overlay_Parameters_Edit" : L"Overlay_Parameters_Preview"),
		"###", EFFECT_PARAMETERS_WINDOW_ID);
	ImGui::PushStyleColor(
		ImGuiCol_ResizeGrip, ImVec4(0.55f, 0.55f, 0.55f, 0.28f));
	ImGui::PushStyleColor(
		ImGuiCol_ResizeGripHovered, ImVec4(0.35f, 0.67f, 0.95f, 0.72f));
	ImGui::PushStyleColor(
		ImGuiCol_ResizeGripActive, ImVec4(0.35f, 0.67f, 0.95f, 1.0f));
	const bool expanded = ImGui::Begin(title.c_str(), _parameterFocusSwitchingEnabled ? nullptr : &_isEffectParametersVisible,
		(!_parameterFocusSwitchingEnabled || IsEditingParameters()) ? ImGuiWindowFlags_None : ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);
	ImGuiWindow* window = ImGui::GetCurrentWindow();
	if (IsEditingParameters() && window->TitleBarRect().Contains(ImGui::GetIO().MousePos)) {
		const auto& shortcut = ScalingWindow::Get().Options().toolbarShortcutLabels.parameters;
		const std::string hint = shortcut.empty() ?
			_GetResourceString(L"Overlay_Parameters_InputHintWithoutShortcut") :
			StrHelper::Concat(_GetResourceString(L"Overlay_Parameters_InputHint"), " ", shortcut);
		_imguiImpl.Tooltip(hint.c_str(), _dpiScale);
	}
	const OverlayWindowRect rect{
		std::clamp(window->Pos.x, 0.0f, std::max(0.0f, displaySize.x - window->SizeFull.x)),
		std::clamp(window->Pos.y, 0.0f, std::max(0.0f, displaySize.y - window->SizeFull.y)),
		window->SizeFull.x, window->SizeFull.y
	};
	ImGui::SetWindowPos(window, { rect.x, rect.y });
	if (!restoreLayout && !_imguiImpl.FrameInputCanceled() &&
		(ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::IsMouseReleased(ImGuiMouseButton_Left))) {
		const auto& previous = _effectParametersWindowRect;
		const bool moved = rect.x != previous.x || rect.y != previous.y;
		const bool resizedX = rect.width != previous.width;
		const bool resizedY = rect.height != previous.height;
		if (moved || resizedX || resizedY) {
			RememberOverlayWindowPosition(windowOption, rect, displaySize.x, displaySize.y, _dpiScale);
			if (resizedX) windowOption.width = rect.width / _dpiScale;
			if (resizedY) windowOption.height = rect.height / _dpiScale;
			_effectParametersWindowLayoutDirty = true;
		}
	}
	_effectParametersWindowRect = rect;
	if (!expanded) {
		ImGui::End();
		ImGui::PopStyleColor(3);
		_parameterResetGesture.Clear();
		return false;
	}
	if (_parameterFocusFailed) ImGui::TextWrapped("%s", _GetResourceString(L"Overlay_Parameters_FocusFailed").c_str());

	if (!renderer.MotionConfigurationNotice().empty()) {
		ImGui::TextWrapped("%s", StrHelper::UTF16ToUTF8(renderer.MotionConfigurationNotice()).c_str());
		ImGui::Separator();
	}

	const ImGuiStyle& style = ImGui::GetStyle();
	const float actionHeight = ImGui::GetFrameHeight() + style.ItemSpacing.y * 2.0f;
	const bool hasSaveError = (_effectParametersSaveState->result.load(std::memory_order_acquire) & 7) != 0;
	const float statusHeight = ImGui::GetTextLineHeight() * (hasSaveError ? 3.0f : 1.0f) + style.ItemSpacing.y;
	const float footerHeight =
		actionHeight + statusHeight + style.ItemSpacing.y * 3.0f;
	const float contentHeight = std::max(
		100.0f * _dpiScale, ImGui::GetContentRegionAvail().y - footerHeight);
	ImGui::BeginChild(
		"##effectParametersContent", ImVec2(0.0f, contentHeight),
		ImGuiChildFlags_None, ImGuiWindowFlags_AlwaysVerticalScrollbar |
		(!_parameterFocusSwitchingEnabled || IsEditingParameters() ? ImGuiWindowFlags_None : ImGuiWindowFlags_NoInputs));

	bool needRedraw = false;
	bool queueFailure = false;
	std::vector<std::tuple<uint32_t, uint32_t, float>> liveUpdates;
	bool parameterEdited = false;
	bool requestRestart = false;
	bool restoreStartupSizing = false;
	ImGui::PushID("frameRefresh");
	ImGui::SeparatorText(_GetResourceString(L"FrameRefresh/Header").c_str());
	ImGui::TextDisabled("%s", _GetResourceString(L"FrameRefresh_RestartNotice").c_str());
	auto refreshChoice = [&](const char* id, const wchar_t* label, auto& value, const auto& keys) {
		ImGui::TextUnformatted(_GetResourceString(label).c_str());
		ImGui::SetNextItemWidth(-1.0f);
		bool changed = false;
		if (ImGui::BeginCombo(id, _GetResourceString(keys[uint32_t(value)]).c_str())) {
			for (uint32_t i = 0; i < std::size(keys); ++i) {
				if (ImGui::Selectable(_GetResourceString(keys[i]).c_str(), i == uint32_t(value))) {
					value = static_cast<std::remove_reference_t<decltype(value)>>(i);
					changed = parameterEdited = needRedraw = true;
				}
			}
			ImGui::EndCombo();
		}
		return changed;
	};
	auto rate = [&](const char* id, const wchar_t* label, float& value) {
		ImGui::TextUnformatted(_GetResourceString(label).c_str());
		ImGui::SetNextItemWidth(-1.0f);
		constexpr int minimum = FrameRefreshSettings::MinimumEditedRate;
		constexpr int maximum = FrameRefreshSettings::MaximumEditedRate;
		int integerValue = static_cast<int>(std::round(std::clamp(value, float(minimum), float(maximum))));
		const bool changed = ImGui::SliderInt(id, &integerValue, minimum, maximum, "%d FPS",
			ImGuiSliderFlags_AlwaysClamp | (_parameterFocusSwitchingEnabled ? ImGuiSliderFlags_None : ImGuiSliderFlags_NoInput));
		if (changed) {
			value = static_cast<float>(integerValue);
			parameterEdited = needRedraw = true;
		}
		return changed;
	};
	static constexpr const wchar_t* contentKeys[]{ L"FrameRefresh_Source/Content", L"FrameRefresh_Custom/Content" };
	static constexpr const wchar_t* pacingKeys[]{ L"FrameSync_Mode_FrontEdge/Content", L"FrameSync_Mode_Async/Content", L"FrameSync_Mode_Reflex/Content" };
	static constexpr const wchar_t* cursorKeys[]{ L"FrameRefresh_Responsive/Content", L"FrameRefresh_OriginalOnly/Content", L"FrameRefresh_Supplement/Content" };
	static constexpr const wchar_t* supplementKeys[]{ L"FrameRefresh_Auto/Content", L"FrameRefresh_Custom/Content" };
	static constexpr const wchar_t* idleKeys[]{ L"FrameRefresh_Off/Content", L"FrameRefresh_Custom/Content" };
	// The toolbar follows the running session; mode selection belongs to settings.
	const bool simpleRefresh = ScalingWindow::Get().Options().isSimpleFrameRefresh;
	int contentMode = _draftFrameRefresh.contentMode == ContentFrameRateMode::Custom ? 1 : 0;
	if (refreshChoice("##contentMode", L"FrameRefresh_Content/Header", contentMode, contentKeys)) {
		_draftFrameRefresh.contentMode = contentMode == 1 ? ContentFrameRateMode::Custom : ContentFrameRateMode::Source;
		_draftFrameRefresh.ContentEdited();
	}
	if (!simpleRefresh) {
		ImGui::BeginDisabled(_draftFrameRefresh.contentMode == ContentFrameRateMode::Source);
		if (refreshChoice("##pacing", L"FrameRefresh_Pacing/Header", _draftFrameRefresh.pacing, pacingKeys))
			_draftFrameRefresh.ContentEdited();
		ImGui::EndDisabled();
	}
	if (_draftFrameRefresh.contentMode == ContentFrameRateMode::Custom &&
		rate("##contentRate", L"FrameRefresh_ContentRate/Header", _draftFrameRefresh.contentRate))
		_draftFrameRefresh.ContentEdited();
	if (!simpleRefresh) {
		if (refreshChoice("##cursorMode", L"FrameRefresh_Cursor/Header", _draftFrameRefresh.cursorMode, cursorKeys))
			_draftFrameRefresh.CursorEdited();
		if (_draftFrameRefresh.cursorMode == CursorRefreshMode::Supplement) {
			refreshChoice("##supplementMode", L"FrameRefresh_CursorSupplement/Header", _draftFrameRefresh.cursorSupplement, supplementKeys);
			if (_draftFrameRefresh.cursorSupplement == CursorSupplementMode::Custom)
				rate("##cursorRate", L"FrameRefresh_CursorRate/Header", _draftFrameRefresh.cursorRate);
		}
	}
	refreshChoice("##idleMode", L"FrameRefresh_Idle/Header", _draftFrameRefresh.idleEnabled, idleKeys);
	if (_draftFrameRefresh.idleEnabled) rate("##idleRate", L"FrameRefresh_IdleRate/Header", _draftFrameRefresh.idleRate);
	if (ImGui::Checkbox(_GetResourceString(L"FrameRefresh_VRR/Header").c_str(), &_draftFrameRefresh.vrrEnabled))
		parameterEdited = needRedraw = true;
	if (!simpleRefresh) {
		if (_draftFrameRefresh.legacyContentLimit > 0 || _draftFrameRefresh.legacySourceTarget >= 0 || _draftFrameRefresh.legacyLimiterOnly || _draftFrameRefresh.legacyResponsiveMinimum)
			ImGui::TextWrapped("%s", _GetResourceString(L"FrameRefresh_LegacyNotice").c_str());
		if (_draftFrameRefresh.legacyContentLimit > 0) ImGui::TextWrapped("%s", fmt::format(
			fmt::runtime(_GetResourceString(L"FrameRefresh_LegacyCapNotice")), _draftFrameRefresh.legacyContentLimit).c_str());
		if (ImGui::Button(_GetResourceString(L"FrameRefresh_ResetButton/Content").c_str())) {
			_draftFrameRefresh = {};
			parameterEdited = needRedraw = true;
		}
	}
	ImGui::PopID();
	auto getDraftValue = [&](size_t effectIdx, const EffectDesc& description,
		std::string_view name, float fallback) noexcept {
		if (name == "__hdrHasPair") {
			const auto plan = BuildHdrComponentPlan(_BuildDraftEffectOptions(), ScalingWindow::Get().Options().legacyHdrCaptureIntent);
			return plan.error == HdrComponentError::None && effectIdx < plan.stages.size() &&
				plan.stages[effectIdx].pairIndex != size_t(-1) ? 1.f : 0.f;
		}
		for (size_t i = 0; i < description.params.size(); ++i) {
			if (description.params[i].name == name &&
				effectIdx < _draftEffectParameterValues.size() &&
				i < _draftEffectParameterValues[effectIdx].size()) {
				return _draftEffectParameterValues[effectIdx][i];
			}
		}
		return fallback;
	};
	auto isSessionLive = [&](size_t effectIdx, size_t parameterIdx) noexcept {
		if (effectIdx < descriptions.size() && descriptions[effectIdx]->name == NVIDIA_VFX_VSR_ID &&
			descriptions[effectIdx]->params[parameterIdx].name == "strength") {
			const auto applied = _parameterSessionSnapshot.applied[effectIdx].parameters.find("category");
			if (NvidiaVfxCategory(getDraftValue(effectIdx, *descriptions[effectIdx], "category", 0.f)) !=
				NvidiaVfxCategory(applied == _parameterSessionSnapshot.applied[effectIdx].parameters.end() ? 0.f : applied->second)) return false;
		}
		if (effectIdx >= runtimeInfos.size() ||
			parameterIdx >= runtimeInfos[effectIdx].size() ||
			runtimeInfos[effectIdx][parameterIdx].applyMode !=
				EffectParameterApplyMode::Live) {
			return false;
		}
		return true;
	};

	for (size_t effectIdx = 0; effectIdx < configuredEffectCount; ++effectIdx) {
		const EffectDesc& description = *descriptions[effectIdx];
		if (description.params.empty()) {
			continue;
		}
		ImGui::PushID(itemId++);
		ImGui::SeparatorText(
			std::string(GetEffectDisplayName(description)).c_str());
		const auto hdrPlan = BuildHdrComponentPlan(_BuildDraftEffectOptions(),
			ScalingWindow::Get().Options().legacyHdrCaptureIntent);
		if (hdrPlan.error == HdrComponentError::None && effectIdx < hdrPlan.stages.size() &&
			hdrPlan.stages[effectIdx].pairIndex != size_t(-1))
			ImGui::TextWrapped("%s", EffectParameterLocalization::HdrPairSource(hdrPlan.stages[effectIdx].pairIndex).c_str());

		const DlssNrStatus* nrStatus = description.name == "DLSSNR\\DLSSNR_AI_Filter" && effectIdx < nrSnapshot.effects.size()
			? &nrSnapshot.effects[effectIdx] : nullptr;
		if (nrStatus) {
			ImGui::TextWrapped("%s", _GetResourceString(DlssNrStateKey(nrStatus->state)).c_str());
			if (nrStatus->Failed()) {
				ImGui::TextWrapped("%s", _GetResourceString(DlssNrReasonKey(nrStatus->failure.reason)).c_str());
				ImGui::TextWrapped("%s", _GetResourceString(DlssNrAdviceKey(*nrStatus)).c_str());
				if (ImGui::Button(_GetResourceString(L"DlssNr_OpenHelp").c_str())) {
					if (const auto& open = ScalingWindow::Get().Options().openDlssNrHelp) open(static_cast<uint32_t>(effectIdx));
				}
			}
		}
		std::string currentGroup;
		// 仅在 _DEBUG 下读取（参数元数据一致性告警），release 下以 maybe_unused 抑制 ClangCL -Werror
		[[maybe_unused]] uint32_t validCount = 0;
		[[maybe_unused]] uint32_t invalidCount = 0;
		for (size_t parameterIdx = 0;
			parameterIdx < description.params.size(); ++parameterIdx) {
			auto parameter = _localizedEffectParameters[effectIdx][parameterIdx];
			const std::string& effectName =
				ScalingWindow::Get().Options().effects[effectIdx].name;
			FilterNvidiaVfxVsrChoices(effectName, parameter, NvidiaVfxCategory(getDraftValue(effectIdx, description, "category", 0.f)));
			if (!IsEffectParameterVisible(effectName, parameter.name,
				[&](std::string_view name, float fallback) { return getDraftValue(effectIdx, description, name, fallback); })) continue;
			if (parameter.group != currentGroup) {
				currentGroup = parameter.group;
				if (!currentGroup.empty()) {
					ImGui::Spacing();
					ImGui::SeparatorText(std::string(currentGroup).c_str());
				}
			}

			const EffectParameterRuntimeInfo* info =
				effectIdx < runtimeInfos.size() &&
				parameterIdx < runtimeInfos[effectIdx].size()
					? &runtimeInfos[effectIdx][parameterIdx] : nullptr;
			const bool hasValueStorage =
				effectIdx < _draftEffectParameterValues.size() &&
				parameterIdx < _draftEffectParameterValues[effectIdx].size();
			const bool backendUnavailable = (nrStatus && nrStatus->Failed()) || (info &&
				info->applyMode == EffectParameterApplyMode::Unavailable);
			const bool restricted = info && info->applyMode == EffectParameterApplyMode::Restricted;
			const wchar_t* unavailableKey = backendUnavailable ? L"Overlay_EffectParameters_BackendUnavailable" : restricted
				? (nrStatus ? L"DlssNr_Parameter_HdrRestricted" : L"Overlay_EffectParameters_Restricted")
				: L"Overlay_EffectParameters_Invalid";
			bool parameterValid = !backendUnavailable && !restricted && hasValueStorage && info &&
				info->name == parameter.name &&
				IsEffectParameterDescriptorValid(parameter);
			float placeholderValue = parameter.constant.index() == 0
				? std::get<0>(parameter.constant).defaultValue
				: static_cast<float>(std::get<1>(parameter.constant).defaultValue);
			if (!std::isfinite(placeholderValue)) {
				placeholderValue = 0.0f;
			}
			const bool frontEdgeSyncEnabled = ScalingWindow::Get().Options().isFrontEdgeSyncEnabled;
			const bool parameterEnabled = IsEffectParameterEnabled(effectName, parameter.name,
				frontEdgeSyncEnabled, [&](std::string_view name, float fallback) {
					return getDraftValue(effectIdx, description, name, fallback);
				});
			// Show the effective mode without overwriting the saved custom preference.
			const bool forcedFollowMode = IsFrameRateFilterEffect(effectName) &&
				frontEdgeSyncEnabled && parameter.name == "frameRateMode";
			float forcedValue = 0.0f;
			float& value = forcedFollowMode ? forcedValue : hasValueStorage
				? _draftEffectParameterValues[effectIdx][parameterIdx]
				: placeholderValue;
			const bool isBoolean = IsBooleanEffectParameter(parameter);
			const bool isChoice = IsChoiceEffectParameter(parameter);
			int currentTick = 0;
			int maximumTick = 0;
			if (!isBoolean && !isChoice && parameterValid && !GetEffectParameterTicks(
				parameter, value, currentTick, maximumTick)) {
				parameterValid = false;
			}
			const bool isLive = parameterValid &&
				isSessionLive(effectIdx, parameterIdx);
			std::string badge = restricted ? _GetResourceString(L"Overlay_EffectParameters_RestrictedBadge") : backendUnavailable ?
				_GetResourceString(L"Overlay_EffectParameters_Unavailable") : parameterValid
				? _GetResourceString(info->automaticRestart ? L"Overlay_EffectParameters_AutoRestart" : isLive
					? L"Overlay_EffectParameters_Live"
					: L"Overlay_EffectParameters_RestartRequired")
				: "[!]";
			if (parameterValid && !forcedFollowMode && !ParameterValuesEqual(value, _appliedEffectParameterValues[effectIdx][parameterIdx])) {
				const float applied = _appliedEffectParameterValues[effectIdx][parameterIdx];
				std::string actual = fmt::format("{:.7g}", applied);
				if (isChoice) {
					const auto choice = std::ranges::find(parameter.choices, int(std::lround(applied)), &EffectParameterChoice::value);
					if (choice != parameter.choices.end()) actual = choice->label;
				}
				badge += " · " + fmt::format(fmt::runtime(_GetResourceString(
					L"Overlay_EffectParameters_NotApplied")), actual);
			}

			const std::string& label =
				parameter.label.empty() ? parameter.name : parameter.label;
			if (parameterValid) ++validCount;
			else if (!backendUnavailable && !restricted) ++invalidCount;

			ImGui::PushID(itemId++);
			ImGui::BeginDisabled(!parameterValid || !parameterEnabled);
			bool changed = false;
			bool parameterHovered = false;
			bool parameterNameHovered = false;
			auto drawParameterName = [&]() {
				ImGui::TextWrapped("%s", label.c_str());
				parameterNameHovered = ImGui::IsItemHovered(
					ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayNone);
				parameterHovered |= ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
				ImGui::SameLine();
				ImGui::TextDisabled("%s", badge.c_str());
			};

			if (isBoolean) {
				bool boolValue = std::lround(value) != 0;
				changed = ImGui::Checkbox("##value", &boolValue);
				parameterHovered |= ImGui::IsItemHovered(
					ImGuiHoveredFlags_AllowWhenDisabled);
				ImGui::SameLine();
				drawParameterName();
				parameterHovered |= ImGui::IsItemHovered(
					ImGuiHoveredFlags_AllowWhenDisabled);
				if (changed) {
					value = boolValue ? 1.0f : 0.0f;
				}
			} else if (isChoice) {
				drawParameterName();
				parameterHovered |= ImGui::IsItemHovered(
					ImGuiHoveredFlags_AllowWhenDisabled);
				ImGui::SetNextItemWidth(-FLT_MIN);
				const int selectedValue = static_cast<int>(std::lround(
					NormalizeEffectParameterValue(parameter, value)));
				const auto selected = std::ranges::find(
					parameter.choices, selectedValue,
					&EffectParameterChoice::value);
				const char* preview = selected == parameter.choices.end()
					? "--" : selected->label.c_str();
				if (ImGui::BeginCombo("##value", preview)) {
					for (const EffectParameterChoice& choice : parameter.choices) {
						const bool isSelected = choice.value == selectedValue;
						if (ImGui::Selectable(choice.label.c_str(), isSelected)) {
							value = static_cast<float>(choice.value);
							changed = true;
						}
						if (isSelected) ImGui::SetItemDefaultFocus();
					}
					ImGui::EndCombo();
				}
				parameterHovered |= ImGui::IsItemHovered(
					ImGuiHoveredFlags_AllowWhenDisabled);
			} else {
				drawParameterName();
				parameterHovered |= ImGui::IsItemHovered(
					ImGuiHoveredFlags_AllowWhenDisabled);
				ImGui::SetNextItemWidth(-FLT_MIN);
				if (parameterValid) {
					const std::string displayValue =
						parameter.constant.index() == 0
							? fmt::format("{:.{}f}", value,
								GetEffectParameterDisplayPrecision(parameter))
							: fmt::format("{}",
								static_cast<int>(std::lround(value)));
					const float previousValue = value;
					if (_parameterFocusSwitchingEnabled) {
						changed = DrawEffectParameterSlider("##value", parameter, value,
							currentTick, maximumTick, displayValue.c_str());
					} else {
						changed = ImGui::SliderInt("##value", &currentTick, 0, maximumTick,
							displayValue.c_str(), ImGuiSliderFlags_AlwaysClamp);
						if (changed) value = GetEffectParameterValueFromTick(parameter, currentTick, maximumTick);
					}
					if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
						!_imguiImpl.LeftPressHasControl() && !resetInput.KeyCtrl &&
						!ImGui::TempInputIsActive(ImGui::GetItemID())) {
						resetPressClaimed = true;
						const uint64_t control = (uint64_t(effectIdx) << 32) | (parameterIdx + 1);
						if (_parameterResetGesture.Press(control, _imguiImpl.LeftPressTimeUs(),
							resetInput.MousePos.x, resetInput.MousePos.y, 4.0f * _dpiScale)) {
							value = NormalizeEffectParameterValue(parameter, placeholderValue);
							changed = !ParameterValuesEqual(previousValue, value);
							ImGui::ClearActiveID();
						}
					}
				} else {
					float unavailableValue = 0.0f;
					ImGui::SliderFloat(
						"##value", &unavailableValue, 0.0f, 1.0f, "--");
				}
				parameterHovered |= ImGui::IsItemHovered(
					ImGuiHoveredFlags_AllowWhenDisabled);
			}
			ImGui::EndDisabled();

			if (changed && parameterValid && parameterEnabled) {
				if (effectName == NVIDIA_VFX_VSR_ID && parameter.name == "category") {
					const auto switched = _BuildDraftEffectOptions();
					for (size_t p = 0; p < description.params.size(); ++p)
						if (description.params[p].name == "strength") _draftEffectParameterValues[effectIdx][p] = switched[effectIdx].parameters.at("strength");
				}
				parameterEdited = true;
				value = NormalizeEffectParameterInput(parameter, value);
				if (isLive) liveUpdates.emplace_back(static_cast<uint32_t>(effectIdx),
					static_cast<uint32_t>(parameterIdx), value);
				needRedraw = true;
			}

			if (parameterNameHovered) {
				std::string help = EffectParameterLocalization::Tooltip(effectName, parameter, parameterEnabled);
				if (effectName != "DLSSNR\\DLSSNR_AI_Filter") {
					if (!parameterValid) {
						help += "\n" + _GetResourceString(unavailableKey);
					} else if (info->automaticRestart) {
						help += "\n" + _GetResourceString(L"Overlay_EffectParameters_Reason_AutoRestart");
					} else if (!isLive) {
						help += "\n" + _GetResourceString(L"Overlay_EffectParameters_RestartRequired");
					}
					if (parameterValid && parameterEnabled && !isBoolean && !isChoice) {
						help += "\n" + fmt::format(fmt::runtime(_GetResourceString(
							L"Overlay_EffectParameters_ResetDefault")), fmt::format("{:.7g}", placeholderValue));
					}
				}
				_imguiImpl.Tooltip(help.c_str(), _dpiScale);
			} else if (parameterHovered) {
				std::string resetHint;
				if (parameterValid && parameterEnabled && !isBoolean && !isChoice) {
					resetHint = fmt::format(fmt::runtime(_GetResourceString(
						L"Overlay_EffectParameters_ResetDefault")),
						fmt::format("{:.7g}", placeholderValue));
				}
				if (!parameterValid) {
					_imguiImpl.Tooltip(_GetResourceString(
						unavailableKey).c_str(), _dpiScale);
				} else if (info->automaticRestart) {
					_imguiImpl.Tooltip(_GetResourceString(L"Overlay_EffectParameters_Reason_AutoRestart").c_str(),
						_dpiScale, resetHint.empty() ? nullptr : resetHint.c_str());
				} else if (!isLive) {
					std::wstring_view reasonKey =
						L"Overlay_EffectParameters_Reason_Native";
					if (info->applyMode == EffectParameterApplyMode::Live) {
						reasonKey =
							L"Overlay_EffectParameters_Reason_Resources";
					} else {
						switch (info->restartReason) {
						case EffectParameterRestartReason::InlineParameters:
							reasonKey =
								L"Overlay_EffectParameters_Reason_Inline";
							break;
						case EffectParameterRestartReason::ResourceRecreation:
							reasonKey =
								L"Overlay_EffectParameters_Reason_Resources";
							break;
						case EffectParameterRestartReason::FrameGuidance:
							reasonKey =
								L"Overlay_EffectParameters_Reason_Guidance";
							break;
						case EffectParameterRestartReason::FrameGeneration:
							reasonKey =
								L"Overlay_EffectParameters_Reason_FrameGeneration";
							break;
						default:
							break;
						}
					}
					_imguiImpl.Tooltip(
						_GetResourceString(reasonKey).c_str(), _dpiScale,
						resetHint.empty() ? nullptr : resetHint.c_str());
				} else if (!resetHint.empty()) {
					_imguiImpl.Tooltip(resetHint.c_str(), _dpiScale);
				}
			}
			ImGui::PopID();
		}

#ifdef _DEBUG
		if (invalidCount > 0) {
			Logger::Get().Warn(fmt::format(
				"Overlay parameter metadata mismatch: effect#{} ({}) "
				"declared={} valid={} invalid={}",
				effectIdx, description.name, description.params.size(),
				validCount, invalidCount));
		}
#endif
		ImGui::PopID();
	}
	ImGui::EndChild();
	if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !resetPressClaimed) {
		_parameterResetGesture.Clear();
	}

	{
		ImGui::Separator();
		if (ImGui::Button(
			_GetResourceString(L"Overlay_EffectParameters_Revert").c_str())) {
			parameterEdited = true;
			_draftEffectParameterValues = _startupEffectParameterValues;
			restoreStartupSizing = true;
			_draftFrameRefresh = _startupFrameRefresh;
			for (size_t effectIdx = 0;
				effectIdx < configuredEffectCount; ++effectIdx) {
				for (size_t parameterIdx = 0;
					parameterIdx <
						_draftEffectParameterValues[effectIdx].size();
					++parameterIdx) {
					if (isSessionLive(effectIdx, parameterIdx)) liveUpdates.emplace_back(
						static_cast<uint32_t>(effectIdx), static_cast<uint32_t>(parameterIdx),
						_draftEffectParameterValues[effectIdx][parameterIdx]);
				}
			}
			needRedraw = true;
		}
		ImGui::SameLine();
		const bool saveFailed = (_effectParametersSaveState->result.load(std::memory_order_acquire) & 7) != 0;
		ImGui::BeginDisabled((restartChangeCount == 0 && !saveFailed) ||
			!ScalingWindow::Get().Options().requestEffectParameters);
		if (ImGui::Button(_GetResourceString(
			L"Overlay_EffectParameters_ApplyAndRestart").c_str())) {
			requestRestart = true;
			needRedraw = true;
		}
		ImGui::EndDisabled();
	}
	bool submitted = true;
	if (parameterEdited || requestRestart) {
		submitted = _RequestEffectParameters(requestRestart ? EffectParametersRequestKind::SaveAndRestart
			: EffectParametersRequestKind::AutoSave, restoreStartupSizing);
	}
	// Publish desired values and enqueue persistence before waking the backend.
	// Otherwise an immediate load failure could race ahead of the edit it rolls back.
	if (submitted && !requestRestart) for (const auto& [effect, parameter, value] : liveUpdates) {
		if (!renderer.QueueEffectParameterUpdate(effect, parameter, value)) queueFailure = true;
	}
	if (queueFailure) ScalingWindow::Get().ShowError(ScalingError::EffectParameterLiveFailed);
	// Recount after this frame's edits (including Revert), keeping persistence
	// separate from the values that have actually reached the backend.
	restartChangeCount = frameSyncChangeCount();
	for (size_t i = 0; i < _draftEffectParameterValues.size(); ++i) {
		for (size_t j = 0; j < _draftEffectParameterValues[i].size(); ++j) {
			if (i < runtimeInfos.size() && j < runtimeInfos[i].size() &&
				runtimeInfos[i][j].applyMode == EffectParameterApplyMode::RestartRequired && !ParameterValuesEqual(
				_draftEffectParameterValues[i][j], _appliedEffectParameterValues[i][j])) {
				++restartChangeCount;
			}
		}
	}
	_lastEffectParametersSaveResult =
		_effectParametersSaveState->result.load(std::memory_order_acquire);
	const uint64_t completedRevision = _lastEffectParametersSaveResult >> 3;
	const auto error = static_cast<EffectParametersSaveError>(_lastEffectParametersSaveResult & 7);
	std::string status;
	if (completedRevision < _effectParametersRevision) {
		status = _GetResourceString(L"Overlay_EffectParameters_RequestPending");
	} else if (error != EffectParametersSaveError::None) {
		std::wstring_view key = L"Overlay_EffectParameters_SaveFailed";
		if (error == EffectParametersSaveError::Conflict) key = L"Overlay_EffectParameters_ScalingModeConflict";
		else if (error == EffectParametersSaveError::SessionExpired) key = L"Overlay_EffectParameters_SessionExpired";
		else if (error == EffectParametersSaveError::SourceUnavailable) key = L"Overlay_EffectParameters_SourceUnavailable";
		status = _GetResourceString(key);
	} else if (_effectParametersRevision) {
		status = _GetResourceString(L"Overlay_EffectParameters_AutoSaved");
	}
	uint32_t livePending = 0;
	for (size_t i = 0; i < _draftEffectParameterValues.size(); ++i)
		for (size_t j = 0; j < _draftEffectParameterValues[i].size(); ++j)
			if (isSessionLive(i, j) && !ParameterValuesEqual(_draftEffectParameterValues[i][j], _appliedEffectParameterValues[i][j])) ++livePending;
	if (_parameterSessionSnapshot.applying || livePending || _parameterSessionSnapshot.applyFailed) {
		if (!status.empty()) status += "  ";
		status += _GetResourceString(_parameterSessionSnapshot.applyFailed && !livePending
			? L"Overlay_EffectParameters_ApplyRolledBack" : L"Overlay_EffectParameters_Applying");
	}
	if (restartChangeCount > 0) {
		if (!status.empty()) status += "  ";
		status += fmt::format(fmt::runtime(_GetResourceString(
			L"Overlay_EffectParameters_ApplyPending")), restartChangeCount);
	}

	ImGui::Separator();
	ImGui::BeginChild(
		"##effectParametersStatus",
		ImVec2(-ImGui::GetFrameHeight(), statusHeight),
		ImGuiChildFlags_None,
		!_parameterFocusSwitchingEnabled ? ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
		: IsEditingParameters() ? ImGuiWindowFlags_None : ImGuiWindowFlags_NoInputs);
	ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
	ImGui::TextWrapped("%s", status.c_str());
	ImGui::PopStyleColor();
	if (ImGui::IsItemHovered() &&
		ImGui::CalcTextSize(status.c_str()).x >
		ImGui::GetContentRegionAvail().x) {
		_imguiImpl.Tooltip(
			status.c_str(), _dpiScale, nullptr, 480.0f * _dpiScale);
	}
	ImGui::EndChild();

	ImGui::End();
	ImGui::PopStyleColor(3);
	return needRedraw;
}

void OverlayDrawer::_DrawProfilerGraph(const ProfilerHistory::Plot& plot, int metric, bool available) noexcept {
	static constexpr const wchar_t* LABELS[]{ L"Overlay_Profiler_GraphFPS", L"Overlay_Profiler_GraphInterval", L"Overlay_Profiler_GraphEffects" };
	static constexpr const wchar_t* HELP[]{ L"Overlay_Profiler_GraphFPSHelp", L"Overlay_Profiler_GraphIntervalHelp", L"Overlay_Profiler_GraphEffectsHelp" };
	const char* unit = metric == 0 ? "FPS" : "ms";
	auto values = [metric](const ProfilerHistory::Point& point) {
		if (metric == 0) return ProfilerHistory::Values{ point.fps, point.fps, point.fpsValid ? 1u : 0u };
		return metric == 1 ? point.interval : point.effects;
	};
	struct Series {
		const ProfilerHistory::Plot* plot;
		std::string label;
		ImGuiCol color;
		bool available;
		ProfilerHistory::Values summary{};
		std::optional<float> current{};
	};
	const int seriesCount = metric == 2 ? 1 : 2;
	Series series[]{
		{ &plot, _GetResourceString(L"Overlay_Profiler_CaptureSeries"), ImGuiCol_PlotLines, available },
		{ &_profilerOutputProfile.plot, _GetResourceString(_profilerOutputProfile.source == ContentFrameCountSource::Submitted
			? L"Overlay_Profiler_OutputSubmittedSeries" : L"Overlay_Profiler_OutputSeries"),
			ImGuiCol_PlotHistogram, _profilerOutputProfile.active && (metric == 0 || _profilerOutputProfile.individualTimestamps) }
	};
	const auto now = plot.points.back().time;
	double plotMaximum = 0;
	for (int i = 0; i < seriesCount; ++i) {
		auto& s = series[i];
		for (const auto& point : s.plot->points) {
			const auto value = values(point);
			if (!value.count) continue;
			s.summary.total += value.total;
			s.summary.count += value.count;
			s.summary.maximum = std::max(s.summary.maximum, value.maximum);
			if (now - point.time <= 500ms) s.current = float(value.Mean());
		}
		if (!s.available || s.plot->suspended) s.current.reset();
		plotMaximum = std::max(plotMaximum, s.summary.maximum);
	}
	ImGui::PushID(metric);
	ImGui::PushTextWrapPos();
	ImGui::TextUnformatted(_GetResourceString(LABELS[metric]).c_str());
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", _GetResourceString(HELP[metric]).c_str());
	for (int i = 0; i < seriesCount; ++i) {
		const auto& s = series[i];
		if (seriesCount > 1) ImGui::TextColored(ImGui::GetStyleColorVec4(s.color), "%s", s.label.c_str());
		const auto value = s.current ? fmt::format("{:.2f} {}", *s.current, unit) : fmt::format("-- {}", unit);
		const float right = ImGui::GetWindowContentRegionMax().x - ImGui::CalcTextSize(value.c_str()).x;
		// A narrow viewport may constrain the normal minimum width. Wrap the
		// value to its own line instead of overlapping the source label.
		if (right >= ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + ImGui::GetStyle().ItemSpacing.x)
			ImGui::SameLine(right);
		ImGui::TextUnformatted(value.c_str());
	}
	const float target = std::max(metric == 0 ? 10.0f : 0.1f, float(plotMaximum) * 1.15f);
	float& scale = _profilerGraphMax[metric];
	scale = target >= scale ? target : std::max(target, scale * std::exp(-ImGui::GetIO().DeltaTime * 0.5f));
	const ImVec2 origin = ImGui::GetCursorScreenPos();
	const ImVec2 size(std::max(1.0f, ImGui::GetContentRegionAvail().x), 50.0f * _dpiScale);
	ImGui::InvisibleButton("history", size);
	ImDrawList* draw = ImGui::GetWindowDrawList();
	const ImVec2 end(origin.x + size.x, origin.y + size.y);
	draw->AddRectFilled(origin, end, ImGui::GetColorU32(ImGuiCol_FrameBg), 3.0f * _dpiScale);
	draw->PushClipRect(origin, end, true);
	auto position = [&](const auto& point, double value) {
		const float age = float(duration<double>(now - point.time).count());
		return ImVec2(end.x - std::clamp(age / 10.0f, 0.0f, 1.0f) * size.x,
			end.y - 2.0f * _dpiScale - float(value / scale) * (size.y - 4.0f * _dpiScale));
	};
	bool hasSamples = false;
	for (int i = 0; i < seriesCount; ++i) {
		const auto& s = series[i];
		hasSamples = hasSamples || s.summary.count != 0;
		const ImU32 line = ImGui::GetColorU32(s.color);
		const ImU32 peak = ImGui::GetColorU32(metric == 2 ? ImGuiCol_PlotLinesHovered : s.color, 0.55f);
		std::optional<ImVec2> previous;
		for (const auto& point : s.plot->points) {
			const auto value = values(point);
			if (!value.count) { previous.reset(); continue; }
			const auto mean = position(point, value.Mean());
			if (previous) draw->AddLine(*previous, mean, line, _dpiScale);
			else draw->AddCircleFilled(mean, _dpiScale, line);
			if (metric != 0) draw->AddLine(mean, position(point, value.maximum), peak, _dpiScale);
			previous = mean;
		}
	}
	const auto limit = fmt::format("{:.1f} {}", scale, unit);
	draw->AddText(ImVec2(origin.x + 4 * _dpiScale, origin.y + 2 * _dpiScale), ImGui::GetColorU32(ImGuiCol_TextDisabled), limit.c_str());
	if (!hasSamples) draw->AddText(ImVec2(origin.x + 4 * _dpiScale, origin.y + 22 * _dpiScale),
		ImGui::GetColorU32(ImGuiCol_TextDisabled), _GetResourceString(available ? L"Overlay_Profiler_WaitingSamples" : L"Overlay_Profiler_TimingUnavailable").c_str());
	draw->PopClipRect();
	if (ImGui::IsItemHovered()) {
		const float age = std::clamp((end.x - ImGui::GetIO().MousePos.x) / size.x, 0.0f, 1.0f) * 10.0f;
		std::string tooltip = fmt::format("-{:.1f} s\n{}", age, _GetResourceString(L"Overlay_Profiler_MeanPeak"));
		for (int i = 0; i < seriesCount; ++i) {
			const auto& s = series[i];
			const ProfilerHistory::Point* nearest = &s.plot->points.front();
			float distance = std::numeric_limits<float>::max();
			for (const auto& point : s.plot->points) {
				const float delta = std::abs(float(duration<double>(now - point.time).count()) - age);
				if (delta < distance) { distance = delta; nearest = &point; }
			}
			const auto value = values(*nearest);
			const auto sample = value.count ? fmt::format("{:.2f} / {:.2f} {}", value.Mean(), value.maximum, unit) : "--";
			tooltip += seriesCount > 1 ? fmt::format("\n{}: {}", s.label, sample) : "\n" + sample;
		}
		ImGui::SetTooltip("%s", tooltip.c_str());
	}
	for (int i = 0; i < seriesCount; ++i) {
		const auto& s = series[i];
		if (!s.summary.count) continue;
		const auto label = seriesCount > 1 ? fmt::format("{} {}", s.label, _GetResourceString(L"Overlay_Profiler_MeanPeak"))
			: _GetResourceString(L"Overlay_Profiler_MeanPeak");
		ImGui::TextDisabled("%s: %.2f / %.2f %s", label.c_str(), s.summary.Mean(), s.summary.maximum, unit);
	}
	if (metric == 0 && (plot.points.back().warming || _profilerOutputProfile.plot.points.back().warming))
		ImGui::TextDisabled("%s", _GetResourceString(L"Overlay_Profiler_Warming").c_str());
	if (metric == 1 && !_profilerOutputProfile.individualTimestamps)
		ImGui::TextDisabled("%s", _GetResourceString(L"Overlay_Profiler_OutputIntervalUnavailable").c_str());
	ImGui::Spacing();
	ImGui::PopTextWrapPos();
	ImGui::PopID();
}


bool OverlayDrawer::_BeginProfilerWindow() noexcept {
	const ImVec2 displaySize = ImGui::GetIO().DisplaySize;
	const float viewportMargin = 16.0f * _dpiScale;
	const ImVec2 maxWindowSize{
		std::max(1.0f, displaySize.x - viewportMargin),
		std::max(1.0f, displaySize.y - viewportMargin)
	};
	const ImVec2 minWindowSize{
		std::min(PROFILER_MIN_WIDTH * _dpiScale, maxWindowSize.x),
		std::min(PROFILER_MIN_HEIGHT * _dpiScale, maxWindowSize.y)
	};
	ImGui::SetNextWindowSizeConstraints(minWindowSize, maxWindowSize);
	OverlayWindowOption& windowOption = _overlayOptions->windows.at(PROFILER_WINDOW_ID);
	const bool restoreLayout = !_profilerWindowLayoutInitialized ||
		_profilerViewport.x != displaySize.x || _profilerViewport.y != displaySize.y;
	if (restoreLayout) {
		const auto rect = RestoreProfilerWindow(windowOption, displaySize.x, displaySize.y, _dpiScale);
		ImGui::SetNextWindowSize({ rect.width, rect.height });
		ImGui::SetNextWindowPos({ rect.x, rect.y });
		_profilerWindowLayoutInitialized = true;
		_profilerViewport = displaySize;
	}

	const std::string title = StrHelper::Concat(_GetResourceString(L"Overlay_Profiler"), "##", PROFILER_WINDOW_ID);
	ImGui::PushStyleColor(ImGuiCol_ResizeGrip, ImVec4(0.55f, 0.55f, 0.55f, 0.28f));
	ImGui::PushStyleColor(ImGuiCol_ResizeGripHovered, ImVec4(0.35f, 0.67f, 0.95f, 0.72f));
	ImGui::PushStyleColor(ImGuiCol_ResizeGripActive, ImVec4(0.35f, 0.67f, 0.95f, 1.0f));
	const bool expanded = ImGui::Begin(title.c_str(), &_isProfilerVisible);
	ImGui::PopStyleColor(3);
	ImGuiWindow* window = ImGui::GetCurrentWindow();
	const OverlayWindowRect rect{
		std::clamp(window->Pos.x, 0.0f, std::max(0.0f, displaySize.x - window->SizeFull.x)),
		std::clamp(window->Pos.y, 0.0f, std::max(0.0f, displaySize.y - window->SizeFull.y)),
		window->SizeFull.x, window->SizeFull.y
	};
	ImGui::SetWindowPos(window, { rect.x, rect.y });
	if (!restoreLayout && !_imguiImpl.FrameInputCanceled() &&
		(ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::IsMouseReleased(ImGuiMouseButton_Left))) {
		const auto& previous = _profilerWindowRect;
		const bool moved = rect.x != previous.x || rect.y != previous.y;
		const bool resizedX = rect.width != previous.width;
		const bool resizedY = rect.height != previous.height;
		if (moved || resizedX || resizedY) {
			RememberOverlayWindowPosition(windowOption, rect, displaySize.x, displaySize.y, _dpiScale);
			if (resizedX) windowOption.width = rect.width / _dpiScale;
			if (resizedY) windowOption.height = rect.height / _dpiScale;
			_profilerWindowLayoutDirty = true;
		}
	}
	_profilerWindowRect = rect;
	return expanded;
}

bool OverlayDrawer::_DrawProfilerGraphs(const EffectsProfilerSnapshot& profile) noexcept {
	ImGui::Spacing();
	const bool expanded = ImGui::TreeNodeEx("profilerGraphs", ImGuiTreeNodeFlags_SpanAvailWidth,
		"%s", _GetResourceString(L"Overlay_Profiler_Graphs").c_str());
	const bool toggled = ImGui::IsItemToggledOpen();
	if (expanded) {
		ImGui::TextDisabled("-10 s");
		ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - ImGui::CalcTextSize("0 s").x);
		ImGui::TextDisabled("0 s");
		const auto now = steady_clock::now();
		const auto plot = profile.history.Read(now);
		_profilerOutputProfile = ScalingWindow::Get().Renderer().PresentationProfile(now);
		if (plot.suspended) ImGui::TextUnformatted(_GetResourceString(L"Overlay_Profiler_Paused").c_str());
		_DrawProfilerGraph(plot, 0, profile.history.Active());
		_DrawProfilerGraph(plot, 1, profile.history.Active());
		_DrawProfilerGraph(plot, 2, profile.gpuAvailable);
		ImGui::TreePop();
	}
	return toggled;
}

bool OverlayDrawer::_DrawProfiler(const EffectsProfilerSnapshot& profile, uint32_t fps, int& itemId) noexcept {
	_lastProfilerDrawTime = steady_clock::now();
	const auto& effectTimings = profile.passTimings;
	_profilerTimingKnown = profile.gpuAvailable && !profile.history.Suspended() &&
		profile.sampleTime != ProfilerHistory::Time{} && _lastProfilerDrawTime - profile.sampleTime <= 1s;
	if (_profilerGeneration != profile.generation) {
		_profilerGeneration = profile.generation;
		_lastUpdateTime = {};
		for (auto& value : _effectTimingsStatistics) value = {};
		for (auto& value : _lastestAvgEffectTimings) value = 0;
		_profilerGraphMax = {};
		_profilerOutputProfile = {};
	}
	const ScalingOptions& options = ScalingWindow::Get().Options();
	const Renderer& renderer = ScalingWindow::Get().Renderer();

	const uint32_t passCount = (uint32_t)_effectTimingsStatistics.size();

	bool needRedraw = false;

	// Samples arrive asynchronously and are consumed once by the visible UI.
	// Ignore a stale chain's sample while effect descriptors are being updated.
	if (!effectTimings.empty() && effectTimings.size() == passCount) {
		steady_clock::time_point now = steady_clock::now();
		if (_lastUpdateTime == steady_clock::time_point{}) {
			// 后端渲染的第一帧
			_lastUpdateTime = now;

			for (uint32_t i = 0; i < passCount; ++i) {
				_lastestAvgEffectTimings[i] = effectTimings[i];
			}
		} else {
			if (now - _lastUpdateTime > 500ms) {
				// 更新间隔不少于 500ms，而不是 500ms 更新一次
				_lastUpdateTime = now;

				for (uint32_t i = 0; i < passCount; ++i) {
					auto& [total, count] = _effectTimingsStatistics[i];
					if (count > 0) {
						_lastestAvgEffectTimings[i] = total / count;
					}

					count = 0;
					total = 0;
				}
			}

			for (uint32_t i = 0; i < passCount; ++i) {
				auto& [total, count] = _effectTimingsStatistics[i];
				// 有时会跳过某些效果的渲染，即渲染时间为 0，这时不应计入
				if (effectTimings[i] > 1e-3) {
					++count;
					total += effectTimings[i];
				}
			}
		}
	}

	if (!_BeginProfilerWindow()) {
		ImGui::End();
		return needRedraw;
	}

	ImGui::PushTextWrapPos();
	ImGui::TextUnformatted(StrHelper::Concat("GPU: ", _hardwareInfo.gpuName).c_str());
	const std::string& captureMethodStr = _GetResourceString(L"Overlay_Profiler_CaptureMethod");
	ImGui::TextUnformatted(StrHelper::Concat(captureMethodStr.c_str(), ": ", renderer.FrameSource().Name()).c_str());
	if (options.IsStatisticsForDynamicDetectionEnabled() &&
		options.duplicateFrameDetectionMode == DuplicateFrameDetectionMode::Dynamic) {
		const std::pair<uint32_t, uint32_t> statistics =
			renderer.FrameSource().GetStatisticsForDynamicDetection();
		ImGui::TextUnformatted(StrHelper::Concat(_GetResourceString(L"Overlay_Profiler_DynamicDetection"), ": ").c_str());
		ImGui::SameLine(0, 0);
		ImGui::PushFont(_fontMonoNumbers);
		ImGui::TextUnformatted(fmt::format("{}/{} ({:.1f}%)", statistics.first, statistics.second,
			statistics.second == 0 ? 0.0f : statistics.first * 100.0f / statistics.second).c_str());
		ImGui::PopFont();
	}
	const auto rate = renderer.PresentationRate();
	const bool submittedRate = rate.source == ContentFrameCountSource::Submitted;

	const std::string& frameRateStr = _GetResourceString(renderer.HasFrameGeneration() ?
		(submittedRate ? L"Overlay_FG_SubmittedRate" : L"Overlay_Profiler_FrameRateWithFG") : L"Overlay_Profiler_FrameRate");
	ImGui::TextUnformatted(fmt::format("{}: {}", frameRateStr, _FormatFrameRate(fps)).c_str());
	if (renderer.HasFrameGeneration()) {
		ImGui::TextUnformatted(fmt::format("{}: {}", _GetResourceString(submittedRate ?
			L"Overlay_FG_SubmittedGeneratedRate" : L"Overlay_FG_GeneratedRate"),
			rate.totalKnown ? std::to_string(rate.generated) : "--").c_str());
		ImGui::TextUnformatted(fmt::format("{}: {}", _GetResourceString(L"Overlay_FG_IdleSubmissions"),
			renderer.PresentationRate().idleSubmissions).c_str());
		if (renderer.IsWaitingForNewContent()) ImGui::TextUnformatted(_GetResourceString(L"Overlay_FG_Waiting").c_str());
	}
	ImGui::PopTextWrapPos();

	const std::vector<const EffectDesc*>& effectDescs = renderer.ActiveEffectDescs();
	const uint32_t nEffect = (uint32_t)effectDescs.size();

	SmallVector<_EffectDrawInfo, 4> effectDrawInfos(effectDescs.size());
	bool hasUnmeasuredFG = false;

	{
		uint32_t idx = 0;
		for (uint32_t i = 0; i < nEffect; ++i) {
			_EffectDrawInfo& drawInfo = effectDrawInfos[i];
			drawInfo.desc = effectDescs[i];
			drawInfo.label = GetEffectDisplayName(*drawInfo.desc);
			const auto fg = ClassifyFrameGenerationEffect(drawInfo.desc->name);
			if (fg == FrameGenerationEffectKind::FSR3 || fg == FrameGenerationEffectKind::XeSSX2 ||
				fg == FrameGenerationEffectKind::XeSSMultiFrame) {
				hasUnmeasuredFG = true;
				drawInfo.label += " (" + _GetResourceString(L"Overlay_FG_InputCopyTiming") + ")";
			}

			uint32_t nPass = (uint32_t)drawInfo.desc->passes.size();
			drawInfo.passTimings = { _lastestAvgEffectTimings.begin() + idx, nPass };
			idx += nPass;

			for (float t : drawInfo.passTimings) {
				drawInfo.totalTime += t;
			}
		}
	}

	static bool showPasses = false;

	// 开发者选项
	const bool hasDeveloperContent = options.IsDeveloperMode() && std::ranges::any_of(
		effectDrawInfos, [](const auto& effect) { return effect.passTimings.size() > 1; });
	if (!hasDeveloperContent) showPasses = false;
	if (hasDeveloperContent) {
		ImGui::Spacing();
		const std::string& developerOptionsStr = _GetResourceString(L"Home_Advanced_DeveloperOptions/Header");
		if (ImGui::CollapsingHeader(developerOptionsStr.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
			const std::string& buttonStr = _GetResourceString(showPasses
				? L"Overlay_Profiler_Timings_SwitchToEffects"
				: L"Overlay_Profiler_Timings_SwitchToPasses");
			if (ImGui::Button(buttonStr.c_str())) {
				showPasses = !showPasses;
				// 需要再次渲染以处理滚动条导致的布局变化
				needRedraw = true;
			}
		}
	}

#ifdef MP_DEBUG_INFO_ON_OVERLAY
	ImGui::Spacing();
	if (ImGui::CollapsingHeader("调试信息", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::TextUnformatted(StrHelper::Concat("源矩形: ",
			RectToStr(renderer.SrcRect())).c_str());
		ImGui::TextUnformatted(StrHelper::Concat("目标矩形: ",
			RectToStr(renderer.DestRect())).c_str());
		ImGui::TextUnformatted(StrHelper::Concat("渲染矩形: ",
			RectToStr(ScalingWindow::Get().RendererRect())).c_str());
		RECT scalingWndRect;
		GetWindowRect(ScalingWindow::Get().Handle(), &scalingWndRect);
		ImGui::TextUnformatted(StrHelper::Concat("缩放窗口矩形: ",
			RectToStr(scalingWndRect)).c_str());

		bool isTopMost = GetWindowExStyle(ScalingWindow::Get().Handle()) & WS_EX_TOPMOST;
		ImGui::TextUnformatted(
			StrHelper::Concat("缩放窗口置顶: ", isTopMost ? "是" : "否").c_str());

		ImGui::TextUnformatted(StrHelper::Concat("已捕获光标: ",
			ScalingWindow::Get().CursorManager().IsCursorCaptured() ? "是" : "否").c_str());

		RECT cursorClip;
		GetClipCursor(&cursorClip);
		ImGui::TextUnformatted(StrHelper::Concat("光标限制区域: ",
			RectToStr(cursorClip)).c_str());
	}
#endif

	ImGui::Spacing();
	// 效果渲染用时
	const std::string& timingsStr = _GetResourceString(L"Overlay_Profiler_Timings");
	if (ImGui::CollapsingHeader(timingsStr.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
		if (!profile.gpuAvailable) ImGui::TextDisabled("%s", _GetResourceString(L"Overlay_Profiler_TimingUnavailable").c_str());
		if (profile.partial) {
			ImGui::TextDisabled("%s", _GetResourceString(L"Overlay_Profiler_PartialTiming").c_str());
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", _GetResourceString(L"Overlay_Profiler_PartialTimingHelp").c_str());
		}
		if (hasUnmeasuredFG) ImGui::TextWrapped("%s", _GetResourceString(L"Overlay_FG_UnmeasuredTiming").c_str());
		float effectsTotalTime = 0.0f;
		for (const _EffectDrawInfo& drawInfo : effectDrawInfos) {
			effectsTotalTime += drawInfo.totalTime;
		}

		SmallVector<ImColor, 4> colors;
		colors.reserve(_timelineColors.size());
		if (nEffect == 1) {
			colors.resize(_timelineColors.size());
			for (size_t i = 0; i < _timelineColors.size(); ++i) {
				colors[i] = OverlayHelper::TIMELINE_COLORS[_timelineColors[i]];
			}
		} else if (showPasses) {
			uint32_t i = 0;
			for (const _EffectDrawInfo& drawInfo : effectDrawInfos) {
				if (drawInfo.passTimings.size() == 1) {
					colors.push_back(OverlayHelper::TIMELINE_COLORS[_timelineColors[i]]);
					++i;
					continue;
				}

				++i;
				for (uint32_t j = 0; j < drawInfo.passTimings.size(); ++j) {
					colors.push_back(OverlayHelper::TIMELINE_COLORS[_timelineColors[i]]);
					++i;
				}
			}
		} else {
			size_t i = 0;
			for (const _EffectDrawInfo& drawInfo : effectDrawInfos) {
				colors.push_back(OverlayHelper::TIMELINE_COLORS[_timelineColors[i]]);

				++i;
				if (drawInfo.passTimings.size() > 1) {
					i += drawInfo.passTimings.size();
				}
			}
		}

		static int selectedIdx = -1;

		if (nEffect > 1 || showPasses) {
			ImGui::Spacing();
			ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(0, 0));
			ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.5f, 0.5f));
			ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(5, 5));
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));

			if (effectsTotalTime > 0) {
				if (showPasses) {
					if (ImGui::BeginTable("timeline", (int)passCount)) {
						for (uint32_t i = 0; i < passCount; ++i) {
							if (_lastestAvgEffectTimings[i] < 1e-3f) {
								continue;
							}

							ImGui::TableSetupColumn(
								std::to_string(i).c_str(),
								ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoResize | ImGuiTableColumnFlags_NoReorder,
								_lastestAvgEffectTimings[i] / effectsTotalTime
							);
						}

						ImGui::TableNextRow();

						uint32_t i = 0;
						for (const _EffectDrawInfo& drawInfo : effectDrawInfos) {
							for (uint32_t j = 0, end = (uint32_t)drawInfo.passTimings.size(); j < end; ++j) {
								if (drawInfo.passTimings[j] < FLOAT_EPSILON<float>) {
									continue;
								}

								ImGui::TableNextColumn();

								std::string name;
								if (drawInfo.passTimings.size() == 1) {
									name = std::string(GetEffectDisplayName(*drawInfo.desc));
								} else if (nEffect == 1) {
									name = drawInfo.desc->passes[j].desc;
								} else {
									name = StrHelper::Concat(
										GetEffectDisplayName(*drawInfo.desc), "/",
										drawInfo.desc->passes[j].desc
									);
								}

								_DrawTimelineItem(itemId, colors[i], _dpiScale, name, drawInfo.passTimings[j],
									effectsTotalTime, selectedIdx == (int)i);

								++i;
							}
						}

						ImGui::EndTable();
					}
				} else {
					if (ImGui::BeginTable("timeline", nEffect)) {
						for (uint32_t i = 0; i < nEffect; ++i) {
							if (effectDrawInfos[i].totalTime < FLOAT_EPSILON<float>) {
								continue;
							}

							ImGui::TableSetupColumn(
								std::to_string(i).c_str(),
								ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoResize | ImGuiTableColumnFlags_NoReorder,
								effectDrawInfos[i].totalTime / effectsTotalTime
							);
						}

						ImGui::TableNextRow();

						for (uint32_t i = 0; i < nEffect; ++i) {
							auto& drawInfo = effectDrawInfos[i];
							if (drawInfo.totalTime < FLOAT_EPSILON<float>) {
								continue;
							}

							ImGui::TableNextColumn();
							_DrawTimelineItem(
								itemId,
								colors[i],
								_dpiScale,
								drawInfo.label,
								drawInfo.totalTime,
								effectsTotalTime,
								selectedIdx == (int)i
							);
						}

						ImGui::EndTable();
					}
				}
			} else {
				// 还未统计出时间时渲染占位
				if (ImGui::BeginTable("timeline", 1)) {
					ImGui::TableSetupColumn("0", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoResize | ImGuiTableColumnFlags_NoReorder);
					ImGui::TableNextRow();
					ImGui::TableNextColumn();

					ImU32 color = ImColor();
					ImGui::PushStyleColor(ImGuiCol_HeaderActive, color);
					ImGui::PushStyleColor(ImGuiCol_HeaderHovered, color);
					ImGui::Selectable("");
					ImGui::PopStyleColor(2);

					ImGui::EndTable();
				}
			}

			ImGui::PopStyleVar(4);

			ImGui::Spacing();
		}

		selectedIdx = -1;

		if (ImGui::BeginTable("timings", 1, ImGuiTableFlags_PadOuterX)) {
			ImGui::TableSetupColumn(nullptr, ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoResize | ImGuiTableColumnFlags_NoReorder);

			if (nEffect == 1) {
				int hovered = _DrawEffectTimings(itemId, effectDrawInfos[0], showPasses, colors, true);
				if (hovered >= 0) {
					selectedIdx = hovered;
				}
			} else {
				int idx = 0;
				for (const _EffectDrawInfo& effectDesc : effectDrawInfos) {
					int idxBegin = idx;

					std::span<const ImColor> colorSpan;
					if (!showPasses || effectDesc.passTimings.size() == 1) {
						colorSpan = std::span(colors.begin() + idx, colors.begin() + idx + 1);
						++idx;
					} else {
						colorSpan = std::span(colors.begin() + idx, colors.begin() + idx + effectDesc.passTimings.size());
						idx += (int)effectDesc.passTimings.size();
					}

					int hovered = _DrawEffectTimings(itemId, effectDesc, showPasses, colorSpan, false);
					if (hovered >= 0) {
						selectedIdx = idxBegin + hovered;
					}
				}
			}

			ImGui::EndTable();
		}

		if (nEffect > 1) {
			ImGui::Separator();

			if (ImGui::BeginTable("total", 1, ImGuiTableFlags_PadOuterX)) {
				ImGui::TableSetupColumn(nullptr, ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoResize | ImGuiTableColumnFlags_NoReorder);

				_DrawTimingItem(itemId, _GetResourceString(hasUnmeasuredFG ? L"Overlay_FG_MeasuredTotal" : L"Overlay_Profiler_Timings_Total").c_str(), nullptr, effectsTotalTime);

				ImGui::EndTable();
			}
		}
		if (_DrawProfilerGraphs(profile)) needRedraw = true;
	}

	ImGui::End();
	return needRedraw;
}

const std::string& OverlayDrawer::_GetResourceString(const std::wstring_view& key) noexcept {
	static phmap::flat_hash_map<std::wstring_view, std::string> cache;

	if (auto it = cache.find(key); it != cache.end()) {
		return it->second;
	}

	return cache[key] = StrHelper::UTF16ToUTF8(ScalingWindow::Get().GetLocalizedString(key));
}

float OverlayDrawer::_CalcToolbarAlpha() const noexcept {
	if (ScalingWindow::Get().IsResizingOrMoving()) {
		// 调整缩放窗口大小时不能按需渲染，所以不要改变工具栏透明度
		return _lastToolbarAlpha;
	}

	// 鼠标被工具栏中的按钮捕获时不要隐藏工具栏
	if (_isToolbarPinned || _isToolbarItemActive || _toolbarPlacement.IsDragging()) {
		return 1.0f;
	}

	std::optional<ImVec4> windowRect = _imguiImpl.GetWindowRect(TOOLBAR_WINDOW_ID);
	if (!windowRect) {
		return 0.0f;
	}

	// Both docks clip the outer rounded edge. Use only the visible rectangle
	// for reveal distance, rather than extending the bottom hit zone offscreen.
	windowRect->y = std::max(0.0f, windowRect->y);
	windowRect->w = std::min(ImGui::GetIO().DisplaySize.y, windowRect->w);

	// ImGui::GetIO().MousePos 在调整缩放窗口大小或鼠标被前台窗口捕获时不是真实位置，这里应重新计算
	const POINT cursorPos = ScalingWindow::Get().CursorManager().CursorPos();
	const RECT& destRect = ScalingWindow::Get().Renderer().DestRect();
	const float cursorX = float(cursorPos.x - destRect.left);
	const float cursorY = float(cursorPos.y - destRect.top);

	// 计算离边或角最短的距离
	float dist = 0;
	if (cursorX < windowRect->x) {
		if (cursorY < windowRect->y) {
			dist = std::hypot(windowRect->x - cursorX, windowRect->y - cursorY);
		} else if (cursorY > windowRect->w) {
			dist = std::hypot(windowRect->x - cursorX, cursorY - windowRect->w);
		} else {
			dist = windowRect->x - cursorX;
		}
	} else if (cursorX > windowRect->z) {
		if (cursorY < windowRect->y) {
			dist = std::hypot(cursorX - windowRect->z, windowRect->y - cursorY);
		} else if (cursorY > windowRect->w) {
			dist = std::hypot(cursorX - windowRect->z, cursorY - windowRect->w);
		} else {
			dist = cursorX - windowRect->z;
		}
	} else {
		if (cursorY < windowRect->y) {
			dist = windowRect->y - cursorY;
		} else if (cursorY > windowRect->w) {
			dist = cursorY - windowRect->w;
		} else {
			dist = 0;
		}
	}
	dist /= _dpiScale;

	return (40.0f - std::clamp(dist - 10.0f, 0.0f, 40.0f)) / 40.0f;
}

void OverlayDrawer::_ClearStatesIfNoVisibleWindow() noexcept {
	if (AnyVisibleWindow()) {
		return;
	}

	ClearStates();
}

}
