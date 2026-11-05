// Included only by an isolated, portable test App.cpp. Uses the product XBF,
// compiled x:Bind callbacks and ViewModel, without starting a scaling session.
#include "ScalingModesPage.h"
#include "ScalingMode.h"
#include "ScalingModesService.h"
#include "EffectParametersViewModel.h"
#include "EffectDesc.h"
#include "ErrorService.h"
#include "DLSSNRPerformanceParameters.h"
#include "LocalizationService.h"
#include <winrt/Windows.UI.Xaml.Input.h>

namespace winrt::Magpie::implementation {
using namespace ::Magpie;

template<class F> void VisitChoiceProbeTree(const DependencyObject& node, const F& visit) {
    visit(node);
    const int count = VisualTreeHelper::GetChildrenCount(node);
    for (int i = 0; i < count; ++i) VisitChoiceProbeTree(VisualTreeHelper::GetChild(node, i), visit);
}

static void RequireChoiceProbe(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

static void CheckChoiceProbeLocalization(const ComboBox& combo, std::string_view name) {
    const std::wstring_view language(LocalizationService::Get().Language());
    const auto parameter = combo.DataContext().as<winrt::Magpie::ScalingModeParameter>();
    const bool interval = name == "sparseUpdateInterval";
    std::vector<hstring> expected;
    hstring expectedLabel, expectedTooltip;
    if (language == L"zh-hans") {
        if (interval) {
            expected = {L"每 2 帧", L"每 3 帧", L"每 4 帧"};
            expectedLabel = L"NR 更新间隔";
            expectedTooltip = L"隔帧每 2、3 或 4 个新捕获帧更新一次完整 NR，默认每 2 帧。重复刷新、重绘和生成帧不计数。间隔越长，推理工作越少，历史修正也越旧。";
        }
        else {
            expected = {L"逐帧", L"隔帧"};
            expectedLabel = L"处理模式";
            expectedTooltip = L"逐帧完整处理每个新捕获帧；隔帧按 NR 更新间隔完整计算，并连续复用历史增强。";
        }
    }
    else if (language == L"zh-hant") {
        if (interval) {
            expected = {L"每 2 幀", L"每 3 幀", L"每 4 幀"};
            expectedLabel = L"NR 更新間隔";
            expectedTooltip = L"隔幀每 2、3 或 4 個新擷取幀更新一次完整 NR，預設每 2 幀。重複重新整理、重繪和生成幀不計數。間隔越長，推論工作越少，歷史修正也越舊。";
        }
        else {
            expected = {L"逐幀", L"隔幀"};
            expectedLabel = L"處理模式";
            expectedTooltip = L"逐幀完整處理每個新擷取幀；隔幀按 NR 更新間隔完整計算，並連續複用歷史增強。";
        }
    }
    else if (language == L"en-us") {
        if (interval) {
            expected = {L"Every 2 Frames", L"Every 3 Frames", L"Every 4 Frames"};
            expectedLabel = L"NR Update Interval";
            expectedTooltip = L"Alternating Frames updates full NR every 2, 3 or 4 new captures, defaulting to 2. Repeated refreshes, redraws and generated frames do not advance the interval. Longer intervals reduce inference work and use older corrections.";
        }
        else {
            expected = {L"Every Frame", L"Alternating Frames"};
            expectedLabel = L"Processing Mode";
            expectedTooltip = L"Every Frame fully processes each new capture. Alternating Frames updates full NR at the selected interval and continuously reuses historical enhancement.";
        }
    }
    else throw std::runtime_error("Unexpected probe language");
    const uint32_t count = static_cast<uint32_t>(expected.size());
    RequireChoiceProbe(parameter.Label() == expectedLabel && parameter.Tooltip() == expectedTooltip,
        "Choice label or tooltip fell back instead of using the requested language");
    RequireChoiceProbe(parameter.IsChoice() && !parameter.IsBoolean() &&
        parameter.Choices().Size() == count && combo.Items().Size() == count,
        "Choice picker has the wrong type or option count");
    for (uint32_t i = 0; i < count; ++i) {
        RequireChoiceProbe(parameter.Choices().GetAt(i) == expected[i] &&
            unbox_value<hstring>(combo.Items().GetAt(i)) == expected[i],
            "Choice option label fell back instead of using the requested language");
    }
    const auto row = combo.Parent();
    row.as<FrameworkElement>().UpdateLayout();
    bool visibleLabel = false;
    VisitChoiceProbeTree(row, [&](const auto& node) {
        const auto label = node.template try_as<TextBlock>();
        if (label && label.Text() == expectedLabel && label.ActualWidth() > 0 && label.ActualHeight() > 0)
            visibleLabel = true;
    });
    RequireChoiceProbe(visibleLabel, "Localized choice label has no visible row content");
    const auto tooltip = ToolTipService::GetToolTip(combo).try_as<ToolTip>();
    const auto tooltipText = tooltip ? tooltip.Content().try_as<TextBlock>() : nullptr;
    RequireChoiceProbe(tooltipText && tooltipText.Text() == expectedTooltip,
        "Actual choice tooltip differs from the localized short help");
    Logger::Get().Info(fmt::format("{} PASS: language={} choices={} label={} tooltip={}",
        interval ? "IntervalLocalization" : "ProcessingLocalization",
        to_string(hstring(language)), count, to_string(parameter.Label()), to_string(parameter.Tooltip())));
}

// Read the actual product-template content: layout-only fixtures replace
// bindings with literal labels and cannot detect an empty runtime Binding.
static void CheckChoiceProbeLabels(const DependencyObject& root) {
    unsigned companions = 0, choiceTooltips = 0;
    VisitChoiceProbeTree(root, [&](const auto& node) {
        const auto element = node.template try_as<FrameworkElement>();
        const auto parameter = element ? element.DataContext().template try_as<winrt::Magpie::ScalingModeParameter>() : nullptr;
        if (!parameter) return;
        const auto checkbox = node.template try_as<CheckBox>();
        hstring expectedTooltip;
        if (checkbox && checkbox.Name() == L"ParameterCompanion") {
            const auto companion = parameter.Companion();
            const auto label = checkbox.Content().template try_as<TextBlock>();
            RequireChoiceProbe(companion && !companion.Label().empty() && label && label.Text() == companion.Label(),
                "Paired checkbox label is empty or differs from Companion.Label");
            RequireChoiceProbe(label.ActualWidth() > 0 && label.ActualHeight() > 0,
                "Paired checkbox label has no visible layout extent");
            const auto row = checkbox.Parent().template as<Grid>();
            const auto position = checkbox.TransformToVisual(row).TransformPoint({0, 0});
            RequireChoiceProbe(position.X >= 0 && position.X + checkbox.ActualWidth() <= row.ActualWidth() + .5,
                "Paired checkbox label extends beyond its parameter row");
            expectedTooltip = companion.Tooltip();
            ++companions;
        } else if (node.template try_as<ComboBox>()) {
            expectedTooltip = parameter.Tooltip();
            ++choiceTooltips;
        } else {
            return;
        }
        const auto tooltip = ToolTipService::GetToolTip(element).template try_as<ToolTip>();
        const auto tooltipText = tooltip ? tooltip.Content().template try_as<TextBlock>() : nullptr;
        RequireChoiceProbe(tooltipText && !expectedTooltip.empty() && tooltipText.Text() == expectedTooltip,
            "Parameter tooltip is empty or differs from its bound model");
    });
    RequireChoiceProbe(companions == 4 && choiceTooltips > 0,
        "Expected advanced toggle and all three pass companions with choice tooltips");
}

template<class Predicate>
static Windows::Foundation::IAsyncAction WaitForChoiceProbe(Predicate ready, const char* description) {
    const auto dispatcher = App::Get().Dispatcher();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!ready()) {
        if (std::chrono::steady_clock::now() >= deadline)
            throw hresult_error(E_FAIL, to_hstring(std::string("Probe timed out waiting for ") + description));
        co_await resume_after(std::chrono::milliseconds(10));
        co_await dispatcher;
    }
}

struct ChoiceProbePopupState {
    bool open = false, closing = false, expectedClose = false;
    unsigned opened = 0, closed = 0, unexpectedClosings = 0;
    const char* phase = "initial";
};

static void TraceChoiceProbePopup(const char* event, const ChoiceProbePopupState& state) {
    DWORD foregroundProcess = 0;
    const HWND foreground = GetForegroundWindow();
    GetWindowThreadProcessId(foreground, &foregroundProcess);
    Logger::Get().Info(fmt::format(
        "ChoiceProbePopup event={} phase={} open={} closing={} expectedClose={} opened={} closed={} foregroundPid={} foreground=0x{:x} focus=0x{:x} leftDown={} escapeDown={}",
        event, state.phase, state.open, state.closing, state.expectedClose, state.opened, state.closed,
        foregroundProcess, uintptr_t(foreground), uintptr_t(GetFocus()),
        (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0, (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0));
}

static Windows::Foundation::IAsyncAction OpenChoiceProbePopup(Controls::Flyout parent, Button anchor,
    std::shared_ptr<ChoiceProbePopupState> state) {
    RequireChoiceProbe(!state->open && !state->closing, "Probe tried to reopen before Closed");
    const unsigned opened = state->opened, closed = state->closed;
    state->phase = "show"; state->expectedClose = false;
    TraceChoiceProbePopup("ShowAt", *state);
    parent.ShowAt(anchor);
    co_await WaitForChoiceProbe([&] {
        RequireChoiceProbe(state->closed == closed, "Flyout closed before Opened");
        return state->opened > opened;
    }, "parent Opened");
    state->phase = "realize";
    co_await WaitForChoiceProbe([&] {
        RequireChoiceProbe(state->closed == closed && !state->closing, "Flyout closed during control realization");
        if (!state->open) return false;
        parent.Content().as<FrameworkElement>().UpdateLayout();
        unsigned choices = 0, sliders = 0;
        VisitChoiceProbeTree(parent.Content().as<DependencyObject>(), [&](const auto& node) {
            if (node.template try_as<ComboBox>()) ++choices;
            if (node.template try_as<Slider>()) ++sliders;
        });
        return choices > 0 && sliders > 0;
    }, "actual choice and slider realization");
    state->phase = "inspect";
}

static Windows::Foundation::IAsyncAction CloseChoiceProbePopup(Controls::Flyout parent,
    std::shared_ptr<ChoiceProbePopupState> state) {
    state->phase = "hide"; state->expectedClose = true;
    if (state->open || state->closing) {
        const unsigned closed = state->closed;
        TraceChoiceProbePopup("Hide", *state);
        if (!state->closing) parent.Hide();
        co_await WaitForChoiceProbe([&] { return state->closed > closed; }, "parent Closed");
    }
    state->expectedClose = false;
    RequireChoiceProbe(!state->open && !state->closing, "Flyout did not finish closing");
}

static void CheckChoiceSetterIsolation() {
    const std::vector<std::pair<int, hstring>> choices{{0,L"0"},{1,L"1"},{2,L"2"},{3,L"3"},{4,L"4"},{5,L"5"}};
    for (const double value : {0.0,1.0,2.0,3.0,4.0,5.0,99.0,1.5}) {
        const auto model = make_self<ScalingModeParameter>(0, L"choice", value, choices);
        unsigned changes = 0;
        model->PropertyChanged([&](auto&&, auto&&) { ++changes; });
        model->BooleanValue(model->BooleanValue());
        model->Value(1); model->SliderValue(1); model->ChoiceIndex(-1); model->ChoiceValue(99);
        RequireChoiceProbe(model->Value() == value && changes == 0, "Wrong-type callback changed choice/draft");
        model->ChoiceValue(2);
        RequireChoiceProbe(model->ChoiceValue() == 2, "Valid choice edit rejected");
        model->IsEnabled(false); changes = 0;
        model->ChoiceValue(1); model->ChoiceIndex(1); model->BooleanValue(true); model->SliderValue(0);
        RequireChoiceProbe(model->ChoiceValue() == 2 && changes == 0, "Disabled choice accepted an edit");
    }
    const std::vector<std::pair<int, hstring>> intervals{{2,L"2"},{3,L"3"},{4,L"4"}};
    const auto interval = make_self<ScalingModeParameter>(0, L"sparseUpdateInterval", 2, intervals);
    for (int index : {0,1,2}) {
        interval->ChoiceIndex(index);
        RequireChoiceProbe(interval->ChoiceValue() == index + 2 && interval->ChoiceIndex() == index,
            "Interval dropdown index was used as its stored capture count");
    }
    const auto number = make_self<ScalingModeParameter>(0, L"numeric", .5f, 0.f, 2.f, .1f, 1.f);
    unsigned changes = 0;
    number->PropertyChanged([&](auto&&, auto&&) { ++changes; });
    number->BooleanValue(true); number->ChoiceValue(1); number->ChoiceIndex(0);
    RequireChoiceProbe(number->Value() == .5 && changes == 0, "Wrong-type callback changed numeric value");
    number->SliderValue(.75);
    RequireChoiceProbe(number->Value() == .75, "Numeric slider edit rejected");
    const auto boolean = make_self<ScalingModeParameter>(0, L"boolean", true);
    changes = 0;
    boolean->PropertyChanged([&](auto&&, auto&&) { ++changes; });
    boolean->BooleanValue(true); boolean->Value(.5); boolean->SliderValue(0); boolean->ChoiceValue(0);
    RequireChoiceProbe(boolean->Value() == 1 && changes == 0, "Wrong-type/unchanged callback changed Boolean");
    boolean->BooleanValue(false);
    RequireChoiceProbe(boolean->Value() == 0 && changes == 1, "Boolean edit did not publish exactly once");
}

static fire_and_forget RunParameterFlyoutAppProbe() {
    const auto dispatcher = App::Get().Dispatcher();
    bool passed = false;
    try {
        CheckChoiceSetterIsolation();
        const auto* effectInfo = EffectsService::Get().GetEffect(L"DLSSNR\\DLSSNR_AI_Filter");
        RequireChoiceProbe(effectInfo != nullptr, "DLSSNR metadata is unavailable");
        const auto& info = *effectInfo;
        auto values = []() -> auto& { return AppSettings::Get().ScalingModes().at(0).effects.at(0).parameters; };
        RequireChoiceProbe(!values().contains(L"opticalFlowScaleOrder"), "Loader retained retired optical-flow order");
        RequireChoiceProbe(values().at(L"modelPreset") == 2 && values().at(L"processingMode") == 0 &&
            values().at(L"__dlssnrModeVersion") == DLSSNR_MODE_VERSION && values().at(L"sparseUpdateInterval") == 4 &&
            values().at(L"resolutionControl") == 1 && values().at(L"inputResolutionPercent") == 75,
            "Legacy asynchronous mode did not migrate to Every Frame or changed independent settings");
        for (const auto name : {L"residualLowFrequencyGain", L"style", L"pass2_style", L"pass3_style"})
            RequireChoiceProbe(values().at(name) > 1.f, "Loaded numeric reopen fixture lost its value above one");
        RequireChoiceProbe(std::ranges::none_of(info.params, [](const auto& parameter) {
            return parameter.name == "opticalFlowScaleOrder";
        }), "Retired optical-flow order is still in product metadata");
        const auto initial = values();
        auto edits = std::make_shared<unsigned>(0);
        auto revoker = ScalingModesService::Get().EffectParametersChanged(auto_revoke,
            [edits](uint32_t, uint32_t) { ++*edits; });
        co_await WaitForChoiceProbe([] {
            return App::Get().RootPage().as<FrameworkElement>().IsLoaded();
        }, "root page Loaded");
        const HWND host = App::Get().MainWindow().Handle();
        const bool initiallyVisible = IsWindowVisible(host) != FALSE;
        if (!initiallyVisible) {
            // The harness starts hidden. The first ShowWindow may consume
            // STARTUPINFO's SW_HIDE; explicitly show only this test host.
            ShowWindow(host, SW_SHOWNORMAL);
            if (!IsWindowVisible(host)) ShowWindow(host, SW_SHOWNORMAL);
        }
        App::Get().MainWindow().Show();
        Logger::Get().Info(fmt::format("ChoiceProbeHost initiallyVisible={} shown={}",
            initiallyVisible, IsWindowVisible(host) != FALSE));
        co_await WaitForChoiceProbe([] {
            return IsWindowVisible(App::Get().MainWindow().Handle()) != FALSE;
        }, "own host visibility");
        App::Get().RootPage()->NavigateToScalingModes();
        co_await WaitForChoiceProbe([] {
            bool realized = false;
            VisitChoiceProbeTree(App::Get().RootPage().as<DependencyObject>(), [&](const auto& node) {
                if (auto expander = node.template try_as<winrt::Magpie::SettingsExpander>()) {
                    realized = true; expander.IsExpanded(true);
                }
            });
            return realized;
        }, "scaling-mode expander realization");
        Button anchor{nullptr}; Controls::Flyout parent{nullptr};
        co_await WaitForChoiceProbe([&] {
            VisitChoiceProbeTree(App::Get().RootPage().as<DependencyObject>(), [&](const auto& node) {
                const auto button = node.template try_as<Button>();
                const auto flyout = button ? button.Flyout().template try_as<Controls::Flyout>() : nullptr;
                const auto content = flyout ? flyout.Content().template try_as<ContentControl>() : nullptr;
                if (content && content.Content().template try_as<winrt::Magpie::EffectParametersViewModel>()) {
                    anchor = button; parent = flyout;
                }
            });
            return anchor && anchor.IsLoaded() && anchor.ActualWidth() > 0;
        }, "actual parameter anchor realization");
        RequireChoiceProbe(bool(parent), "Actual parameter anchor was not realized");
        const auto popup = std::make_shared<ChoiceProbePopupState>();
        auto openedRevoker = parent.Opened(auto_revoke, [popup](auto&&, auto&&) {
            popup->open = true; popup->closing = false; ++popup->opened;
            TraceChoiceProbePopup("Opened", *popup);
        });
        auto closingRevoker = parent.Closing(auto_revoke, [popup](auto&&, auto&&) {
            popup->closing = true;
            if (!popup->expectedClose) ++popup->unexpectedClosings;
            TraceChoiceProbePopup("Closing", *popup);
        });
        auto closedRevoker = parent.Closed(auto_revoke, [popup](auto&&, auto&&) {
            popup->open = popup->closing = false; ++popup->closed;
            TraceChoiceProbePopup("Closed", *popup);
        });
        auto findControl = [&](const std::string_view name, auto tag) {
            decltype(tag) found{nullptr};
            VisitChoiceProbeTree(parent.Content().as<DependencyObject>(), [&](const auto& node) {
                const auto control = node.template try_as<decltype(tag)>();
                if (!control) return;
                const auto parameter = control.DataContext().template try_as<winrt::Magpie::ScalingModeParameter>();
                if (parameter && info.params.at(get_self<ScalingModeParameter>(parameter)->Index()).name == name) found = control;
            });
            return found;
        };
        auto findPerformanceToggle = [&] {
            CheckBox advanced{nullptr};
            VisitChoiceProbeTree(parent.Content().as<DependencyObject>(), [&](const auto& node) {
                const auto checkbox = node.template try_as<CheckBox>();
                if (checkbox && checkbox.Name() == L"ParameterCompanion") {
                    const auto model = checkbox.DataContext().template try_as<winrt::Magpie::ScalingModeParameter>();
                    if (model && info.params.at(get_self<ScalingModeParameter>(model)->Index()).name == "residualShowAdvanced") advanced = checkbox;
                }
            });
            RequireChoiceProbe(bool(advanced), "Performance advanced checkbox was not realized");
            return advanced;
        };
        auto checkPerformanceOrder = [&] {
            std::vector<std::string_view> order;
            VisitChoiceProbeTree(parent.Content().as<DependencyObject>(), [&](const auto& node) {
                if (!node.template try_as<ComboBox>() && !node.template try_as<Slider>()) return;
                const auto control = node.template as<FrameworkElement>();
                const auto parameter = control.DataContext().template try_as<winrt::Magpie::ScalingModeParameter>();
                if (!parameter || !parameter.IsVisible()) return;
                const auto& name = info.params.at(get_self<ScalingModeParameter>(parameter)->Index()).name;
                if (IsDLSSNRPerformanceParameter(name)) order.push_back(name);
            });
            std::vector<std::string_view> expected{"resolutionControl", "inputResolutionPercent",
                "processingMode"};
            const auto mode = values().at(L"processingMode");
            if (mode == 1) expected.push_back("sparseUpdateInterval");
            if (mode == 1) expected.push_back("residualTransfer");
            expected.insert(expected.end(), {"opticalFlowMethod", "nvidiaOpticalFlowQuality"});
            RequireChoiceProbe(order == expected,
                "Actual performance controls differ from the requested order");
        };
        for (unsigned iteration = 0; iteration < 6; ++iteration) {
            const auto before = values(); const unsigned beforeEdits = *edits;
            co_await OpenChoiceProbePopup(parent, anchor, popup);
            RequireChoiceProbe(values() == before && *edits == beforeEdits,
                "Opening actual parameter template changed configuration or emitted an edit");
            unsigned choices = 0, sliders = 0;
            VisitChoiceProbeTree(parent.Content().as<DependencyObject>(), [&](const auto& node) {
                const auto element = node.template try_as<FrameworkElement>();
                const auto parameter = element ? element.DataContext().template try_as<winrt::Magpie::ScalingModeParameter>() : nullptr;
                if (!parameter) return;
                if (node.template try_as<ComboBox>()) { ++choices; RequireChoiceProbe(parameter.IsChoice(), "Non-choice dropdown was instantiated"); }
                if (node.template try_as<Slider>()) { ++sliders; RequireChoiceProbe(parameter.IsFloat(), "Non-numeric slider was instantiated"); }
                if (node.template try_as<CheckBox>()) RequireChoiceProbe(parameter.IsBoolean(), "Non-Boolean checkbox was instantiated");
            });
            RequireChoiceProbe(choices > 0 && sliders > 0, "Actual parameter controls were not instantiated");
            CheckChoiceProbeLabels(parent.Content().as<DependencyObject>());
            if (iteration == 0) RequireChoiceProbe(values() == initial && *edits == 0, "Initial page load emitted an edit");

            // The paired advanced checkbox binds to Companion. Operate the
            // real control, then confirm the only saved mutation is its key.
            const auto advanced = findPerformanceToggle();
            for (bool expanded : {false, true}) {
                popup->phase = "advanced-toggle";
                auto expected = values(); expected[L"performanceShowAdvanced"] = expanded ? 1.f : 0.f;
                advanced.IsChecked(expanded);
                co_await WaitForChoiceProbe([&] {
                    return values() == expected && bool(findControl("processingMode", ComboBox{nullptr})) == expanded;
                }, "advanced visibility and saved key");
                RequireChoiceProbe(values() == expected, "Advanced visibility rewrote an unrelated value");
            }
            checkPerformanceOrder();
            parent.Content().as<FrameworkElement>().UpdateLayout();
            CheckChoiceProbeLabels(parent.Content().as<DependencyObject>());
            const auto stable = values(); const unsigned stableEdits = *edits;
            popup->phase = "focus-loss"; popup->expectedClose = true;
            anchor.Focus(FocusState::Programmatic);
            co_await WaitForChoiceProbe([&] {
                return Windows::UI::Xaml::Input::FocusManager::GetFocusedElement(anchor.XamlRoot()) == anchor || !popup->open;
            }, "anchor focus or focus dismissal");
            RequireChoiceProbe(values() == stable && *edits == stableEdits, "Focus loss emitted an unrelated edit");
            co_await CloseChoiceProbePopup(parent, popup);
            RequireChoiceProbe(values() == stable && *edits == stableEdits, "Close/unload rewrote parameters");
        }
        co_await OpenChoiceProbePopup(parent, anchor, popup);
        for (const auto name : {"modelPreset", "processingMode"}) {
            const int choiceCount = std::string_view(name) == "processingMode" ? 2 : 3;
            for (int value = 0; value < choiceCount; ++value) {
                popup->phase = "choice-edit";
                auto combo = findControl(name, ComboBox{nullptr});
                RequireChoiceProbe(bool(combo), "Required choice was not realized");
                if (std::string_view(name) == "processingMode") CheckChoiceProbeLocalization(combo, name);
                combo.SelectedIndex(value);
                co_await WaitForChoiceProbe([&] {
                    return values().at(StrHelper::UTF8ToUTF16(name)) == float(value);
                }, "explicit choice persistence");
                RequireChoiceProbe(values().at(StrHelper::UTF8ToUTF16(name)) == float(value), "Explicit choice did not persist");
                co_await CloseChoiceProbePopup(parent, popup);
                const auto expected = values(); const unsigned expectedEdits = *edits;
                co_await OpenChoiceProbePopup(parent, anchor, popup);
                combo = findControl(name, ComboBox{nullptr});
                RequireChoiceProbe(combo && combo.SelectedIndex() == value && values() == expected && *edits == expectedEdits,
                    "Explicit choice changed after close/reopen");
                checkPerformanceOrder();
                if (std::string_view(name) == "processingMode") {
                    const auto resolution = findControl("resolutionControl", ComboBox{nullptr});
                    const auto parameter = get_self<ScalingModeParameter>(
                        resolution.DataContext().as<winrt::Magpie::ScalingModeParameter>());
                    RequireChoiceProbe(parameter->IsChoiceAllowed(0) && parameter->IsChoiceAllowed(1),
                        "Processing mode restricted independent resolution choices");
                    auto interval = findControl("sparseUpdateInterval", ComboBox{nullptr});
                    RequireChoiceProbe(bool(interval) == (value == 1),
                        "Capture interval visibility followed the wrong mode");
                    if (value == 1) for (int selected : {0,1,2}) {
                        interval = findControl("sparseUpdateInterval", ComboBox{nullptr});
                        CheckChoiceProbeLocalization(interval, "sparseUpdateInterval");
                        auto expectedInterval = values();
                        expectedInterval[L"sparseUpdateInterval"] = float(selected + 2);
                        interval.SelectedIndex(selected);
                        co_await WaitForChoiceProbe([&] { return values() == expectedInterval; },
                            "interval choice index maps to stored capture value");
                        co_await CloseChoiceProbePopup(parent, popup);
                        const auto saved = values(); const unsigned savedEdits = *edits;
                        co_await OpenChoiceProbePopup(parent, anchor, popup);
                        interval = findControl("sparseUpdateInterval", ComboBox{nullptr});
                        RequireChoiceProbe(interval && interval.SelectedIndex() == selected &&
                            values() == saved && *edits == savedEdits,
                            "Interval changed after close/reopen");
                    }
                    for (const auto retired : {"recoveryPolicy", "bufferedDelayFrames", "asyncResidualComposition"})
                        RequireChoiceProbe(!findControl(retired, ComboBox{nullptr}) &&
                            !findControl(retired, Slider{nullptr}), "Retired product control was realized");
                }
            }
        }
        // Continue the adaptive/runtime-status checks after exercising both modes.
        findControl("processingMode", ComboBox{nullptr}).SelectedIndex(0);
        co_await WaitForChoiceProbe([&] { return values().at(L"processingMode") == 0; }, "restore every-frame probe mode");
        findControl("nvidiaOpticalFlowQuality", ComboBox{nullptr}).SelectedIndex(4);
        co_await WaitForChoiceProbe([&] { return values().at(L"nvidiaOpticalFlowQuality") == 5; }, "restore NVOF quality probe value");
        for (const auto name : {"residualLowFrequencyGain", "style", "pass2_style", "pass3_style"}) {
            const auto key = StrHelper::UTF8ToUTF16(name);
            const std::vector<float> cases = std::string_view(name) == "residualLowFrequencyGain"
                ? std::vector<float>{1.2f, 1.5f, 2.f} : std::vector<float>{2.f};
            for (const float value : cases) {
                popup->phase = "numeric-edit";
                auto slider = findControl(name, Slider{nullptr});
                RequireChoiceProbe(slider && slider.IsEnabled(), "Required numeric slider was not realized or enabled");
                // Exercise the real two-way binding even after a persisted test run.
                slider.Value(0);
                co_await WaitForChoiceProbe([&] { return values().at(key) == 0.f; }, "numeric reset before edit");
                auto expected = values(); expected[key] = value;
                slider.Value(value);
                co_await WaitForChoiceProbe([&] { return values() == expected; }, "numeric edit persistence");
                const unsigned expectedEdits = *edits;
                co_await CloseChoiceProbePopup(parent, popup);
                RequireChoiceProbe(values() == expected && *edits == expectedEdits, "Numeric close/unload rewrote values");
                co_await OpenChoiceProbePopup(parent, anchor, popup);
                slider = findControl(name, Slider{nullptr});
                RequireChoiceProbe(slider && std::abs(slider.Value() - double(value)) < .00001 &&
                    values() == expected && *edits == expectedEdits,
                    "Numeric value above one changed after close/reopen");
                Logger::Get().Info(fmt::format("NumericReopen PASS: parameter={} requested={} displayed={} saved={}",
                    name, value, slider.Value(), values().at(key)));
            }
        }
        auto resolution = findControl("resolutionControl", ComboBox{nullptr});
        RequireChoiceProbe(bool(resolution), "Resolution control was not realized");
        popup->phase = "runtime-display";
        resolution.SelectedIndex(0);
        co_await WaitForChoiceProbe([&] {
            const auto slider = findControl("inputResolutionPercent", Slider{nullptr});
            return values().at(L"resolutionControl") == 0 && slider && !slider.IsEnabled();
        }, "adaptive read-only control");
        const auto manual = values().at(L"inputResolutionPercent");
        const auto beforeStatus = values(); const unsigned statusEdits = *edits;
        const auto& mode = AppSettings::Get().ScalingModes().at(0);
        const auto session = std::make_shared<DlssNrSessionState>();
        session->Begin(99, {true});
        DlssNrStatus status;
        status.state = DlssNrState::Running; status.activeInputResolutionPercent = 55;
        RequireChoiceProbe(session->Update(99, 0, status), "Status fixture was not published");
        IssueContext context;
        context.scalingModeIdentity = mode.runtimeIdentity;
        context.effectIdentities = {mode.effects.at(0).runtimeIdentity};
        context.effectNames = {mode.effects.at(0).name};
        context.dlssNrSession = session;
        ErrorService::Get().BeginScalingSession(session, context);
        auto percent = findControl("inputResolutionPercent", Slider{nullptr});
        RequireChoiceProbe(percent && !percent.IsEnabled() && percent.Value() == 55 &&
            values() == beforeStatus && *edits == statusEdits, "Active percentage changed saved manual value or emitted an edit");
        auto wrongContext = context;
        const auto wrongIdentity = std::make_shared<const uint8_t>(uint8_t{0});
        wrongContext.effectIdentities = {wrongIdentity};
        ErrorService::Get().BeginScalingSession(session, wrongContext);
        RequireChoiceProbe(percent.Value() == manual && values() == beforeStatus && *edits == statusEdits,
            "Another effect's runtime percentage leaked into this page");
        ErrorService::Get().BeginScalingSession(session, context);
        resolution.SelectedIndex(1);
        co_await WaitForChoiceProbe([&] { return percent.IsEnabled() && percent.Value() == manual; }, "manual percentage restoration");
        RequireChoiceProbe(percent.IsEnabled() && percent.Value() == manual && values().at(L"inputResolutionPercent") == manual,
            "Switching to manual did not restore the saved percentage");
        checkPerformanceOrder();
        RequireChoiceProbe(values().at(L"modelPreset") == 2 && values().at(L"processingMode") == 0 && manual == 75 &&
            values().at(L"__dlssnrModeVersion") == DLSSNR_MODE_VERSION && values().at(L"sparseUpdateInterval") == 4 &&
            !values().contains(L"opticalFlowScaleOrder"), "Save candidate lost chosen values or revived the retired option");
        RequireChoiceProbe(AppSettings::Get().Save(), "Isolated portable config save failed");
        co_await CloseChoiceProbePopup(parent, popup);
        RequireChoiceProbe(popup->unexpectedClosings == 0, "Actual popup closed unexpectedly during probe actions");
        Logger::Get().Info(fmt::format("ParameterFlyoutAppProbe PASS: six numeric reopen cases above one, paired labels and tooltips, retired-key/mode migration, performance order, choice-type guards, {} actual-template opens, 12 advanced toggles, two processing-mode edits, interval 2/3/4 labels/index mapping/reopen, focus/unload, runtime identity/display isolation, saved config and event-based lifecycle; unexpectedClosings={}", popup->opened, popup->unexpectedClosings));
        passed = true;
    } catch (const hresult_error& error) {
        Logger::Get().Error(fmt::format("ParameterFlyoutAppProbe FAIL: HRESULT=0x{:x} {}", uint32_t(error.code().value), to_string(error.message())));
    } catch (const std::exception& error) {
        Logger::Get().Error(fmt::format("ParameterFlyoutAppProbe FAIL: {}", error.what()));
    }
    XamlHelper::ClosePopups(App::Get().RootPage()->XamlRoot());
    co_await resume_after(std::chrono::milliseconds(250)); co_await dispatcher;
    Logger::Get().Flush();
    App::Get().MainWindow().Destroy();
    PostQuitMessage(passed ? 0 : 1);
}
} // namespace winrt::Magpie::implementation
