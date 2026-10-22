// Isolated visible XAML-island probe. Never loads Magpie, services or config.
#define NOMINMAX
#include <windows.h>
#undef GetCurrentTime
#include <windows.ui.xaml.hosting.desktopwindowxamlsource.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Controls.Primitives.h>
#include <winrt/Windows.UI.Xaml.Hosting.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Input.h>
#include <winrt/Windows.UI.Xaml.Markup.h>
#include <winrt/Windows.UI.Xaml.Automation.Peers.h>
#ifdef MAGPIE_FLYOUT_PROBE_WINUI
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.XamlTypeInfo.h>
#endif
#include "../src/Magpie/EffectChoiceItems.h"
#include <iostream>
#include <chrono>
#include <string>

using namespace winrt;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::UI::Xaml::Controls::Primitives;
using namespace Windows::UI::Xaml::Hosting;
using namespace Windows::UI::Xaml::Media;

#ifdef MAGPIE_FLYOUT_PROBE_WINUI
struct ProbeApp : ApplicationT<ProbeApp, Markup::IXamlMetadataProvider> {
    Microsoft::UI::Xaml::XamlTypeInfo::XamlControlsXamlMetaDataProvider provider;
    Markup::IXamlType GetXamlType(Interop::TypeName const& type) { return provider.GetXamlType(type); }
    Markup::IXamlType GetXamlType(hstring const& type) { return provider.GetXamlType(type); }
    com_array<Markup::XmlnsDefinition> GetXmlnsDefinitions() { return provider.GetXmlnsDefinitions(); }
};
#endif

HWND island = nullptr;
bool parentOpen = false;
unsigned closeCount = 0;
const auto started = std::chrono::steady_clock::now();
void Trace(const char* event, intptr_t a = 0, intptr_t b = 0) {
    std::cout << std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started).count() << "us " << event
        << " a=" << a << " b=" << b << " parent=" << parentOpen << std::endl;
}
LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_SIZE) {
        Trace("WM_SIZE", LOWORD(l), HIWORD(l));
        if (island) SetWindowPos(island, nullptr, 0, 0, LOWORD(l), HIWORD(l), SWP_NOZORDER);
    } else if (msg == WM_ACTIVATE) Trace("WM_ACTIVATE", LOWORD(w), l);
    else if (msg == WM_DPICHANGED) Trace("WM_DPICHANGED", LOWORD(w));
    return DefWindowProc(hwnd, msg, w, l);
}

void CALLBACK FocusEvent(HWINEVENTHOOK, DWORD event, HWND window, LONG, LONG, DWORD, DWORD) {
    DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
    Trace(event == EVENT_SYSTEM_FOREGROUND ? "system.Foreground" : "system.Focus", intptr_t(window), pid);
}

