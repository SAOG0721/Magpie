#pragma once
#include "ScalingModesPage.g.h"
#include "ErrorService.h"
#include "ScalingModeItem.h"
#include "ScalingModesViewModel.h"
#include "EffectParameterResetGesture.h"
#include "EffectPickerModel.h"
#include "EffectPickerLayout.h"
#include <unordered_set>
#include <array>

namespace winrt::Magpie::implementation {

struct ScalingModesPage : ScalingModesPageT<ScalingModesPage> {
	ScalingModesPage();
	void RevealIssueTarget(const ::Magpie::IssueContext& context);

	winrt::Magpie::ScalingModesViewModel ViewModel() const noexcept {
		return *_viewModel;
	}

	void ComboBox_DropDownOpened(IInspectable const& sender, IInspectable const&);

	void NumberBox_Loaded(IInspectable const& sender, RoutedEventArgs const&);
	void ParameterSlider_Loaded(IInspectable const& sender, RoutedEventArgs const&);
	void ParameterSlider_Unloaded(IInspectable const& sender, RoutedEventArgs const&);
	void ParameterSlider_LostFocus(IInspectable const& sender, RoutedEventArgs const&);
	void ParameterSlider_KeyDown(IInspectable const& sender, Input::KeyRoutedEventArgs const&);
	void ParameterSlider_WheelChanged(IInspectable const& sender, Input::PointerRoutedEventArgs const&);
	void ParameterValue_KeyDown(IInspectable const& sender, Input::KeyRoutedEventArgs const&);
	void ParameterSlider_DataContextChanged(FrameworkElement const& sender,
		DataContextChangedEventArgs const&);
	void ParameterSlider_ValueChanged(IInspectable const& sender,
		Controls::Primitives::RangeBaseValueChangedEventArgs const&);

	void EffectSettingsCard_Loaded(IInspectable const& sender, RoutedEventArgs const&);

	void EffectParametersFlyout_Opening(IInspectable const& sender, IInspectable const&);
	void EffectParametersFlyout_Opened(IInspectable const& sender, IInspectable const&);
	void EffectParametersFlyout_Closing(IInspectable const& sender, IInspectable const&);
	void EffectParametersFlyout_Closed(IInspectable const& sender, IInspectable const&);
	void ParameterChoice_DropDownOpened(IInspectable const& sender, IInspectable const&);
	void ParameterChoice_DropDownClosed(IInspectable const& sender, IInspectable const&);
	void ParameterChoice_SelectionChanged(IInspectable const& sender, IInspectable const&);
	void ParameterAnchor_Loaded(IInspectable const& sender, RoutedEventArgs const&);
	void ParameterAnchor_Unloaded(IInspectable const& sender, RoutedEventArgs const&);

	void AddEffectButton_Click(IInspectable const& sender, RoutedEventArgs const&);

	void NewScalingModeButton_Click(IInspectable const& sender, RoutedEventArgs const&);
	void OpenConfigFolderButton_Click(IInspectable const& sender, RoutedEventArgs const&);

	fire_and_forget ResetScalingModesButton_Click(
		IInspectable const& sender,
		RoutedEventArgs const& args);

	void ScalingModeRenameButton_Loaded(IInspectable const& sender, RoutedEventArgs const&);

	void ScalingModeRenameButton_DataContextChanged(
		FrameworkElement const& sender,
		DataContextChangedEventArgs const& args);

	void RenameTextBox_Loaded(IInspectable const& sender, RoutedEventArgs const&);

	void RemoveScalingModeButton_Click(IInspectable const& sender, RoutedEventArgs const&);

	void ReorderHandle_PointerPressed(
		IInspectable const& sender,
		Input::PointerRoutedEventArgs const& args);

	void ReorderHandle_PointerMoved(
		IInspectable const& sender,
		Input::PointerRoutedEventArgs const& args);

	void ReorderHandle_PointerReleased(
		IInspectable const& sender,
		Input::PointerRoutedEventArgs const& args);

	void ReorderHandle_PointerCanceled(
		IInspectable const& sender,
		Input::PointerRoutedEventArgs const& args);

	void ReorderHandle_PointerCaptureLost(
		IInspectable const& sender,
		Input::PointerRoutedEventArgs const& args);
private:
	bool _TryRevealIssueTarget();
	::Magpie::IssueContext _issueTarget;
	std::wstring _issueModeName;
	std::vector<std::wstring> _issueEffectNames;
	uint32_t _issueEffectIndex = UINT32_MAX;
	event_token _issueLayoutToken{};
	void _ParameterSlider_Pressed(IInspectable const& sender,
		Input::PointerRoutedEventArgs const& args);
	void _ParameterSlider_Moved(IInspectable const& sender,
		Input::PointerRoutedEventArgs const& args);
	void _ParameterSlider_Released(IInspectable const& sender,
		Input::PointerRoutedEventArgs const& args);
	void _ParameterSlider_Canceled(IInspectable const& sender,
		Input::PointerRoutedEventArgs const& args);
	void _ParameterSlider_CaptureLost(IInspectable const& sender,
		Input::PointerRoutedEventArgs const& args);
	void _RefreshParameterSliderHint(Slider const& slider);
	::Magpie::EffectParameterResetGesture _parameterResetGesture;
	IInspectable _parameterSliderPressed{ nullptr };
	IInspectable _parameterSliderMoved{ nullptr };
	IInspectable _parameterSliderReleased{ nullptr };
	IInspectable _parameterSliderCanceled{ nullptr };
	IInspectable _parameterSliderCaptureLost{ nullptr };
	std::unordered_set<uintptr_t> _parameterSliders;
	weak_ref<Slider> _resetHeldSlider;
	bool _capturingResetPointer = false;

