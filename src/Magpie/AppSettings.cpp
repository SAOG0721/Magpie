#include "pch.h"
#include "AppSettings.h"
#include "ConfigRecovery.h"
#include "ConfigLocations.h"
#include "DLSSNRPerformanceParameters.h"
#include "OpticalFlowDefaults.h"
#include "App.h"
#include "ErrorService.h"
#include "EffectsService.h"
#include "EffectDesc.h"
#include "AutoStartHelper.h"
#include "AdminStartupSettings.h"
#include "CommonSharedConstants.h"
#include "JsonHelper.h"
#include "LocalizationService.h"
#include "Logger.h"
#include "MainWindow.h"
#include "Profile.h"
#include "ProfileFrameSync.h"
#include "ProfileFrameRefresh.h"
#include "ProfileRtss.h"
#include "resource.h"
#include "ScalingMode.h"
#include "ScalingModesService.h"
#include "ShortcutHelper.h"
#include "StrHelper.h"
#include "Win32Helper.h"
#include <rapidjson/prettywriter.h>
#include <ShellScalingApi.h>
#include <ShlObj.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>

using namespace winrt;
using namespace winrt::Magpie;

namespace Magpie {

// Enhanced settings use an isolated v4e directory; legacy v4 is import-only.
static constexpr uint32_t EXPERIMENTAL_DLSSNR_SETTINGS_VERSION = 2;
static constexpr uint32_t EXPERIMENTAL_DLSS_SR_SETTINGS_VERSION = 1;

_AppSettingsData::_AppSettingsData() {}

_AppSettingsData::~_AppSettingsData() {}

// 将热键存储为 uint32_t
// 不能存储为字符串，因为某些键的字符相同，如句号和小键盘的点
static uint32_t EncodeShortcut(const Shortcut& shortcut) noexcept {
	uint32_t value = shortcut.code;
	if (shortcut.win) {
		value |= 0x100;
	}
	if (shortcut.ctrl) {
		value |= 0x200;
	}
	if (shortcut.alt) {
		value |= 0x400;
	}
	if (shortcut.shift) {
		value |= 0x800;
	}
	return value;
}

static void DecodeShortcut(uint32_t value, Shortcut& shortcut) noexcept {
	if (value > 0xfff) {
		return;
	}

	shortcut.code = value & 0xff;
	shortcut.win = value & 0x100;
	shortcut.ctrl = value & 0x200;
	shortcut.alt = value & 0x400;
	shortcut.shift = value & 0x800;
}

static void WriteProfile(rapidjson::PrettyWriter<rapidjson::StringBuffer>& writer, const Profile& profile) noexcept {
	writer.StartObject();
	if (!profile.name.empty()) {
		writer.Key("name");
		writer.String(StrHelper::UTF16ToUTF8(profile.name).c_str());
		writer.Key("packaged");
		writer.Bool(profile.isPackaged);
		writer.Key("pathRule");
		writer.String(StrHelper::UTF16ToUTF8(profile.pathRule).c_str());
		writer.Key("classNameRule");
		writer.String(StrHelper::UTF16ToUTF8(profile.classNameRule).c_str());
		writer.Key("launcherPath");
		writer.String(StrHelper::UTF16ToUTF8(profile.launcherPath.native()).c_str());
		writer.Key("autoScale");
		writer.Uint((uint32_t)profile.autoScale);
		writer.Key("launchParameters");
		writer.String(StrHelper::UTF16ToUTF8(profile.launchParameters).c_str());
	}

	WriteProfileFrameRefresh(writer, profile.frameRefresh);
	WriteProfileRtss(writer, profile.rtss);
	writer.Key("parameterFocusSwitching");
	writer.Bool(profile.isParameterFocusSwitchingEnabled);
	writer.Key("fullscreenToolbarDock");
	writer.Uint(uint32_t(profile.toolbarDocks.fullscreen));
	writer.Key("windowedToolbarDock");
	writer.Uint(uint32_t(profile.toolbarDocks.windowed));
	writer.Key("scalingMode");
	writer.Int(profile.scalingMode);
	writer.Key("captureMethod");
	writer.Uint((uint32_t)profile.captureMethod);
	writer.Key("multiMonitorUsage");
	writer.Uint((uint32_t)profile.multiMonitorUsage);
	if (!profile.preferredMonitorId.empty()) {
		writer.Key("preferredMonitorId");
		writer.String(StrHelper::UTF16ToUTF8(profile.preferredMonitorId).c_str());
		writer.Key("preferredMonitorName");
		writer.String(StrHelper::UTF16ToUTF8(profile.preferredMonitorName).c_str());
	}

	writer.Key("initialWindowedScaleFactor");
	writer.Uint((uint32_t)profile.initialWindowedScaleFactor);
	writer.Key("customInitialWindowedScaleFactor");
	writer.Double(profile.customInitialWindowedScaleFactor);

	writer.Key("graphicsCardId");
	writer.StartObject();
	writer.Key("idx");
	writer.Int(profile.graphicsCardId.idx);
	writer.Key("vendorId");
	writer.Uint(profile.graphicsCardId.vendorId);
	writer.Key("deviceId");
	writer.Uint(profile.graphicsCardId.deviceId);
	writer.EndObject();

	writer.Key("3DGameMode");
	writer.Bool(profile.Is3DGameMode());
	writer.Key("captureTitleBar");
	writer.Bool(profile.IsCaptureTitleBar());
	writer.Key("adjustCursorSpeed");
	writer.Bool(profile.IsAdjustCursorSpeed());
	writer.Key("disableDirectFlip");
	writer.Bool(profile.IsDirectFlipDisabled());
	writer.Key("enableHdrCompatibility");
	writer.Bool(profile.IsHdrCompatibilityEnabled());

	writer.Key("cursorScaling");
	writer.Uint((uint32_t)profile.cursorScaling);
	writer.Key("customCursorScaling");
	writer.Double(profile.customCursorScaling);
	writer.Key("cursorInterpolationMode");
	writer.Uint((uint32_t)profile.cursorInterpolationMode);
	writer.Key("autoHideCursorEnabled");
	writer.Bool(profile.isAutoHideCursorEnabled);
	writer.Key("autoHideCursorDelay");
	writer.Double(profile.autoHideCursorDelay);

	writer.Key("croppingEnabled");
	writer.Bool(profile.isCroppingEnabled);
	writer.Key("cropping");
	writer.StartObject();
	writer.Key("left");
	writer.Double(profile.cropping.Left);
	writer.Key("top");
	writer.Double(profile.cropping.Top);
	writer.Key("right");
	writer.Double(profile.cropping.Right);
	writer.Key("bottom");
	writer.Double(profile.cropping.Bottom);
	writer.EndObject();

	writer.Key("destAlignment");
	writer.Uint((uint32_t)profile.destAlignment);

	writer.EndObject();
}

static void ReplaceIcon(HINSTANCE hInst, HWND hWnd, bool large) noexcept {
	HICON hIconApp = NULL;
	LoadIconMetric(hInst, MAKEINTRESOURCE(IDI_APP), large ? LIM_LARGE : LIM_SMALL, &hIconApp);
	HICON hIconOld = (HICON)SendMessage(hWnd, WM_SETICON, large ? ICON_BIG : ICON_SMALL, (LPARAM)hIconApp);
	if (hIconOld) {
		DestroyIcon(hIconOld);
	}
}

struct StartupDiagnostic {
	std::filesystem::path path;
	std::wstring details;
	std::wstring copied;
	std::wstring copyFailed;
	std::wstring openFailed;
};

static HRESULT CALLBACK TaskDialogCallback(
	HWND hWnd,
	UINT msg,
	WPARAM wParam,
	LPARAM /*lParam*/,
	LONG_PTR lpRefData
) {
	if (msg == TDN_BUTTON_CLICKED && (wParam == 100 || wParam == 101)) {
		auto& diagnostic = *reinterpret_cast<StartupDiagnostic*>(lpRefData);
		std::wstring feedback;
		try {
			if (wParam == 100) {
				if (!Win32Helper::ShellOpen(diagnostic.path.parent_path().c_str())) feedback = diagnostic.openFailed;
			} else {
				Windows::ApplicationModel::DataTransfer::DataPackage data;
				data.SetText(diagnostic.details);
				Windows::ApplicationModel::DataTransfer::Clipboard::SetContent(data);
				feedback = diagnostic.copied;
			}
		} catch (...) {
			feedback = diagnostic.copyFailed;
		}
		if (!feedback.empty()) {
			const auto content = diagnostic.details + L"\n\n" + feedback;
			SendMessageW(hWnd, TDM_SET_ELEMENT_TEXT, TDE_CONTENT, reinterpret_cast<LPARAM>(content.c_str()));
		}
		return S_FALSE;
	}
	if (msg == TDN_CREATED) {
		// 将任务栏图标替换为 Magpie 的图标
		// GetModuleHandle 获取 exe 文件的句柄
		HINSTANCE hInst = GetModuleHandle(nullptr);
		ReplaceIcon(hInst, hWnd, true);
		ReplaceIcon(hInst, hWnd, false);

		// 删除标题栏中的图标
		INT_PTR style = GetWindowLongPtr(hWnd, GWL_STYLE);
		SetWindowLongPtr(hWnd, GWL_STYLE, style & ~WS_SYSMENU);
	}

	return S_OK;
}

static void ShowErrorMessage(const wchar_t* mainInstruction, const wchar_t* content,
	const std::filesystem::path& path, uint32_t systemError = 0) noexcept {
	ResourceLoader resourceLoader =
		ResourceLoader::GetForCurrentView(CommonSharedConstants::APP_RESOURCE_MAP_ID);
	const hstring errorStr = resourceLoader.GetString(L"AppSettings_Dialog_Error");
	const hstring exitStr = resourceLoader.GetString(L"AppSettings_Dialog_Exit");
	const hstring openStr = resourceLoader.GetString(L"ErrorDetails_OpenConfigDirectory");
	const hstring copyStr = resourceLoader.GetString(L"ErrorDetails_Copy");
	StartupDiagnostic diagnostic{ path, content,
		std::wstring(resourceLoader.GetString(L"ErrorDetails_Copied")),
		std::wstring(resourceLoader.GetString(L"AppSettings_CopyFailed")),
		std::wstring(resourceLoader.GetString(L"ErrorDetails_OpenConfigFailed")) };
	if (systemError) diagnostic.details += L"\n" + std::wstring(resourceLoader.GetString(L"ErrorDetails_SystemCode")) +
		fmt::format(L": {} (0x{:08X})", systemError, systemError);
	TASKDIALOG_BUTTON buttons[] = { { 100, openStr.c_str() }, { 101, copyStr.c_str() }, { IDCANCEL, exitStr.c_str() } };
	TASKDIALOGCONFIG tdc{
		.cbSize = sizeof(TASKDIALOGCONFIG),
		.dwFlags = TDF_SIZE_TO_CONTENT,
		.pszWindowTitle = errorStr.c_str(),
		.pszMainIcon = TD_ERROR_ICON,
		.pszMainInstruction = mainInstruction,
		.pszContent = diagnostic.details.c_str(),
		.cButtons = static_cast<UINT>(std::size(buttons)),
		.pButtons = buttons,
		.nDefaultButton = IDCANCEL,
		.pfCallback = TaskDialogCallback,
		.lpCallbackData = reinterpret_cast<LONG_PTR>(&diagnostic)
	};
	TaskDialogIndirect(&tdc, nullptr, nullptr, nullptr);
}

AppSettings::~AppSettings() {}

bool AppSettings::Initialize() noexcept {
	Logger& logger = Logger::Get();

	std::filesystem::path existingConfigPath;
	if (!_UpdateConfigPath(&existingConfigPath)) {
		const DWORD pathError = GetLastError();
		logger.Error("_UpdateConfigPath 失败");
		const auto loader = ResourceLoader::GetForCurrentView(CommonSharedConstants::APP_RESOURCE_MAP_ID);
		const auto path = _configPath.empty() ? Win32Helper::GetExePath() : _configPath;
		const auto content = std::wstring(loader.GetString(L"AppSettings_ConfigLocationFailed")) + L"\n" + path.native();
		ShowErrorMessage(loader.GetString(L"AppSettings_ErrorDialog_ReadFailed").c_str(), content.c_str(), path, pathError);
		return false;
	}

	logger.Info(StrHelper::Concat("便携模式: ", _isPortableMode ? "是" : "否"));

	if (existingConfigPath.empty()) {
		logger.Info("不存在配置文件");
		_SetDefaultScalingModes();
		_SetDefaultShortcuts();
		if (!_isAlwaysRunAsAdmin || Win32Helper::IsProcessElevated()) SaveAsync();
		return true;
	}

	// 此时 ResourceLoader 使用“首选语言”
	
	std::string configText;
	uint32_t readError = 0;
	if (!ConfigPersistence::ReadFileBytes(existingConfigPath, configText, &readError)) {
		logger.Error("读取配置文件失败");
		ResourceLoader resourceLoader =
			ResourceLoader::GetForCurrentView(CommonSharedConstants::APP_RESOURCE_MAP_ID);
		hstring title = resourceLoader.GetString(L"AppSettings_ErrorDialog_ReadFailed");
		hstring content = resourceLoader.GetString(L"AppSettings_ErrorDialog_ConfigLocation");
		const auto guidance = resourceLoader.GetString(readError == ERROR_ACCESS_DENIED ?
			L"AppSettings_ReadAccessDenied" : L"AppSettings_ReadFailedGuidance");
		const auto details = std::wstring(guidance) + L"\n\n" +
			fmt::format(fmt::runtime(std::wstring_view(content)), existingConfigPath.native());
		ShowErrorMessage(title.c_str(), details.c_str(), existingConfigPath, readError);
		return false;
	}

	const auto loader = ResourceLoader::GetForCurrentView(CommonSharedConstants::APP_RESOURCE_MAP_ID);
	auto failRecovery = [&](const wchar_t* key, const std::filesystem::path& path, DWORD error) {
		const auto details = std::wstring(loader.GetString(key)) + L"\n" + path.native();
		ShowErrorMessage(loader.GetString(L"AppSettings_Dialog_Error").c_str(), details.c_str(), path, error);
		return false;
	};
	try {
		const std::string recoveredName = StrHelper::UTF16ToUTF8(loader.GetString(L"AppSettings_RecoveredGroup"));
		const ConfigRecovery::ParameterRules rules = [](std::string_view effectName, std::string_view parameterName)
			-> std::optional<ConfigRecovery::ParameterRule> {
			// Mode numbers depend on the effect's saved schema marker. Defer them
			// to its version-aware migration before validating current choices.
			if (effectName == "DLSSNR\\DLSSNR_AI_Filter" && parameterName == "processingMode") return std::nullopt;
			const auto* effect = EffectsService::Get().GetEffect(StrHelper::UTF8ToUTF16(effectName));
			if (!effect) return std::nullopt;
			const auto it = std::ranges::find(effect->params, parameterName, &EffectParameterDesc::name);
			if (it == effect->params.end()) return std::nullopt;
			return std::visit([&](const auto& constant) -> ConfigRecovery::ParameterRule {
				ConfigRecovery::ParameterRule rule{ static_cast<float>(constant.minValue),
					static_cast<float>(constant.maxValue), static_cast<float>(constant.defaultValue),
					std::is_integral_v<decltype(constant.defaultValue)>, {} };
				for (const auto& choice : it->choices) rule.choices.push_back(choice.value);
				return rule;
			}, it->constant);
		};
		auto plan = ConfigRecovery::Prepare(configText,
			ConfigPersistence::Read(existingConfigPath.native() + L".bak"), recoveredName, rules);
		const auto files = ConfigRecovery::FilesFor(_configPath, configText);
		bool recovered = plan.kind != ConfigRecovery::Kind::None;
		bool reused = false;
		if (recovered) {
			if (Win32Helper::FileExists(files.result.c_str())) {
				const auto previous = ConfigPersistence::Read(files.result);
				if (ConfigPersistence::IsValid(previous)) {
					auto cached = ConfigRecovery::Prepare(previous, {}, recoveredName, rules);
					if (cached.kind == ConfigRecovery::Kind::None) {
						plan.document = std::move(cached.document);
						plan.defaultModes = false;
						reused = true;
					}
				}
				// An unusable cache never blocks repair of the preserved input.
				if (!reused) logger.Warn("Rebuilding an outdated or damaged configuration recovery record");
			}
		}
		if (plan.defaultModes) _SetDefaultScalingModes();
		const bool shortcutsChanged = _LoadShortcuts(static_cast<const rapidjson::Document&>(plan.document).GetObj());
		_LoadSettings(static_cast<const rapidjson::Document&>(plan.document).GetObj());
		_isConfigMigrationNeeded |= ApplyOpticalFlowDefaultsMigration(
			_scalingModes, _experimentalOpticalFlowDefaultsVersion);
		// Retire the hidden HDR toggle in every loaded profile, including the
		// default and older versioned configurations. Preserve all other flags.
		bool hdrSettingsChanged = false;
		const auto disableHdr = [&](Profile& profile) {
			if (!profile.IsHdrCompatibilityEnabled()) return;
			profile.IsHdrCompatibilityEnabled(false);
			hdrSettingsChanged = true;
		};
		disableHdr(_defaultProfile);
		for (Profile& profile : _profiles) disableHdr(profile);
		if (hdrSettingsChanged) logger.Info("HDR compatibility temporarily disabled in saved profiles");
		// Load the startup preference before requesting elevation, but defer all
		// migration writes to the elevated replacement. This also covers portable
		// configurations in directories the ordinary token cannot write.
		if (_isAlwaysRunAsAdmin && !Win32Helper::IsProcessElevated()) return true;
		if (recovered && !ConfigRecovery::Preserve(existingConfigPath, configText, files))
			return failRecovery(L"AppSettings_BackupFailed", files.original, GetLastError());
		// Existing versioned migrations also preserve the input before their first write.
		if (_isConfigMigrationNeeded && !recovered) {
			if (!ConfigRecovery::Preserve(existingConfigPath, configText, files))
				return failRecovery(L"AppSettings_BackupFailed", files.original, GetLastError());
			recovered = true;
			plan.kind = ConfigRecovery::Kind::Repaired;
			plan.fields.push_back("/experimental settings migration");
		}
		if (recovered) {
			const std::string result = _Serialize(*this);
			ConfigSaveState recoverySave;
			if (!reused && !ConfigPersistence::WriteAtomic(files.result, result, 1, recoverySave))
				return failRecovery(L"AppSettings_RecoveryWriteFailed", files.result, GetLastError());
			// Finish persistence before startup succeeds. A write failure leaves the
			// original and completed recovery available for a subsequent save attempt.
			if (!ConfigPersistence::WriteAtomic(_configPath, result, ++_saveState->nextRevision, *_saveState))
				return failRecovery(L"AppSettings_RecoveryWriteFailed", _configPath, GetLastError());
			// Successful imports, migrations and selective repairs stay silent.
			// Unusable items are explained in their effect rows. Only a total reset
			// needs a startup notice because no original groups could be recovered.
			if (plan.kind == ConfigRecovery::Kind::Defaults) _recoveredConfigPath = files.original;
			_recoveryNotice = plan.kind == ConfigRecovery::Kind::Backup ? ScalingError::ConfigurationRecoveredBackup :
				plan.kind == ConfigRecovery::Kind::Defaults ? ScalingError::ConfigurationResetDefaults :
				plan.kind == ConfigRecovery::Kind::Repaired ? ScalingError::ConfigurationRepaired :
				ScalingError::ConfigurationRecoveredPartial;
			for (const auto& field : plan.fields) _recoveryDetails += "\n" + field;
			logger.Warn(fmt::format("Configuration recovery completed: kind={} reused={} repairedFields={} original={}",
				static_cast<int>(plan.kind), reused, plan.fields.size(), StrHelper::UTF16ToUTF8(files.original.native())));
		} else if (hdrSettingsChanged || shortcutsChanged || !Win32Helper::FileExists(_configPath.c_str())) {
			SaveAsync();
		}
		return true;
	} catch (...) {
		logger.Error("Configuration recovery failed with an exception");
		return failRecovery(L"AppSettings_RecoveryWriteFailed", _configPath, ERROR_INVALID_DATA);
	}
}

void AppSettings::PublishStartupNotice() noexcept {
	if (ScalingModesService::Get().HasDuplicateNames()) {
		ErrorService::Get().Report(ScalingError::DuplicateScalingModeNames);
	}
	if (_recoveredConfigPath.empty()) return;
	ErrorService::Get().Report(_recoveryNotice,
		StrHelper::UTF16ToUTF8(_recoveredConfigPath.native()) + _recoveryDetails);
}

bool AppSettings::Save() noexcept {
	const uint64_t revision = ++_saveState->nextRevision;
	const uint64_t issueRevision = ErrorService::Get().Revision();
    try {
        _UpdateWindowPlacement();
        const std::string json = _Serialize(*this);
		if (ConfigPersistence::WriteAtomic(_configPath, json, revision, *_saveState)) {
			ErrorService::Get().ConfigurationSaved(revision, issueRevision);
			return true;
		}
        const DWORD error = GetLastError();
        Logger::Get().Win32Error("Save configuration failed");
        ErrorService::Get().Report(ScalingError::ConfigurationWriteFailed,
            StrHelper::UTF16ToUTF8(_configPath.native()), nullptr, error, {}, revision);
    } catch (...) {
        Logger::Get().Error("Save configuration failed with an exception");
        ErrorService::Get().Report(ScalingError::ConfigurationWriteFailed,
			StrHelper::UTF16ToUTF8(_configPath.native()), nullptr, 0, {}, revision);
    }
    return false;
}

fire_and_forget AppSettings::SaveAsync(std::function<void(bool)> onCompleted) noexcept {
	bool succeeded = false;
	std::filesystem::path path;
	const uint64_t revision = ++_saveState->nextRevision;
	const uint64_t issueRevision = ErrorService::Get().Revision();
	try {
		path = _configPath;
		_UpdateWindowPlacement();
		// Snapshot on the UI thread; background work owns only serialized data.
		const std::string json = _Serialize(*this);
		const auto state = _saveState;
		co_await resume_background();
		succeeded = ConfigPersistence::WriteAtomic(path, json, revision, *state);
		if (!succeeded) {
			const DWORD error = GetLastError();
			Logger::Get().Win32Error("Save configuration failed");
			ErrorService::Get().Report(ScalingError::ConfigurationWriteFailed,
				StrHelper::UTF16ToUTF8(path.native()), nullptr, error, {}, revision);
		} else {
			ErrorService::Get().ConfigurationSaved(revision, issueRevision);
		}
	} catch (...) {
		Logger::Get().Error("Save configuration failed with an exception");
		ErrorService::Get().Report(ScalingError::ConfigurationWriteFailed,
			StrHelper::UTF16ToUTF8(path.native()), nullptr, 0, {}, revision);
	}
	// This callback must not access UI objects. Parameter saves only publish an
	// atomic result into shared state, including after their overlay is closed.
	try {
		if (onCompleted) onCompleted(succeeded);
	} catch (...) {
		Logger::Get().Error("Configuration save completion failed");
	}
}

void AppSettings::IsPortableMode(bool value) noexcept {
	if (_isPortableMode == value) return;
	const auto previousPath = _configPath;
	const auto previousDirectory = _configDir;
	const bool previousPortable = _isPortableMode;
	auto restoreLocation = [&]() {
		_isPortableMode = previousPortable;
		_configPath = previousPath;
		_configDir = previousDirectory;
	};
	_isPortableMode = value;
	// Commit a newer revision to the destination before removing our old
	// portable file. Pending older saves then cannot recreate that file.
	if (!_UpdateConfigPath()) {
		const auto failedPath = _configPath;
		const DWORD error = GetLastError();
		restoreLocation();
		ErrorService::Get().Report(ScalingError::ConfigurationWriteFailed,
			StrHelper::UTF16ToUTF8(failedPath.native()), nullptr, error);
		return;
	}
	if (!Save()) {
		restoreLocation();
		return;
	}
	if (!value && !DeleteFileW(previousPath.c_str())) {
		const DWORD error = GetLastError();
		if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
			restoreLocation();
			ErrorService::Get().Report(ScalingError::ConfigurationWriteFailed,
				StrHelper::UTF16ToUTF8(previousPath.native()), nullptr, error);
			return;
		}
	}
	Logger::Get().Info(value ? "Portable configuration enabled" : "User configuration enabled");
}

