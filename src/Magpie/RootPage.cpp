#include "pch.h"
#include "RootPage.h"
#include "ScalingModesPage.h"
#if __has_include("RootPage.g.cpp")
#include "RootPage.g.cpp"
#endif
#include "XamlHelper.h"
#include "Win32Helper.h"
#include "ProfileService.h"
#include "AppXReader.h"
#include "IconHelper.h"
#include "ControlHelper.h"
#include "ThemeHelper.h"
#include "ContentDialogHelper.h"
#include "LocalizationService.h"
#include "App.h"
#include "TitleBarControl.h"
#include "MainWindow.h"
#include "CandidateWindowItem.h"
#include "CommonSharedConstants.h"
#include "Logger.h"
#include <cmath>
#include <winrt/Windows.Devices.Input.h>
#include <winrt/Windows.UI.Input.h>

using namespace ::Magpie;
using namespace winrt;
using namespace Windows::Graphics::Display;
using namespace Windows::Graphics::Imaging;
using namespace Windows::UI::ViewManagement;
using namespace Windows::UI;
using namespace Windows::UI::Xaml::Controls::Primitives;
using namespace Windows::UI::Xaml::Input;
using namespace Windows::UI::Xaml::Media::Animation;
using namespace Windows::UI::Xaml::Media::Imaging;