int main(int argc, char** argv) {
    try {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        init_apartment(apartment_type::single_threaded);
#ifdef MAGPIE_FLYOUT_PROBE_WINUI
        const auto app = make_self<ProbeApp>();
#endif
        auto manager = WindowsXamlManager::InitializeForCurrentThread();
        const auto foregroundHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
            nullptr, FocusEvent, 0, 0, WINEVENT_OUTOFCONTEXT);
        const auto focusHook = SetWinEventHook(EVENT_OBJECT_FOCUS, EVENT_OBJECT_FOCUS,
            nullptr, FocusEvent, 0, 0, WINEVENT_OUTOFCONTEXT);
#ifdef MAGPIE_FLYOUT_PROBE_WINUI
        Microsoft::UI::Xaml::Controls::XamlControlsResources resources;
        resources.ControlsResourcesVersion(Microsoft::UI::Xaml::Controls::ControlsResourcesVersion::Version2);
        app->Resources().MergedDictionaries().Append(resources);
#endif
        WNDCLASS wc{};
        wc.lpfnWndProc = WindowProc;
        wc.hInstance = GetModuleHandle(nullptr);
        wc.lpszClassName = L"MagpieFlyoutProbe";
        RegisterClass(&wc);
        HWND host = CreateWindowEx(0, wc.lpszClassName, L"Magpie isolated parameter flyout probe",
            WS_OVERLAPPEDWINDOW, 100, 100, 850, 700, nullptr, nullptr, wc.hInstance, nullptr);
        DesktopWindowXamlSource source;
        auto native = source.as<IDesktopWindowXamlSourceNative2>();
        check_hresult(native->AttachToWindow(host));
        check_hresult(native->get_WindowHandle(&island));
        Grid root;
        Button anchor;
        anchor.Content(box_value(L"Effect parameters (isolated probe)"));
        anchor.HorizontalAlignment(HorizontalAlignment::Center);
        anchor.VerticalAlignment(VerticalAlignment::Top);
        anchor.Margin({0, 80, 0, 0});
        root.Children().Append(anchor);
        source.Content(root);
        RECT client{}; GetClientRect(host, &client);
        SetWindowPos(island, nullptr, 0, 0, client.right, client.bottom, SWP_SHOWWINDOW);
        Flyout flyout;
        flyout.ShouldConstrainToRootBounds(false);
        StackPanel parameters;
        parameters.Spacing(8);
        ComboBox category, quality;
        category.HorizontalAlignment(HorizontalAlignment::Stretch);
        quality.HorizontalAlignment(HorizontalAlignment::Stretch);
        category.ItemsSource(Magpie::MakeEffectChoiceItems({L"Super resolution", L"Restoration", L"Streaming", L"Denoising", L"Deblur"}));
        auto choices = Magpie::MakeEffectChoiceItems({L"Low", L"Medium", L"High", L"Ultra"});
        quality.ItemsSource(choices);
        category.SelectedIndex(0); quality.SelectedIndex(1);
        TextBlock categoryLabel, qualityLabel;
        categoryLabel.Text(L"Category"); qualityLabel.Text(L"Quality level");
        parameters.Children().Append(categoryLabel); parameters.Children().Append(category);
        parameters.Children().Append(qualityLabel); parameters.Children().Append(quality);
        ScrollViewer scroll;
        scroll.Width(260); scroll.MaxHeight(500);
        scroll.HorizontalScrollBarVisibility(ScrollBarVisibility::Auto);
        scroll.HorizontalScrollMode(ScrollMode::Enabled);
        scroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
        scroll.Content(parameters);
        ContentControl content; content.IsTabStop(false); content.Content(scroll);
        flyout.Content(content); anchor.Flyout(flyout);
        anchor.Loaded([](auto&&, auto&&) { Trace("anchor.Loaded"); });
        anchor.Unloaded([](auto&&, auto&&) { Trace("anchor.Unloaded"); });
        flyout.Opening([](auto&&, auto&&) { Trace("parent.Opening"); });
        flyout.Opened([](auto&&, auto&&) { parentOpen = true; Trace("parent.Opened"); });
        flyout.Closing([](auto&&, auto&&) { Trace("parent.Closing"); });
        flyout.Closed([](auto&&, auto&&) { parentOpen = false; ++closeCount; Trace("parent.Closed"); });
        choices.VectorChanged([](auto&&, auto&& args) { Trace("choices.VectorChanged", int(args.CollectionChange())); });
        category.DropDownOpened([](auto&&, auto&&) { Trace("category.DropDownOpened"); });
        category.DropDownClosed([](auto&&, auto&&) { Trace("category.DropDownClosed"); });
        quality.DropDownOpened([](auto&&, auto&&) { Trace("quality.DropDownOpened"); });
        quality.DropDownClosed([](auto&&, auto&&) { Trace("quality.DropDownClosed"); });
        quality.SelectionChanged([&](auto&&, auto&&) { Trace("quality.SelectionChanged", quality.SelectedIndex()); });
        bool streaming = false;
        category.SelectionChanged([&](auto&&, auto&&) {
            Trace("category.SelectionChanged.begin", category.SelectedIndex());
            const bool next = category.SelectedIndex() == 2;
            if (next != streaming) {
                streaming = next;
                choices.ReplaceAll(next ? std::vector<hstring>{L"Medium", L"Ultra"}
                    : std::vector<hstring>{L"Low", L"Medium", L"High", L"Ultra"});
                // Match existing Choices + ChoiceIndex notifications on the same collection.
                quality.ItemsSource(choices); quality.SelectedIndex(next ? 0 : 1);
            }
            Trace("category.SelectionChanged.end", category.SelectedIndex());
        });
        auto pump = [&](unsigned ms) {
            auto deadline = GetTickCount64() + ms;
            do {
                MSG msg{};
                while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    BOOL processed = FALSE; native->PreTranslateMessage(&msg, &processed);
                    if (!processed) { TranslateMessage(&msg); DispatchMessage(&msg); }
                }
                MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            } while (GetTickCount64() < deadline);
        };
        ShowWindow(host, SW_SHOWNORMAL); SetForegroundWindow(host); UpdateWindow(host); pump(500);
        Trace("host", intptr_t(host), GetDpiForWindow(host));
        flyout.ShowAt(anchor); pump(300);
        if (!parentOpen) throw std::runtime_error("Parent did not open");
        const std::string mode = argc > 1 ? argv[1] : "programmatic";
        auto key = [&](WORD vk) {
            DWORD pid = 0; GetWindowThreadProcessId(GetForegroundWindow(), &pid);
            Trace("keyboard.foreground", intptr_t(GetForegroundWindow()), pid);
            if (pid != GetCurrentProcessId()) throw std::runtime_error("Probe lost foreground; refuse input");
            INPUT input[2]{};
            input[0].type = input[1].type = INPUT_KEYBOARD;
            input[0].ki.wVk = input[1].ki.wVk = vk;
            input[1].ki.dwFlags = KEYEVENTF_KEYUP;
            if (SendInput(2, input, sizeof(INPUT)) != 2) throw std::runtime_error("SendInput failed");
            pump(60);
        };
        POINT originalCursor{}; GetCursorPos(&originalCursor);
        for (unsigned i = 0; i < 40; ++i) {
            const auto combo = i < 16 ? quality : category;
            combo.IsDropDownOpen(true); pump(300);
            const int next = (combo.SelectedIndex() + 1) % combo.Items().Size();
            if (mode == "mouse") {
                const auto item = combo.ContainerFromIndex(next).as<ComboBoxItem>();
                const auto origin = item.TransformToVisual(root).TransformPoint({0, 0});
                const double dpi = GetDpiForWindow(host) / 96.0;
                POINT point{LONG((origin.X + item.ActualWidth() / 2) * dpi),
                    LONG((origin.Y + item.ActualHeight() / 2) * dpi)};
                ClientToScreen(island, &point);
                DWORD pid = 0; GetWindowThreadProcessId(WindowFromPoint(point), &pid);
                Trace("mouse.target", point.x, point.y); Trace("mouse.targetPid", pid, GetCurrentProcessId());
                EnumWindows([](HWND hwnd, LPARAM) -> BOOL {
                    DWORD owner = 0; GetWindowThreadProcessId(hwnd, &owner);
                    if (owner == GetCurrentProcessId() && IsWindowVisible(hwnd)) {
                        RECT r{}; GetWindowRect(hwnd, &r); wchar_t name[256]{}; GetClassName(hwnd, name, 256);
                        std::cout << "WINDOW " << intptr_t(hwnd) << ' ' << to_string(name) << ' '
                            << r.left << ',' << r.top << ',' << r.right << ',' << r.bottom << std::endl;
                    }
                    return TRUE;
                }, 0);
                if (pid != GetCurrentProcessId()) throw std::runtime_error("Mouse target is not probe; refuse input");
                SetCursorPos(point.x, point.y); pump(60);
                INPUT input[2]{};
                input[0].type = input[1].type = INPUT_MOUSE;
                input[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN; input[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
                if (SendInput(2, input, sizeof(INPUT)) != 2) throw std::runtime_error("Mouse SendInput failed");
                pump(300);
            } else if (mode == "keyboard") {
                combo.Focus(FocusState::Keyboard);
                key(VK_HOME);
                for (int j = 0; j < next; ++j) key(VK_DOWN);
                key(VK_RETURN);
            } else {
                combo.SelectedIndex(next); pump(60); combo.IsDropDownOpen(false); pump(60);
            }
            Trace("iteration", i, combo.SelectedIndex());
            if (!parentOpen) throw std::runtime_error("Unexpected parent close");
            if (combo.SelectedIndex() != next) throw std::runtime_error("Selection not committed");
        }
        if (mode == "mouse") SetCursorPos(originalCursor.x, originalCursor.y);
        Trace(("PASS " + mode + " selections").c_str(), 40, closeCount);
        flyout.Hide(); pump(200);
        source.Content(nullptr); source.Close(); island = nullptr;
        UnhookWinEvent(foregroundHook); UnhookWinEvent(focusHook);
        DestroyWindow(host); manager.Close();
        return 0;
    } catch (hresult_error const& e) {
        std::cerr << "HRESULT " << std::hex << uint32_t(e.code().value) << ' ' << to_string(e.message()) << std::endl;
        return 1;
    } catch (std::exception const& e) {
        std::cerr << e.what() << std::endl; return 1;
    }
}