void AppSettings::Language(int value) {
	if (_language == value) {
		return;
	}

	_language = value;
	SaveAsync();
}

void AppSettings::Theme(AppTheme value) {
	if (_theme == value) {
		return;
	}

	_theme = value;
	ThemeChanged.Invoke(value);

	SaveAsync();
}

void AppSettings::SetShortcut(ShortcutAction action, const Shortcut& value) {
	if (_shortcuts[(size_t)action] == value) {
		return;
	}

	_shortcuts[(size_t)action] = value;
	Logger::Get().Info(fmt::format("热键 {} 已更改为 {}", ShortcutHelper::ToString(action), StrHelper::UTF16ToUTF8(value.ToString())));
	ShortcutChanged.Invoke(action);

	SaveAsync();
}

void AppSettings::CountdownSeconds(uint32_t value) noexcept {
	if (_countdownSeconds == value) {
		return;
	}

	_countdownSeconds = value;
	CountdownSecondsChanged.Invoke(value);

	SaveAsync();
}

void AppSettings::IsDeveloperMode(bool value) noexcept {
	_isDeveloperMode = value;
	if (!value) {
		// 关闭开发者模式则禁用所有开发者选项
		_isDebugMode = false;
		_isBenchmarkMode = false;
		_isEffectCacheDisabled = false;
		_isFontCacheDisabled = false;
		_isSaveEffectSources = false;
		_isWarningsAreErrors = false;
		_isStatisticsForDynamicDetectionEnabled = false;
		_isFP16Disabled = false;
	}

	SaveAsync();
}

