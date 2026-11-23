#pragma once
#include "Event.h"
#include "ScalingOptions.h"
#include "RtssService.h"
#include "ProfileViewModel.g.h"

namespace Magpie {
struct Profile;
}

namespace winrt::Magpie::implementation {

struct ProfileViewModel : ProfileViewModelT<ProfileViewModel>,
                          wil::notify_property_changed_base<ProfileViewModel> {
	ProfileViewModel(int profileIdx, bool initializePageData = true);
	~ProfileViewModel();

	void Rebind(uint32_t index) noexcept;

	IconElement Icon() const noexcept {
		return _icon;
	}

	bool IsNotDefaultProfile() const noexcept;

	bool IsProgramExist() const noexcept {
		return _isProgramExist;
	}

	bool IsNotPackaged() const noexcept;

	fire_and_forget OpenProgramLocation() const noexcept;

	fire_and_forget ChangeExeForLaunching() noexcept;

	hstring Name() const noexcept;

	void Launch() const noexcept;

	hstring RenameText() const noexcept {
		return _renameText;
	}

	void RenameText(const hstring& value);

	bool IsRenameConfirmButtonEnabled() const noexcept {
		return _isRenameConfirmButtonEnabled;
	}

	void Rename();

	void RenameFlyout_Opening();

	void RenameTextBox_KeyDown(IInspectable const&, Input::KeyRoutedEventArgs const& args);

	int RenameTextBoxSelectionStart() const noexcept {
		return static_cast<int>(_renameText.size());
	}

	void RenameButton_Click();

	bool CanDrag() const noexcept;
	bool CanMoveUp() const noexcept;
	bool CanMoveDown() const noexcept;
	void MoveUp();
	void MoveDown();

	void Delete();

	IVector<IInspectable> ScalingModes() const noexcept;

	int ScalingMode() const noexcept;
	void ScalingMode(int value);

	IVector<IInspectable> CaptureMethods() const noexcept;

	int CaptureMethod() const noexcept;
	void CaptureMethod(int value);

	bool IsCaptureMethodDesktopDuplication() const noexcept;

	int AutoScale() const noexcept;
	void AutoScale(int value);

	bool IsParameterFocusSwitchingEnabled() const noexcept;
	void IsParameterFocusSwitchingEnabled(bool value);

	bool Is3DGameMode() const noexcept;
	void Is3DGameMode(bool value);

	bool IsHdrCompatibilityEnabled() const noexcept;
	void IsHdrCompatibilityEnabled(bool value);

	bool HasMultipleMonitors() const noexcept;

	IVector<IInspectable> MonitorOptions() const noexcept {
		return _monitorOptions;
	}

	int MonitorSelection() const noexcept;
	void MonitorSelection(int value);

	int InitialWindowedScaleFactor() const noexcept;
	void InitialWindowedScaleFactor(int value);

	double CustomInitialWindowedScaleFactor() const noexcept;
	void CustomInitialWindowedScaleFactor(double value);

	IVector<IInspectable> GraphicsCards() const noexcept;

	IInspectable GraphicsCard() const noexcept;
	void GraphicsCard(IInspectable const& value);

	bool IsGraphicsCardSelectionEnabled() const noexcept;

	bool IsNoGraphicsCard() const noexcept;
	bool ShowGraphicsCardUnavailableWarning() const noexcept;

	int32_t FrameRefreshModeIndex() const noexcept;
	void FrameRefreshModeIndex(int32_t value);
	bool IsSimpleFrameRefresh() const noexcept;
	bool IsAdvancedFrameRefresh() const noexcept;
	int32_t ContentFrameRateModeIndex() const noexcept;
	bool SimpleRtssSync() const noexcept;
	void SimpleRtssSync(bool value);
	int32_t RtssModeIndex() const noexcept;
	void RtssModeIndex(int32_t value);
	int32_t RtssTargetScopeIndex() const noexcept;
	void RtssTargetScopeIndex(int32_t value);
	bool ShowRtssOptions() const noexcept;
	bool ShowContentPacing() const noexcept;
	bool ShowRtssDownload() const noexcept;
	bool CanRetryRtss() const noexcept;
	void RetryRtss();
	int32_t RtssScopeIndex() const noexcept;
	void RtssScopeIndex(int32_t value);
	int32_t RtssRateModeIndex() const noexcept;
	void RtssRateModeIndex(int32_t value);
	double RtssCustomRate() const noexcept;
	void RtssCustomRate(double value);
	double RtssSyncedRate() const noexcept;
	int32_t RtssExitPolicyIndex() const noexcept;
	void RtssExitPolicyIndex(int32_t value);
	bool IsRtssManaged() const noexcept;
	bool ShowRtssCustomRate() const noexcept;
	bool ShowRtssSyncedRate() const noexcept;
	bool IsRtssSyncAvailable() const noexcept;
	bool ShowRtssGlobalNotice() const noexcept;
	hstring RtssStatusTitle() const;
	hstring RtssStatusDescription() const;
	hstring RtssStatusGlyph() const;
	bool IsRtssBusy() const;
	bool ShowRtssManagement() const;
	hstring RtssProgramStatus() const;
	hstring RtssSelfStatus() const;
	hstring RtssManagementResult() const;
	bool ShowRtssManagementResult() const;
	bool CanRecheckRtss() const;
	hstring RtssStatusText() const;
	IVector<hstring> RtssStatusSegments() const;
	hstring RtssSyncText() const;
	bool CanLaunchRtss() const;
	bool CanInitializeMagpie() const;
	bool ShowRtssInitialize() const;
	hstring RtssInitializeText() const;
	void InitializeMagpieRtss();
	bool CanLaunchAfterburner() const;
	hstring RtssLaunchText() const;
	hstring AfterburnerLaunchText() const;
	void RefreshRtss();
	void LaunchRtss();
	void LaunchAfterburner();
	void ContentFrameRateModeIndex(int32_t value);
	double ContentFrameRate() const noexcept;
	void ContentFrameRate(double value);
	int32_t FrameSyncModeIndex() const noexcept;
	void FrameSyncModeIndex(int32_t value);
	bool IsContentPacingEnabled() const noexcept;
	bool ShowContentFrameRate() const noexcept;
	int32_t CursorRefreshModeIndex() const noexcept;
	void CursorRefreshModeIndex(int32_t value);
	int32_t CursorSupplementModeIndex() const noexcept;
	void CursorSupplementModeIndex(int32_t value);
	double CursorSupplementRate() const noexcept;
	void CursorSupplementRate(double value);
	bool ShowCursorSupplement() const noexcept;
	bool ShowCursorSupplementRate() const noexcept;
	int32_t IdleRedrawModeIndex() const noexcept;
	void IdleRedrawModeIndex(int32_t value);
	double IdleRedrawRate() const noexcept;
	void IdleRedrawRate(double value);
	bool ShowIdleRedrawRate() const noexcept;
	int32_t DuplicateFrameDetectionMode() const noexcept;
	void DuplicateFrameDetectionMode(int32_t value);
	bool ShowFrameRefreshNotice() const noexcept;
	hstring FrameRefreshNotice() const noexcept;
	void ResetFrameRefresh();
	bool IsVRREnabled() const noexcept;
	void IsVRREnabled(bool value);

	bool IsCaptureTitleBar() const noexcept;
	void IsCaptureTitleBar(bool value);

	bool CanCaptureTitleBar() const noexcept;

	bool IsCroppingEnabled() const noexcept;
	void IsCroppingEnabled(bool value);

	double CroppingLeft() const noexcept;
	void CroppingLeft(double value);

	double CroppingTop() const noexcept;
	void CroppingTop(double value);

	double CroppingRight() const noexcept;
	void CroppingRight(double value);

	double CroppingBottom() const noexcept;
	void CroppingBottom(double value);

	bool IsAdjustCursorSpeed() const noexcept;
	void IsAdjustCursorSpeed(bool value);

	int CursorScaling() const noexcept;
	void CursorScaling(int value);

	double CustomCursorScaling() const noexcept;
	void CustomCursorScaling(double value);

	int CursorInterpolationMode() const noexcept;
	void CursorInterpolationMode(int value);
	bool IsAutoHideCursorEnabled() const noexcept;
	void IsAutoHideCursorEnabled(bool value);

	double AutoHideCursorDelay() const noexcept;
	void AutoHideCursorDelay(double value);

	hstring AutoHideCursorDelayText() const noexcept;

	hstring LaunchParameters() const noexcept;
	void LaunchParameters(const hstring& value);

	int DestAlignment() const noexcept;
	void DestAlignment(int value);

	bool IsDirectFlipDisabled() const noexcept;
	void IsDirectFlipDisabled(bool value);

private:
	void _NotifyFrameRefreshChanged();
	void _NotifyRtssChanged();
	void _SaveRtss();
	void _LoadRtssPreview();
	::Magpie::RtssStatus _RtssStatus() const;
	void _SaveFrameRefresh();
	fire_and_forget _LoadIcon();

	void _BuildGraphicsCards();
	void _RefreshGraphicsCardSettings();

	void _BuildMonitorOptions();

	bool _isProgramExist = true;

	hstring _renameText;
	std::wstring_view _trimedRenameText;

	uint32_t _index = 0;
	// 可以保存此指针的原因是: 用户停留在此页面时不会有缩放配置被创建或删除
	::Magpie::Profile* _data = nullptr;

	::Magpie::MultithreadEvent<bool>::EventRevoker _appThemeChangedRevoker;
	::Magpie::Event<uint32_t>::EventRevoker _dpiChangedRevoker;
	::Magpie::Event<>::EventRevoker _adaptersChangedRevoker;
	::Magpie::Event<::Magpie::Profile&>::EventRevoker _frameSyncChangedRevoker;
	::Magpie::Event<>::EventRevoker _frameRefreshModeChangedRevoker;
	::Magpie::Event<>::EventRevoker _rtssStatusRevoker;
	::Magpie::Event<bool>::EventRevoker _scalingStatusRevoker;
	::Magpie::RtssStatus _rtssPreview;
	uint64_t _rtssPreviewGeneration = 0;

	IconElement _icon{ nullptr };
	IVector<IInspectable> _monitorOptions{ nullptr };
	IVector<IInspectable> _graphicsCards{ nullptr };
	std::vector<::Magpie::GraphicsCardId> _graphicsCardOptions;
	std::vector<std::wstring> _monitorIds;
	std::vector<std::wstring> _monitorNames;

	const bool _isDefaultProfile = true;
	bool _isRenameConfirmButtonEnabled = false;
	// Ignore binding writes while rebuilding the list; stale item identities are also rejected.
	bool _isHandlingAdapterChanged = false;
};

}
