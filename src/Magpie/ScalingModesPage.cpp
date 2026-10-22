#include "pch.h"
#include "ScalingModesPage.h"
#include "ScalingMode.h"
#if __has_include("ScalingModesPage.g.cpp")
#include "ScalingModesPage.g.cpp"
#endif
#include "ControlHelper.h"
#include "ContentDialogHelper.h"
#include "EffectsService.h"
#include "EffectHelper.h"
#include "EffectParametersViewModel.h"
#include "SettingsExpander.h"
#include "EffectParameterPopupLayout.h"
#include "App.h"
#include "AppSettings.h"
#include "CommonSharedConstants.h"
#include "MainWindow.h"
#include "Logger.h"
#include "ToastService.h"
#include "Win32Helper.h"
#include <cmath>
#include <parallel_hashmap/phmap.h>
#include <winrt/Windows.Devices.Input.h>
#include <winrt/Windows.UI.Input.h>
#include <shellscalingapi.h>

using namespace ::Magpie;
using namespace winrt;
using namespace Windows::UI::Xaml::Controls::Primitives;
using namespace Windows::UI::Xaml::Input;

namespace winrt::Magpie::implementation {

ScalingModesPage::ScalingModesPage() {
}

template<class T>
static T FindIssueElement(const DependencyObject& parent, std::wstring_view name = {}) {
	if (!parent) return nullptr;
	if (const auto match = parent.try_as<T>(); match && (name.empty() || std::wstring_view(match.Name()) == name)) return match;
	for (int i = 0; i < VisualTreeHelper::GetChildrenCount(parent); ++i)
		if (const auto found = FindIssueElement<T>(VisualTreeHelper::GetChild(parent, i), name)) return found;
	return nullptr;
}

void ScalingModesPage::RevealIssueTarget(const ::Magpie::IssueContext& context) {
	_issueTarget = context;
	_issueModeName = context.scalingModeName;
	_issueEffectNames = context.effectNames;
	_issueEffectIndex = context.effectIndex;
	if (_issueLayoutToken.value) { LayoutUpdated(_issueLayoutToken); _issueLayoutToken = {}; }
	// The view model loads groups incrementally; resolve again after layout.
	_issueLayoutToken = LayoutUpdated([weak = get_weak()](const IInspectable&, const IInspectable&) {
		if (const auto self = weak.get(); self && self->_TryRevealIssueTarget()) {
			self->LayoutUpdated(self->_issueLayoutToken);
			self->_issueLayoutToken = {};
		}
	});
}

bool ScalingModesPage::_TryRevealIssueTarget() {
	const auto loader = ResourceLoader::GetForViewIndependentUse(CommonSharedConstants::APP_RESOURCE_MAP_ID);
	const auto& modes = AppSettings::Get().ScalingModes();
	size_t target = modes.size();
	bool ambiguous = false;
	for (size_t i = 0; i < modes.size(); ++i) if (modes[i].name == _issueModeName) {
		if (target != modes.size()) ambiguous = true;
		target = i;
	}
	bool matches = !ambiguous && target < modes.size() && modes[target].effects.size() == _issueEffectNames.size();
	if (matches) matches = modes[target].runtimeIdentity == _issueTarget.scalingModeIdentity.lock() &&
		_issueTarget.effectIdentities.size() == _issueEffectNames.size();
	if (matches) for (size_t i = 0; i < _issueEffectNames.size(); ++i)
		matches &= modes[target].effects[i].name == _issueEffectNames[i] &&
			modes[target].effects[i].runtimeIdentity == _issueTarget.effectIdentities[i].lock();
	IssueTargetNotice().IsOpen(true);
	if (!matches || (_issueEffectIndex != UINT32_MAX && _issueEffectIndex >= _issueEffectNames.size())) {
		IssueTargetNotice().Message(loader.GetString(L"ErrorDetails_EffectsChanged"));
		return true;
	}
	IssueTargetNotice().Message(hstring(fmt::format(fmt::runtime(std::wstring(loader.GetString(L"ErrorDetails_EffectsLocated"))),
		_issueModeName, _issueEffectIndex == UINT32_MAX ? L"" : fmt::format(L" / #{} {}", _issueEffectIndex + 1, _issueEffectNames[_issueEffectIndex]))));
	const auto items = _viewModel->ScalingModes();
	if (target >= items.Size()) return false;
	const auto item = items.GetAt(static_cast<uint32_t>(target));
	ScalingModeList().ScrollIntoView(item);
	const auto container = ScalingModeList().ContainerFromItem(item);
	const auto expander = FindIssueElement<winrt::Magpie::SettingsExpander>(container);
	if (!expander) return false;
	expander.IsExpanded(true);
	if (_issueEffectIndex == UINT32_MAX) { expander.StartBringIntoView(); return true; }
	const auto effects = FindIssueElement<ListView>(expander, L"IssueEffectList");
	if (!effects || _issueEffectIndex >= effects.Items().Size()) return false;
	const auto effect = effects.Items().GetAt(_issueEffectIndex);
	effects.ScrollIntoView(effect);
	if (const auto control = effects.ContainerFromItem(effect).try_as<Control>()) {
		control.StartBringIntoView();
		control.Focus(FocusState::Programmatic);
		return true;
	}
	return false;
}

void ScalingModesPage::ComboBox_DropDownOpened(IInspectable const& sender, IInspectable const&) {
	ControlHelper::ComboBox_DropDownOpened(sender);
}

void ScalingModesPage::NumberBox_Loaded(IInspectable const& sender, RoutedEventArgs const&) {
	ControlHelper::NumberBox_Loaded(sender);
}

void ScalingModesPage::_RefreshParameterSliderHint(Slider const& slider) {
	if (auto parameter = slider.DataContext().try_as<Magpie::ScalingModeParameter>()) {
		const auto loader = ResourceLoader::GetForCurrentView(
			CommonSharedConstants::APP_RESOURCE_MAP_ID);
		const auto text = fmt::format(fmt::runtime(std::wstring_view(loader.GetString(
			L"Overlay_EffectParameters_ResetDefault"))),
			fmt::format(L"{:.7g}", get_self<ScalingModeParameter>(parameter)->DefaultValue()));
		ToolTipService::SetToolTip(slider, box_value(text));
	}
}

void ScalingModesPage::ParameterSlider_Loaded(IInspectable const& sender, RoutedEventArgs const&) {
	const auto slider = sender.as<Slider>();
	_RefreshParameterSliderHint(slider);
	if (!_parameterSliders.insert(reinterpret_cast<uintptr_t>(get_abi(slider))).second) return;
	if (!_parameterSliderPressed) {
		_parameterSliderPressed = box_value(PointerEventHandler{ get_weak(), &ScalingModesPage::_ParameterSlider_Pressed });
		_parameterSliderMoved = box_value(PointerEventHandler{ get_weak(), &ScalingModesPage::_ParameterSlider_Moved });
		_parameterSliderReleased = box_value(PointerEventHandler{ get_weak(), &ScalingModesPage::_ParameterSlider_Released });
		_parameterSliderCanceled = box_value(PointerEventHandler{ get_weak(), &ScalingModesPage::_ParameterSlider_Canceled });
		_parameterSliderCaptureLost = box_value(PointerEventHandler{ get_weak(), &ScalingModesPage::_ParameterSlider_CaptureLost });
	}
	// Thumb and track class handlers can already have consumed the event.
	slider.AddHandler(UIElement::PointerPressedEvent(), _parameterSliderPressed, true);
	slider.AddHandler(UIElement::PointerMovedEvent(), _parameterSliderMoved, true);
	slider.AddHandler(UIElement::PointerReleasedEvent(), _parameterSliderReleased, true);
	slider.AddHandler(UIElement::PointerCanceledEvent(), _parameterSliderCanceled, true);
	slider.AddHandler(UIElement::PointerCaptureLostEvent(), _parameterSliderCaptureLost, true);
}

void ScalingModesPage::ParameterSlider_Unloaded(IInspectable const& sender, RoutedEventArgs const&) {
	const auto slider = sender.as<Slider>();
	if (_parameterSliders.erase(reinterpret_cast<uintptr_t>(get_abi(slider)))) {
		slider.RemoveHandler(UIElement::PointerPressedEvent(), _parameterSliderPressed);
		slider.RemoveHandler(UIElement::PointerMovedEvent(), _parameterSliderMoved);
		slider.RemoveHandler(UIElement::PointerReleasedEvent(), _parameterSliderReleased);
		slider.RemoveHandler(UIElement::PointerCanceledEvent(), _parameterSliderCanceled);
		slider.RemoveHandler(UIElement::PointerCaptureLostEvent(), _parameterSliderCaptureLost);
	}
	_parameterResetGesture.Clear();
	_resetHeldSlider = {};
}

void ScalingModesPage::ParameterSlider_LostFocus(IInspectable const&, RoutedEventArgs const&) {
	_parameterResetGesture.Clear();
	_resetHeldSlider = {};
}

void ScalingModesPage::ParameterSlider_KeyDown(IInspectable const& sender, KeyRoutedEventArgs const& args) {
	const auto parameter = sender.as<Slider>().DataContext().try_as<Magpie::ScalingModeParameter>();
	if (!parameter) return;
	const auto impl = get_self<ScalingModeParameter>(parameter);
	if (!impl->IsPreciseInput()) return;
	const auto key = args.Key();
	using Windows::System::VirtualKey;
	if (key != VirtualKey::Left && key != VirtualKey::Right && key != VirtualKey::Up && key != VirtualKey::Down) return;
	impl->StepValue(key == VirtualKey::Right || key == VirtualKey::Up, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
	args.Handled(true);
}

void ScalingModesPage::ParameterValue_KeyDown(IInspectable const& sender, KeyRoutedEventArgs const& args) {
	if (args.Key() != Windows::System::VirtualKey::Enter) return;
	const auto input = sender.as<TextBox>();
	if (const auto parameter = input.DataContext().try_as<Magpie::ScalingModeParameter>())
		get_self<ScalingModeParameter>(parameter)->EditableValue(input.Text());
	args.Handled(true);
}

void ScalingModesPage::ParameterSlider_WheelChanged(IInspectable const& sender, PointerRoutedEventArgs const& args) {
	const auto slider = sender.as<Slider>();
	const auto parameter = slider.DataContext().try_as<Magpie::ScalingModeParameter>();
	if (!slider.IsEnabled() || !parameter) return;
	const auto impl = get_self<ScalingModeParameter>(parameter);
	if (!impl->IsPreciseInput()) return;
	const auto delta = args.GetCurrentPoint(slider).Properties().MouseWheelDelta();
	if (!delta) return;
	impl->StepValue(delta > 0, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
	args.Handled(true);
}

void ScalingModesPage::ParameterSlider_DataContextChanged(
	FrameworkElement const& sender, DataContextChangedEventArgs const&) {
	_parameterResetGesture.Clear();
	_resetHeldSlider = {};
	_RefreshParameterSliderHint(sender.as<Slider>());
}

void ScalingModesPage::ParameterSlider_ValueChanged(
	IInspectable const& sender, RangeBaseValueChangedEventArgs const&) {
	const auto held = _resetHeldSlider.get();
	if (!held || held != sender.try_as<Slider>()) return;
	if (auto parameter = held.DataContext().try_as<Magpie::ScalingModeParameter>()) {
		auto impl = get_self<ScalingModeParameter>(parameter);
		impl->ResetToDefault();
		if (held.Value() != impl->SliderValue()) held.Value(impl->SliderValue());
	}
}

void ScalingModesPage::_ParameterSlider_Pressed(
	IInspectable const& sender, PointerRoutedEventArgs const& args) {
	const auto slider = sender.as<Slider>();
	const auto point = args.GetCurrentPoint(slider);
	const auto parameter = slider.DataContext().try_as<Magpie::ScalingModeParameter>();
	if (point.PointerDevice().PointerDeviceType() != Windows::Devices::Input::PointerDeviceType::Mouse ||
		!point.Properties().IsLeftButtonPressed() || (GetKeyState(VK_CONTROL) & 0x8000) ||
		!slider.IsEnabled() || !parameter || !parameter.IsFloat() || !parameter.IsVisible()) {
		_parameterResetGesture.Clear();
		return;
	}
	if (!_parameterResetGesture.Press(reinterpret_cast<uintptr_t>(get_abi(slider)),
		point.Timestamp(), point.Position().X, point.Position().Y, 4.0f)) return;

	// Take over the second press from the Thumb. Keep its value fixed until up.
	_resetHeldSlider = slider;
	_capturingResetPointer = true;
	slider.CapturePointer(args.Pointer());
	_capturingResetPointer = false;
	auto impl = get_self<ScalingModeParameter>(parameter);
	impl->ResetToDefault();
	slider.Value(impl->SliderValue());
	args.Handled(true);
}

void ScalingModesPage::_ParameterSlider_Moved(
	IInspectable const& sender, PointerRoutedEventArgs const& args) {
	const auto point = args.GetCurrentPoint(sender.as<Slider>());
	_parameterResetGesture.Move(point.Position().X, point.Position().Y, 4.0f);
	if (_resetHeldSlider.get() == sender.try_as<Slider>()) args.Handled(true);
}

void ScalingModesPage::_ParameterSlider_Released(
	IInspectable const& sender, PointerRoutedEventArgs const& args) {
	const auto slider = sender.as<Slider>();
	const auto point = args.GetCurrentPoint(slider);
	_parameterResetGesture.Move(point.Position().X, point.Position().Y, 4.0f);
	_parameterResetGesture.Release();
	if (_resetHeldSlider.get() == slider) {
		ParameterSlider_ValueChanged(sender, nullptr);
		_resetHeldSlider = {};
		slider.ReleasePointerCapture(args.Pointer());
		args.Handled(true);
	}
}

void ScalingModesPage::_ParameterSlider_Canceled(
	IInspectable const& sender, PointerRoutedEventArgs const&) {
	ParameterSlider_ValueChanged(sender, nullptr);
	_parameterResetGesture.Clear();
	_resetHeldSlider = {};
}

void ScalingModesPage::_ParameterSlider_CaptureLost(
	IInspectable const& sender, PointerRoutedEventArgs const& args) {
	// Releasing capture after a normal button-up is not an interrupted click.
	if (_capturingResetPointer ||
		!args.GetCurrentPoint(nullptr).Properties().IsLeftButtonPressed()) return;
	_ParameterSlider_Canceled(sender, args);
}

void ScalingModesPage::EffectSettingsCard_Loaded(IInspectable const& sender, RoutedEventArgs const&) {
	XamlHelper::UpdateThemeOfTooltips(sender.try_as<DependencyObject>(), ActualTheme());
}

void ScalingModesPage::EffectParametersFlyout_Opening(
	IInspectable const& sender,
	IInspectable const&
) {
	TraceParameterFlyout("parent.Opening", uintptr_t(get_abi(sender)));
	Flyout flyout = sender.try_as<Flyout>();
	ContentControl content = flyout ? flyout.Content().try_as<ContentControl>() : nullptr;
	winrt::Magpie::EffectParametersViewModel parameters = content ?
		content.Content().try_as<winrt::Magpie::EffectParametersViewModel>() : nullptr;
	if (!parameters || !XamlRoot()) return;

	const auto anchor = flyout.Target();
	TraceParameterFlyout("parent.context", uintptr_t(get_abi(flyout)),
		intptr_t(get_abi(anchor)), intptr_t(get_abi(parameters)));
	const auto& window = App::Get().MainWindow();
	POINT position{};
	if (anchor) {
		const auto center = anchor.TransformToVisual(XamlRoot().Content()).TransformPoint(
			{ float(anchor.ActualWidth() / 2), float(anchor.ActualHeight() / 2) });
		position = window.XamlRootPointToScreen(center);
	} else {
		position = window.XamlRootPointToScreen({ 0, 0 });
	}
	const auto monitor = MonitorFromPoint(position, MONITOR_DEFAULTTONEAREST);
	MONITORINFO info{ sizeof(info) };
	if (!GetMonitorInfo(monitor, &info)) return;
	UINT dpiX = window.CurrentDpi(), dpiY = dpiX;
	GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
	const auto size = GetEffectParameterPopupSize(info.rcWork.right - info.rcWork.left,
		info.rcWork.bottom - info.rcWork.top, dpiX / 96.0, XamlRoot().RasterizationScale());
	// The native desktop popup chooses/flips its position at the screen edge.
	// Leave room for its chrome while sizing against this monitor's work area.
	get_self<EffectParametersViewModel>(parameters)->UpdateLayoutSize(
		size.width, size.height);
}

void ScalingModesPage::EffectParametersFlyout_Opened(IInspectable const& sender, IInspectable const&) {
	TraceParameterFlyout("parent.Opened", uintptr_t(get_abi(sender)));
}

void ScalingModesPage::EffectParametersFlyout_Closing(IInspectable const& sender, IInspectable const&) {
	TraceParameterFlyout("parent.Closing", uintptr_t(get_abi(sender)));
}

void ScalingModesPage::EffectParametersFlyout_Closed(IInspectable const& sender, IInspectable const&) {
	TraceParameterFlyout("parent.Closed", uintptr_t(get_abi(sender)));
}

void ScalingModesPage::ParameterChoice_DropDownOpened(IInspectable const& sender, IInspectable const&) {
	if (!IsParameterFlyoutTraceEnabled()) return;
	TraceParameterFlyout("child.DropDownOpened", uintptr_t(get_abi(sender)), sender.as<ComboBox>().SelectedIndex());
}

void ScalingModesPage::ParameterChoice_DropDownClosed(IInspectable const& sender, IInspectable const&) {
	if (!IsParameterFlyoutTraceEnabled()) return;
	TraceParameterFlyout("child.DropDownClosed", uintptr_t(get_abi(sender)), sender.as<ComboBox>().SelectedIndex());
}

void ScalingModesPage::ParameterChoice_SelectionChanged(IInspectable const& sender, IInspectable const&) {
	if (!IsParameterFlyoutTraceEnabled()) return;
	const auto combo = sender.as<ComboBox>();
	TraceParameterFlyout("child.SelectionChanged", uintptr_t(get_abi(sender)), combo.SelectedIndex(),
		intptr_t(get_abi(combo.DataContext())));
}

void ScalingModesPage::ParameterAnchor_Loaded(IInspectable const& sender, RoutedEventArgs const&) {
	TraceParameterFlyout("anchor.Loaded", uintptr_t(get_abi(sender)));
}

void ScalingModesPage::ParameterAnchor_Unloaded(IInspectable const& sender, RoutedEventArgs const&) {
	TraceParameterFlyout("anchor.Unloaded", uintptr_t(get_abi(sender)));
}

void ScalingModesPage::AddEffectButton_Click(IInspectable const& sender, RoutedEventArgs const&) {
	try {
		if (!_effectPicker) _BuildEffectPicker();
		const auto btn = sender.as<Button>();
		_pickerMode = btn.Tag().as<winrt::Magpie::ScalingModeItem>();
		_SizeEffectPicker(btn);
		_pickerSearch.Text(L"");
		_RefreshEffectPicker();
		_effectPicker.XamlRoot(XamlRoot());
		_effectPicker.ShowAt(btn);
	} catch (const hresult_error& error) {
		Logger::Get().ComError("Open effect picker: " + to_string(error.message()), error.code());
		_pickerMode = nullptr;
		try { if (_effectPicker) _effectPicker.Hide(); } catch (...) {}
		_effectPicker = nullptr;
		_pickerRoot = nullptr;
		_pickerRows.clear();
		ToastService::Get().ShowMessageInApp(L"请重新打开效果器选择器",
			fmt::format(L"本次打开未修改效果组。请再次点击“添加效果器”；若仍失败，请提供 logs\\magpie.log。错误码：0x{:08X}",
				static_cast<uint32_t>(error.code().value)), std::chrono::seconds(8));
	}
}

void ScalingModesPage::NewScalingModeButton_Click(IInspectable const&, RoutedEventArgs const&) {
	_viewModel->AddScalingMode();
}

void ScalingModesPage::OpenConfigFolderButton_Click(IInspectable const&, RoutedEventArgs const&) {
	const auto& configDir = AppSettings::Get().ConfigDir();
	if (!Win32Helper::ShellOpen(configDir.c_str())) {
		const auto loader = ResourceLoader::GetForCurrentView(CommonSharedConstants::APP_RESOURCE_MAP_ID);
		ToastService::Get().ShowMessageInApp(loader.GetString(L"ScalingModes_General_ConfigFolder/Text"),
			fmt::format(L"{}\n{}", std::wstring_view(loader.GetString(L"ErrorDetails_OpenConfigFailed")), configDir.native()),
			std::chrono::seconds(8));
	}
}

fire_and_forget ScalingModesPage::ResetScalingModesButton_Click(
	IInspectable const&,
	RoutedEventArgs const&) {
	if (ContentDialogHelper::IsAnyDialogOpen()) {
		co_return;
	}

	ResourceLoader resourceLoader =
		ResourceLoader::GetForCurrentView(CommonSharedConstants::APP_RESOURCE_MAP_ID);
	ContentDialog dialog;
	dialog.XamlRoot(XamlRoot());
	dialog.RequestedTheme(ActualTheme());
	dialog.Title(box_value(resourceLoader.GetString(L"ScalingModes_ResetDialog_Title")));
	dialog.Content(box_value(resourceLoader.GetString(L"ScalingModes_ResetDialog_Content")));
	dialog.PrimaryButtonText(resourceLoader.GetString(L"ScalingModes_ResetDialog_Confirm"));
	dialog.CloseButtonText(resourceLoader.GetString(L"ScalingModes_ResetDialog_Cancel"));
	dialog.DefaultButton(ContentDialogButton::Close);

	if (co_await ContentDialogHelper::ShowAsync(dialog) == ContentDialogResult::Primary) {
		ScalingModesService::Get().ResetScalingModes();
	}
}

void ScalingModesPage::ScalingModeRenameButton_Loaded(
	IInspectable const& sender,
	RoutedEventArgs const&) {
	Button button = sender.try_as<Button>();
	_QueueAutoRename(button, button.DataContext());
}

void ScalingModesPage::ScalingModeRenameButton_DataContextChanged(
	FrameworkElement const& sender,
	DataContextChangedEventArgs const& args) {
	_QueueAutoRename(sender.try_as<Button>(), args.NewValue());
}

void ScalingModesPage::_QueueAutoRename(
	Button const& button,
	IInspectable const& candidateItem) noexcept {
	if (!button || !button.IsLoaded()) {
		return;
	}

	winrt::Magpie::ScalingModeItem item =
		candidateItem.try_as<winrt::Magpie::ScalingModeItem>();
	if (!item) {
		return;
	}

	weak_ref<Button> weakButton(button);
	App::Get().Dispatcher().TryEnqueue(DispatcherQueuePriority::Low, [weakButton, item]() {
		Button currentButton = weakButton.get();
		if (!currentButton || !currentButton.IsLoaded()) {
			return;
		}

		winrt::Magpie::ScalingModeItem currentItem =
			currentButton.DataContext().try_as<winrt::Magpie::ScalingModeItem>();
		if (!currentItem || currentItem != item) {
			return;
		}

		ScalingModeItem* itemImpl = get_self<ScalingModeItem>(item);
		if (!itemImpl->TakeAutoRenameRequest()) {
			return;
		}

		try {
			currentButton.Flyout().ShowAt(currentButton);
		} catch (...) {
			// The item can be removed or recycled before the queued callback runs.
			// In that case, opening the rename flyout is best-effort.
		}
	});
}

void ScalingModesPage::RenameTextBox_Loaded(IInspectable const& sender, RoutedEventArgs const&) {
	TextBox textBox = sender.try_as<TextBox>();
	textBox.Focus(FocusState::Programmatic);
	textBox.SelectAll();
}

void ScalingModesPage::RemoveScalingModeButton_Click(IInspectable const& sender, RoutedEventArgs const&) {
	Button button = sender.try_as<Button>();
	if (!button) return;
	const auto item = button.Tag().try_as<winrt::Magpie::ScalingModeItem>();
	if (!item) return;
	ScalingModeItem* scalingModeItem = get_self<ScalingModeItem>(item);
	if (scalingModeItem->IsInUse()) {
		// 如果有缩放配置正在使用此缩放模式则弹出确认弹窗
		FlyoutBase::GetAttachedFlyout(button).ShowAt(button);
	} else {
		scalingModeItem->Remove();
	}
}

ListView ScalingModesPage::_FindParentListView(DependencyObject const& element) const noexcept {
	DependencyObject current = VisualTreeHelper::GetParent(element);
	while (current) {
		if (ListView listView = current.try_as<ListView>()) {
			return listView;
		}
		current = VisualTreeHelper::GetParent(current);
	}

	return nullptr;
}

uint32_t ScalingModesPage::_GetReorderTargetIndex(double pointerY) const noexcept {
	if (!_reorderListView || !_reorderItems || !_reorderItem) {
		return 0;
	}

	if (_reorderOriginalIndex >= _reorderItems.Size() ||
		_reorderItemCenters.size() != _reorderItems.Size()) {
		return _reorderOriginalIndex;
	}

	try {
		uint32_t targetIndex = 0;
		bool foundContainer = false;
		const uint32_t size = _reorderItems.Size();
		for (uint32_t i = 0; i < size; ++i) {
			if (i == _reorderOriginalIndex) {
				continue;
			}

			const double centerY = _reorderItemCenters[i];
			if (!std::isfinite(centerY)) {
				continue;
			}

			foundContainer = true;
			if (pointerY < centerY) {
				break;
			}
			++targetIndex;
		}

		return foundContainer ? std::min(targetIndex, size - 1) : _reorderOriginalIndex;
	} catch (...) {
		return _reorderOriginalIndex;
	}
}

void ScalingModesPage::_PrepareReorderPreview() noexcept {
	_reorderPreviewItems.clear();
	_reorderItemCenters.clear();
	_reorderSlotExtent = 0;

	if (!_reorderListView || !_reorderItems || !_reorderContainer) {
		return;
	}

	try {
		const uint32_t size = _reorderItems.Size();
		_reorderPreviewItems.reserve(size > 0 ? size - 1 : 0);
		_reorderItemCenters.assign(size, std::numeric_limits<double>::quiet_NaN());

		double draggedTop = 0;
		double nextTop = 0;
		bool hasDraggedTop = false;
		bool hasNextTop = false;

		for (uint32_t i = 0; i < size; ++i) {
			FrameworkElement container =
				_reorderListView.ContainerFromIndex(i).try_as<FrameworkElement>();
			if (!container) {
				continue;
			}

			const Point topLeft =
				container.TransformToVisual(_reorderListView).TransformPoint({});
			_reorderItemCenters[i] = topLeft.Y + container.ActualHeight() / 2;

			if (i == _reorderOriginalIndex) {
				draggedTop = topLeft.Y;
				hasDraggedTop = true;
				continue;
			}
			if (i == _reorderOriginalIndex + 1) {
				nextTop = topLeft.Y;
				hasNextTop = true;
			}

			ReorderPreviewItem preview;
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
			_reorderPreviewItems.emplace_back(std::move(preview));
		}

		if (hasDraggedTop && hasNextTop && nextTop > draggedTop) {
			_reorderSlotExtent = static_cast<float>(nextTop - draggedTop);
		} else {
			const Thickness margin = _reorderContainer.Margin();
			_reorderSlotExtent = static_cast<float>(
				_reorderContainer.ActualHeight() + margin.Top + margin.Bottom);
		}
	} catch (...) {
		_ClearReorderPreview();
	}
}

void ScalingModesPage::_UpdateReorderPreview(uint32_t targetIndex) noexcept {
	if (_reorderSlotExtent <= 0) {
		return;
	}

	try {
		for (const ReorderPreviewItem& preview : _reorderPreviewItems) {
			double offset = 0;
			if (_reorderOriginalIndex < targetIndex &&
				preview.index > _reorderOriginalIndex && preview.index <= targetIndex) {
				offset = -_reorderSlotExtent;
			} else if (targetIndex < _reorderOriginalIndex &&
				preview.index >= targetIndex && preview.index < _reorderOriginalIndex) {
				offset = _reorderSlotExtent;
			}

			preview.previewTransform.TranslateY(offset);
		}
	} catch (...) {
		// Containers can be unrealized if the list scrolls during a drag. The next
		// pointer event will continue updating the remaining realized containers.
	}
}

void ScalingModesPage::_ClearReorderPreview() noexcept {
	for (const ReorderPreviewItem& preview : _reorderPreviewItems) {
		try {
			preview.container.RenderTransform(preview.originalRenderTransform);
		} catch (...) {
		}
	}

	_reorderPreviewItems.clear();
	_reorderItemCenters.clear();
	_reorderSlotExtent = 0;
}

void ScalingModesPage::_QueueFinishReorder(bool commit) noexcept {
	if (!_reorderHandle) {
		return;
	}

	// PointerCaptureLost is normally raised after PointerReleased. Once a valid
	// release has requested a commit, the later capture-lost notification must not
	// turn it into a cancellation.
	_queuedReorderCommit = _queuedReorderCommit || commit;
	if (_isReorderFinishQueued) {
		return;
	}

	_isReorderFinishQueued = true;
	const uint32_t pointerId = _reorderPointerId;
	try {
		weak_ref<ScalingModesPage> weakThis = get_weak();
		if (App::Get().Dispatcher().TryEnqueue(
			DispatcherQueuePriority::Low,
			[weakThis, pointerId]() {
				com_ptr<ScalingModesPage> self = weakThis.get();
				if (!self || !self->_isReorderFinishQueued ||
					self->_reorderPointerId != pointerId) {
					return;
				}

				const bool shouldCommit = std::exchange(self->_queuedReorderCommit, false);
				self->_isReorderFinishQueued = false;
				self->_FinishReorder(shouldCommit);
			})) {
			return;
		}
	} catch (...) {
	}

	// Dispatch only fails during shutdown. Do not touch XAML objects from the
	// current input callback; the page teardown will release the references.
	_isReorderFinishQueued = false;
	_queuedReorderCommit = false;
}

void ScalingModesPage::_FinishReorder(bool commit) noexcept {
	FrameworkElement container = std::exchange(_reorderContainer, nullptr);
	IObservableVector<IInspectable> items = std::exchange(_reorderItems, nullptr);
	IInspectable item = std::exchange(_reorderItem, nullptr);
	const uint32_t targetIndex = _reorderTargetIndex;
	const bool wasDragging = _isReorderDragging;

	if (commit && wasDragging && items && item) {
		try {
			uint32_t currentIndex = 0;
			if (items.IndexOf(item, currentIndex) && currentIndex != targetIndex) {
				// Keep the preview transforms until the reordered collection has completed
				// its layout. Clearing them first exposes the old slots for one rendered
				// frame, the same release-time twitch fixed in the profile list.
				items.RemoveAt(currentIndex);
				items.InsertAt(std::min(targetIndex, items.Size()), item);
				if (_reorderListView) {
					_reorderListView.UpdateLayout();
				}
			}
		} catch (...) {
			// The page may be closing while the pointer is captured. In that case the
			// collection or its item can already be detached, so cancel the reorder.
		}
	}

	_ClearReorderPreview();

	if (container) {
		try {
			container.RenderTransform(_originalRenderTransform);
			container.Opacity(_originalOpacity);
			Canvas::SetZIndex(container, _originalZIndex);
		} catch (...) {
			// The container may have been unrealized while completion was queued.
		}
	}

	_reorderHandle = nullptr;
	_reorderListView = nullptr;
	_originalRenderTransform = nullptr;
	_dragTransform = nullptr;
	_reorderPointerId = 0;
	_reorderOriginalIndex = 0;
	_isReorderDragging = false;
	_isReorderFinishQueued = false;
	_queuedReorderCommit = false;
}

void ScalingModesPage::ReorderHandle_PointerPressed(
	IInspectable const& sender,
	PointerRoutedEventArgs const& args) {
	try {
		FrameworkElement handle = sender.try_as<FrameworkElement>();
		if (_reorderHandle || !handle || !handle.Tag()) {
			return;
		}

		auto pointerPoint = args.GetCurrentPoint(handle);
		if (pointerPoint.PointerDevice().PointerDeviceType() ==
			Windows::Devices::Input::PointerDeviceType::Mouse &&
			!pointerPoint.Properties().IsLeftButtonPressed()) {
			return;
		}

		ListView listView = _FindParentListView(handle);
		if (!listView) {
			return;
		}

		IObservableVector<IInspectable> items =
			listView.ItemsSource().try_as<IObservableVector<IInspectable>>();
		uint32_t itemIndex = 0;
		if (!items || items.Size() < 2 || !items.IndexOf(handle.Tag(), itemIndex)) {
			return;
		}

		FrameworkElement container =
			listView.ContainerFromIndex(itemIndex).try_as<FrameworkElement>();
		if (!container || !handle.CapturePointer(args.Pointer())) {
			return;
		}

		_reorderHandle = handle;
		_reorderContainer = container;
		_reorderListView = listView;
		_reorderItems = items;
		_reorderItem = handle.Tag();
		_reorderOriginalIndex = itemIndex;
		_originalRenderTransform = container.RenderTransform();
		_originalOpacity = container.Opacity();
		_originalZIndex = Canvas::GetZIndex(container);
		_dragTransform = CompositeTransform();

		TransformGroup transforms;
		if (_originalRenderTransform) {
			transforms.Children().Append(_originalRenderTransform);
		}
		transforms.Children().Append(_dragTransform);
		container.RenderTransform(transforms);
		container.Opacity(0.92);
		Canvas::SetZIndex(container, 1000);
		_PrepareReorderPreview();

		_pointerStartY = args.GetCurrentPoint(listView).Position().Y;
		_reorderTargetIndex = itemIndex;
		_reorderPointerId = args.Pointer().PointerId();
		_isReorderDragging = false;
		args.Handled(true);
	} catch (const hresult_error& e) {
		Logger::Get().ComWarn("启动拖拽失败", e.code());
		_QueueFinishReorder(false);
	} catch (...) {
		Logger::Get().Warn("启动拖拽失败");
		_QueueFinishReorder(false);
	}
}

void ScalingModesPage::ReorderHandle_PointerMoved(
	IInspectable const& sender,
	PointerRoutedEventArgs const& args) {
	try {
		if (!_reorderHandle || sender != _reorderHandle ||
			args.Pointer().PointerId() != _reorderPointerId) {
			return;
		}

		const double pointerY = args.GetCurrentPoint(_reorderListView).Position().Y;
		const double deltaY = pointerY - _pointerStartY;
		if (!_isReorderDragging && std::abs(deltaY) >= 3) {
			_isReorderDragging = true;
		}

		if (_isReorderDragging) {
			_dragTransform.TranslateY(deltaY);
			const uint32_t targetIndex = _GetReorderTargetIndex(pointerY);
			if (targetIndex != _reorderTargetIndex) {
				_reorderTargetIndex = targetIndex;
				_UpdateReorderPreview(targetIndex);
			}
		}
		args.Handled(true);
	} catch (const hresult_error& e) {
		Logger::Get().ComWarn("更新拖拽位置失败", e.code());
		_QueueFinishReorder(false);
	} catch (...) {
		Logger::Get().Warn("更新拖拽位置失败");
		_QueueFinishReorder(false);
	}
}

void ScalingModesPage::ReorderHandle_PointerReleased(
	IInspectable const& sender,
	PointerRoutedEventArgs const& args) {
	try {
		if (!_reorderHandle || sender != _reorderHandle ||
			args.Pointer().PointerId() != _reorderPointerId) {
			return;
		}

		args.Handled(true);
		_QueueFinishReorder(true);
	} catch (const hresult_error& e) {
		Logger::Get().ComWarn("完成拖拽失败", e.code());
		_QueueFinishReorder(false);
	} catch (...) {
		Logger::Get().Warn("完成拖拽失败");
		_QueueFinishReorder(false);
	}
}

void ScalingModesPage::ReorderHandle_PointerCanceled(
	IInspectable const& sender,
	PointerRoutedEventArgs const& args) {
	try {
		if (!_reorderHandle || sender != _reorderHandle ||
			args.Pointer().PointerId() != _reorderPointerId) {
			return;
		}

		args.Handled(true);
		_QueueFinishReorder(false);
	} catch (const hresult_error& e) {
		Logger::Get().ComWarn("取消拖拽失败", e.code());
		_QueueFinishReorder(false);
	} catch (...) {
		Logger::Get().Warn("取消拖拽失败");
		_QueueFinishReorder(false);
	}
}

void ScalingModesPage::ReorderHandle_PointerCaptureLost(
	IInspectable const& sender,
	PointerRoutedEventArgs const& args) {
	try {
		if (_reorderHandle && sender == _reorderHandle &&
			args.Pointer().PointerId() == _reorderPointerId) {
			_QueueFinishReorder(false);
		}
	} catch (const hresult_error& e) {
		Logger::Get().ComWarn("处理拖拽捕获丢失失败", e.code());
		_QueueFinishReorder(false);
	} catch (...) {
		Logger::Get().Warn("处理拖拽捕获丢失失败");
		_QueueFinishReorder(false);
	}
}

}