void AppSettings::IsAlwaysRunAsAdmin(bool value) noexcept {
	if (_isAlwaysRunAsAdmin == value) {
		return;
	}

	_isAlwaysRunAsAdmin = value;
	SaveAsync();

	// 更新启动任务
	if (AutoStartHelper::IsAutoStartEnabled()) {
		AutoStartHelper::EnableAutoStart(value);
	}
}

void AppSettings::UpdateAdminAutoStart() noexcept {
	if (_adminAutoStartDefaultsVersion >= ADMIN_STARTUP_DEFAULTS_VERSION) return;
	if (_isAlwaysRunAsAdmin && !Win32Helper::IsProcessElevated()) return;
	if (AutoStartHelper::IsAutoStartEnabled() && !AutoStartHelper::EnableAutoStart(_isAlwaysRunAsAdmin)) {
		Logger::Get().Warn("Could not migrate the administrator auto-start task"); return;
	}
	_adminAutoStartDefaultsVersion = ADMIN_STARTUP_DEFAULTS_VERSION;
	SaveAsync();
}

void AppSettings::IsShowNotifyIcon(bool value) noexcept {
	if (_isShowNotifyIcon == value) {
		return;
	}

	_isShowNotifyIcon = value;
	IsShowNotifyIconChanged.Invoke(value);

	SaveAsync();
}

static std::filesystem::path GetSystemScreenshotsDir() noexcept {
	// 如果 Screenshots 文件夹不存在将失败
	wil::unique_cotaskmem_string folder;
	HRESULT hr = SHGetKnownFolderPath(
		FOLDERID_Screenshots, KF_FLAG_DEFAULT, NULL, folder.put());
	if (SUCCEEDED(hr)) {
		return folder.get();
	}

	// 屏幕截图文件夹默认路径是 %USERPROFILE%\Pictures\Screenshots

	hr = SHGetKnownFolderPath(
		FOLDERID_Pictures, KF_FLAG_DEFAULT, NULL, folder.put());
	if (SUCCEEDED(hr)) {
		return StrHelper::Concat(folder.get(), L"\\Screenshots");
	}

	hr = SHGetKnownFolderPath(
		FOLDERID_Profile, KF_FLAG_DEFAULT, NULL, folder.put());
	if (SUCCEEDED(hr)) {
		return StrHelper::Concat(folder.get(), L"\\Pictures\\Screenshots");
	}
	
	Logger::Get().ComError("SHGetKnownFolderPath 失败", hr);
	return {};
}

