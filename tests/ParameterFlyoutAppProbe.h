// Included only by the generated isolated App.cpp, never by product sources.
#include "ScalingModesPage.h"
#include "ScalingMode.h"
#include "EffectParametersViewModel.h"
#include "ParameterFlyoutTrace.h"
#include <winrt/Windows.UI.Xaml.Automation.Peers.h>
#include <winrt/Windows.UI.Xaml.Automation.Provider.h>

namespace winrt::Magpie::implementation {
using namespace ::Magpie;

static void CALLBACK ProbeForegroundEvent(HWINEVENTHOOK, DWORD, HWND window, LONG, LONG, DWORD, DWORD) {
    DWORD process = 0; GetWindowThreadProcessId(window, &process);
    TraceParameterFlyout("probe.foreground", uintptr_t(window), process);
}

static void TraceProbeFocus(const char* event) {
    GUITHREADINFO gui{sizeof(gui)};
    GetGUIThreadInfo(GetCurrentThreadId(), &gui);
    TraceParameterFlyout(event, uintptr_t(GetFocus()), intptr_t(gui.hwndActive), intptr_t(GetForegroundWindow()));
}

template<class F> void VisitProbeTree(const DependencyObject& node, const F& visit) {
    visit(node);
    const int count = VisualTreeHelper::GetChildrenCount(node);
    for (int i = 0; i < count; ++i) VisitProbeTree(VisualTreeHelper::GetChild(node, i), visit);
}

static fire_and_forget RunParameterFlyoutAppProbe() {
    const auto dispatcher = App::Get().Dispatcher();
    wchar_t interaction[16]{};
    GetEnvironmentVariableW(L"MAGPIE_FLYOUT_PROBE_INTERACTION", interaction, 16);
    const bool mouse = std::wstring_view(interaction) == L"mouse";
    wchar_t constrain[2]{};
    GetEnvironmentVariableW(L"MAGPIE_FLYOUT_PROBE_CONSTRAIN", constrain, 2);
    POINT originalCursor{}; GetCursorPos(&originalCursor);
    const auto foregroundHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
        nullptr, ProbeForegroundEvent, 0, 0, WINEVENT_OUTOFCONTEXT);
    bool passed = false;
    try {
        co_await resume_after(std::chrono::milliseconds(800));
        co_await dispatcher;
        App::Get().RootPage()->NavigateToScalingModes();
        co_await resume_after(std::chrono::milliseconds(800));
        co_await dispatcher;
        VisitProbeTree(App::Get().RootPage().as<DependencyObject>(), [](const auto& node) {
            if (auto expander = node.template try_as<winrt::Magpie::SettingsExpander>()) expander.IsExpanded(true);
        });
        co_await resume_after(std::chrono::milliseconds(500));
        co_await dispatcher;
        Button anchor{nullptr};
        Controls::Flyout parent{nullptr};
        VisitProbeTree(App::Get().RootPage().as<DependencyObject>(), [&](const auto& node) {
            const auto button = node.template try_as<Button>();
            const auto flyout = button ? button.Flyout().template try_as<Controls::Flyout>() : nullptr;
            const auto content = flyout ? flyout.Content().template try_as<ContentControl>() : nullptr;
            if (content && content.Content().template try_as<winrt::Magpie::EffectParametersViewModel>()) {
                anchor = button; parent = flyout;
            }
        });
        if (!parent) throw std::runtime_error("Actual parameter anchor was not realized");
        auto open = std::make_shared<bool>(false);
        parent.Opened([open](auto&&, auto&&) { *open = true; });
        parent.Closing([](auto&&, auto&&) { TraceProbeFocus("probe.parent.Closing.focus"); });
        parent.Closed([open](auto&&, auto&&) { *open = false; });
        if (constrain[0] == L'1') parent.ShouldConstrainToRootBounds(true);
        TraceParameterFlyout("probe.mode", uintptr_t(get_abi(parent)), mouse, parent.ShouldConstrainToRootBounds());
        parent.ShowAt(anchor);
        co_await resume_after(std::chrono::milliseconds(400));
        co_await dispatcher;
        ComboBox category{nullptr}, quality{nullptr};
        VisitProbeTree(parent.Content().as<DependencyObject>(), [&](const auto& node) {
            const auto combo = node.template try_as<ComboBox>();
            if (!combo) return;
            const auto parameter = combo.DataContext().template try_as<winrt::Magpie::ScalingModeParameter>();
            if (!parameter) return;
            if (get_self<ScalingModeParameter>(parameter)->Index() == 0) category = combo;
            if (get_self<ScalingModeParameter>(parameter)->Index() == 1) quality = combo;
        });
        if (!category || !quality || !*open) throw std::runtime_error("Actual VSR dropdowns were not realized");
        RECT hostRect{}; GetWindowRect(App::Get().MainWindow().Handle(), &hostRect);
        TraceParameterFlyout("probe.host.xy", uintptr_t(App::Get().MainWindow().Handle()), hostRect.left, hostRect.top);
        TraceParameterFlyout("probe.host.size", uintptr_t(App::Get().MainWindow().Handle()), hostRect.right-hostRect.left, hostRect.bottom-hostRect.top);
        category.DropDownOpened([](auto&&, auto&&) { TraceProbeFocus("probe.category.Opened.focus"); });
        quality.DropDownOpened([](auto&&, auto&&) { TraceProbeFocus("probe.quality.Opened.focus"); });
        // First isolate same-category quality; then cycle every category with
        // four intervening quality selections (including both streaming tiers).
        for (unsigned i = 0; i < 66; ++i) {
            const bool editCategory = i >= 16 && (i - 16) % 5 == 0;
            auto combo = editCategory ? category : quality;
            combo.IsDropDownOpen(true);
            co_await resume_after(std::chrono::milliseconds(300));
            co_await dispatcher;
            if (!*open) throw std::runtime_error("Parent closed before selection; no value mutation yet");
            const int next = (combo.SelectedIndex() + 1) % combo.Items().Size();
            const auto container = combo.ContainerFromIndex(next).as<FrameworkElement>();
            if (mouse) {
                const auto origin = container.TransformToVisual(anchor.XamlRoot().Content()).TransformPoint({0, 0});
                const auto point = App::Get().MainWindow().XamlRootPointToScreen({
                    origin.X + float(container.ActualWidth()/2), origin.Y + float(container.ActualHeight()/2)});
                DWORD targetProcess = 0;
                const auto target = WindowFromPoint(point);
                GetWindowThreadProcessId(target, &targetProcess);
                TraceParameterFlyout("probe.mouse.target", uintptr_t(target), targetProcess, intptr_t(GetWindow(target, GW_OWNER)));
                TraceParameterFlyout("probe.mouse.xy", uintptr_t(target), point.x, point.y);
                if (targetProcess != GetCurrentProcessId()) throw std::runtime_error("Mouse target not owned by probe; no input sent");
                SetCursorPos(point.x, point.y);
                co_await resume_after(std::chrono::milliseconds(40));
                co_await dispatcher;
                GetWindowThreadProcessId(WindowFromPoint(point), &targetProcess);
                if (targetProcess != GetCurrentProcessId()) throw std::runtime_error("Mouse target changed; no input sent");
                INPUT input[2]{};
                input[0].type = input[1].type = INPUT_MOUSE;
                input[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
                input[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
                if (SendInput(2, input, sizeof(INPUT)) != 2) throw std::runtime_error("Probe mouse SendInput failed");
            } else {
                TraceParameterFlyout("probe.programmaticSelection", uintptr_t(get_abi(combo)), next);
                combo.SelectedIndex(next);
            }
            co_await resume_after(std::chrono::milliseconds(150));
            co_await dispatcher;
            combo.IsDropDownOpen(false);
            co_await resume_after(std::chrono::milliseconds(150));
            co_await dispatcher;
            ::Magpie::TraceParameterFlyout("probe.iteration", uintptr_t(get_abi(combo)), i, combo.SelectedIndex());
            if (!*open) throw std::runtime_error("Parent closed during actual-template selection");
            if (combo.SelectedIndex() != next) throw std::runtime_error("Actual-template selection did not commit");
            const auto model = combo.DataContext().as<winrt::Magpie::ScalingModeParameter>();
            const auto& values = AppSettings::Get().ScalingModes().at(0).effects.at(0).parameters;
            if (values.at(editCategory ? L"category" : L"strength") != float(model.ChoiceValue()))
                throw std::runtime_error("Selection and actual configuration disagree");
        }
        passed = AppSettings::Get().Save();
        if (!passed) throw std::runtime_error("Isolated portable config save failed");
        Logger::Get().Info(fmt::format("ParameterFlyoutAppProbe PASS: 66 actual-template {} selections, saved values agree, no unexpected parent close",
            mouse ? "mouse" : "programmatic"));
        parent.Hide();
    } catch (const hresult_error& error) {
        Logger::Get().Error(fmt::format("ParameterFlyoutAppProbe FAIL: HRESULT=0x{:x} {}",
            uint32_t(error.code().value), to_string(error.message())));
    } catch (const std::exception& error) {
        Logger::Get().Error(fmt::format("ParameterFlyoutAppProbe FAIL: {}", error.what()));
    }
    // Let this probe's popups finish closing before destroying the XAML island.
    // The product keeps WindowsXamlManager alive for the same animation boundary.
    XamlHelper::ClosePopups(App::Get().RootPage()->XamlRoot());
    co_await resume_after(std::chrono::milliseconds(250));
    co_await dispatcher;
    Logger::Get().Flush();
    if (mouse) SetCursorPos(originalCursor.x, originalCursor.y);
    UnhookWinEvent(foregroundHook);
    App::Get().MainWindow().Destroy();
    PostQuitMessage(passed ? 0 : 1);
}

} // namespace winrt::Magpie::implementation
