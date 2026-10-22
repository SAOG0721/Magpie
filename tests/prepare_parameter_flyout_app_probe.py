"""Generate an isolated App.cpp; product source and compiled objects stay unchanged."""
import pathlib
import sys

source, output = map(pathlib.Path, sys.argv[1:])
text = (source / 'src/Magpie/App.cpp').read_text(encoding='utf-8-sig')

def replace_function(signature, replacement):
    global text
    start = text.index(signature)
    opening = text.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    text = text[:start] + replacement + text[end:]

text = text.replace('CommonSharedConstants::WM_MAGPIE_SHOWME', 'L"Magpie.ParameterFlyoutProbe.Show"')
text = text.replace('CommonSharedConstants::WM_MAGPIE_QUIT', 'L"Magpie.ParameterFlyoutProbe.Quit"')
text = text.replace('#include <CoreWindow.h>', '#include <CoreWindow.h>\n#include "ParameterFlyoutAppProbe.h"')
replace_function('bool App::Initialize(const wchar_t* arguments)', r'''bool App::Initialize(const wchar_t*) {
    IncreaseTimerResolution();
    // Refuse startup before reading any user configuration or global services.
    const auto probeConfig = Win32Helper::GetExePath().parent_path() / L"config" / L"v4e" / L"config.json";
    const auto probeMarker = Win32Helper::GetExePath().parent_path() / L"parameter-flyout-probe.marker";
    if (!std::filesystem::is_regular_file(probeConfig) || !std::filesystem::is_regular_file(probeMarker)) return false;
    SetEnvironmentVariableW(L"MAGPIE_TRACE_PARAMETER_FLYOUT", L"1");
    InitMessages();
    if (!_CheckSingleInstance()) return false;
    _mainWindow = std::make_unique<class MainWindow>();
    EffectsService::Get().Initialize();
    _windowsXamlManager = Hosting::WindowsXamlManager::InitializeForCurrentThread();
    _dispatcher = winrt::DispatcherQueue::GetForCurrentThread();
    LocalizationService::Get().EarlyInitialize();
    auto& settings = AppSettings::Get();
    if (!settings.Initialize() || !settings.IsPortableMode()) return false;
    SettingsCard::RegisterDependencyProperties();
    SettingsExpander::RegisterDependencyProperties();
    SettingsGroup::RegisterDependencyProperties();
    ControlSizeTrigger::RegisterDependencyProperties();
    IsEqualStateTrigger::RegisterDependencyProperties();
    IsNullStateTrigger::RegisterDependencyProperties();
    TextBlockHelper::RegisterDependencyProperties();
    _themeChangedRevoker = settings.ThemeChanged(auto_revoke, std::bind_front(&App::_AppSettings_ThemeChanged, this));
    _AppSettings_ThemeChanged(settings.Theme());
    LocalizationService::Get().Initialize();
    ToastService::Get().Initialize();
    ErrorService::Get().Initialize();
    ThemeHelper::Initialize();
    // No shortcuts, hooks, scaling runtime, RTSS, update checker, tray icon,
    // adapter monitor, elevation restart or autostart synchronization.
    if (!_mainWindow->Create()) return false;
    RunParameterFlyoutAppProbe();
    return true;
}''')
replace_function('void App::_Uninitialize()', r'''void App::_Uninitialize() {
    ToastService::Get().Uninitialize();
    EffectsService::Get().Uninitialize();
    _colorValuesChangedRevoker.revoke();
    _themeChangedRevoker.Revoke();
    _ReleaseMutexes();
}''')
replace_function('bool App::_CheckSingleInstance() noexcept', r'''bool App::_CheckSingleInstance() noexcept {
    bool exists = false;
    return _hSingleInstanceMutex.try_create(L"Magpie.ParameterFlyoutProbe.Isolated",
        CREATE_MUTEX_INITIAL_OWNER, MUTEX_ALL_ACCESS, nullptr, &exists) && !exists;
}''')
output.mkdir(parents=True, exist_ok=True)
(output / 'App.cpp').write_text(text, encoding='utf-8')
print('Generated isolated App.cpp: unique instance identity; no global services; portable config required.')