static bool IsSubfolder(const std::wstring& sub, const std::wstring& parent) noexcept {
	if (!sub.starts_with(parent)) {
		return false;
	}

	if (parent.size() == sub.size()) {
		return true;
	}

	return sub[parent.size()] == L'\\';
}

// 失败时返回空字符串
std::filesystem::path AppSettings::ScreenshotsDir() const noexcept {
	if (_screenshotsDir.empty()) {
		// 系统“屏幕截图”文件夹
		return GetSystemScreenshotsDir();
	} else if (_screenshotsDir.is_relative()) {
		// 相对路径
		std::wstring workingDir;
		HRESULT hr = wil::GetCurrentDirectoryW(workingDir);
		if (FAILED(hr)) {
			Logger::Get().ComError("wil::GetCurrentDirectoryW 失败", hr);
			return {};
		}

		if (_screenshotsDir == L".") {
			return std::filesystem::path(std::move(workingDir));
		} else {
			return (std::filesystem::path(std::move(workingDir)) / _screenshotsDir).lexically_normal();
		}
	} else {
		// 绝对路径
		return _screenshotsDir;
	}
}

void AppSettings::ScreenshotsDir(const std::filesystem::path& value) noexcept {
	assert(!value.empty());

	if (value == GetSystemScreenshotsDir()) {
		// 系统“屏幕截图”文件夹
		_screenshotsDir.clear();
	} else {
		std::wstring workingDir;
		HRESULT hr = wil::GetCurrentDirectoryW(workingDir);
		if (FAILED(hr)) {
			Logger::Get().ComError("wil::GetCurrentDirectoryW 失败", hr);
			return;
		}

		if (IsSubfolder(value, workingDir)) {
			// 保存位置在工作文件夹内则转换为相对路径
			if (value.native().size() == workingDir.size()) {
				_screenshotsDir = L".";
			} else {
				_screenshotsDir = StrHelper::Concat(
					L".",
					std::wstring(value.native().begin() + workingDir.size(), value.native().end())
				);
			}
		} else {
			// 绝对路径
			_screenshotsDir = value;
		}
	}

	SaveAsync();
}

void AppSettings::_UpdateWindowPlacement() noexcept {
	const HWND hwndMain = implementation::App::Get().MainWindow().Handle();;
	if (!hwndMain) {
		return;
	}

	WINDOWPLACEMENT wp{ sizeof(wp) };
	if (!GetWindowPlacement(hwndMain, &wp)) {
		Logger::Get().Win32Error("GetWindowPlacement 失败");
		return;
	}

	// rcNormalPosition 使用工作区坐标，应转换为屏幕坐标。
	// 见 https://github.com/Blinue/nt5src/blob/daad8a087a4e75422ec96b7911f1df4669989611/Source/XPSP1/NT/windows/core/ntuser/kernel/winmgr.c#L752
	HMONITOR hMon = MonitorFromWindow(hwndMain, MONITOR_DEFAULTTOPRIMARY);
	MONITORINFO mi{ sizeof(mi) };
	if (!GetMonitorInfo(hMon, &mi)) {
		Logger::Get().Win32Error("GetMonitorInfo 失败");
		return;
	}

	const LONG workingAreaOffsetX = mi.rcWork.left - mi.rcMonitor.left;
	const LONG workingAreaOffsetY = mi.rcWork.top - mi.rcMonitor.top;
	_mainWindowCenter = {
		(wp.rcNormalPosition.left + wp.rcNormalPosition.right) / 2.0f + workingAreaOffsetX,
		(wp.rcNormalPosition.top + wp.rcNormalPosition.bottom) / 2.0f + workingAreaOffsetY,
	};

	const float dpiFactor = GetDpiForWindow(hwndMain) / float(USER_DEFAULT_SCREEN_DPI);
	_mainWindowSizeInDips = {
		(wp.rcNormalPosition.right - wp.rcNormalPosition.left) / dpiFactor,
		(wp.rcNormalPosition.bottom - wp.rcNormalPosition.top) / dpiFactor,
	};

	// 最小化时保留最小化前的最大化状态，这样自动重启后恢复窗口时不会
	// 把“从最大化窗口最小化”误记为普通窗口。
	if (!IsIconic(hwndMain)) {
		_isMainWindowMaximized = wp.showCmd == SW_MAXIMIZE;
	}
}

std::string AppSettings::_Serialize(const _AppSettingsData& data) {
	rapidjson::StringBuffer json;
	rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(json);
	writer.StartObject();

	writer.Key("language");
	if (data._language < 0) {
		writer.String("");
	} else {
		const wchar_t* language = LocalizationService::SupportedLanguages()[data._language];
		writer.String(StrHelper::UTF16ToUTF8(language).c_str());
	}

	writer.Key("theme");
	writer.Uint((uint32_t)data._theme);

	writer.Key("windowPos");
	writer.StartObject();
	writer.Key("centerX");
	writer.Double(data._mainWindowCenter.X);
	writer.Key("centerY");
	writer.Double(data._mainWindowCenter.Y);
	writer.Key("width");
	writer.Double(data._mainWindowSizeInDips.Width);
	writer.Key("height");
	writer.Double(data._mainWindowSizeInDips.Height);
	writer.Key("maximized");
	writer.Bool(data._isMainWindowMaximized);
	writer.EndObject();

	writer.Key("shortcuts");
	writer.StartObject();
	writer.Key("scale");
	writer.Uint(EncodeShortcut(data._shortcuts[(size_t)ShortcutAction::Scale]));
	writer.Key("windowedModeScale");
	writer.Uint(EncodeShortcut(data._shortcuts[(size_t)ShortcutAction::WindowedModeScale]));
	writer.Key("toolbar");
	writer.Uint(EncodeShortcut(data._shortcuts[(size_t)ShortcutAction::Toolbar]));
	writer.Key("profiler");
	writer.Uint(EncodeShortcut(data._shortcuts[(size_t)ShortcutAction::Profiler]));
	writer.Key("effectParameters");
	writer.Uint(EncodeShortcut(data._shortcuts[(size_t)ShortcutAction::EffectParameters]));
	writer.Key("screenshot");
	writer.Uint(EncodeShortcut(data._shortcuts[(size_t)ShortcutAction::Screenshot]));
	writer.Key("toolbarPin");
	writer.Uint(EncodeShortcut(data._shortcuts[(size_t)ShortcutAction::ToolbarPin]));
	writer.Key("comparison");
	writer.Uint(EncodeShortcut(data._shortcuts[(size_t)ShortcutAction::Comparison]));

	writer.EndObject();

	writer.Key("countdownSeconds");
	writer.Uint(data._countdownSeconds);
	writer.Key("developerMode");
	writer.Bool(data._isDeveloperMode);
	writer.Key("debugMode");
	writer.Bool(data._isDebugMode);
	writer.Key("benchmarkMode");
	writer.Bool(data._isBenchmarkMode);
	writer.Key("disableTopmost");
	writer.Bool(data._isTopmostDisabled);
	writer.Key("disableEffectCache");
	writer.Bool(data._isEffectCacheDisabled);
	writer.Key("disableFontCache");
	writer.Bool(data._isFontCacheDisabled);
	writer.Key("saveEffectSources");
	writer.Bool(data._isSaveEffectSources);
	writer.Key("warningsAreErrors");
	writer.Bool(data._isWarningsAreErrors);
	writer.Key("allowScalingMaximized");
	writer.Bool(data._isAllowScalingMaximized);
	writer.Key("simulateExclusiveFullscreen");
	writer.Bool(data._isSimulateExclusiveFullscreen);
	writer.Key("alwaysRunAsAdmin");
	writer.Bool(data._isAlwaysRunAsAdmin);
	writer.Key("adminStartupDefaultsVersion");
	writer.Uint(data._adminStartupDefaultsVersion);
	writer.Key("adminAutoStartDefaultsVersion");
	writer.Uint(data._adminAutoStartDefaultsVersion);
	writer.Key("showNotifyIcon");
	writer.Bool(data._isShowNotifyIcon);
	writer.Key("smoothMotionCompatibilityMode");
	writer.Bool(data._isSmoothMotionCompatibilityMode);
	writer.Key("inlineParams");
	writer.Bool(data._isInlineParams);
	writer.Key("autoCheckForUpdates");
	writer.Bool(data._isAutoCheckForUpdates);
	writer.Key("checkForPreviewUpdates");
	writer.Bool(data._isCheckForPreviewUpdates);
	writer.Key("updateCheckDate");
	writer.Int64(data._updateCheckDate.time_since_epoch().count());
	writer.Key("duplicateFrameDetectionMode");
	writer.Uint((uint32_t)data._duplicateFrameDetectionMode);
	writer.Key("simpleFrameRefresh");
	writer.Bool(data._isSimpleFrameRefresh);
	writer.Key("enableStatisticsForDynamicDetection");
	writer.Bool(data._isStatisticsForDynamicDetectionEnabled);
	writer.Key("stopEffectsOnTaskSwitch");
	writer.Bool(data._isStopEffectsOnTaskSwitchEnabled);
	writer.Key("vrr");
	writer.Bool(data._isVRREnabled);
	writer.Key("experimentalFrameRefreshVersion");
	writer.Uint(1);
	writer.Key("disableFP16");
	writer.Bool(data._isFP16Disabled);
	writer.Key("experimentalDlssnrSettingsVersion");
	writer.Uint(data._experimentalDlssnrSettingsVersion);
	writer.Key("experimentalDlssSrSettingsVersion");
	writer.Uint(data._experimentalDlssSrSettingsVersion);
	writer.Key("experimentalDepthRemovalVersion");
	writer.Uint(data._experimentalDepthRemovalVersion);
	writer.Key("experimentalXeSSFGSettingsVersion");
	writer.Uint(data._experimentalXeSSFGSettingsVersion);
	writer.Key("experimentalOpticalFlowDefaultsVersion");
	writer.Uint(data._experimentalOpticalFlowDefaultsVersion);

	ScalingModesService::Export(writer, data._scalingModes);

	writer.Key("experimentalProfileFrameSyncVersion");
	writer.Uint(1);
	writer.Key("profiles");
	writer.StartArray();
	WriteProfile(writer, data._defaultProfile);
	for (const Profile& rule : data._profiles) {
		WriteProfile(writer, rule);
	}
	writer.EndArray();

	writer.Key("overlay");
	writer.StartObject();
	writer.Key("fullscreenInitialToolbarState");
	writer.Uint((uint32_t)data._fullscreenInitialToolbarState);
	writer.Key("windowedInitialToolbarState");
	writer.Uint((uint32_t)data._windowedInitialToolbarState);
	writer.Key("screenshotsDir");
	writer.String(StrHelper::UTF16ToUTF8(data._screenshotsDir.native()).c_str());
	writer.Key("screenshotContent");
	writer.Uint(static_cast<uint32_t>(data._screenshotContent));
	writer.Key("profiler");
	WriteOverlayProfilerOptions(writer, data._overlayOptions.profiler);
	writer.Key("windows");
	writer.StartObject();
	for (const auto& [name, windowOption] : data._overlayOptions.windows) {
		writer.Key(name.c_str());
		writer.StartObject();
		writer.Key("hArea");
		writer.Uint(windowOption.hArea);
		writer.Key("vArea");
		writer.Uint(windowOption.vArea);
		writer.Key("hPos");
		writer.Double(windowOption.hPos);
		writer.Key("vPos");
		writer.Double(windowOption.vPos);
		writer.Key("width");
		writer.Double(windowOption.width);
		writer.Key("height");
		writer.Double(windowOption.height);
		writer.Key("zoom");
		writer.Double(windowOption.zoom);
		writer.EndObject();
	}
	writer.EndObject();
	writer.EndObject();

	writer.EndObject();

	return { json.GetString(), json.GetLength() };
}

