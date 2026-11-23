#include "pch.h"
#include "ProfileViewModel.h"
#if __has_include("ProfileViewModel.g.cpp")
#include "ProfileViewModel.g.cpp"
#endif
#include "XamlHelper.h"
#include "Profile.h"
#include "AppXReader.h"
#include "IconHelper.h"
#include "ProfileService.h"
#include "StrHelper.h"
#include "Win32Helper.h"
#include "AppSettings.h"
#include "Logger.h"
#include "ScalingMode.h"
#include "ScalingService.h"
#include "FileDialogHelper.h"
#include "CommonSharedConstants.h"
#include "App.h"
#include "MainWindow.h"
#include "AdaptersService.h"
#include <limits>

using namespace ::Magpie;
using namespace winrt;
using namespace Windows::Graphics::Display;
using namespace Windows::Graphics::Imaging;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::UI::Xaml::Media::Imaging;

namespace winrt::Magpie::implementation {

ProfileViewModel::ProfileViewModel(int profileIdx, bool initializePageData)
	: _isDefaultProfile(profileIdx < 0) {
	if (_isDefaultProfile) {
		_data = &ProfileService::Get().DefaultProfile();
	} else {
		_index = (uint32_t)profileIdx;
		_data = &ProfileService::Get().GetProfile(profileIdx);

		// 占位
		if (initializePageData) {
			_icon = FontIcon();

			_appThemeChangedRevoker = App::Get().ThemeChanged(auto_revoke, [this](bool) { _LoadIcon(); });
			_dpiChangedRevoker = App::Get().MainWindow().DpiChanged(
				auto_revoke, [this](uint32_t) { _LoadIcon(); });
		}

		if (_data->isPackaged) {
			AppXReader appxReader;
			_isProgramExist = appxReader.Initialize(_data->pathRule);
		} else {
			_isProgramExist = Win32Helper::FileExists(_data->pathRule.c_str());
		}

		if (initializePageData) {
			_LoadIcon();
		}
	}

	if (!initializePageData) {
		return;
	}

	_frameSyncChangedRevoker = ProfileService::Get().FrameRefreshChanged(auto_revoke, [this](const Profile& profile) {
		if (profile.runtimeIdentity != _data->runtimeIdentity) return;
		_NotifyFrameRefreshChanged();
	});
	_frameRefreshModeChangedRevoker = AppSettings::Get().FrameRefreshModeChanged(auto_revoke, [this] {
		_NotifyFrameRefreshChanged();
		_LoadRtssPreview();
	});
	_BuildMonitorOptions();
	_BuildGraphicsCards();
	_rtssStatusRevoker = RtssService::Get().StatusChanged(auto_revoke, [this] {
		_NotifyRtssChanged();
		if (RtssService::Get().Status().profileIdentity.lock() != _data->runtimeIdentity) _LoadRtssPreview();
	});
	_scalingStatusRevoker = ScalingService::Get().IsScalingChanged(auto_revoke, [this](bool) { _NotifyRtssChanged(); });
	_LoadRtssPreview();

	_adaptersChangedRevoker = AdaptersService::Get().AdaptersChanged(auto_revoke,
		std::bind_front(&ProfileViewModel::_RefreshGraphicsCardSettings, this));
}

ProfileViewModel::~ProfileViewModel() {}

void ProfileViewModel::Rebind(uint32_t index) noexcept {
	if (_isDefaultProfile) {
		return;
	}

	_index = index;
	_data = &ProfileService::Get().GetProfile(index);
	RaisePropertyChanged(L"Name");
	RaisePropertyChanged(L"CanDrag");
	RaisePropertyChanged(L"CanMoveUp");
	RaisePropertyChanged(L"CanMoveDown");
}

bool ProfileViewModel::IsNotDefaultProfile() const noexcept {
	return _data && !_data->name.empty();
}

bool ProfileViewModel::IsNotPackaged() const noexcept {
	return _data && !_data->isPackaged;
}

fire_and_forget ProfileViewModel::OpenProgramLocation() const noexcept {
	if (!_data || !_isProgramExist) {
		co_return;
	}

	std::wstring programLocation;
	if (_data->isPackaged) {
		AppXReader appxReader;
		[[maybe_unused]] bool result = appxReader.Initialize(_data->pathRule);
		assert(result);

		programLocation = appxReader.GetExecutablePath();
		if (programLocation.empty()) {
			// 找不到可执行文件则打开应用文件夹
			Win32Helper::ShellOpen(appxReader.GetPackagePath().c_str());
			co_return;
		}
	} else {
		programLocation = _data->pathRule;
	}

	co_await resume_background();
	Win32Helper::OpenFolderAndSelectFile(programLocation.c_str());
}

static std::wstring ExtractFolder(const std::wstring& path) noexcept {
	const size_t delimPos = path.find_last_of(L'\\');
	if (delimPos == std::wstring::npos) {
		assert(false);
		return {};
	}

	std::wstring result = path.substr(0, delimPos);
	if (Win32Helper::DirExists(result.c_str())) {
		return result;
	} else {
		return {};
	}
}

static std::wstring GetStartFolderForSettingLauncher(const Profile& profile) noexcept {
	if (profile.launcherPath.empty()) {
		// 无启动器
		return ExtractFolder(profile.pathRule);
	}

	std::wstring result = ExtractFolder(profile.launcherPath);
	if (result.empty()) {
		// 启动器路径失效
		return ExtractFolder(profile.pathRule);
	} else {
		return result;
	}
}

fire_and_forget ProfileViewModel::ChangeExeForLaunching() noexcept {
	if (!_data || !_isProgramExist || _data->isPackaged) {
		co_return;
	}

	const ResourceLoader resourceLoader =
		ResourceLoader::GetForCurrentView(CommonSharedConstants::APP_RESOURCE_MAP_ID);
	const hstring titleStr = ResourceLoader::GetForCurrentView(CommonSharedConstants::APP_RESOURCE_MAP_ID)
		.GetString(L"Dialog_SelectLauncher_Title");
	const hstring exeFileStr = ResourceLoader::GetForCurrentView(CommonSharedConstants::APP_RESOURCE_MAP_ID)
		.GetString(L"Dialog_ExeFile");

	std::wstring startFolder = GetStartFolderForSettingLauncher(*_data);

	auto weakThis = get_weak();

	// 在主线程使用 IFileOpenDialog 有些问题，尤其在 Win10 中
	co_await resume_background();

	com_ptr<IFileOpenDialog> fileDialog = try_create_instance<IFileOpenDialog>(CLSID_FileOpenDialog);
	if (!fileDialog) {
		Logger::Get().Error("创建 FileSaveDialog 失败");
		co_return;
	}

	fileDialog->SetTitle(titleStr.c_str());
	{
		const COMDLG_FILTERSPEC fileType{ exeFileStr.c_str(), L"*.exe" };
		fileDialog->SetFileTypes(1, &fileType);
	}
	fileDialog->SetDefaultExtension(L"exe");

	if (!startFolder.empty()) {
		com_ptr<IShellItem> shellItem;
		SHCreateItemFromParsingName(startFolder.c_str(), nullptr, IID_PPV_ARGS(&shellItem));
		fileDialog->SetFolder(shellItem.get());
	}

	// GH#1158
	// 选择 exe 的快捷方式时 IFileOpenDialog 默认解析它指向的文件路径，导致参数丢失。
	// FOS_NODEREFERENCELINKS 可以禁止解析，直接返回 lnk 文件。
	std::optional<std::filesystem::path> launcherPath = FileDialogHelper::OpenFileDialog(
		fileDialog.get(), FOS_STRICTFILETYPES | FOS_NODEREFERENCELINKS);
	if (!launcherPath || launcherPath->empty() || *launcherPath == _data->pathRule) {
		co_return;
	}

	co_await App::Get().Dispatcher();

	if (weakThis.get()) {
		_data->launcherPath = std::move(*launcherPath);
		AppSettings::Get().SaveAsync();
	}
}

hstring ProfileViewModel::Name() const noexcept {
	if (!_data) {
		return {};
	}

	if (_data->name.empty()) {
		return ResourceLoader::GetForCurrentView(CommonSharedConstants::APP_RESOURCE_MAP_ID)
			.GetString(L"Root_Defaults/Content");
	} else {
		return hstring(_data->name);
	}
}

static void LaunchPackagedApp(const Profile& profile) noexcept {
	// 关于启动打包应用的讨论:
	// https://github.com/microsoft/WindowsAppSDK/issues/2856#issuecomment-1224409948
	// 使用 CLSCTX_LOCAL_SERVER 以在独立的进程中启动应用
	// 见 https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nn-shobjidl_core-iapplicationactivationmanager
	com_ptr<IApplicationActivationManager> aam =
		try_create_instance<IApplicationActivationManager>(CLSID_ApplicationActivationManager, CLSCTX_LOCAL_SERVER);
	if (!aam) {
		Logger::Get().Error("创建 ApplicationActivationManager 失败");
		return;
	}

	// 确保启动为前台窗口
	HRESULT hr = CoAllowSetForegroundWindow(aam.get(), nullptr);
	if (FAILED(hr)) {
		Logger::Get().ComError("创建 CoAllowSetForegroundWindow 失败", hr);
	}

	DWORD procId;
	hr = aam->ActivateApplication(profile.pathRule.c_str(), profile.launchParameters.c_str(), AO_NONE, &procId);
	if (FAILED(hr)) {
		Logger::Get().ComError("IApplicationActivationManager::ActivateApplication 失败", hr);
		return;
	}
}

static void LaunchWin32App(const Profile& profile) noexcept {
	const std::wstring& path = !profile.launcherPath.empty() &&
		Win32Helper::FileExists(profile.launcherPath.c_str()) ? profile.launcherPath.native() : profile.pathRule;
	Win32Helper::ShellOpen(path.c_str(), profile.launchParameters.c_str());
}

void ProfileViewModel::Launch() const noexcept {
	if (!_data || !_isProgramExist) {
		return;
	}

	if (_data->isPackaged) {
		LaunchPackagedApp(*_data);
	} else {
		LaunchWin32App(*_data);
	}
}

void ProfileViewModel::RenameText(const hstring& value) {
	if (!_data) {
		return;
	}

	_renameText = value;
	RaisePropertyChanged(L"RenameText");

	_trimedRenameText = value;
	StrHelper::Trim(_trimedRenameText);
	bool newEnabled = !_trimedRenameText.empty() && _trimedRenameText != _data->name;
	if (_isRenameConfirmButtonEnabled != newEnabled) {
		_isRenameConfirmButtonEnabled = newEnabled;
		RaisePropertyChanged(L"IsRenameConfirmButtonEnabled");
	}
}

void ProfileViewModel::Rename() {
	if (_isDefaultProfile || !_data || !_isRenameConfirmButtonEnabled) {
		return;
	}

	ProfileService::Get().RenameProfile(_index, _trimedRenameText);
	RaisePropertyChanged(L"Name");
}

void ProfileViewModel::RenameFlyout_Opening() {
	if (_isDefaultProfile || !_data) {
		return;
	}

	RenameText(Name());
	RaisePropertyChanged(L"RenameTextBoxSelectionStart");
}

void ProfileViewModel::RenameTextBox_KeyDown(
	IInspectable const&,
	Input::KeyRoutedEventArgs const& args) {
	if (args.Key() == VirtualKey::Enter) {
		RenameButton_Click();
	}
}

void ProfileViewModel::RenameButton_Click() {
	if (_isDefaultProfile || !_data || !_isRenameConfirmButtonEnabled) {
		return;
	}

	XamlHelper::ClosePopups(App::Get().RootPage()->XamlRoot());
	Rename();
}

bool ProfileViewModel::CanDrag() const noexcept {
	return !_isDefaultProfile && _data && ProfileService::Get().GetProfileCount() > 1;
}

bool ProfileViewModel::CanMoveUp() const noexcept {
	return CanDrag() && _index > 0;
}

bool ProfileViewModel::CanMoveDown() const noexcept {
	return CanDrag() && _index + 1 < ProfileService::Get().GetProfileCount();
}

void ProfileViewModel::MoveUp() {
	if (CanMoveUp()) ProfileService::Get().MoveProfile(_index, _index - 1);
}

void ProfileViewModel::MoveDown() {
	if (CanMoveDown()) ProfileService::Get().MoveProfile(_index, _index + 1);
}

void ProfileViewModel::Delete() {
	if (_isDefaultProfile || !_data) {
		return;
	}

	auto lifetime = get_strong();
	XamlHelper::ClosePopups(App::Get().RootPage()->XamlRoot());
	ProfileService::Get().RemoveProfile(_index);
	_data = nullptr;
}

IVector<IInspectable> ProfileViewModel::ScalingModes() const noexcept {
	ResourceLoader resourceLoader =
		ResourceLoader::GetForCurrentView(CommonSharedConstants::APP_RESOURCE_MAP_ID);
	
	std::vector<IInspectable> scalingModes;
	scalingModes.push_back(box_value(resourceLoader.GetString(L"Profile_General_ScalingMode_None")));
	for (const ::Magpie::ScalingMode& sm : AppSettings::Get().ScalingModes()) {
		scalingModes.push_back(box_value(sm.name));
	}

	return single_threaded_vector(std::move(scalingModes));
}

int ProfileViewModel::ScalingMode() const noexcept {
	return _data->scalingMode + 1;
}

void ProfileViewModel::ScalingMode(int value) {
	_data->scalingMode = value - 1;
	AppSettings::Get().SaveAsync();

	RaisePropertyChanged(L"ScalingMode");
}

IVector<IInspectable> ProfileViewModel::CaptureMethods() const noexcept {
	ResourceLoader resourceLoader =
		ResourceLoader::GetForCurrentView(CommonSharedConstants::APP_RESOURCE_MAP_ID);

	std::vector<IInspectable> captureMethods;
	captureMethods.reserve(4);
	captureMethods.push_back(box_value(L"Graphics Capture"));
	if (Win32Helper::GetOSVersion().Is20H1OrNewer()) {
		// Desktop Duplication 要求 Win10 20H1+
		captureMethods.push_back(box_value(L"Desktop Duplication"));
	}
	captureMethods.push_back(box_value(L"GDI"));
	captureMethods.push_back(box_value(L"DwmSharedSurface"));

	return single_threaded_vector(std::move(captureMethods));
}

int ProfileViewModel::CaptureMethod() const noexcept {
	if (Win32Helper::GetOSVersion().Is20H1OrNewer() || _data->captureMethod < CaptureMethod::DesktopDuplication) {
		return (int)_data->captureMethod;
	} else {
		return (int)_data->captureMethod - 1;
	}
}

void ProfileViewModel::CaptureMethod(int value) {
	if (value < 0) {
		return;
	}

	if (!Win32Helper::GetOSVersion().Is20H1OrNewer() && value >= (int)CaptureMethod::DesktopDuplication) {
		++value;
	}

	enum CaptureMethod captureMethod = (enum CaptureMethod)value;
	if (_data->captureMethod == captureMethod) {
		return;
	}

	_data->captureMethod = captureMethod;
	RaisePropertyChanged(L"CaptureMethod");
	RaisePropertyChanged(L"IsCaptureMethodDesktopDuplication");
	RaisePropertyChanged(L"CanCaptureTitleBar");

	AppSettings::Get().SaveAsync();
}

bool ProfileViewModel::IsCaptureMethodDesktopDuplication() const noexcept {
	return _data->captureMethod == CaptureMethod::DesktopDuplication;
}

int ProfileViewModel::AutoScale() const noexcept {
	return (int)_data->autoScale;
}

void ProfileViewModel::AutoScale(int value) {
	if (value < 0) {
		return;
	}

	enum AutoScale enumValue = (enum AutoScale)value;
	if (_data->autoScale == enumValue) {
		return;
	}

	_data->autoScale = enumValue;
	AppSettings::Get().SaveAsync();

	RaisePropertyChanged(L"AutoScale");

	if (value) {
		// 立即检查前台窗口是否应自动缩放
		ScalingService::Get().CheckForeground();
	}
}

bool ProfileViewModel::IsParameterFocusSwitchingEnabled() const noexcept {
	return _data->isParameterFocusSwitchingEnabled;
}

void ProfileViewModel::IsParameterFocusSwitchingEnabled(bool value) {
	if (_data->isParameterFocusSwitchingEnabled == value) return;
	_data->isParameterFocusSwitchingEnabled = value;
	AppSettings::Get().SaveAsync();
	RaisePropertyChanged(L"IsParameterFocusSwitchingEnabled");
}

bool ProfileViewModel::Is3DGameMode() const noexcept {
	return _data->Is3DGameMode();
}

void ProfileViewModel::Is3DGameMode(bool value) {
	if (_data->Is3DGameMode() == value) {
		return;
	}

	_data->Is3DGameMode(value);
	AppSettings::Get().SaveAsync();

	RaisePropertyChanged(L"Is3DGameMode");
}

bool ProfileViewModel::IsHdrCompatibilityEnabled() const noexcept {
	return _data->IsHdrCompatibilityEnabled();
}

void ProfileViewModel::IsHdrCompatibilityEnabled(bool value) {
	if (_data->IsHdrCompatibilityEnabled() == value) {
		return;
	}

	_data->IsHdrCompatibilityEnabled(value);
	AppSettings::Get().SaveAsync();

	RaisePropertyChanged(L"IsHdrCompatibilityEnabled");
}

bool ProfileViewModel::HasMultipleMonitors() const noexcept {
	return GetSystemMetrics(SM_CMONITORS) > 1;
}

void ProfileViewModel::_BuildMonitorOptions() {
	ResourceLoader resourceLoader =
		ResourceLoader::GetForCurrentView(CommonSharedConstants::APP_RESOURCE_MAP_ID);
	std::vector<IInspectable> options;
	options.emplace_back(box_value(resourceLoader.GetString(
		L"Profile_General_Multimonitor_Closest/Content")));
	options.emplace_back(box_value(resourceLoader.GetString(
		L"Profile_General_Multimonitor_Intersected/Content")));
	options.emplace_back(box_value(resourceLoader.GetString(
		L"Profile_General_Multimonitor_All/Content")));

	const std::vector<Win32Helper::DisplayMonitorInfo> monitors =
		Win32Helper::GetDisplayMonitors();
	_monitorIds.reserve(monitors.size() + 1);
	_monitorNames.reserve(monitors.size() + 1);
	for (size_t i = 0; i < monitors.size(); ++i) {
		const Win32Helper::DisplayMonitorInfo& monitor = monitors[i];
		const SIZE size = Win32Helper::GetSizeOfRect(monitor.rect);
		options.emplace_back(box_value(hstring(fmt::format(
			L"{} — {}×{} (#{})",
			monitor.friendlyName,
			size.cx,
			size.cy,
			i + 1))));
		_monitorIds.emplace_back(monitor.deviceId);
		_monitorNames.emplace_back(monitor.friendlyName);
	}

	if (_data->multiMonitorUsage == ::Magpie::MultiMonitorUsage::Specific &&
		!_data->preferredMonitorId.empty()) {
		const bool configuredMonitorFound = std::any_of(
			_monitorIds.begin(), _monitorIds.end(), [this](const std::wstring& id) {
				return CompareStringOrdinal(
					id.c_str(), (int)id.size(),
					_data->preferredMonitorId.c_str(),
					(int)_data->preferredMonitorId.size(), TRUE) == CSTR_EQUAL;
			});
		if (!configuredMonitorFound) {
			const hstring unavailable = resourceLoader.GetString(
				L"Profile_General_Multimonitor_Unavailable");
			const std::wstring name = _data->preferredMonitorName.empty() ?
				_data->preferredMonitorId : _data->preferredMonitorName;
			options.emplace_back(box_value(hstring(fmt::format(
				L"{} ({})", name, unavailable.c_str()))));
			_monitorIds.emplace_back(_data->preferredMonitorId);
			_monitorNames.emplace_back(name);
		}
	}

	_monitorOptions = single_threaded_vector(std::move(options));
}

int ProfileViewModel::MonitorSelection() const noexcept {
	if (_data->multiMonitorUsage != ::Magpie::MultiMonitorUsage::Specific) {
		return (int)_data->multiMonitorUsage;
	}

	for (size_t i = 0; i < _monitorIds.size(); ++i) {
		if (CompareStringOrdinal(
			_monitorIds[i].c_str(), (int)_monitorIds[i].size(),
			_data->preferredMonitorId.c_str(),
			(int)_data->preferredMonitorId.size(), TRUE) == CSTR_EQUAL) {
			return (int)i + (int)::Magpie::MultiMonitorUsage::Specific;
		}
	}

	return (int)::Magpie::MultiMonitorUsage::Closest;
}

void ProfileViewModel::MonitorSelection(int value) {
	if (value < 0) {
		return;
	}

	const int specificBase = (int)::Magpie::MultiMonitorUsage::Specific;
	::Magpie::MultiMonitorUsage usage;
	std::wstring monitorId;
	std::wstring monitorName;
	if (value < specificBase) {
		usage = (::Magpie::MultiMonitorUsage)value;
	} else {
		const size_t monitorIndex = (size_t)(value - specificBase);
		if (monitorIndex >= _monitorIds.size()) {
			return;
		}
		usage = ::Magpie::MultiMonitorUsage::Specific;
		monitorId = _monitorIds[monitorIndex];
		monitorName = _monitorNames[monitorIndex];
	}

	if (_data->multiMonitorUsage == usage &&
		_data->preferredMonitorId == monitorId) {
		return;
	}

	_data->multiMonitorUsage = usage;
	_data->preferredMonitorId = std::move(monitorId);
	_data->preferredMonitorName = std::move(monitorName);
	AppSettings::Get().SaveAsync();

	RaisePropertyChanged(L"MonitorSelection");
}

int ProfileViewModel::InitialWindowedScaleFactor() const noexcept {
	return InitialWindowedScaleFactorToSelectedIndex(_data->initialWindowedScaleFactor);
}

void ProfileViewModel::InitialWindowedScaleFactor(int value) {
	if (value < 0 || value >= (int)InitialWindowedScaleFactor::COUNT) {
		return;
	}

	::Magpie::InitialWindowedScaleFactor factor = InitialWindowedScaleFactorFromSelectedIndex(value);
	if (_data->initialWindowedScaleFactor == factor) {
		return;
	}

	_data->initialWindowedScaleFactor = factor;
	AppSettings::Get().SaveAsync();

	RaisePropertyChanged(L"InitialWindowedScaleFactor");
}

double ProfileViewModel::CustomInitialWindowedScaleFactor() const noexcept {
	return _data->customInitialWindowedScaleFactor;
}

void ProfileViewModel::CustomInitialWindowedScaleFactor(double value) {
	if (_data->customInitialWindowedScaleFactor == value) {
		return;
	}

	_data->customInitialWindowedScaleFactor = std::isnan(value) ? 1.0f : (float)value;
	AppSettings::Get().SaveAsync();

	RaisePropertyChanged(L"CustomInitialWindowedScaleFactor");
}

IVector<IInspectable> ProfileViewModel::GraphicsCards() const noexcept {
	return _graphicsCards;
}

void ProfileViewModel::_BuildGraphicsCards() {
	std::vector<IInspectable> graphicsCards;
	std::vector<GraphicsCardId> options;
	const auto& adapterInfos = AdaptersService::Get().AdapterInfos();
	ResourceLoader resources = ResourceLoader::GetForCurrentView(CommonSharedConstants::APP_RESOURCE_MAP_ID);
	const auto add = [&](hstring const& label, GraphicsCardId id) {
		graphicsCards.push_back(box_value(label));
		options.push_back(id);
	};

	if (adapterInfos.empty()) {
		// A disabled placeholder must not erase a saved, temporarily absent GPU.
		add(resources.GetString(L"Profile_General_GraphicsCard_None"), _data->graphicsCardId);
	} else {
		add(resources.GetString(L"Profile_General_GraphicsCard_Auto"), {});
		for (const AdapterInfo& ai : adapterInfos) {
			add(hstring(ai.description), { (int)ai.idx, ai.vendorId, ai.deviceId });
		}
		if (ShowGraphicsCardUnavailableWarning()) {
			add(resources.GetString(L"Profile_General_GraphicsCard_Unavailable"), _data->graphicsCardId);
		}
	}
	_graphicsCardOptions = std::move(options);
	_graphicsCards = single_threaded_vector(std::move(graphicsCards));
}

IInspectable ProfileViewModel::GraphicsCard() const noexcept {
	if (!_graphicsCards) {
		return nullptr;
	}
	const auto& id = _data->graphicsCardId;
	for (uint32_t i = 0; i < _graphicsCardOptions.size(); ++i) {
		const auto& option = _graphicsCardOptions[i];
		if (option.idx == id.idx && option.vendorId == id.vendorId && option.deviceId == id.deviceId) {
			return _graphicsCards.GetAt(i);
		}
	}
	return nullptr;
}

void ProfileViewModel::GraphicsCard(IInspectable const& value) {
	if (!value || !_graphicsCards || _isHandlingAdapterChanged || IsNoGraphicsCard()) {
		return;
	}
	// Bind to item identity, not its shifting list index. Deferred writes from an
	// earlier list (including its Auto item) must never modify the current intent.
	uint32_t index = 0;
	if (!_graphicsCards.IndexOf(value, index)) {
		return;
	}
	const auto selected = _graphicsCardOptions[index];
	const auto& current = _data->graphicsCardId;
	if (selected.idx == current.idx && selected.vendorId == current.vendorId && selected.deviceId == current.deviceId) {
		return;
	}
	_data->graphicsCardId = selected;
	AppSettings::Get().SaveAsync();
	_RefreshGraphicsCardSettings();
}

bool ProfileViewModel::IsGraphicsCardSelectionEnabled() const noexcept {
	return !IsNoGraphicsCard();
}

bool ProfileViewModel::IsNoGraphicsCard() const noexcept {
	return AdaptersService::Get().AdapterInfos().empty();
}

bool ProfileViewModel::ShowGraphicsCardUnavailableWarning() const noexcept {
	const auto& id = _data->graphicsCardId;
	const auto& adapters = AdaptersService::Get().AdapterInfos();
	if (adapters.empty() || (id.idx < 0 && id.vendorId == 0 && id.deviceId == 0)) {
		return false;
	}
	return std::none_of(adapters.begin(), adapters.end(), [&](const AdapterInfo& ai) {
		return (int)ai.idx == id.idx && ai.vendorId == id.vendorId && ai.deviceId == id.deviceId;
	});
}

void ProfileViewModel::_NotifyFrameRefreshChanged() {
	RaisePropertyChanged(L"FrameRefreshModeIndex");
	RaisePropertyChanged(L"IsSimpleFrameRefresh");
	RaisePropertyChanged(L"IsAdvancedFrameRefresh");
	RaisePropertyChanged(L"IsVRREnabled");
	_NotifyRtssChanged();
	RaisePropertyChanged(L"ContentFrameRateModeIndex");
	RaisePropertyChanged(L"ContentFrameRate");
	RaisePropertyChanged(L"FrameSyncModeIndex");
	RaisePropertyChanged(L"IsContentPacingEnabled");
	RaisePropertyChanged(L"ShowContentFrameRate");
	RaisePropertyChanged(L"CursorRefreshModeIndex");
	RaisePropertyChanged(L"CursorSupplementModeIndex");
	RaisePropertyChanged(L"CursorSupplementRate");
	RaisePropertyChanged(L"ShowCursorSupplement");
	RaisePropertyChanged(L"ShowCursorSupplementRate");
	RaisePropertyChanged(L"IdleRedrawModeIndex");
	RaisePropertyChanged(L"IdleRedrawRate");
	RaisePropertyChanged(L"ShowIdleRedrawRate");
	RaisePropertyChanged(L"FrameRefreshNotice");
	RaisePropertyChanged(L"ShowFrameRefreshNotice");
}

void ProfileViewModel::_SaveFrameRefresh() {
	ProfileService::Get().FrameRefreshChanged.Invoke(*_data);
	AppSettings::Get().SaveAsync();
}

int32_t ProfileViewModel::FrameRefreshModeIndex() const noexcept { return IsSimpleFrameRefresh() ? 0 : 1; }
void ProfileViewModel::FrameRefreshModeIndex(int32_t value) {
	if (value < 0 || value > 1) return;
	AppSettings::Get().IsSimpleFrameRefresh(value == 0);
}
bool ProfileViewModel::IsSimpleFrameRefresh() const noexcept { return AppSettings::Get().IsSimpleFrameRefresh(); }
bool ProfileViewModel::IsAdvancedFrameRefresh() const noexcept { return !IsSimpleFrameRefresh(); }

bool ProfileViewModel::IsVRREnabled() const noexcept { return _data->frameRefresh.vrrEnabled; }
void ProfileViewModel::IsVRREnabled(bool value) {
	if (_data->frameRefresh.vrrEnabled == value) return;
	_data->frameRefresh.vrrEnabled = value;
	_SaveFrameRefresh();
}

void ProfileViewModel::_NotifyRtssChanged() {
	for (const auto property : { L"RtssStatusTitle", L"RtssStatusDescription", L"RtssStatusGlyph", L"IsRtssBusy", L"ShowRtssManagement", L"RtssProgramStatus", L"RtssSelfStatus", L"RtssManagementResult", L"ShowRtssManagementResult", L"CanRecheckRtss", L"SimpleRtssSync", L"RtssModeIndex", L"RtssTargetScopeIndex", L"ShowRtssOptions", L"ShowContentPacing", L"ShowRtssDownload", L"CanRetryRtss", L"RtssScopeIndex", L"RtssRateModeIndex", L"RtssCustomRate", L"RtssSyncedRate", L"RtssExitPolicyIndex",
		L"IsRtssManaged", L"ShowRtssCustomRate", L"ShowRtssSyncedRate", L"IsRtssSyncAvailable", L"ShowRtssGlobalNotice", L"RtssStatusText",
		L"RtssStatusSegments", L"CanInitializeMagpie", L"ShowRtssInitialize", L"RtssInitializeText",
		L"RtssSyncText", L"CanLaunchRtss", L"CanLaunchAfterburner", L"RtssLaunchText", L"AfterburnerLaunchText" })
		RaisePropertyChanged(property);
}

void ProfileViewModel::_SaveRtss() {
	ProfileService::Get().RtssSettingsChanged.Invoke(*_data);
	AppSettings::Get().SaveAsync();
	_NotifyRtssChanged();
	_LoadRtssPreview();
}

void ProfileViewModel::_LoadRtssPreview() {
	const auto generation = ++_rtssPreviewGeneration;
	const auto identity = _data->runtimeIdentity;
	// Packaged identities and the default profile require the actual session target.
	const auto executable = _data->isPackaged ? std::wstring{} : _data->pathRule;
	RtssService::Get().Preview(EffectiveRtssSettings(_data->rtss, IsSimpleFrameRefresh()), executable, [weak = get_weak(), generation, identity](RtssStatus status) {
		if (auto self = weak.get(); self && self->_rtssPreviewGeneration == generation && self->_data->runtimeIdentity == identity) {
			self->_rtssPreview = std::move(status);
			self->_NotifyRtssChanged();
		}
	});
}

RtssStatus ProfileViewModel::_RtssStatus() const {
	const auto& service = RtssService::Get();
	const auto inventory = service.InventoryStatus();
	if (service.HasSession(_data->runtimeIdentity)) {
		auto current = service.CurrentSessionStatus(_data->runtimeIdentity);
		// A read-only recheck can discover a repaired dependency without claiming
		// that the target cap has been applied. Keep retry explicit.
		if (inventory.inventoryKnown &&
			((current.state == RtssState::NotInstalled && !inventory.rtss.executable.empty()) ||
			 (current.state == RtssState::NotRunning && inventory.rtss.running))) {
			current.state = RtssState::Failed;
			current.failureStage = inventory.readReady ? L"Rtss_Detail_RetryReady" : inventory.failureStage;
		}
		return current;
	}
	auto result = _rtssPreview;
	result.rtss = inventory.rtss;
	result.afterburner = inventory.afterburner;
	result.readReady = inventory.readReady;
	result.applyReady = inventory.applyReady;
	result.initializeReady = inventory.initializeReady;
	result.scope = EffectiveRtssSettings(_data->rtss, IsSimpleFrameRefresh()).scope;
	// Manual initialization/tool results belong to management details. They
	// must never replace the target's disabled/waiting state.
	if (!IsRtssManaged()) result.state = RtssState::Disabled;
	else if (ShowRtssSyncedRate() && !IsRtssSyncAvailable()) result.state = RtssState::WaitingContent;
	else {
		const auto terminal = service.SessionStatus();
		if (service.IsLatestStatus(terminal, _data->runtimeIdentity) &&
			(terminal.state == RtssState::RestoreFailed || terminal.state == RtssState::Conflict)) return terminal;
		if (inventory.state != RtssState::Checking && inventory.rtss.executable.empty()) result.state = RtssState::NotInstalled;
		else if (result.state == RtssState::Read || result.state == RtssState::MissingProfile || result.state == RtssState::NotRunning)
			result.state = RtssState::WaitingTarget;
	}
	if (result.state == RtssState::Disabled || result.state == RtssState::WaitingContent || result.state == RtssState::WaitingTarget) {
		result.rateKnown = false; result.profile.clear(); result.failureStage.clear();
		result.rateUnavailable = result.profileSaved = result.reloaded = false;
		result.detectionDisabled = result.targetOverridesGlobal = false;
	}
	return result;
}

bool ProfileViewModel::SimpleRtssSync() const noexcept { return _data->rtss.simpleContentSync; }
void ProfileViewModel::SimpleRtssSync(bool value) {
	if (!IsSimpleFrameRefresh() || _data->rtss.simpleContentSync == value) return;
	_data->rtss.simpleContentSync = value; _SaveRtss();
}
int32_t ProfileViewModel::RtssModeIndex() const noexcept {
	return _data->rtss.scope == RtssScope::None ? 0 : _data->rtss.rateMode == RtssRateMode::ContentSync ? 1 : 2;
}
void ProfileViewModel::RtssModeIndex(int32_t value) {
	if (IsSimpleFrameRefresh() || value < 0 || value > 2 || value == RtssModeIndex()) return;
	if (!value) _data->rtss.scope = RtssScope::None;
	else {
		if (_data->rtss.scope == RtssScope::None) _data->rtss.scope = RtssScope::Target;
		_data->rtss.rateMode = value == 1 ? RtssRateMode::ContentSync : RtssRateMode::Custom;
	}
	_SaveRtss();
}
int32_t ProfileViewModel::RtssTargetScopeIndex() const noexcept { return _data->rtss.scope == RtssScope::Global ? 0 : 1; }
void ProfileViewModel::RtssTargetScopeIndex(int32_t value) {
	if (value < 0 || value > 1 || !ShowRtssOptions()) return;
	RtssScopeIndex(value == 0 ? 1 : 2);
}
bool ProfileViewModel::ShowRtssOptions() const noexcept { return IsAdvancedFrameRefresh() && IsRtssManaged(); }
bool ProfileViewModel::ShowContentPacing() const noexcept { return IsAdvancedFrameRefresh() && ShowContentFrameRate(); }
bool ProfileViewModel::ShowRtssDownload() const noexcept { return _RtssStatus().state == RtssState::NotInstalled; }
bool ProfileViewModel::CanRetryRtss() const noexcept {
	const auto s = _RtssStatus();
	return CanRecheckRtss() && RtssService::Get().HasSession(_data->runtimeIdentity) &&
		(s.state == RtssState::Failed || s.state == RtssState::ToolStartFailed ||
		 s.state == RtssState::NotInstalled || s.state == RtssState::Incompatible || s.state == RtssState::NotRunning);
}
void ProfileViewModel::RetryRtss() { if (CanRetryRtss()) RtssService::Get().Retry(_data->runtimeIdentity); }

int32_t ProfileViewModel::RtssScopeIndex() const noexcept { return int32_t(_data->rtss.scope); }
void ProfileViewModel::RtssScopeIndex(int32_t value) {
	if (IsSimpleFrameRefresh()) return;
	if (value < 0 || value > 2 || RtssScopeIndex() == value) return;
	_data->rtss.scope = RtssScope(value); _SaveRtss();
}
int32_t ProfileViewModel::RtssRateModeIndex() const noexcept { return int32_t(_data->rtss.rateMode); }
void ProfileViewModel::RtssRateModeIndex(int32_t value) {
	if (IsSimpleFrameRefresh()) return;
	if (value < 0 || value > 1 || RtssRateModeIndex() == value || (value == 1 && !IsRtssSyncAvailable())) return;
	_data->rtss.rateMode = RtssRateMode(value); _SaveRtss();
}
double ProfileViewModel::RtssCustomRate() const noexcept { return _data->rtss.customRate; }
void ProfileViewModel::RtssCustomRate(double value) {
	if (IsSimpleFrameRefresh()) return;
	// A hidden editor's delayed binding callback must not change the manual preference.
	if (!IsRtssManaged() || !ShowRtssCustomRate()) return;
	const auto rate = RtssSettings::EditedRate(value);
	if (!rate) { RaisePropertyChanged(L"RtssCustomRate"); return; }
	if (*rate == _data->rtss.customRate) {
		if (value != *rate) RaisePropertyChanged(L"RtssCustomRate");
		return;
	}
	_data->rtss.customRate = *rate; _SaveRtss();
}
double ProfileViewModel::RtssSyncedRate() const noexcept {
	if (IsRtssManaged() && ShowRtssSyncedRate() && IsRtssSyncAvailable()) {
		if (const auto rate = RtssService::Get().ContentSyncRate(_data->runtimeIdentity)) return double(rate);
	}
	return std::numeric_limits<double>::quiet_NaN();
}
int32_t ProfileViewModel::RtssExitPolicyIndex() const noexcept { return int32_t(_data->rtss.exitPolicy); }
void ProfileViewModel::RtssExitPolicyIndex(int32_t value) {
	if (IsSimpleFrameRefresh()) return;
	if (value < 0 || value > 1 || RtssExitPolicyIndex() == value) return;
	_data->rtss.exitPolicy = RtssExitPolicy(value); _SaveRtss();
}
bool ProfileViewModel::IsRtssManaged() const noexcept { return EffectiveRtssSettings(_data->rtss, IsSimpleFrameRefresh()).scope != RtssScope::None; }
bool ProfileViewModel::ShowRtssCustomRate() const noexcept { return ShowRtssOptions() && _data->rtss.rateMode == RtssRateMode::Custom; }
bool ProfileViewModel::ShowRtssSyncedRate() const noexcept { return IsRtssManaged() && EffectiveRtssSettings(_data->rtss, IsSimpleFrameRefresh()).rateMode == RtssRateMode::ContentSync; }
bool ProfileViewModel::IsRtssSyncAvailable() const noexcept { return _data->frameRefresh.contentMode != ContentFrameRateMode::Source; }
bool ProfileViewModel::ShowRtssGlobalNotice() const noexcept { return _data->rtss.scope == RtssScope::Global; }

namespace {
bool RtssFailed(RtssState state) {
	return state == RtssState::Failed || state == RtssState::ToolStartFailed || state == RtssState::Incompatible ||
		state == RtssState::NotRunning || state == RtssState::Conflict || state == RtssState::RestoreFailed;
}
bool RtssBusy(RtssState state) {
	return state == RtssState::Checking || state == RtssState::StartingTool ||
		state == RtssState::InitializingMagpie || state == RtssState::Applying;
}
}

hstring ProfileViewModel::RtssStatusTitle() const {
	const auto loader = ResourceLoader::GetForCurrentView(CommonSharedConstants::APP_RESOURCE_MAP_ID);
	const auto state = _RtssStatus().state;
	const wchar_t* key = L"Rtss_Status_Failed";
	switch (state) {
	case RtssState::Disabled: key = L"Rtss_Status_Disabled"; break;
	case RtssState::WaitingContent: key = L"Rtss_Title_WaitingContent"; break;
	case RtssState::NotInstalled: key = L"Rtss_Status_NotInstalled"; break;
	case RtssState::WaitingTarget: case RtssState::Read: case RtssState::MissingProfile: key = L"Rtss_Status_WaitingTarget"; break;
	case RtssState::Checking: key = L"Rtss_Status_Checking"; break;
	case RtssState::StartingTool: key = L"Rtss_Status_StartingTool"; break;
	case RtssState::InitializingMagpie: key = L"Rtss_Status_InitializingMagpie"; break;
	case RtssState::Applying: key = L"Rtss_Status_Applying"; break;
	case RtssState::Written: key = L"Rtss_Status_Written"; break;
	case RtssState::Restored: key = L"Rtss_Status_Restored"; break;
	case RtssState::Kept: key = L"Rtss_Status_Kept"; break;
	default: break;
	}
	return loader.GetString(key);
}
hstring ProfileViewModel::RtssStatusDescription() const {
	const auto loader = ResourceLoader::GetForCurrentView(CommonSharedConstants::APP_RESOURCE_MAP_ID);
	const auto s = _RtssStatus();
	std::wstring text;
	const auto add = [&](std::wstring_view value) { if (!value.empty()) { if (!text.empty()) text += L" · "; text += value; } };
	const wchar_t* key = nullptr;
	switch (s.state) {
	case RtssState::Disabled: key = L"Rtss_Detail_Disabled"; break;
	case RtssState::WaitingContent: key = L"Rtss_Detail_WaitingContent"; break;
	case RtssState::WaitingTarget: key = L"Rtss_Detail_WaitingTarget"; break;
	case RtssState::Checking: key = L"Rtss_Detail_Checking"; break;
	case RtssState::StartingTool: key = L"Rtss_Detail_Starting"; break;
	case RtssState::InitializingMagpie: key = L"Rtss_Detail_Initializing"; break;
	case RtssState::Applying: key = L"Rtss_Detail_Applying"; break;
	case RtssState::NotInstalled: key = L"Rtss_Detail_NotInstalled"; break;
	case RtssState::NotRunning: key = L"Rtss_Status_NotRunning"; break;
	case RtssState::Incompatible: key = L"Rtss_Status_Incompatible"; break;
	case RtssState::Conflict: key = L"Rtss_Status_Conflict"; break;
	case RtssState::RestoreFailed: key = L"Rtss_Status_RestoreFailed"; break;
	default: break;
	}
	if (key) add(loader.GetString(key));
	if (s.rateKnown) add(s.timeLimit ? std::wstring(loader.GetString(L"Rtss_TimeLimit")) : s.currentRate > 0 ?
		fmt::format(fmt::runtime(std::wstring_view(loader.GetString(L"Rtss_ProfileRate"))), s.currentRate) : std::wstring(loader.GetString(L"Rtss_NoLimit")));
	if (s.state == RtssState::Written) add(loader.GetString(s.scope == RtssScope::Global ? L"Rtss_Detail_Global" : L"Rtss_Detail_Target"));
	if (!s.profile.empty() && (s.rateKnown || RtssFailed(s.state))) add(s.profile);
	if (s.rateUnavailable) add(loader.GetString(L"Rtss_RateUnknown"));
	if (s.profileSaved) add(loader.GetString(L"Rtss_ProfileSaved"));
	if (!s.failureStage.empty()) add(loader.GetString(s.failureStage));
	if (s.detectionDisabled) add(loader.GetString(L"Rtss_DetectionDisabled"));
	if (s.targetOverridesGlobal) add(loader.GetString(L"Rtss_TargetOverridesGlobal"));
	const auto& service = RtssService::Get();
	if (service.HasSession(_data->runtimeIdentity) && ScalingService::Get().IsScaling() &&
		(RtssFailed(s.state) || s.state == RtssState::NotInstalled)) add(loader.GetString(L"Rtss_ScalingContinues"));
	// A disabled preference must not hide a failed cleanup of this profile.
	const auto terminal = service.SessionStatus();
	if ((s.state == RtssState::Disabled || s.state == RtssState::WaitingContent) && service.IsLatestStatus(terminal, _data->runtimeIdentity) &&
		(terminal.state == RtssState::RestoreFailed || terminal.state == RtssState::Conflict)) {
		add(loader.GetString(terminal.state == RtssState::RestoreFailed ? L"Rtss_Status_RestoreFailed" : L"Rtss_Status_Conflict"));
		if (terminal.profileSaved) add(loader.GetString(L"Rtss_ProfileSaved"));
		if (!terminal.failureStage.empty()) add(loader.GetString(terminal.failureStage));
	}
	return hstring(text);
}
hstring ProfileViewModel::RtssStatusGlyph() const {
	const auto state = _RtssStatus().state;
	if (RtssBusy(state)) return L"";
	if (state == RtssState::Written) return L"\xE73E";
	if (RtssFailed(state)) return L"\xE783";
	if (state == RtssState::NotInstalled) return L"\xE896";
	return L"\xE823";
}
bool ProfileViewModel::IsRtssBusy() const { return RtssBusy(_RtssStatus().state); }
bool ProfileViewModel::ShowRtssManagement() const {
	// Tools can be inspected before a fixed target rate or an active session exists.
	// Individual actions still use their existing capability and busy-state checks.
	return true;
}
bool ProfileViewModel::CanRecheckRtss() const {
	const auto& service = RtssService::Get();
	return !service.IsRechecking() && !service.IsLaunching() && !service.IsInitializingMagpie() && !service.IsPreparing() && !IsRtssBusy();
}
hstring ProfileViewModel::RtssProgramStatus() const {
	const auto loader = ResourceLoader::GetForCurrentView(CommonSharedConstants::APP_RESOURCE_MAP_ID);
	const auto s = RtssService::Get().InventoryStatus();
	return loader.GetString(RtssService::Get().IsRechecking() || s.state == RtssState::Checking ? L"Rtss_Inventory_Checking" :
		s.rtss.executable.empty() ? L"Rtss_Inventory_Missing" : s.rtss.running ? L"Rtss_Inventory_Running" : L"Rtss_Inventory_Stopped");
}
hstring ProfileViewModel::RtssSelfStatus() const {
	const auto loader = ResourceLoader::GetForCurrentView(CommonSharedConstants::APP_RESOURCE_MAP_ID);
	const auto& service = RtssService::Get();
	return loader.GetString(service.IsInitializingMagpie() || _RtssStatus().state == RtssState::InitializingMagpie ? L"Rtss_Inventory_Initializing" :
		service.MagpieInitialization() == RtssInitializationState::Initialized ? L"Rtss_Inventory_Unlimited" :
		service.MagpieInitialization() == RtssInitializationState::Uninitialized ? L"Rtss_Inventory_Uninitialized" : L"Rtss_Inventory_Unknown");
}
hstring ProfileViewModel::RtssManagementResult() const {
	const auto loader = ResourceLoader::GetForCurrentView(CommonSharedConstants::APP_RESOURCE_MAP_ID);
	const auto& service = RtssService::Get();
	const auto s = service.ManagementStatus();
	if (!s.selfInitialization && !s.manualOperation) return {};
	const wchar_t* key = L"Rtss_Management_Failed";
	if (s.selfInitialization) {
		if (s.state == RtssState::MagpieInitialized || s.state == RtssState::MagpieAlreadyUnlimited) key = L"Rtss_Status_MagpieInitialized";
		else if (service.IsInitializingMagpie()) key = L"Rtss_Status_InitializingMagpie";
	} else {
		const bool afterburner = s.profile == L"MSI Afterburner";
		key = s.state == RtssState::ToolStarted ? L"Rtss_Management_Started" :
			s.state == RtssState::StartingTool ? (afterburner ? L"Rtss_Status_StartingAfterburner" : L"Rtss_Status_StartingTool") :
			afterburner ? L"Rtss_Status_AfterburnerStartFailed" : L"Rtss_Management_LaunchFailed";
	}
	std::wstring text = s.profile;
	if (!text.empty()) text += L" · ";
	text += loader.GetString(key);
	if (s.profileSaved) { text += L" · "; text += loader.GetString(L"Rtss_ProfileSaved"); }
	if (!s.failureStage.empty()) { text += L" · "; text += loader.GetString(s.failureStage); }
	return hstring(text);
}
bool ProfileViewModel::ShowRtssManagementResult() const { return !RtssManagementResult().empty(); }

IVector<hstring> ProfileViewModel::RtssStatusSegments() const {
	const auto loader = ResourceLoader::GetForCurrentView(CommonSharedConstants::APP_RESOURCE_MAP_ID);
	const auto s = _RtssStatus();
	const wchar_t* key = L"Rtss_Status_Checking";
	switch (s.state) {
	case RtssState::Disabled: key = L"Rtss_Status_Disabled"; break;
	case RtssState::NotInstalled: key = L"Rtss_Status_NotInstalled"; break;
	case RtssState::NotRunning: key = L"Rtss_Status_NotRunning"; break;
	case RtssState::Incompatible: key = L"Rtss_Status_Incompatible"; break;
	case RtssState::Failed: key = L"Rtss_Status_Failed"; break;
	case RtssState::WaitingTarget: key = L"Rtss_Status_WaitingTarget"; break;
	case RtssState::Read: key = L"Rtss_Status_Read"; break;
	case RtssState::MissingProfile: key = L"Rtss_Status_MissingProfile"; break;
	case RtssState::WaitingContent: key = L"Rtss_Status_WaitingContent"; break;
	case RtssState::Applying: key = L"Rtss_Status_Applying"; break;
	case RtssState::Written: key = L"Rtss_Status_Written"; break;
	case RtssState::Conflict: key = L"Rtss_Status_Conflict"; break;
	case RtssState::Kept: key = L"Rtss_Status_Kept"; break;
	case RtssState::Restored: key = L"Rtss_Status_Restored"; break;
	case RtssState::RestoreFailed: key = L"Rtss_Status_RestoreFailed"; break;
	case RtssState::StartingTool: key = s.profile == L"MSI Afterburner" ? L"Rtss_Status_StartingAfterburner" : L"Rtss_Status_StartingTool"; break;
	case RtssState::ToolStartFailed: key = s.profile == L"MSI Afterburner" ? L"Rtss_Status_AfterburnerStartFailed" : L"Rtss_Status_ToolStartFailed"; break;
	case RtssState::InitializingMagpie: key = L"Rtss_Status_InitializingMagpie"; break;
	case RtssState::MagpieInitialized: key = L"Rtss_Status_MagpieInitialized"; break;
	case RtssState::MagpieAlreadyUnlimited: key = L"Rtss_Status_MagpieAlreadyUnlimited"; break;
	default: break;
	}
	// XAML ItemsSource also needs the boxed IInspectable collection interfaces.
	auto segments = single_threaded_observable_vector<hstring>();
	auto add = [&](const std::wstring& value) {
		if (!value.empty()) segments.Append(hstring(segments.Size() ? L"\u00a0-\u00a0" + value : value));
	};
	if (s.state != RtssState::Disabled && s.state != RtssState::WaitingContent) add(s.profile);
	add(std::wstring(loader.GetString(key)));
	if (s.rateKnown) add(s.timeLimit ? std::wstring(loader.GetString(L"Rtss_TimeLimit")) : s.currentRate > 0 ?
		fmt::format(fmt::runtime(std::wstring_view(loader.GetString(L"Rtss_ProfileRate"))), s.currentRate) : std::wstring(loader.GetString(L"Rtss_NoLimit")));
	if (s.rateUnavailable) add(std::wstring(loader.GetString(L"Rtss_RateUnknown")));
	if (s.profileSaved) add(std::wstring(loader.GetString(L"Rtss_ProfileSaved")));
	if (!s.failureStage.empty()) add(std::wstring(loader.GetString(s.failureStage)));
	if (s.detectionDisabled) add(std::wstring(loader.GetString(L"Rtss_DetectionDisabled")));
	if (s.targetOverridesGlobal) add(std::wstring(loader.GetString(L"Rtss_TargetOverridesGlobal")));
	if (s.profile != L"MSI Afterburner" && RtssService::Get().HasSession(_data->runtimeIdentity) &&
		(s.state == RtssState::Failed || s.state == RtssState::ToolStartFailed || s.state == RtssState::Incompatible ||
		 s.state == RtssState::NotInstalled || s.state == RtssState::NotRunning || s.state == RtssState::Conflict))
		add(std::wstring(loader.GetString(L"Rtss_ScalingContinues")));
	return segments.as<IVector<hstring>>();
}

hstring ProfileViewModel::RtssStatusText() const {
	std::wstring text;
	for (const auto& segment : RtssStatusSegments()) text += segment;
	return hstring(text);
}

hstring ProfileViewModel::RtssSyncText() const {
	const auto loader = ResourceLoader::GetForCurrentView(CommonSharedConstants::APP_RESOURCE_MAP_ID);
	if (!IsRtssSyncAvailable()) return loader.GetString(L"Rtss_SyncUnavailable");
	if (_data->rtss.rateMode == RtssRateMode::Custom) return loader.GetString(L"Rtss_CustomAtStart");
	return loader.GetString(L"Rtss_SyncAtStart");
}

bool ProfileViewModel::CanLaunchRtss() const {
	const auto s = RtssService::Get().InventoryStatus(); return !s.rtss.executable.empty() && !s.rtss.running && CanRecheckRtss();
}
bool ProfileViewModel::CanLaunchAfterburner() const {
	const auto s = RtssService::Get().InventoryStatus(); return !s.afterburner.executable.empty() && !s.afterburner.running && CanRecheckRtss();
}
bool ProfileViewModel::CanInitializeMagpie() const {
	const auto s = RtssService::Get().InventoryStatus();
	return s.initializeReady &&
		CanRecheckRtss();
}
bool ProfileViewModel::ShowRtssInitialize() const {
	return RtssService::Get().MagpieInitialization() == RtssInitializationState::Uninitialized;
}
hstring ProfileViewModel::RtssInitializeText() const {
	return ResourceLoader::GetForCurrentView(CommonSharedConstants::APP_RESOURCE_MAP_ID).GetString(
		RtssService::Get().IsInitializingMagpie() ? L"Rtss_InitializingMagpie" : L"Rtss_InitializeMagpie");
}
void ProfileViewModel::InitializeMagpieRtss() { if (CanInitializeMagpie()) RtssService::Get().InitializeMagpie(); }
hstring ProfileViewModel::RtssLaunchText() const {
	const auto s = RtssService::Get().InventoryStatus();
	return ResourceLoader::GetForCurrentView(CommonSharedConstants::APP_RESOURCE_MAP_ID).GetString(s.rtss.executable.empty() ? L"Rtss_LaunchRtssMissing" :
		s.rtss.running ? L"Rtss_LaunchRtssRunning" : L"Rtss_LaunchRtss");
}
hstring ProfileViewModel::AfterburnerLaunchText() const {
	const auto s = RtssService::Get().InventoryStatus();
	return ResourceLoader::GetForCurrentView(CommonSharedConstants::APP_RESOURCE_MAP_ID).GetString(s.afterburner.executable.empty() ? L"Rtss_LaunchAfterburnerMissing" :
		s.afterburner.running ? L"Rtss_LaunchAfterburnerRunning" : L"Rtss_LaunchAfterburner");
}
void ProfileViewModel::RefreshRtss() {
	if (!CanRecheckRtss()) return;
	RtssService::Get().Recheck(false);
	_LoadRtssPreview();
}
void ProfileViewModel::LaunchRtss() { RtssService::Get().Launch(false); }
void ProfileViewModel::LaunchAfterburner() { RtssService::Get().Launch(true); }

int32_t ProfileViewModel::ContentFrameRateModeIndex() const noexcept {
	return _data->frameRefresh.contentMode == ContentFrameRateMode::Custom ? 1 : 0;
}
void ProfileViewModel::ContentFrameRateModeIndex(int32_t value) {
	if (value < 0 || value > 1) return;
	const auto next = value == 1 ? ContentFrameRateMode::Custom : ContentFrameRateMode::Source;
	if (_data->frameRefresh.contentMode == next) return;
	_data->frameRefresh.contentMode = next;
	_data->frameRefresh.ContentEdited();
	_SaveFrameRefresh();
}

double ProfileViewModel::ContentFrameRate() const noexcept { return double(_data->frameRefresh.contentRate); }
void ProfileViewModel::ContentFrameRate(double value) {
	const auto next = FrameRefreshSettings::ValidateEditedRate(value, 60);
	if (_data->frameRefresh.contentRate == next) return;
	_data->frameRefresh.contentRate = next;
	_data->frameRefresh.ContentEdited();
	_SaveFrameRefresh();
}

int32_t ProfileViewModel::FrameSyncModeIndex() const noexcept { return int32_t(_data->frameRefresh.pacing); }
void ProfileViewModel::FrameSyncModeIndex(int32_t value) {
	if (IsSimpleFrameRefresh()) return;
	if (value < 0 || value > 2) return;
	const auto next = FrameSyncMode(value);
	if (_data->frameRefresh.pacing == next) return;
	_data->frameRefresh.pacing = next;
	_data->frameRefresh.ContentEdited();
	_SaveFrameRefresh();
}

int32_t ProfileViewModel::CursorRefreshModeIndex() const noexcept { return int32_t(_data->frameRefresh.cursorMode); }
void ProfileViewModel::CursorRefreshModeIndex(int32_t value) {
	if (IsSimpleFrameRefresh()) return;
	if (value < 0 || value > 2) return;
	const auto next = CursorRefreshMode(value);
	if (_data->frameRefresh.cursorMode == next) return;
	_data->frameRefresh.cursorMode = next;
	_data->frameRefresh.CursorEdited();
	_SaveFrameRefresh();
}

int32_t ProfileViewModel::CursorSupplementModeIndex() const noexcept { return int32_t(_data->frameRefresh.cursorSupplement); }
void ProfileViewModel::CursorSupplementModeIndex(int32_t value) {
	if (IsSimpleFrameRefresh()) return;
	if (value < 0 || value > 1) return;
	const auto next = CursorSupplementMode(value);
	if (_data->frameRefresh.cursorSupplement == next) return;
	_data->frameRefresh.cursorSupplement = next;
	_SaveFrameRefresh();
}

double ProfileViewModel::CursorSupplementRate() const noexcept { return double(_data->frameRefresh.cursorRate); }
void ProfileViewModel::CursorSupplementRate(double value) {
	if (IsSimpleFrameRefresh()) return;
	const auto next = FrameRefreshSettings::ValidateEditedRate(value, 60);
	if (_data->frameRefresh.cursorRate == next) return;
	_data->frameRefresh.cursorRate = next;
	_SaveFrameRefresh();
}

int32_t ProfileViewModel::IdleRedrawModeIndex() const noexcept { return int32_t(_data->frameRefresh.idleEnabled); }
void ProfileViewModel::IdleRedrawModeIndex(int32_t value) {
	if (value < 0 || value > 1) return;
	const auto next = bool(value);
	if (_data->frameRefresh.idleEnabled == next) return;
	_data->frameRefresh.idleEnabled = next;
	_SaveFrameRefresh();
}

double ProfileViewModel::IdleRedrawRate() const noexcept { return double(_data->frameRefresh.idleRate); }
void ProfileViewModel::IdleRedrawRate(double value) {
	const auto next = FrameRefreshSettings::ValidateEditedRate(value, 30);
	if (_data->frameRefresh.idleRate == next) return;
	_data->frameRefresh.idleRate = next;
	_SaveFrameRefresh();
}

bool ProfileViewModel::IsContentPacingEnabled() const noexcept { return _data->frameRefresh.contentMode != ContentFrameRateMode::Source; }
bool ProfileViewModel::ShowContentFrameRate() const noexcept { return _data->frameRefresh.contentMode == ContentFrameRateMode::Custom; }
bool ProfileViewModel::ShowCursorSupplement() const noexcept { return _data->frameRefresh.cursorMode == CursorRefreshMode::Supplement; }
bool ProfileViewModel::ShowCursorSupplementRate() const noexcept { return ShowCursorSupplement() && _data->frameRefresh.cursorSupplement == CursorSupplementMode::Custom; }
bool ProfileViewModel::ShowIdleRedrawRate() const noexcept { return _data->frameRefresh.idleEnabled; }
int32_t ProfileViewModel::DuplicateFrameDetectionMode() const noexcept {
	return int32_t(AppSettings::Get().DuplicateFrameDetectionMode());
}
void ProfileViewModel::DuplicateFrameDetectionMode(int32_t value) {
	if (IsSimpleFrameRefresh()) return;
	if (value < 0 || value > int32_t(::Magpie::DuplicateFrameDetectionMode::Never)) return;
	const auto mode = ::Magpie::DuplicateFrameDetectionMode(value);
	AppSettings& settings = AppSettings::Get();
	if (settings.DuplicateFrameDetectionMode() == mode) return;
	if (mode != ::Magpie::DuplicateFrameDetectionMode::Dynamic)
		settings.IsStatisticsForDynamicDetectionEnabled(false);
	settings.DuplicateFrameDetectionMode(mode);
	RaisePropertyChanged(L"DuplicateFrameDetectionMode");
}
bool ProfileViewModel::ShowFrameRefreshNotice() const noexcept {
	return !FrameRefreshNotice().empty();
}
hstring ProfileViewModel::FrameRefreshNotice() const noexcept {
	const auto loader = ResourceLoader::GetForViewIndependentUse(CommonSharedConstants::APP_RESOURCE_MAP_ID);
	const auto s = EffectiveFrameRefreshSettings(_data->frameRefresh, IsSimpleFrameRefresh());
	std::wstring text;
	if (s.legacyContentLimit > 0 || s.legacySourceTarget >= 0 || s.legacyLimiterOnly || s.legacyResponsiveMinimum) {
		text += loader.GetString(L"FrameRefresh_LegacyNotice");
	}
	if (s.legacyContentLimit > 0) {
		text += L"\n";
		text += fmt::format(fmt::runtime(std::wstring_view(loader.GetString(L"FrameRefresh_LegacyCapNotice"))), s.legacyContentLimit);
	}
	if (s.idleEnabled && ((s.contentMode == ContentFrameRateMode::Custom && s.idleRate > s.contentRate) ||
		(s.legacyContentLimit > 0 && s.idleRate > s.legacyContentLimit))) {
		if (!text.empty()) text += L"\n";
		text += loader.GetString(L"FrameRefresh_IdleLimitNotice");
	}
	return hstring(text);
}
void ProfileViewModel::ResetFrameRefresh() {
	if (IsSimpleFrameRefresh()) {
		// Reset the exposed settings without discarding hidden advanced preferences.
		const FrameRefreshSettings defaults;
		auto& settings = _data->frameRefresh;
		settings.contentMode = defaults.contentMode;
		settings.contentRate = defaults.contentRate;
		settings.ContentEdited();
		settings.idleEnabled = defaults.idleEnabled;
		settings.idleRate = defaults.idleRate;
		settings.vrrEnabled = defaults.vrrEnabled;
		SimpleRtssSync(RtssSettings{}.simpleContentSync);
	} else {
		_data->frameRefresh = {};
		DuplicateFrameDetectionMode(int32_t(::Magpie::DuplicateFrameDetectionMode::Never));
	}
	_SaveFrameRefresh();
}

bool ProfileViewModel::IsCaptureTitleBar() const noexcept {
	return _data->IsCaptureTitleBar();
}

void ProfileViewModel::IsCaptureTitleBar(bool value) {
	if (_data->IsCaptureTitleBar() == value) {
		return;
	}

	_data->IsCaptureTitleBar(value);
	AppSettings::Get().SaveAsync();

	RaisePropertyChanged(L"IsCaptureTitleBar");
}

bool ProfileViewModel::CanCaptureTitleBar() const noexcept {
	return _data->captureMethod == CaptureMethod::GraphicsCapture
		|| _data->captureMethod == CaptureMethod::DesktopDuplication;
}

bool ProfileViewModel::IsCroppingEnabled() const noexcept {
	return _data->isCroppingEnabled;
}

void ProfileViewModel::IsCroppingEnabled(bool value) {
	if (_data->isCroppingEnabled == value) {
		return;
	}

	_data->isCroppingEnabled = value;
	AppSettings::Get().SaveAsync();

	RaisePropertyChanged(L"IsCroppingEnabled");
}

double ProfileViewModel::CroppingLeft() const noexcept {
	return _data->cropping.Left;
}

void ProfileViewModel::CroppingLeft(double value) {
	if (_data->cropping.Left == value) {
		return;
	}

	_data->cropping.Left = std::isnan(value) ? 0.0f : (float)value;
	AppSettings::Get().SaveAsync();

	RaisePropertyChanged(L"CroppingLeft");
}

double ProfileViewModel::CroppingTop() const noexcept {
	return _data->cropping.Top;
}

void ProfileViewModel::CroppingTop(double value) {
	if (_data->cropping.Top == value) {
		return;
	}

	// 用户已清空数字框则重置为 0
	_data->cropping.Top = std::isnan(value) ? 0.0f : (float)value;
	AppSettings::Get().SaveAsync();

	RaisePropertyChanged(L"CroppingTop");
}

double ProfileViewModel::CroppingRight() const noexcept {
	return _data->cropping.Right;
}

void ProfileViewModel::CroppingRight(double value) {
	if (_data->cropping.Right == value) {
		return;
	}

	_data->cropping.Right = std::isnan(value) ? 0.0f : (float)value;
	AppSettings::Get().SaveAsync();

	RaisePropertyChanged(L"CroppingRight");
}

double ProfileViewModel::CroppingBottom() const noexcept {
	return _data->cropping.Bottom;
}

void ProfileViewModel::CroppingBottom(double value) {
	if (_data->cropping.Bottom == value) {
		return;
	}

	_data->cropping.Bottom = std::isnan(value) ? 0.0f : (float)value;
	AppSettings::Get().SaveAsync();

	RaisePropertyChanged(L"CroppingBottom");
}

bool ProfileViewModel::IsAdjustCursorSpeed() const noexcept {
	return _data->IsAdjustCursorSpeed();
}

void ProfileViewModel::IsAdjustCursorSpeed(bool value) {
	if (_data->IsAdjustCursorSpeed() == value) {
		return;
	}

	_data->IsAdjustCursorSpeed(value);
	AppSettings::Get().SaveAsync();

	RaisePropertyChanged(L"IsAdjustCursorSpeed");
}

int ProfileViewModel::CursorScaling() const noexcept {
	return (int)_data->cursorScaling;
}

void ProfileViewModel::CursorScaling(int value) {
	if (value < 0) {
		return;
	}

	::Magpie::CursorScaling cursorScaling = (::Magpie::CursorScaling)value;
	if (_data->cursorScaling == cursorScaling) {
		return;
	}

	_data->cursorScaling = cursorScaling;
	AppSettings::Get().SaveAsync();

	RaisePropertyChanged(L"CursorScaling");
}

double ProfileViewModel::CustomCursorScaling() const noexcept {
	return _data->customCursorScaling;
}

void ProfileViewModel::CustomCursorScaling(double value) {
	if (_data->customCursorScaling == value) {
		return;
	}

	_data->customCursorScaling = std::isnan(value) ? 1.0f : (float)value;
	AppSettings::Get().SaveAsync();

	RaisePropertyChanged(L"CustomCursorScaling");
}

int ProfileViewModel::CursorInterpolationMode() const noexcept {
	return (int)_data->cursorInterpolationMode;
}

void ProfileViewModel::CursorInterpolationMode(int value) {
	if (value < 0) {
		return;
	}

	::Magpie::CursorInterpolationMode cursorInterpolationMode = (::Magpie::CursorInterpolationMode)value;
	if (_data->cursorInterpolationMode == cursorInterpolationMode) {
		return;
	}

	_data->cursorInterpolationMode = cursorInterpolationMode;
	AppSettings::Get().SaveAsync();

	RaisePropertyChanged(L"CursorInterpolationMode");
}

bool ProfileViewModel::IsAutoHideCursorEnabled() const noexcept {
	return _data->isAutoHideCursorEnabled;
}

void ProfileViewModel::IsAutoHideCursorEnabled(bool value) {
	if (_data->isAutoHideCursorEnabled == value) {
		return;
	}

	_data->isAutoHideCursorEnabled = value;
	AppSettings::Get().SaveAsync();

	RaisePropertyChanged(L"IsAutoHideCursorEnabled");
}

double ProfileViewModel::AutoHideCursorDelay() const noexcept {
	return _data->autoHideCursorDelay;
}

void ProfileViewModel::AutoHideCursorDelay(double value) {
	if (_data->autoHideCursorDelay == value) {
		return;
	}

	_data->autoHideCursorDelay = std::isnan(value) ? 3.0f : (float)value;
	AppSettings::Get().SaveAsync();

	RaisePropertyChanged(L"AutoHideCursorDelay");
	RaisePropertyChanged(L"AutoHideCursorDelayText");
}

hstring ProfileViewModel::AutoHideCursorDelayText() const noexcept {
	return App::Get().DoubleFormatter().FormatDouble(AutoHideCursorDelay());
}

hstring ProfileViewModel::LaunchParameters() const noexcept {
	return hstring(_data->launchParameters);
}

void ProfileViewModel::LaunchParameters(const hstring& value) {
	std::wstring_view trimmed(value);
	StrHelper::Trim(trimmed);
	_data->launchParameters = trimmed;
	AppSettings::Get().SaveAsync();

	RaisePropertyChanged(L"LaunchParameters");
}

int ProfileViewModel::DestAlignment() const noexcept {
	return (int)_data->destAlignment;
}

void ProfileViewModel::DestAlignment(int value) {
	if (value < 0) {
		return;
	}

	::Magpie::DestAlignment alignment = (::Magpie::DestAlignment)value;
	if (_data->destAlignment == alignment) {
		return;
	}

	_data->destAlignment = alignment;
	AppSettings::Get().SaveAsync();

	RaisePropertyChanged(L"DestAlignment");
}

bool ProfileViewModel::IsDirectFlipDisabled() const noexcept {
	return _data->IsDirectFlipDisabled();
}

void ProfileViewModel::IsDirectFlipDisabled(bool value) {
	if (_data->IsDirectFlipDisabled() == value) {
		return;
	}

	_data->IsDirectFlipDisabled(value);
	AppSettings::Get().SaveAsync();

	RaisePropertyChanged(L"IsDirectFlipDisabled");
}

fire_and_forget ProfileViewModel::_LoadIcon() {
	std::wstring iconPath;
	SoftwareBitmap iconBitmap{ nullptr };

	auto weakThis = get_weak();

	if (_isProgramExist) {
		const bool preferLightTheme = App::Get().IsLightTheme();
		const bool isPackaged = _data->isPackaged;
		const std::wstring path = _data->pathRule;
		const uint32_t iconSize =
			(uint32_t)std::lroundf(32.0f * App::Get().MainWindow().CurrentDpi() / USER_DEFAULT_SCREEN_DPI);

		co_await resume_background();

		if (isPackaged) {
			AppXReader appxReader;
			[[maybe_unused]] bool result = appxReader.Initialize(path);
			assert(result);

			std::variant<std::wstring, SoftwareBitmap> uwpIcon =
				appxReader.GetIcon(iconSize, preferLightTheme);
			if (uwpIcon.index() == 0) {
				iconPath = std::get<0>(uwpIcon);
			} else {
				iconBitmap = std::get<1>(uwpIcon);
			}
		} else {
			iconBitmap = IconHelper::ExtractIconFromExe(path.c_str(), iconSize);
		}

		co_await App::Get().Dispatcher();
		if (!weakThis.get()) {
			co_return;
		}
	}

	if (!iconPath.empty()) {
		BitmapIcon icon;
		icon.ShowAsMonochrome(false);
		icon.UriSource(Uri(iconPath));

		_icon = std::move(icon);
	} else if (iconBitmap) {
		SoftwareBitmapSource imageSource;
		co_await imageSource.SetBitmapAsync(iconBitmap);

		if (!weakThis.get()) {
			co_return;
		}

		MUXC::ImageIcon imageIcon;
		imageIcon.Source(imageSource);

		_icon = std::move(imageIcon);
	} else {
		_icon = nullptr;
	}

	RaisePropertyChanged(L"Icon");
}

void ProfileViewModel::_RefreshGraphicsCardSettings() {
	_isHandlingAdapterChanged = true;
	auto reset = wil::scope_exit([this]() { _isHandlingAdapterChanged = false; });
	_BuildGraphicsCards();
	RaisePropertyChanged(L"IsGraphicsCardSelectionEnabled");
	RaisePropertyChanged(L"IsNoGraphicsCard");
	RaisePropertyChanged(L"ShowGraphicsCardUnavailableWarning");
	RaisePropertyChanged(L"GraphicsCards");
	RaisePropertyChanged(L"GraphicsCard");
}

}