namespace winrt::Magpie::implementation {

static constexpr uint32_t FIRST_PROFILE_ITEM_IDX = 4;

RootPage::RootPage() {
	// 设置 Language 属性帮助 XAML 选择合适的字体，比如繁体中文使用 Microsoft JhengHei UI，
	// 日语使用 Yu Gothic UI
	Language(LocalizationService::Get().Language());
}

RootPage::~RootPage() {
	_FinishProfileReorder(false);
	ContentDialogHelper::CloseActiveDialog();

	// 不手动置空会内存泄露
	// 似乎是 XAML Islands 的 bug？
	ContentFrame().Content(nullptr);

	// 每次主窗口关闭都清理 AppXReader 的缓存
	AppXReader::ClearCache();
}

void RootPage::InitializeComponent() {
	RootPageT::InitializeComponent();

	_appThemeChangedRevoker = App::Get().ThemeChanged(
		auto_revoke, [this](bool) { _UpdateTheme(true); });
	_UpdateTheme(false);

	_dpiChangedRevoker = App::Get().MainWindow().DpiChanged(
		auto_revoke, [this](uint32_t) { _UpdateIcons(false); });

	ProfileService& profileService = ProfileService::Get();
	_profileAddedRevoker = profileService.ProfileAdded(
		auto_revoke, std::bind_front(&RootPage::_ProfileService_ProfileAdded, this));
	_profileRenamedRevoker = profileService.ProfileRenamed(
		auto_revoke, std::bind_front(&RootPage::_ProfileService_ProfileRenamed, this));
	_profileRemovedRevoker = profileService.ProfileRemoved(
		auto_revoke, std::bind_front(&RootPage::_ProfileService_ProfileRemoved, this));
	_profileMovedRevoker = profileService.ProfileMoved(
		auto_revoke, std::bind_front(&RootPage::_ProfileService_ProfileMoved, this));

	const Win32Helper::OSVersion& osVersion = Win32Helper::GetOSVersion();
	if (osVersion.Is22H2OrNewer()) {
		// Win11 22H2+ 使用系统的 Mica 背景
		MUXC::BackdropMaterial::SetApplyToRootOrPageBackground(*this, true);
	}

	IVector<IInspectable> navMenuItems = RootNavigationView().MenuItems();
	const std::vector<Profile>& profiles = AppSettings::Get().Profiles();
	_profileNavigationViewModels.reserve(profiles.size());
	for (uint32_t i = 0; i < profiles.size(); ++i) {
		MUXC::NavigationViewItem item = _CreateProfileNavigationViewItem(i, profiles[i]);
		// 用于占位
		navMenuItems.InsertAt(navMenuItems.Size() - 1, item);
	}

	// XAML Islands 的 CoreInput 走独立 InputSink，不一定在主消息循环产生 WM_LBUTTONDOWN。
	// 当窗口化轻退浮窗因拖出边界松手而丢失轻退状态时，在 RootPage 收到指针按下时兜底关闭，
	// 并释放被点中控件的指针捕获以避免同一下点击误触底层按钮。
	AddHandler(
		UIElement::PointerPressedEvent(),
		box_value(PointerEventHandler{ [weak = get_weak()](IInspectable const&, PointerRoutedEventArgs const& args) {
			const auto self = weak.get();
			if (!self || !self->XamlRoot()) return;
			const DependencyObject source = args.OriginalSource().try_as<DependencyObject>();
			if (XamlHelper::IsInsideOpenWindowedPopup(self->XamlRoot(), source)) return;
			if (XamlHelper::CloseLightDismissWindowedPopups(self->XamlRoot())) {
				for (DependencyObject cur = source; cur; cur = VisualTreeHelper::GetParent(cur)) {
					if (const auto ui = cur.try_as<UIElement>()) {
						if (auto captures = ui.PointerCaptures(); captures && captures.Size() > 0) {
							ui.ReleasePointerCaptures();
						}
					}
				}
				args.Handled(true);
			}
		} }),
		true
	);
}

static void SkipToggleSwitchAnimations(const DependencyObject& elem) {
	FrameworkElement rootGrid = VisualTreeHelper::GetChild(elem, 0).try_as<FrameworkElement>();

	for (VisualStateGroup group : VisualStateManager::GetVisualStateGroups(rootGrid)) {
		for (VisualState state : group.States()) {
			if (Storyboard storyboard = state.Storyboard()) {
				storyboard.SkipToFill();
			}
		}
	}
}

void RootPage::RootPage_Loaded(IInspectable const&, RoutedEventArgs const&) {
	// 消除焦点框
	IsTabStop(true);
	Focus(FocusState::Programmatic);
	IsTabStop(false);

	// 设置 NavigationView 内的 Tooltip 的主题
	XamlHelper::UpdateThemeOfTooltips(RootNavigationView(), ActualTheme());

	// 启动时跳过 ToggleSwitch 的动画
	std::vector<DependencyObject> elems{ *this };
	do {
		std::vector<DependencyObject> temp;

		for (const DependencyObject& elem : elems) {
			const int count = VisualTreeHelper::GetChildrenCount(elem);
			for (int i = 0; i < count; ++i) {
				DependencyObject current = VisualTreeHelper::GetChild(elem, i);

				if (get_class_name(current) == name_of<ToggleSwitch>()) {
					SkipToggleSwitchAnimations(current);
				} else {
					temp.emplace_back(std::move(current));
				}
			}
		}

		elems = std::move(temp);
	} while (!elems.empty());
}

void RootPage::NavigationView_SelectionChanged(
	MUXC::NavigationView const&,
	MUXC::NavigationViewSelectionChangedEventArgs const& args
) {
	auto contentFrame = ContentFrame();

	if (args.IsSettingsSelected()) {
		contentFrame.Navigate(xaml_typename<SettingsPage>());
	} else {
		IInspectable selectedItem = args.SelectedItem();
		if (!selectedItem) {
			contentFrame.Content(nullptr);
			return;
		}

		IInspectable tag = selectedItem.try_as<MUXC::NavigationViewItem>().Tag();
		if (tag) {
			hstring tagStr = unbox_value<hstring>(tag);
			Interop::TypeName typeName;
			if (tagStr == L"Home") {
				typeName = xaml_typename<HomePage>();
			} else if (tagStr == L"ScalingModes") {
				typeName = xaml_typename<winrt::Magpie::ScalingModesPage>();
			} else if (tagStr == L"About") {
				typeName = xaml_typename<AboutPage>();
			} else {
				typeName = xaml_typename<HomePage>();
			}

			contentFrame.Navigate(typeName);
		} else {
			// 缩放配置页面
			MUXC::NavigationView nv = RootNavigationView();
			uint32_t index;
			if (nv.MenuItems().IndexOf(nv.SelectedItem(), index)) {
				contentFrame.Navigate(xaml_typename<ProfilePage>(), box_value((int)index - 4));
			}
		}
	}
}

void RootPage::NavigationView_PaneOpening(MUXC::NavigationView const&, IInspectable const&) {
	if (Win32Helper::GetOSVersion().IsWin11()) {
		// Win11 中 Tooltip 自动适应主题
		return;
	}

	XamlHelper::UpdateThemeOfTooltips(*this, ActualTheme());

	// UpdateThemeOfTooltips 中使用的 hack 会使 NavigationViewItem 在展开时不会自动删除 Tooltip
	// 因此这里手动删除
	const MUXC::NavigationView& nv = RootNavigationView();
	for (const IInspectable& item : nv.MenuItems()) {
		ToolTipService::SetToolTip(item.try_as<DependencyObject>(), nullptr);
	}
	for (const IInspectable& item : nv.FooterMenuItems()) {
		ToolTipService::SetToolTip(item.try_as<DependencyObject>(), nullptr);
	}
}

void RootPage::NavigationView_PaneClosing(MUXC::NavigationView const&, MUXC::NavigationViewPaneClosingEventArgs const&) {
	XamlHelper::UpdateThemeOfTooltips(*this, ActualTheme());
}

void RootPage::NavigationView_DisplayModeChanged(MUXC::NavigationView const& nv, MUXC::NavigationViewDisplayModeChangedEventArgs const&) {
	bool isExpanded = nv.DisplayMode() == MUXC::NavigationViewDisplayMode::Expanded;
	nv.IsPaneToggleButtonVisible(!isExpanded);
	if (isExpanded) {
		// 延迟设置 IsPaneOpen 才能起作用
		App::Get().Dispatcher().TryEnqueue(DispatcherQueuePriority::Low, [nv(MUXC::NavigationView(nv))]() {
			nv.IsPaneOpen(true);
		});
	}

	// !!! HACK !!!
	// 使导航栏的可滚动区域不会覆盖标题栏
	FrameworkElement menuItemsScrollViewer = nv.try_as<IControlProtected>()
		.GetTemplateChild(L"MenuItemsScrollViewer").try_as<FrameworkElement>();
	menuItemsScrollViewer.Margin({ 0,isExpanded ? TitleBar().ActualHeight() : 0.0,0,0});

	XamlHelper::UpdateThemeOfTooltips(*this, ActualTheme());
}

void RootPage::NavigationView_ItemInvoked(MUXC::NavigationView const&, MUXC::NavigationViewItemInvokedEventArgs const& args) {
	if (args.InvokedItemContainer() == NewProfileNavigationViewItem()) {
		_newProfileViewModel->PrepareForOpen();

		// 同步调用 ShowAt 有时会失败
		App::Get().Dispatcher().TryEnqueue([that(get_strong())]() {
			that->NewProfileFlyout().ShowAt(that->NewProfileNavigationViewItem());
		});
	}
}

void RootPage::ComboBox_DropDownOpened(IInspectable const& sender, IInspectable const&) const {
	ControlHelper::ComboBox_DropDownOpened(sender);
}

void RootPage::NewProfileConfirmButton_Click(IInspectable const&, RoutedEventArgs const&) {
	_newProfileViewModel->Confirm();
	NewProfileFlyout().Hide();
}

void RootPage::NewProfileNameContextFlyout_Opening(IInspectable const&, IInspectable const&) {
	auto menuItems = NewProfileNameContextFlyout().Items();
	
	int idx = _newProfileViewModel->CandidateWindowIndex();
	if (idx < 0) {
		// 隐藏所有选项
		for (const MenuFlyoutItemBase& item : menuItems) {
			if (IInspectable tag = item.Tag(); tag && tag.try_as<int>()) {
				item.Visibility(Visibility::Collapsed);
			}
		}

		return;
	}

	CandidateWindowItem* selectedItem = get_self<CandidateWindowItem>(
		_newProfileViewModel->CandidateWindows().GetAt(idx).try_as<winrt::Magpie::CandidateWindowItem>());

	// 设置每个选项的可见性
	bool shouldInit = true;
	for (const MenuFlyoutItemBase& item : menuItems) {
		IInspectable tag = item.Tag();
		if (!tag) {
			continue;
		}

		std::optional<int> id = tag.try_as<int>();
		if (!id) {
			continue;
		}

		shouldInit = false;

		if (*id == 1) {
			// 填入进程名选项
			item.Visibility(selectedItem->AUMID().empty() ? Visibility::Visible : Visibility::Collapsed);
		} else if (*id == 2) {
			// 填入应用名选项
			item.Visibility(selectedItem->AUMID().empty() ? Visibility::Collapsed : Visibility::Visible);
		} else {
			// 填入窗口标题选项
			item.Visibility(Visibility::Visible);
		}
	}

	if (!shouldInit) {
		return;
	}

	// 惰性初始化
	ResourceLoader resourceLoader =
		ResourceLoader::GetForCurrentView(CommonSharedConstants::APP_RESOURCE_MAP_ID);

	// 填入进程名
	MenuFlyoutItem item1;
	FontIcon icon1;
	icon1.Glyph(L"\xE9F5");
	item1.Text(resourceLoader.GetString(L"Root_NewProfileFlyout_NameContextFlyout_ProcessName"));
	item1.Icon(icon1);
	RoutedEventHandler clickHandler([this](IInspectable const&, IInspectable const&) {
		_UpdateNewProfileNameTextBox(false);
	});
	item1.Click(clickHandler);
	item1.Tag(box_value(1));
	menuItems.Append(item1);

	// 填入应用名
	MenuFlyoutItem item2;
	FontIcon icon2;
	icon2.Glyph(L"\xECAA");
	item2.Text(resourceLoader.GetString(L"Root_NewProfileFlyout_NameContextFlyout_AppName"));
	item2.Icon(icon2);
	item2.Click(clickHandler);
	item2.Tag(box_value(2));
	menuItems.Append(item2);

	if (selectedItem->AUMID().empty()) {
		item2.Visibility(Visibility::Collapsed);
	} else {
		item1.Visibility(Visibility::Collapsed);
	}

	// 填入窗口标题
	MenuFlyoutItem item3;
	FontIcon icon3;
	icon3.Glyph(L"\xE737");
	item3.Icon(icon3);
	item3.Text(resourceLoader.GetString(L"Root_NewProfileFlyout_NameContextFlyout_WindowTitle"));
	item3.Click([this](IInspectable const&, IInspectable const&) {
		_UpdateNewProfileNameTextBox(true);
	});
	item3.Tag(box_value(3));
	menuItems.Append(item3);
}

void RootPage::NewProfileNameTextBox_KeyDown(IInspectable const&, Input::KeyRoutedEventArgs const& args) {
	if (args.Key() == VirtualKey::Enter && _newProfileViewModel->IsConfirmButtonEnabled()) {
		NewProfileConfirmButton_Click(nullptr, nullptr);
	}
}

void RootPage::ProfileMoreOptionsButton_Click(
	IInspectable const& sender,
	RoutedEventArgs const&) {
	_profileMoreOptionsButton = sender.try_as<Button>();
}

static void QueueShowProfileAttachedFlyout(
	IInspectable const& sender,
	Button const& placementTarget) {
	FrameworkElement menuItem = sender.try_as<FrameworkElement>();
	if (!menuItem || !placementTarget) {
		return;
	}

	FlyoutBase flyout = FlyoutBase::GetAttachedFlyout(menuItem);
	if (flyout) {
		App::Get().Dispatcher().TryEnqueue(
			DispatcherQueuePriority::Low,
			[flyout, placementTarget]() {
				try {
					flyout.ShowAt(placementTarget);
				} catch (...) {
					// The profile item may have been removed before the queued callback.
				}
			});
	}
}

void RootPage::ProfileRenameMenuItem_Click(
	IInspectable const& sender,
	RoutedEventArgs const&) {
	Button placementTarget = std::exchange(_profileMoreOptionsButton, nullptr);
	QueueShowProfileAttachedFlyout(sender, placementTarget);
}

void RootPage::ProfileDeleteMenuItem_Click(
	IInspectable const& sender,
	RoutedEventArgs const&) {
	Button placementTarget = std::exchange(_profileMoreOptionsButton, nullptr);
	QueueShowProfileAttachedFlyout(sender, placementTarget);
}

void RootPage::ProfileRenameTextBox_Loaded(
	IInspectable const& sender,
	RoutedEventArgs const&) {
	TextBox textBox = sender.try_as<TextBox>();
	if (textBox) {
		textBox.Focus(FocusState::Programmatic);
		textBox.SelectAll();
	}
}

void RootPage::NavigateToAboutPage() {
	MUXC::NavigationView nv = RootNavigationView();
	nv.SelectedItem(nv.FooterMenuItems().GetAt(0));
}

bool RootPage::NavigateToIssueProfile(std::wstring_view name, std::wstring_view pathRule, std::wstring_view classRule) {
	const auto& profiles = AppSettings::Get().Profiles();
	int selected = -1;
	if (!name.empty() || !pathRule.empty() || !classRule.empty()) {
		const auto found = std::ranges::find_if(profiles, [&](const Profile& profile) {
			return profile.name == name && profile.pathRule == pathRule && profile.classNameRule == classRule;
		});
		if (found == profiles.end()) return false;
		selected = static_cast<int>(found - profiles.begin());
	}
	auto nv = RootNavigationView();
	nv.SelectedItem(nv.MenuItems().GetAt(static_cast<uint32_t>(selected + int(FIRST_PROFILE_ITEM_IDX))));
	return true;
}

void RootPage::NavigateToHome() {
	RootNavigationView().SelectedItem(RootNavigationView().MenuItems().GetAt(0));
}

void RootPage::NavigateToIssueEffects(const ::Magpie::IssueContext& context) {
	NavigateToScalingModes();
	if (const auto page = ContentFrame().Content().try_as<winrt::Magpie::ScalingModesPage>())
		get_self<ScalingModesPage>(page)->RevealIssueTarget(context);
}

void RootPage::NavigateToScalingModes() {
	auto nv = RootNavigationView();
	nv.SelectedItem(nv.MenuItems().GetAt(1));
}

TitleBarControl& RootPage::TitleBar() {
	return *get_self<TitleBarControl>(RootPageT::TitleBar());
}

static Color Win32ColorToWinRTColor(COLORREF color) {
	return { 255, GetRValue(color), GetGValue(color), GetBValue(color) };
}

void RootPage::_UpdateTheme(bool updateIcons) {
	const bool isLightTheme = App::Get().IsLightTheme();

	if (IsLoaded() && (ActualTheme() == ElementTheme::Light) == isLightTheme) {
		// 无需切换
		return;
	}

	if (!Win32Helper::GetOSVersion().Is22H2OrNewer()) {
		const Windows::UI::Color bkgColor = Win32ColorToWinRTColor(
			isLightTheme ? ThemeHelper::LIGHT_TINT_COLOR : ThemeHelper::DARK_TINT_COLOR);
		Background(SolidColorBrush(bkgColor));
	}

	ElementTheme newTheme = isLightTheme ? ElementTheme::Light : ElementTheme::Dark;
	RequestedTheme(newTheme);

	XamlHelper::UpdateThemeOfXamlPopups(XamlRoot(), newTheme);
	XamlHelper::UpdateThemeOfTooltips(*this, newTheme);

	if (updateIcons && IsLoaded()) {
		_UpdateIcons(true);
	}
}

MUXC::NavigationViewItem RootPage::_CreateProfileNavigationViewItem(
	uint32_t index,
	const Profile& profile) {
	MUXC::NavigationViewItem item;
	item.HorizontalContentAlignment(HorizontalAlignment::Stretch);

	auto viewModel = make_self<ProfileViewModel>(static_cast<int>(index), false);
	// Keep the profile template off NavigationViewItem itself. Its presenter also owns the
	// icon slot, and applying the template there can make the whole profile row appear as
	// a scaled-down icon instead of the executable icon.
	ContentPresenter contentPresenter;
	contentPresenter.HorizontalAlignment(HorizontalAlignment::Stretch);
	contentPresenter.HorizontalContentAlignment(HorizontalAlignment::Stretch);
	contentPresenter.Content(*viewModel);
	contentPresenter.ContentTemplate(Resources()
		.Lookup(box_value(L"ProfileNavigationItemContentTemplate"))
		.as<DataTemplate>());
	item.Content(contentPresenter);
	_profileNavigationViewModels.emplace_back(std::move(viewModel));

	// Reserve the icon slot while the executable icon is loaded asynchronously.
	item.Icon(FontIcon());
	_LoadIcon(item, profile);
	return item;
}

void RootPage::_RebindProfileNavigationViewModels() noexcept {
	for (uint32_t i = 0; i < _profileNavigationViewModels.size(); ++i) {
		_profileNavigationViewModels[i]->Rebind(i);
	}
}

fire_and_forget RootPage::_LoadIcon(MUXC::NavigationViewItem const& item, const Profile& profile) {
	weak_ref<MUXC::NavigationViewItem> weakRef(item);

	bool preferLightTheme = App::Get().IsLightTheme();
	bool isPackaged = profile.isPackaged;
	std::wstring path = profile.pathRule;
	const uint32_t iconSize = (uint32_t)std::lroundf(
		16.0f * App::Get().MainWindow().CurrentDpi() / USER_DEFAULT_SCREEN_DPI);

	co_await resume_background();

	std::wstring iconPath;
	SoftwareBitmap iconBitmap{ nullptr };

	if (isPackaged) {
		AppXReader reader;
		if (reader.Initialize(path)) {
			std::variant<std::wstring, SoftwareBitmap> uwpIcon =
				reader.GetIcon(iconSize, preferLightTheme);
			if (uwpIcon.index() == 0) {
				iconPath = std::get<0>(uwpIcon);
			} else {
				iconBitmap = std::get<1>(uwpIcon);
			}
		}
	} else {
		iconBitmap = IconHelper::ExtractIconFromExe(path.c_str(), iconSize);
	}

	co_await App::Get().Dispatcher();

	auto strongRef = weakRef.get();
	if (!strongRef) {
		co_return;
	}

	if (!iconPath.empty()) {
		BitmapIcon icon;
		icon.ShowAsMonochrome(false);
		icon.UriSource(Uri(iconPath));
		icon.Width(16);
		icon.Height(16);

		strongRef.Icon(icon);
	} else if (iconBitmap) {
		SoftwareBitmapSource imageSource;
		co_await imageSource.SetBitmapAsync(iconBitmap);

		MUXC::ImageIcon imageIcon;
		imageIcon.Width(16);
		imageIcon.Height(16);
		imageIcon.Source(imageSource);

		strongRef.Icon(imageIcon);
	} else {
		FontIcon icon;
		icon.Glyph(L"\uECAA");
		strongRef.Icon(icon);
	}
}

void RootPage::_UpdateIcons(bool skipDesktop) {
	IVector<IInspectable> navMenuItems = RootNavigationView().MenuItems();
	const std::vector<Profile>& profiles = AppSettings::Get().Profiles();

	for (uint32_t i = 0; i < profiles.size(); ++i) {
		if (skipDesktop && !profiles[i].isPackaged) {
			continue;
		}

		MUXC::NavigationViewItem item = navMenuItems.GetAt(FIRST_PROFILE_ITEM_IDX + i)
			.try_as<MUXC::NavigationViewItem>();
		_LoadIcon(item, profiles[i]);
	}
}

MUXC::NavigationViewItem RootPage::_FindParentProfileNavigationViewItem(
	DependencyObject const& element) const noexcept {
	DependencyObject current = VisualTreeHelper::GetParent(element);
	while (current) {
		if (MUXC::NavigationViewItem item = current.try_as<MUXC::NavigationViewItem>()) {
			return item;
		}
		current = VisualTreeHelper::GetParent(current);
	}

	return nullptr;
}

uint32_t RootPage::_GetProfileReorderTargetIndex(double pointerY) const noexcept {
	const uint32_t size = static_cast<uint32_t>(_profileNavigationViewModels.size());
	if (!_profileReorderItem || _profileReorderOriginalIndex >= size ||
		_profileReorderItemCenters.size() != size) {
		return _profileReorderOriginalIndex;
	}

	try {
		uint32_t targetIndex = 0;
		bool foundContainer = false;
		for (uint32_t i = 0; i < size; ++i) {
			if (i == _profileReorderOriginalIndex) {
				continue;
			}

			const double centerY = _profileReorderItemCenters[i];
			if (!std::isfinite(centerY)) {
				continue;
			}

			foundContainer = true;
			if (pointerY < centerY) {
				break;
			}
			++targetIndex;
		}

		return foundContainer ? std::min(targetIndex, size - 1) : _profileReorderOriginalIndex;
	} catch (...) {
		return _profileReorderOriginalIndex;
	}
}

void RootPage::_PrepareProfileReorderPreview() noexcept {
	_profileReorderPreviewItems.clear();
	_profileReorderItemCenters.clear();
	_profileReorderSlotExtent = 0;

	if (!_profileReorderContainer || !_profileReorderItem) {
		return;
	}

	try {
		MUXC::NavigationView nv = RootNavigationView();
		IVector<IInspectable> menuItems = nv.MenuItems();
		const uint32_t size = static_cast<uint32_t>(_profileNavigationViewModels.size());
		_profileReorderPreviewItems.reserve(size > 0 ? size - 1 : 0);
		_profileReorderItemCenters.assign(size, std::numeric_limits<double>::quiet_NaN());

		double draggedTop = 0;
		double nextTop = 0;
		bool hasDraggedTop = false;
		bool hasNextTop = false;

		for (uint32_t i = 0; i < size; ++i) {
			FrameworkElement container = menuItems
				.GetAt(FIRST_PROFILE_ITEM_IDX + i)
				.try_as<FrameworkElement>();
			if (!container) {
				continue;
			}

			const Point topLeft = container.TransformToVisual(nv).TransformPoint({});
			_profileReorderItemCenters[i] = topLeft.Y + container.ActualHeight() / 2;

			if (i == _profileReorderOriginalIndex) {
				draggedTop = topLeft.Y;
				hasDraggedTop = true;
				continue;
			}
			if (i == _profileReorderOriginalIndex + 1) {
				nextTop = topLeft.Y;
				hasNextTop = true;
			}

			ProfileReorderPreviewItem preview;
			preview.index = i;
			preview.container = container;
			preview.originalRenderTransform = container.RenderTransform();
			preview.previewTransform = CompositeTransform();

			TransformGroup transforms;
			if (preview.originalRenderTransform) {
				transforms.Children().Append(preview.originalRenderTransform);
			}
			transforms.Children().Append(preview.previewTransform);
			container.RenderTransform(transforms);

			_profileReorderPreviewItems.emplace_back(std::move(preview));
		}

		if (hasDraggedTop && hasNextTop && nextTop > draggedTop) {
			_profileReorderSlotExtent = nextTop - draggedTop;
		} else {
			const Thickness margin = _profileReorderContainer.Margin();
			_profileReorderSlotExtent =
				_profileReorderContainer.ActualHeight() + margin.Top + margin.Bottom;
		}
	} catch (...) {
		_ClearProfileReorderPreview();
	}
}

void RootPage::_UpdateProfileReorderPreview(uint32_t targetIndex) noexcept {
	if (_profileReorderSlotExtent <= 0) {
		return;
	}

	try {
		for (ProfileReorderPreviewItem& preview : _profileReorderPreviewItems) {
			double offset = 0;
			if (_profileReorderOriginalIndex < targetIndex &&
				preview.index > _profileReorderOriginalIndex && preview.index <= targetIndex) {
				offset = -_profileReorderSlotExtent;
			} else if (targetIndex < _profileReorderOriginalIndex &&
				preview.index >= targetIndex && preview.index < _profileReorderOriginalIndex) {
				offset = _profileReorderSlotExtent;
			}

			if (preview.targetOffset == offset) {
				continue;
			}

			preview.previewTransform.TranslateY(offset);
			preview.targetOffset = offset;
		}
	} catch (...) {
		// The next pointer event can continue updating any remaining containers.
	}
}

void RootPage::_ClearProfileReorderPreview() noexcept {
	for (ProfileReorderPreviewItem& preview : _profileReorderPreviewItems) {
		try {
			preview.container.RenderTransform(preview.originalRenderTransform);
		} catch (...) {
		}
	}

	_profileReorderPreviewItems.clear();
	_profileReorderItemCenters.clear();
	_profileReorderSlotExtent = 0;
}

void RootPage::_QueueFinishProfileReorder(bool commit) noexcept {
	if (!_profileReorderHandle) {
		return;
	}

	_queuedProfileReorderCommit = _queuedProfileReorderCommit || commit;
	if (_isProfileReorderFinishQueued) {
		return;
	}

	_isProfileReorderFinishQueued = true;
	const uint32_t pointerId = _profileReorderPointerId;
	try {
		weak_ref<RootPage> weakThis = get_weak();
		if (App::Get().Dispatcher().TryEnqueue(
			DispatcherQueuePriority::Low,
			[weakThis, pointerId]() {
				com_ptr<RootPage> self = weakThis.get();
				if (!self || !self->_isProfileReorderFinishQueued ||
					self->_profileReorderPointerId != pointerId) {
					return;
				}

				const bool shouldCommit =
					std::exchange(self->_queuedProfileReorderCommit, false);
				self->_isProfileReorderFinishQueued = false;
				self->_FinishProfileReorder(shouldCommit);
			})) {
			return;
		}
	} catch (...) {
	}

	_isProfileReorderFinishQueued = false;
	_queuedProfileReorderCommit = false;
}

void RootPage::_FinishProfileReorder(bool commit) noexcept {
	FrameworkElement container = std::exchange(_profileReorderContainer, nullptr);
	MUXC::NavigationViewItem item = std::exchange(_profileReorderItem, nullptr);
	const uint32_t targetIndex = _profileReorderTargetIndex;
	const bool wasDragging = _isProfileReorderDragging;

	if (commit && wasDragging && item) {
		try {
			MUXC::NavigationView nv = RootNavigationView();
			IVector<IInspectable> menuItems = nv.MenuItems();
			uint32_t menuIndex = 0;
			if (menuItems.IndexOf(item, menuIndex) && menuIndex >= FIRST_PROFILE_ITEM_IDX) {
				const uint32_t currentIndex = menuIndex - FIRST_PROFILE_ITEM_IDX;
				const uint32_t profileCount = ProfileService::Get().GetProfileCount();
				if (currentIndex < profileCount && targetIndex < profileCount &&
					currentIndex != targetIndex) {
					// NavigationView performs layout synchronously for collection changes.
					// Remove all preview transforms before it receives the notification. The
					// atomic menu replacement below completes in the same UI tick, matching
					// the immediate exchange behavior used by the scaling-mode list.
					_ClearProfileReorderPreview();
					if (container) {
						container.RenderTransform(_profileOriginalRenderTransform);
					}

					if (ProfileService::Get().MoveProfile(currentIndex, targetIndex)) {
						nv.UpdateLayout();
					}
				}
			}
		} catch (...) {
			// The page can be closing while pointer completion is queued.
		}
	}

	_ClearProfileReorderPreview();

	if (container) {
		try {
			container.RenderTransform(_profileOriginalRenderTransform);
			container.Opacity(_profileOriginalOpacity);
			Canvas::SetZIndex(container, _profileOriginalZIndex);
		} catch (...) {
		}
	}

	_profileReorderHandle = nullptr;
	_profileOriginalRenderTransform = nullptr;
	_profileDragTransform = nullptr;
	_profileReorderPointerId = 0;
	_profileReorderOriginalIndex = 0;
	_isProfileReorderDragging = false;
	_isProfileReorderFinishQueued = false;
	_queuedProfileReorderCommit = false;

}

void RootPage::ProfileReorderHandle_PointerPressed(
	IInspectable const& sender,
	PointerRoutedEventArgs const& args) {
	try {
		FrameworkElement handle = sender.try_as<FrameworkElement>();
		if (_profileReorderHandle || !handle || !handle.Tag() ||
			_profileNavigationViewModels.size() < 2) {
			return;
		}

		auto pointerPoint = args.GetCurrentPoint(handle);
		if (pointerPoint.PointerDevice().PointerDeviceType() ==
			Windows::Devices::Input::PointerDeviceType::Mouse &&
			!pointerPoint.Properties().IsLeftButtonPressed()) {
			return;
		}

		MUXC::NavigationViewItem item = _FindParentProfileNavigationViewItem(handle);
		if (!item) {
			return;
		}

		IVector<IInspectable> menuItems = RootNavigationView().MenuItems();
		uint32_t menuIndex = 0;
		if (!menuItems.IndexOf(item, menuIndex) || menuIndex < FIRST_PROFILE_ITEM_IDX) {
			return;
		}

		const uint32_t profileIndex = menuIndex - FIRST_PROFILE_ITEM_IDX;
		if (profileIndex >= _profileNavigationViewModels.size() ||
			!handle.CapturePointer(args.Pointer())) {
			return;
		}

		FrameworkElement container = item;
		_profileReorderHandle = handle;
		_profileReorderContainer = container;
		_profileReorderItem = item;
		_profileReorderOriginalIndex = profileIndex;
		_profileOriginalRenderTransform = container.RenderTransform();
		_profileOriginalOpacity = container.Opacity();
		_profileOriginalZIndex = Canvas::GetZIndex(container);
		_profileDragTransform = CompositeTransform();

		TransformGroup transforms;
		if (_profileOriginalRenderTransform) {
			transforms.Children().Append(_profileOriginalRenderTransform);
		}
		transforms.Children().Append(_profileDragTransform);
		container.RenderTransform(transforms);
		container.Opacity(0.92);
		Canvas::SetZIndex(container, 1000);
		_PrepareProfileReorderPreview();

		_profilePointerStartY = args.GetCurrentPoint(RootNavigationView()).Position().Y;
		_profileReorderTargetIndex = profileIndex;
		_profileReorderPointerId = args.Pointer().PointerId();
		_isProfileReorderDragging = false;
		args.Handled(true);
	} catch (const hresult_error& e) {
		Logger::Get().ComWarn("Failed to start profile drag", e.code());
		_QueueFinishProfileReorder(false);
	} catch (...) {
		Logger::Get().Warn("Failed to start profile drag");
		_QueueFinishProfileReorder(false);
	}
}

void RootPage::ProfileReorderHandle_PointerMoved(
	IInspectable const& sender,
	PointerRoutedEventArgs const& args) {
	try {
		if (!_profileReorderHandle || sender != _profileReorderHandle ||
			args.Pointer().PointerId() != _profileReorderPointerId) {
			return;
		}

		const double pointerY = args.GetCurrentPoint(RootNavigationView()).Position().Y;
		const double deltaY = pointerY - _profilePointerStartY;
		if (!_isProfileReorderDragging && std::abs(deltaY) >= 3) {
			_isProfileReorderDragging = true;
		}

		if (_isProfileReorderDragging) {
			_profileDragTransform.TranslateY(deltaY);
			const uint32_t targetIndex = _GetProfileReorderTargetIndex(pointerY);
			if (targetIndex != _profileReorderTargetIndex) {
				_profileReorderTargetIndex = targetIndex;
				_UpdateProfileReorderPreview(targetIndex);
			}
		}
		args.Handled(true);
	} catch (const hresult_error& e) {
		Logger::Get().ComWarn("Failed to update profile drag", e.code());
		_QueueFinishProfileReorder(false);
	} catch (...) {
		Logger::Get().Warn("Failed to update profile drag");
		_QueueFinishProfileReorder(false);
	}
}

void RootPage::ProfileReorderHandle_PointerReleased(
	IInspectable const& sender,
	PointerRoutedEventArgs const& args) {
	if (_profileReorderHandle && sender == _profileReorderHandle &&
		args.Pointer().PointerId() == _profileReorderPointerId) {
		args.Handled(true);
		_QueueFinishProfileReorder(true);
	}
}

void RootPage::ProfileReorderHandle_PointerCanceled(
	IInspectable const& sender,
	PointerRoutedEventArgs const& args) {
	if (_profileReorderHandle && sender == _profileReorderHandle &&
		args.Pointer().PointerId() == _profileReorderPointerId) {
		args.Handled(true);
		_QueueFinishProfileReorder(false);
	}
}

void RootPage::ProfileReorderHandle_PointerCaptureLost(
	IInspectable const& sender,
	PointerRoutedEventArgs const& args) {
	if (_profileReorderHandle && sender == _profileReorderHandle &&
		args.Pointer().PointerId() == _profileReorderPointerId) {
		_QueueFinishProfileReorder(false);
	}
}

void RootPage::_ProfileService_ProfileAdded(Profile& profile) {
	_RebindProfileNavigationViewModels();
	const uint32_t index = ProfileService::Get().GetProfileCount() - 1;
	MUXC::NavigationViewItem item = _CreateProfileNavigationViewItem(index, profile);
	// 用于占位
	IVector<IInspectable> navMenuItems = RootNavigationView().MenuItems();
	navMenuItems.InsertAt(navMenuItems.Size() - 1, item);
	RootNavigationView().SelectedItem(item);
}

void RootPage::_ProfileService_ProfileRenamed(uint32_t idx) {
	if (idx < _profileNavigationViewModels.size()) {
		_profileNavigationViewModels[idx]->Rebind(idx);
	}
}

void RootPage::_ProfileService_ProfileRemoved(uint32_t idx) {
	MUXC::NavigationView nv = RootNavigationView();
	IVector<IInspectable> menuItems = nv.MenuItems();
	nv.SelectedItem(menuItems.GetAt(FIRST_PROFILE_ITEM_IDX - 1));
	menuItems.RemoveAt(FIRST_PROFILE_ITEM_IDX + idx);
	if (idx < _profileNavigationViewModels.size()) {
		_profileNavigationViewModels.erase(_profileNavigationViewModels.begin() + idx);
	}
	_RebindProfileNavigationViewModels();
}

void RootPage::_ProfileService_ProfileMoved(uint32_t fromIndex, uint32_t toIndex) {
	if (fromIndex >= _profileNavigationViewModels.size() ||
		toIndex >= _profileNavigationViewModels.size() ||
		fromIndex == toIndex) {
		return;
	}

	MUXC::NavigationView nv = RootNavigationView();
	IVector<IInspectable> menuItems = nv.MenuItems();
	IInspectable selectedItem = nv.SelectedItem();

	const uint32_t fromMenuIndex = FIRST_PROFILE_ITEM_IDX + fromIndex;
	const uint32_t toMenuIndex = FIRST_PROFILE_ITEM_IDX + toIndex;
	if (fromMenuIndex >= menuItems.Size() || toMenuIndex >= menuItems.Size()) {
		return;
	}

	std::vector<IInspectable> reorderedItems(menuItems.Size(), nullptr);
	menuItems.GetMany(0, reorderedItems);
	IInspectable movedItem = reorderedItems[fromMenuIndex];
	reorderedItems.erase(reorderedItems.begin() + fromMenuIndex);
	reorderedItems.insert(reorderedItems.begin() + toMenuIndex, std::move(movedItem));
	// NavigationView reacts to every collection notification with an internal layout.
	// Replace the complete order atomically so it never observes an intermediate list.
	menuItems.ReplaceAll(reorderedItems);

	com_ptr<ProfileViewModel> movedViewModel =
		std::move(_profileNavigationViewModels[fromIndex]);
	_profileNavigationViewModels.erase(_profileNavigationViewModels.begin() + fromIndex);
	_profileNavigationViewModels.insert(
		_profileNavigationViewModels.begin() + toIndex,
		std::move(movedViewModel));
	_RebindProfileNavigationViewModels();

	if (selectedItem) {
		nv.SelectedItem(selectedItem);
		uint32_t selectedIndex = 0;
		if (menuItems.IndexOf(selectedItem, selectedIndex) &&
			selectedIndex >= FIRST_PROFILE_ITEM_IDX &&
			selectedIndex < FIRST_PROFILE_ITEM_IDX + _profileNavigationViewModels.size()) {
			ContentFrame().Navigate(
				xaml_typename<ProfilePage>(),
				box_value(static_cast<int>(selectedIndex - FIRST_PROFILE_ITEM_IDX)));
		}
	}
}

void RootPage::_UpdateNewProfileNameTextBox(bool fillWithTitle) {
	int idx = _newProfileViewModel->CandidateWindowIndex();
	if (idx < 0) {
		return;
	}

	CandidateWindowItem* selectedItem = get_self<CandidateWindowItem>(
		_newProfileViewModel->CandidateWindows().GetAt(idx).try_as<winrt::Magpie::CandidateWindowItem>());
	hstring text = fillWithTitle ? selectedItem->Title() : selectedItem->DefaultProfileName();

	TextBox textBox = NewProfileNameTextBox();
	if (textBox.Text() == text) {
		return;
	}

	const int size = (int)text.size();
	// 遗憾的是设置 Text 属性会导致撤销/重做历史丢失
	textBox.Text(std::move(text));
	// 修改文本后将光标移到最后
	textBox.Select(size, 0);
	// 如果文本太长，这个调用可以使视口移到光标位置
	textBox.Focus(FocusState::Programmatic);
}

}