// 永远不会失败，遇到不合法的配置项时静默忽略
void AppSettings::_LoadSettings(const rapidjson::GenericObject<true, rapidjson::Value>& root) noexcept {
	_experimentalDlssnrSettingsVersion = 0;
	JsonHelper::ReadUInt(root, "experimentalDlssnrSettingsVersion",
		_experimentalDlssnrSettingsVersion);
	_experimentalDlssSrSettingsVersion = 0;
	JsonHelper::ReadUInt(root, "experimentalDlssSrSettingsVersion",
		_experimentalDlssSrSettingsVersion);
	_experimentalXeSSFGSettingsVersion = 0;
	JsonHelper::ReadUInt(root, "experimentalXeSSFGSettingsVersion", _experimentalXeSSFGSettingsVersion);
	if (_experimentalXeSSFGSettingsVersion < 1) {
		_experimentalXeSSFGSettingsVersion = 1;
		_isConfigMigrationNeeded = true;
	}
	_experimentalOpticalFlowDefaultsVersion = 0;
	JsonHelper::ReadUInt(root, "experimentalOpticalFlowDefaultsVersion",
		_experimentalOpticalFlowDefaultsVersion);
	_experimentalDepthRemovalVersion = 0;
	JsonHelper::ReadUInt(root, "experimentalDepthRemovalVersion",
		_experimentalDepthRemovalVersion);

	{
		std::wstring language;
		JsonHelper::ReadString(root, "language", language);
		if (language.empty()) {
			_language = -1;
		} else {
			StrHelper::ToLowerCase(language);
			std::span<const wchar_t*> languages = LocalizationService::SupportedLanguages();
			auto it = std::find(languages.begin(), languages.end(), language);
			if (it == languages.end()) {
				// 未知的语言设置，重置为使用系统设置
				_language = -1;
			} else {
				_language = int(it - languages.begin());
			}
		}
	}

	{
		uint32_t theme = (uint32_t)AppTheme::System;
		JsonHelper::ReadUInt(root, "theme", theme);
		if (theme <= 2) {
			_theme = (AppTheme)theme;
		} else {
			_theme = AppTheme::System;
		}
	}

	auto windowPosNode = root.FindMember("windowPos");
	if (windowPosNode != root.MemberEnd() && windowPosNode->value.IsObject()) {
		auto windowPosObj = windowPosNode->value.GetObj();

		Point center{};
		Size size{};
		if (JsonHelper::ReadFloat(windowPosObj, "centerX", center.X, true) &&
			JsonHelper::ReadFloat(windowPosObj, "centerY", center.Y, true) &&
			JsonHelper::ReadFloat(windowPosObj, "width", size.Width, true) &&
			JsonHelper::ReadFloat(windowPosObj, "height", size.Height, true)) {
			_mainWindowCenter = center;
			_mainWindowSizeInDips = size;
		} else {
			// 尽最大努力和旧版本兼容
			int x = 0;
			int y = 0;
			uint32_t width = 0;
			uint32_t height = 0;
			if (JsonHelper::ReadInt(windowPosObj, "x", x, true) &&
				JsonHelper::ReadInt(windowPosObj, "y", y, true) &&
				JsonHelper::ReadUInt(windowPosObj, "width", width, true) &&
				JsonHelper::ReadUInt(windowPosObj, "height", height, true)) {
				_mainWindowCenter = {
					x + width / 2.0f,
					y + height / 2.0f
				};

				// 如果窗口位置不存在屏幕则使用主屏幕的缩放，猜错的后果仅是窗口尺寸错误，
				// 无论如何原始缩放信息已经丢失。
				const HMONITOR hMon = MonitorFromPoint(
					{ std::lroundf(_mainWindowCenter.X), std::lroundf(_mainWindowCenter.Y) },
					MONITOR_DEFAULTTOPRIMARY
				);

				UINT dpi = USER_DEFAULT_SCREEN_DPI;
				GetDpiForMonitor(hMon, MDT_EFFECTIVE_DPI, &dpi, &dpi);
				const float dpiFactor = dpi / float(USER_DEFAULT_SCREEN_DPI);
				_mainWindowSizeInDips = {
					width / dpiFactor,
					height / dpiFactor
				};
			}
		}

		JsonHelper::ReadBool(windowPosObj, "maximized", _isMainWindowMaximized);
	}

	if (!JsonHelper::ReadUInt(root, "countdownSeconds", _countdownSeconds, true)) {
		// v0.10.0-preview1 使用 downCount
		JsonHelper::ReadUInt(root, "downCount", _countdownSeconds);
	}
	if (_countdownSeconds == 0 || _countdownSeconds > 5) {
		_countdownSeconds = 3;
	}
	JsonHelper::ReadBool(root, "developerMode", _isDeveloperMode);
	JsonHelper::ReadBool(root, "debugMode", _isDebugMode);
	JsonHelper::ReadBool(root, "benchmarkMode", _isBenchmarkMode);
	JsonHelper::ReadBool(root, "disableTopmost", _isTopmostDisabled);
	JsonHelper::ReadBool(root, "disableEffectCache", _isEffectCacheDisabled);
	JsonHelper::ReadBool(root, "disableFontCache", _isFontCacheDisabled);
	JsonHelper::ReadBool(root, "saveEffectSources", _isSaveEffectSources);
	JsonHelper::ReadBool(root, "warningsAreErrors", _isWarningsAreErrors);
	JsonHelper::ReadBool(root, "allowScalingMaximized", _isAllowScalingMaximized);
	JsonHelper::ReadBool(root, "simulateExclusiveFullscreen", _isSimulateExclusiveFullscreen);
	_isConfigMigrationNeeded |= ReadAdminStartupSettings(root, _isAlwaysRunAsAdmin, _adminStartupDefaultsVersion);
	_adminAutoStartDefaultsVersion = 0;
	JsonHelper::ReadUInt(root, "adminAutoStartDefaultsVersion", _adminAutoStartDefaultsVersion, true);
	if (!JsonHelper::ReadBool(root, "showNotifyIcon", _isShowNotifyIcon, true)) {
		// v0.10 使用 showTrayIcon
		JsonHelper::ReadBool(root, "showTrayIcon", _isShowNotifyIcon);
	}
	JsonHelper::ReadBool(root, "smoothMotionCompatibilityMode", _isSmoothMotionCompatibilityMode);
	JsonHelper::ReadBool(root, "inlineParams", _isInlineParams);
	JsonHelper::ReadBool(root, "autoCheckForUpdates", _isAutoCheckForUpdates);
	JsonHelper::ReadBool(root, "checkForPreviewUpdates", _isCheckForPreviewUpdates);
	{
		int64_t d = 0;
		JsonHelper::ReadInt64(root, "updateCheckDate", d);

		using std::chrono::system_clock;
		_updateCheckDate = system_clock::time_point(system_clock::duration(d));
	}
	{
		uint32_t duplicateFrameDetectionMode = (uint32_t)DuplicateFrameDetectionMode::Never;
		JsonHelper::ReadUInt(root, "duplicateFrameDetectionMode", duplicateFrameDetectionMode);
		if (duplicateFrameDetectionMode > 2) {
			duplicateFrameDetectionMode = (uint32_t)DuplicateFrameDetectionMode::Never;
		}
		_duplicateFrameDetectionMode = (::Magpie::DuplicateFrameDetectionMode)duplicateFrameDetectionMode;
	}
	JsonHelper::ReadBool(root, "enableStatisticsForDynamicDetection", _isStatisticsForDynamicDetectionEnabled);
	_isSimpleFrameRefresh = true;
	JsonHelper::ReadBool(root, "simpleFrameRefresh", _isSimpleFrameRefresh);
	float legacyIdle = 10.0f;
	JsonHelper::ReadFloat(root, "minFrameRate", legacyIdle);
	if (!std::isfinite(legacyIdle) || legacyIdle < 0 || legacyIdle > 1000) legacyIdle = 10.0f;
	const FrameSyncSettings legacyFrameSync = ReadProfileFrameSync(root);
	uint32_t refreshVersion = 0;
	JsonHelper::ReadUInt(root, "experimentalFrameRefreshVersion", refreshVersion);
	const bool migrateLegacyRefresh = refreshVersion < 1;
	if (migrateLegacyRefresh) _isConfigMigrationNeeded = true;
	_defaultProfile.frameRefresh = ReadProfileFrameRefresh(root, legacyFrameSync, legacyIdle, migrateLegacyRefresh);
	_defaultProfile.rtss = ReadProfileRtss(root);
	uint32_t profileFrameSyncVersion = 0;
	JsonHelper::ReadUInt(root, "experimentalProfileFrameSyncVersion", profileFrameSyncVersion);
	if (profileFrameSyncVersion < 1 || root.HasMember("frontEdgeSync") ||
		root.HasMember("frontEdgeSyncFrameRate") || root.HasMember("frameSyncMode")) {
		_isConfigMigrationNeeded = true;
		Logger::Get().Info("Migrating frame sync settings to independent profiles (version 1)");
	}
	// Migrate the former global choice only while loading existing profiles.
	bool legacyParameterFocusSwitching = false;
	JsonHelper::ReadBool(root, "parameterFocusSwitching", legacyParameterFocusSwitching);
	_defaultProfile.isParameterFocusSwitchingEnabled = legacyParameterFocusSwitching;
	if (root.HasMember("parameterFocusSwitching")) _isConfigMigrationNeeded = true;
	_isStopEffectsOnTaskSwitchEnabled = false;
	JsonHelper::ReadBool(root, "stopEffectsOnTaskSwitch", _isStopEffectsOnTaskSwitchEnabled);
	JsonHelper::ReadBool(root, "vrr", _isVRREnabled);
	JsonHelper::ReadBool(root, "disableFP16", _isFP16Disabled);

	[[maybe_unused]] bool result = ScalingModesService::Get().Import(root, true);
	assert(result);
	if (_experimentalDepthRemovalVersion < 1) {
		Logger::Get().Info(fmt::format(
			"Frame Guidance config migration v{}->v1: learned-depth settings normalized",
			_experimentalDepthRemovalVersion));
		_experimentalDepthRemovalVersion = 1;
		_isConfigMigrationNeeded = true;
	}

	if (_experimentalDlssnrSettingsVersion < 2) {
		uint32_t removedLegacyPresets = 0;
		for (ScalingMode& scalingMode : _scalingModes) {
			for (EffectItem& effect : scalingMode.effects) {
				if (effect.name == L"DLSSNR\\DLSSNR_AI_Filter") {
					removedLegacyPresets +=
						static_cast<uint32_t>(effect.parameters.erase(L"preset"));
				}
			}
		}

		Logger::Get().Info(fmt::format(
			"DLSSNR config migration v{}->v2: removed {} legacy preset value(s); "
			"the v0.5.7 nrPreset starts at Default (0)",
			_experimentalDlssnrSettingsVersion,
			removedLegacyPresets));
	}

	if (_experimentalDlssnrSettingsVersion < EXPERIMENTAL_DLSSNR_SETTINGS_VERSION) {
		_experimentalDlssnrSettingsVersion = EXPERIMENTAL_DLSSNR_SETTINGS_VERSION;
		_isConfigMigrationNeeded = true;
	}

	if (_experimentalDlssSrSettingsVersion < EXPERIMENTAL_DLSS_SR_SETTINGS_VERSION) {
		uint32_t migratedEffects = 0;
		for (ScalingMode& scalingMode : _scalingModes) {
			for (EffectItem& effect : scalingMode.effects) {
				if (effect.name == L"DLSS\\DLSS_ZeroMV") {
					effect.name = L"DLSS\\DLSS_SR";
					++migratedEffects;
				}
			}
		}
		Logger::Get().Info(fmt::format(
			"DLSS SR config migration v{}->v1: renamed {} effect identifier(s); "
			"parameters and scaling types preserved",
			_experimentalDlssSrSettingsVersion, migratedEffects));
		_experimentalDlssSrSettingsVersion = EXPERIMENTAL_DLSS_SR_SETTINGS_VERSION;
		_isConfigMigrationNeeded = true;
	}

	auto scaleProfilesNode = root.FindMember("profiles");
	if (scaleProfilesNode == root.MemberEnd()) {
		// v0.10.0-preview1 使用 scalingProfiles
		scaleProfilesNode = root.FindMember("scalingProfiles");
	}
	if (scaleProfilesNode != root.MemberEnd() && scaleProfilesNode->value.IsArray()) {
		auto scaleProfilesArray = scaleProfilesNode->value.GetArray();

		const rapidjson::SizeType size = scaleProfilesArray.Size();
		if (size > 0) {
			if (scaleProfilesArray[0].IsObject()) {
				// 解析默认缩放配置不会失败
				_LoadProfile(scaleProfilesArray[0].GetObj(), _defaultProfile, true, legacyParameterFocusSwitching, legacyFrameSync, legacyIdle, migrateLegacyRefresh);
			}

			if (size > 1) {
				_profiles.reserve((size_t)size - 1);
				for (rapidjson::SizeType i = 1; i < size; ++i) {
					if (!scaleProfilesArray[i].IsObject()) {
						continue;
					}

					Profile& rule = _profiles.emplace_back();
					if (!_LoadProfile(scaleProfilesArray[i].GetObj(), rule, false, legacyParameterFocusSwitching, legacyFrameSync, legacyIdle, migrateLegacyRefresh)) {
						_profiles.pop_back();
						continue;
					}
				}
			}
		}
	}

	_screenshotContent = ::Magpie::ScreenshotContent::OutputOnly;
	auto overlayNode = root.FindMember("overlay");
	if (overlayNode != root.MemberEnd() && overlayNode->value.IsObject()) {
		auto overlayObj = overlayNode->value.GetObj();
		uint32_t screenshotContent = static_cast<uint32_t>(::Magpie::ScreenshotContent::OutputOnly);
		JsonHelper::ReadUInt(overlayObj, "screenshotContent", screenshotContent, true);
		_screenshotContent = NormalizeScreenshotContent(screenshotContent);

		uint32_t initialToolbarState = (uint32_t)ToolbarState::AutoHide;
		if (JsonHelper::ReadUInt(overlayObj, "fullscreenInitialToolbarState", initialToolbarState, true)) {
			if (initialToolbarState >= (uint32_t)ToolbarState::COUNT) {
				initialToolbarState = (uint32_t)ToolbarState::AutoHide;
			}
			_fullscreenInitialToolbarState = (ToolbarState)initialToolbarState;

			initialToolbarState = (uint32_t)ToolbarState::AutoHide;
			JsonHelper::ReadUInt(overlayObj, "windowedInitialToolbarState", initialToolbarState);
			if (initialToolbarState >= (uint32_t)ToolbarState::COUNT) {
				initialToolbarState = (uint32_t)ToolbarState::AutoHide;
			}
			_windowedInitialToolbarState = (ToolbarState)initialToolbarState;
		} else {
			// v0.12.0-preview1 中工具栏初始状态不区分全屏和窗口模式缩放
			JsonHelper::ReadUInt(overlayObj, "initialToolbarState", initialToolbarState);
			if (initialToolbarState >= (uint32_t)ToolbarState::COUNT) {
				initialToolbarState = (uint32_t)ToolbarState::AutoHide;
			}
			_fullscreenInitialToolbarState = (ToolbarState)initialToolbarState;
			_windowedInitialToolbarState = (ToolbarState)initialToolbarState;
		}

		{
			std::wstring value;
			JsonHelper::ReadString(overlayObj, "screenshotsDir", value);
			_screenshotsDir = std::move(value);
		}

		const auto profilerNode = overlayObj.FindMember("profiler");
		if (profilerNode != overlayObj.MemberEnd() && profilerNode->value.IsObject()) {
			ReadOverlayProfilerOptions(profilerNode->value.GetObj(), _overlayOptions.profiler);
		}

		auto windowsNode = overlayObj.FindMember("windows");
		if (windowsNode != overlayObj.MemberEnd() && windowsNode->value.IsObject()) {
			auto windowsObj = windowsNode->value.GetObj();

			const rapidjson::SizeType size = windowsObj.MemberCount();
			if (size > 0) {
				_overlayOptions.windows.reserve(size);

				for (const auto& windowOptionPair : windowsObj) {
					if (!windowOptionPair.value.IsObject()) {
						continue;
					}

					auto windowOptionObj = windowOptionPair.value.GetObj();

					OverlayWindowOption& windowOption = _overlayOptions.windows[windowOptionPair.name.GetString()];
					JsonHelper::ReadUInt16(windowOptionObj, "hArea", windowOption.hArea);
					JsonHelper::ReadUInt16(windowOptionObj, "vArea", windowOption.vArea);
					JsonHelper::ReadFloat(windowOptionObj, "hPos", windowOption.hPos);
					JsonHelper::ReadFloat(windowOptionObj, "vPos", windowOption.vPos);
					JsonHelper::ReadFloat(windowOptionObj, "width", windowOption.width);
					JsonHelper::ReadFloat(windowOptionObj, "height", windowOption.height);
					JsonHelper::ReadFloat(windowOptionObj, "zoom", windowOption.zoom);
					SanitizeOverlayWindowOption(windowOption);
				}
			}
		}
	}
}