	struct ReorderPreviewItem {
		uint32_t index = 0;
		FrameworkElement container{ nullptr };
		Transform originalRenderTransform{ nullptr };
		CompositeTransform previewTransform{ nullptr };
	};

	void _BuildEffectPicker();
	void _RefreshEffectPicker(std::wstring anchor = {});
	void _ShowEffectPickerDetails(std::wstring_view key);
	void _SetEffectPickerDetails(std::wstring_view title, std::wstring_view text);
	void _QueueEffectPickerDetailsHint();
	void _UpdateEffectPickerDetailsHint();
	void _EffectPickerPointerWheelChanged(IInspectable const& sender, Input::PointerRoutedEventArgs const& args);
	void _AddPickedEffect(std::wstring_view id);
	void _ToggleEffectFamily(std::wstring_view key);
	void _EffectPickerRowKeyDown(std::wstring_view key, Input::KeyRoutedEventArgs const& args);
	void _JumpEffectPickerLetter(int letter);
	void _UpdateEffectPickerIndex();
	void _SizeEffectPicker(Button const& anchor);
	Button _EffectPickerButton(UIElement const& content);
	void _ChooseEffectCategory(std::wstring category, std::wstring subcategory, std::wstring description);
	void _SetEffectCategoryExpanded(size_t index, bool expanded);
	void _UpdateEffectPickerColors();
	struct PickerRow {
		::Magpie::EffectPickerVisibleRow entry;
		Button button{ nullptr };
	};
	struct PickerCategoryRow {
		std::wstring category, subcategory, name, description;
		Border container{ nullptr };
		Button button{ nullptr };
		::Magpie::EffectPickerToggleIcon icon;
		FrameworkElement selectionMark{ nullptr };
		int parent = -1;
		bool hasChildren = false, expanded = false;
	};
	std::vector<PickerCategoryRow> _pickerCategories;
	Border _pickerCategoryPane{ nullptr }, _pickerListPane{ nullptr }, _pickerDetailPane{ nullptr };
	std::vector<PickerRow> _pickerRows;
	std::vector<::Magpie::EffectPickerEntry> _pickerEntries;
	std::unordered_set<std::wstring> _pickerExpandedFamilies, _pickerSearchCollapsedFamilies;
	std::wstring _pickerLastQuery;
	StackPanel _pickerResults{ nullptr };
	StackPanel _pickerDetailContent{ nullptr };
	Grid _pickerDetailArea{ nullptr };
	TextBlock _pickerDetailHint{ nullptr };
	bool _pickerDetailsHintQueued = false;
	std::vector<Button> _pickerLetters;
	std::array<std::wstring, 27> _pickerLetterTargets;
	Flyout _effectPicker{ nullptr };
	Grid _pickerRoot{ nullptr };
	TextBox _pickerSearch{ nullptr };
	TextBlock _pickerCount{ nullptr }, _pickerDetails{ nullptr }, _pickerDetailTitle{ nullptr };
	ScrollViewer _pickerListScroll{ nullptr }, _pickerDetailScroll{ nullptr }, _pickerCategoryScroll{ nullptr };
	winrt::Magpie::ScalingModeItem _pickerMode{ nullptr };
	std::wstring _pickerCategory, _pickerSubcategory;
	bool _pickerChangingCategory = false;

	ListView _FindParentListView(DependencyObject const& element) const noexcept;

	uint32_t _GetReorderTargetIndex(double pointerY) const noexcept;

	void _PrepareReorderPreview() noexcept;

	void _UpdateReorderPreview(uint32_t targetIndex) noexcept;

	void _ClearReorderPreview() noexcept;

	void _QueueFinishReorder(bool commit) noexcept;

	void _FinishReorder(bool commit) noexcept;

	void _QueueAutoRename(
		Button const& button,
		IInspectable const& candidateItem) noexcept;

	com_ptr<ScalingModesViewModel> _viewModel = make_self<ScalingModesViewModel>();

	FrameworkElement _reorderHandle{ nullptr };
	FrameworkElement _reorderContainer{ nullptr };
	ListView _reorderListView{ nullptr };
	IObservableVector<IInspectable> _reorderItems{ nullptr };
	IInspectable _reorderItem{ nullptr };
	Transform _originalRenderTransform{ nullptr };
	CompositeTransform _dragTransform{ nullptr };
	std::vector<ReorderPreviewItem> _reorderPreviewItems;
	std::vector<double> _reorderItemCenters;
	double _originalOpacity = 1;
	double _pointerStartY = 0;
	float _reorderSlotExtent = 0;
	int32_t _originalZIndex = 0;
	uint32_t _reorderOriginalIndex = 0;
	uint32_t _reorderTargetIndex = 0;
	uint32_t _reorderPointerId = 0;
	bool _isReorderDragging = false;
	bool _isReorderFinishQueued = false;
	bool _queuedReorderCommit = false;
};

}

BASIC_FACTORY(ScalingModesPage)