bool AppSettings::_LoadProfile(
	const rapidjson::GenericObject<true, rapidjson::Value>& profileObj,
	Profile& profile,
	bool isDefault,
	bool legacyParameterFocusSwitching,
	const FrameSyncSettings& legacyFrameSync,
	float legacyIdle,
	bool migrateLegacyRefresh
) const noexcept {
	profile.frameRefresh = ReadProfileFrameRefresh(profileObj, legacyFrameSync, legacyIdle, migrateLegacyRefresh);
	profile.rtss = ReadProfileRtss(profileObj);
	profile.isParameterFocusSwitchingEnabled = legacyParameterFocusSwitching;
	JsonHelper::ReadBool(profileObj, "parameterFocusSwitching", profile.isParameterFocusSwitchingEnabled);
	{
		uint32_t fullscreen = 0, windowed = 0;
		JsonHelper::ReadUInt(profileObj, "fullscreenToolbarDock", fullscreen, true);
		JsonHelper::ReadUInt(profileObj, "windowedToolbarDock", windowed, true);
		profile.toolbarDocks = { SanitizeToolbarDock(fullscreen), SanitizeToolbarDock(windowed) };
	}
	if (!isDefault) {
		if (!JsonHelper::ReadString(profileObj, "name", profile.name, true)) {
			return false;
		}

		{
			std::wstring_view nameView(profile.name);
			StrHelper::Trim(nameView);
			if (nameView.empty()) {
				return false;
			}
		}

		if (!JsonHelper::ReadBool(profileObj, "packaged", profile.isPackaged, true)) {
			return false;
		}

		if (!JsonHelper::ReadString(profileObj, "pathRule", profile.pathRule, true)
			|| profile.pathRule.empty()) {
			return false;
		}

		if (!JsonHelper::ReadString(profileObj, "classNameRule", profile.classNameRule, true)
			|| profile.classNameRule.empty()) {
			return false;
		}

		{
			std::wstring value;
			JsonHelper::ReadString(profileObj, "launcherPath", value);
			profile.launcherPath = std::move(value);
		}
		
		// 将旧版本的相对路径转换为绝对路径
		if (!profile.launcherPath.empty() && profile.launcherPath.is_relative()) {
			std::filesystem::path exePath(profile.pathRule);
			profile.launcherPath = (exePath.parent_path() / profile.launcherPath).lexically_normal();
		}

		{
			auto autoScaleNode = profileObj.FindMember("autoScale");
			if (autoScaleNode != profileObj.MemberEnd()) {
				if (autoScaleNode->value.IsUint()) {
					uint32_t value = autoScaleNode->value.GetUint();
					if (value >= (uint32_t)AutoScale::COUNT) {
						value = (uint32_t)AutoScale::Disabled;
					}
					profile.autoScale = (AutoScale)value;
				} else if (autoScaleNode->value.IsBool()) {
					// v0.12 前为布尔值
					profile.autoScale = autoScaleNode->value.GetBool() ?
						AutoScale::Fullscreen : AutoScale::Disabled;
				}
			}
		}
		
		JsonHelper::ReadString(profileObj, "launchParameters", profile.launchParameters);
	}

	JsonHelper::ReadInt(profileObj, "scalingMode", profile.scalingMode);
	if (profile.scalingMode < -1 || profile.scalingMode >= (int)_scalingModes.size()) {
		profile.scalingMode = -1;
	}

	{
		uint32_t captureMethod = (uint32_t)CaptureMethod::GraphicsCapture;
		if (!JsonHelper::ReadUInt(profileObj, "captureMethod", captureMethod, true)) {
			// v0.10.0-preview1 使用 captureMode
			JsonHelper::ReadUInt(profileObj, "captureMode", captureMethod);
		}
		
		if (captureMethod >= (uint32_t)CaptureMethod::COUNT) {
			captureMethod = (uint32_t)CaptureMethod::GraphicsCapture;
		} else if (captureMethod == (uint32_t)CaptureMethod::DesktopDuplication) {
			// Desktop Duplication 捕获模式要求 Win10 20H1+
			if (!Win32Helper::GetOSVersion().Is20H1OrNewer()) {
				captureMethod = (uint32_t)CaptureMethod::GraphicsCapture;
			}
		}
		profile.captureMethod = (CaptureMethod)captureMethod;
	}

	{
		uint32_t multiMonitorUsage = (uint32_t)MultiMonitorUsage::Closest;
		JsonHelper::ReadUInt(profileObj, "multiMonitorUsage", multiMonitorUsage);
		if (multiMonitorUsage >= (uint32_t)MultiMonitorUsage::COUNT) {
			multiMonitorUsage = (uint32_t)MultiMonitorUsage::Closest;
		}
		profile.multiMonitorUsage = (MultiMonitorUsage)multiMonitorUsage;
	}
	JsonHelper::ReadString(profileObj, "preferredMonitorId", profile.preferredMonitorId, true);
	JsonHelper::ReadString(profileObj, "preferredMonitorName", profile.preferredMonitorName, true);

	{
		uint32_t factor = (uint32_t)InitialWindowedScaleFactor::Auto;
		JsonHelper::ReadUInt(profileObj, "initialWindowedScaleFactor", factor);
		if (factor >= (uint32_t)InitialWindowedScaleFactor::COUNT) {
			factor = (uint32_t)InitialWindowedScaleFactor::Auto;
		}
		profile.initialWindowedScaleFactor = (InitialWindowedScaleFactor)factor;
	}

	JsonHelper::ReadFloat(profileObj, "customInitialWindowedScaleFactor",
		profile.customInitialWindowedScaleFactor);
	if (profile.customInitialWindowedScaleFactor < 1.0f) {
		profile.customInitialWindowedScaleFactor = 1.0f;
	}
	
	{
		auto graphicsCardIdNode = profileObj.FindMember("graphicsCardId");
		if (graphicsCardIdNode == profileObj.end()) {
			// v0.10 和 v0.11 只使用索引
			int graphicsCardIdx = -1;
			if (!JsonHelper::ReadInt(profileObj, "graphicsCard", graphicsCardIdx, true)) {
				// v0.10.0-preview1 使用 graphicsAdapter
				uint32_t graphicsAdater = 0;
				JsonHelper::ReadUInt(profileObj, "graphicsAdapter", graphicsAdater);
				graphicsCardIdx = (int)graphicsAdater - 1;
			}

			// 稍后由 ProfileService 设置 vendorId 和 deviceId
			profile.graphicsCardId.idx = graphicsCardIdx;
		} else if (graphicsCardIdNode->value.IsObject()) {
			auto graphicsCardIdObj = graphicsCardIdNode->value.GetObj();

			auto idxNode = graphicsCardIdObj.FindMember("idx");
			if (idxNode != graphicsCardIdObj.end() && idxNode->value.IsInt()) {
				profile.graphicsCardId.idx = idxNode->value.GetInt();
			}

			auto vendorIdNode = graphicsCardIdObj.FindMember("vendorId");
			if (vendorIdNode != graphicsCardIdObj.end() && vendorIdNode->value.IsUint()) {
				profile.graphicsCardId.vendorId = vendorIdNode->value.GetUint();
			}

			auto deviceIdNode = graphicsCardIdObj.FindMember("deviceId");
			if (deviceIdNode != graphicsCardIdObj.end() && deviceIdNode->value.IsUint()) {
				profile.graphicsCardId.deviceId = deviceIdNode->value.GetUint();
			}
		}
	}


	JsonHelper::ReadBoolFlag(profileObj, "3DGameMode", ScalingFlags::Is3DGameMode, profile.scalingFlags);
	if (!JsonHelper::ReadBoolFlag(profileObj, "captureTitleBar", ScalingFlags::CaptureTitleBar, profile.scalingFlags, true)) {
		// v0.10.0-preview1 使用 reserveTitleBar
		JsonHelper::ReadBoolFlag(profileObj, "reserveTitleBar", ScalingFlags::CaptureTitleBar, profile.scalingFlags);
	}
	JsonHelper::ReadBoolFlag(profileObj, "adjustCursorSpeed", ScalingFlags::AdjustCursorSpeed, profile.scalingFlags);
	JsonHelper::ReadBoolFlag(profileObj, "disableDirectFlip", ScalingFlags::DisableDirectFlip, profile.scalingFlags);
	JsonHelper::ReadBoolFlag(profileObj, "enableHdrCompatibility", ScalingFlags::EnableHdrCompatibility, profile.scalingFlags);

	{
		uint32_t cursorScaling = (uint32_t)CursorScaling::NoScaling;
		JsonHelper::ReadUInt(profileObj, "cursorScaling", cursorScaling);
		if (cursorScaling >= (uint32_t)CursorScaling::COUNT) {
			cursorScaling = (uint32_t)CursorScaling::NoScaling;
		}
		profile.cursorScaling = (CursorScaling)cursorScaling;
	}
	
	JsonHelper::ReadFloat(profileObj, "customCursorScaling", profile.customCursorScaling);
	if (profile.customCursorScaling < 0) {
		profile.customCursorScaling = 1.0f;
	}

	{
		uint32_t cursorInterpolationMode = (uint32_t)CursorInterpolationMode::NearestNeighbor;
		JsonHelper::ReadUInt(profileObj, "cursorInterpolationMode", cursorInterpolationMode);
		if (cursorInterpolationMode >= (uint32_t)CursorInterpolationMode::COUNT) {
			cursorInterpolationMode = (uint32_t)CursorInterpolationMode::NearestNeighbor;
		}
		profile.cursorInterpolationMode = (CursorInterpolationMode)cursorInterpolationMode;
	}

	JsonHelper::ReadBool(profileObj, "autoHideCursorEnabled", profile.isAutoHideCursorEnabled);
	JsonHelper::ReadFloat(profileObj, "autoHideCursorDelay", profile.autoHideCursorDelay);
	if (profile.autoHideCursorDelay <= 0.1f - FLOAT_EPSILON<float> ||
		profile.autoHideCursorDelay >= 5.0f + FLOAT_EPSILON<float>)
	{
		profile.autoHideCursorDelay = 3.0f;
	}

	JsonHelper::ReadBool(profileObj, "croppingEnabled", profile.isCroppingEnabled);

	auto croppingNode = profileObj.FindMember("cropping");
	if (croppingNode != profileObj.MemberEnd() && croppingNode->value.IsObject()) {
		auto croppingObj = croppingNode->value.GetObj();

		if (!JsonHelper::ReadFloat(croppingObj, "left", profile.cropping.Left, true)
			|| profile.cropping.Left < 0
			|| !JsonHelper::ReadFloat(croppingObj, "top", profile.cropping.Top, true)
			|| profile.cropping.Top < 0
			|| !JsonHelper::ReadFloat(croppingObj, "right", profile.cropping.Right, true)
			|| profile.cropping.Right < 0
			|| !JsonHelper::ReadFloat(croppingObj, "bottom", profile.cropping.Bottom, true)
			|| profile.cropping.Bottom < 0
		) {
			profile.cropping = {};
		}
	}

	{
		uint32_t destAlignment = (uint32_t)DestAlignment::Center;
		JsonHelper::ReadUInt(profileObj, "destAlignment", destAlignment);
		if (destAlignment >= (uint32_t)DestAlignment::COUNT) {
			destAlignment = (uint32_t)DestAlignment::Center;
		}
		profile.destAlignment = (DestAlignment)destAlignment;
	}

	return true;
}

bool AppSettings::_LoadShortcuts(const rapidjson::GenericObject<true, rapidjson::Value>& root) noexcept {
	// Missing fields inherit defaults; present zero values explicitly remove them.
	_SetDefaultShortcuts();
	auto node = root.FindMember("shortcuts");
	if (node == root.MemberEnd()) node = root.FindMember("hotkeys");
	if (node == root.MemberEnd() || !node->value.IsObject()) return true;

	const auto shortcuts = static_cast<const rapidjson::Value&>(node->value).GetObj();
	bool changed = false;
	auto read = [&](const char* key, ShortcutAction action, const char* legacyKey = nullptr) {
		auto field = shortcuts.FindMember(key);
		if (field == shortcuts.MemberEnd() && legacyKey) field = shortcuts.FindMember(legacyKey);
		if (field == shortcuts.MemberEnd() || !field->value.IsUint() || field->value.GetUint() > 0xfff) {
			changed = true;
			return;
		}
		DecodeShortcut(field->value.GetUint(), _shortcuts[(size_t)action]);
	};
	read("scale", ShortcutAction::Scale);
	read("windowedModeScale", ShortcutAction::WindowedModeScale);
	read("toolbar", ShortcutAction::Toolbar, "overlay");
	read("profiler", ShortcutAction::Profiler);
	read("effectParameters", ShortcutAction::EffectParameters);
	read("screenshot", ShortcutAction::Screenshot);
	read("toolbarPin", ShortcutAction::ToolbarPin);
	read("comparison", ShortcutAction::Comparison);
	return changed;
}

void AppSettings::_SetDefaultShortcuts() noexcept {
	constexpr std::pair<ShortcutAction, uint8_t> defaults[] = {
		{ ShortcutAction::Scale, 'A' },
		{ ShortcutAction::WindowedModeScale, 'Q' },
		{ ShortcutAction::Toolbar, 'D' },
		{ ShortcutAction::Profiler, 'P' },
		{ ShortcutAction::EffectParameters, 'E' },
		{ ShortcutAction::Screenshot, 'S' },
		{ ShortcutAction::ToolbarPin, 'F' },
		{ ShortcutAction::Comparison, 'C' }
	};
	for (const auto& [action, code] : defaults) {
		_shortcuts[(size_t)action] = { .code = code, .alt = true, .shift = true };
	}
}

void AppSettings::_SetDefaultScalingModes() noexcept {
	_scalingModes.resize(6);

	// Lanczos
	{
		auto& lanczos = _scalingModes[0];
		lanczos.name = L"Lanczos";

		auto& lanczosEffect = lanczos.effects.emplace_back();
		lanczosEffect.name = L"Lanczos";
		lanczosEffect.scalingType = ::Magpie::ScalingType::Fit;
	}
	// FSR
	{
		auto& fsr = _scalingModes[1];
		fsr.name = L"FSR";

		fsr.effects.resize(2);
		auto& easu = fsr.effects[0];
		easu.name = L"FSR\\FSR_EASU";
		easu.scalingType = ::Magpie::ScalingType::Fit;
		auto& rcas = fsr.effects[1];
		rcas.name = L"FSR\\FSR_RCAS";
		rcas.parameters[L"sharpness"] = 0.87f;
	}
	// RTX Video VSR Ultra
	{
		auto& vsrUltra = _scalingModes[2];
		vsrUltra.name = L"RTX Video VSR Ultra";
		vsrUltra.effects.resize(2);
		vsrUltra.effects[0].name = L"FrameRate_Filter";
		auto& vsrEffect = vsrUltra.effects[1];
		vsrEffect.name = L"RTXVideo\\RTXVideo_VSR";
		vsrEffect.parameters[L"strength"] = 1.0f;
		vsrEffect.scalingType = ::Magpie::ScalingType::Fit;
	}
	// DLSS Frame Generation
	{
		auto& dlssFg = _scalingModes[3];
		dlssFg.name = L"DLSSFG";
		dlssFg.effects.resize(2);
		dlssFg.effects[0].name = L"FrameRate_Filter";
		dlssFg.effects[1].name = L"DLSSFG\\DLSS_FrameGeneration";
	}
	// XeSS Frame Generation
	{
		auto& xessFg = _scalingModes[4];
		xessFg.name = L"XeSSFG";
		xessFg.effects.resize(2);
		xessFg.effects[0].name = L"FrameRate_Filter";
		xessFg.effects[1].name = L"XeSSFG\\XeSS_FrameGeneration";
	}
	// DLSS Ray Reconstruction
	{
		auto& dlssNr = _scalingModes[5];
		dlssNr.name = L"DLSSNR";
		dlssNr.effects.resize(1);
		dlssNr.effects[0].name = L"DLSSNR\\DLSSNR_AI_Filter";
		dlssNr.effects[0].parameters[L"resolutionControl"] = 0.f;
		dlssNr.effects[0].parameters[L"__dlssnrModeVersion"] = DLSSNR_MODE_VERSION;
	}

	// 全局缩放模式默认为 Lanczos
	_defaultProfile.scalingMode = 0;
}

void AppSettings::ResetScalingModes() noexcept {
	_scalingModes.clear();
	for (Profile& profile : _profiles) {
		// 自定义配置在原缩放模式被删除后回退到全局默认值。
		profile.scalingMode = -1;
	}
	_SetDefaultScalingModes();
	SaveAsync();
}

bool AppSettings::_UpdateConfigPath(std::filesystem::path* existingConfigPath) noexcept {
	wil::unique_cotaskmem_string localAppData;
	const HRESULT hr = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, localAppData.put());
	if (FAILED(hr)) {
		Logger::Get().ComError("SHGetKnownFolderPath failed", hr);
		SetLastError(HRESULT_FACILITY(hr) == FACILITY_WIN32 ? HRESULT_CODE(hr) : static_cast<DWORD>(hr));
		return false;
	}
	const auto exeDirectory = std::filesystem::path(Win32Helper::GetExePath()).parent_path();
	if (existingConfigPath) {
		const auto selected = ConfigLocations::Select(exeDirectory, localAppData.get());
		_configPath = selected.destination;
		_configDir = _configPath.parent_path();
		if (selected.error) {
			SetLastError(selected.error);
			Logger::Get().Win32Error("Inspect configuration location failed");
			SetLastError(selected.error);
			return false;
		}
		_isPortableMode = selected.portable;
		*existingConfigPath = selected.source;
	} else {
		_configDir = ConfigLocations::Directory(exeDirectory, localAppData.get(), _isPortableMode);
		_configPath = _configDir / CommonSharedConstants::CONFIG_FILENAME;
	}

	// 确保配置文件夹存在
	if (!Win32Helper::CreateDir(_configDir.native(), true)) {
		const DWORD error = GetLastError();
		Logger::Get().Win32Error("创建配置文件夹失败");
		SetLastError(error);
		return false;
	}

	return true;
}

}
