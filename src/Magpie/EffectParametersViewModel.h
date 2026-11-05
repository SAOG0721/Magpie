#pragma once
#include "EffectParameterGroupViewModel.g.h"
#include "EffectParametersViewModel.g.h"
#include "ScalingModeParameter.g.h"
#include <parallel_hashmap/phmap.h>
#include "Event.h"
#include "EffectChoiceItems.h"
#include "EffectParameterValue.h"
#include "ParameterFlyoutTrace.h"
#include <optional>
#include <cmath>
#include <limits>

namespace Magpie {
struct EffectInfo;
}

namespace winrt::Magpie::implementation {

struct ScalingModeParameter : ScalingModeParameterT<ScalingModeParameter>,
		wil::notify_property_changed_base<ScalingModeParameter> {
	ScalingModeParameter(uint32_t index, hstring label, bool initValue)
		: _index(index), _label(std::move(label)), _isBoolean(true),
		_value(initValue ? 1.0 : 0.0) {}

	ScalingModeParameter(
		uint32_t index,
		hstring label,
		float initValue,
		float minimum,
		float maximum,
		float step,
		float defaultValue
	) : _index(index), _label(std::move(label)), _minimum(minimum),
		_maximum(maximum), _step(step), _defaultValue(defaultValue), _value(initValue) {}

	ScalingModeParameter(
		uint32_t index,
		hstring label,
		double initValue,
		std::vector<std::pair<int, hstring>> choices
	) : _index(index), _label(std::move(label)), _isChoice(true),
		_value(initValue) {
		_RebuildChoices(choices);
		// Unsupported imported values remain an unselected draft until the user
		// explicitly chooses an option. Never reinterpret a removed enum as 0.
	}

	void _RebuildChoices(
		const std::vector<std::pair<int, hstring>>& choices
	) {
		// Keep the same collection and selection while a dropdown is active.
		bool unchanged = _choices && _choices.Size() == choices.size() && _choiceValues.size() == choices.size();
		for (size_t i = 0; unchanged && i < choices.size(); ++i)
			unchanged = _choiceValues[i] == choices[i].first && _choices.GetAt(uint32_t(i)) == choices[i].second;
		if (unchanged) return;
		std::vector<hstring> items;
		items.reserve(choices.size());
		_choiceValues.clear();
		_choiceValues.reserve(choices.size());
		for (const auto& [value, choiceLabel] : choices) {
			_choiceValues.push_back(value);
			items.push_back(choiceLabel);
		}
		if (_choices) {
			::Magpie::TraceParameterFlyout("Choices.Reset", uintptr_t(this), _choices.Size(), items.size());
			_choices.ReplaceAll(items);
		} else {
			_choices = ::Magpie::MakeEffectChoiceItems(std::move(items));
		}
		assert(_choices.Size() == choices.size());
		assert(_choices.try_as<IIterable<IInspectable>>());
		RaisePropertyChanged(L"Choices");
		RaisePropertyChanged(L"ChoiceIndex");
	}

	uint32_t Index() const noexcept {
		return _index;
	}

	bool IsBoolean() const noexcept {
		return _isBoolean;
	}
	bool IsCheckBox() const noexcept { return _isBoolean && !isDisclosure; }
	bool IsDisclosure() const noexcept { return isDisclosure; }
	hstring DisclosureGlyph() const { return BooleanValue() ? L"\uE70D" : L"\uE76C"; }
	bool isDisclosure = false;

	bool IsChoice() const noexcept {
		return _isChoice;
	}

	bool IsFloat() const noexcept {
		return !_isBoolean && !_isChoice;
	}

	bool IsVisible() const noexcept {
		return _isVisible;
	}

	void IsVisible(bool value) {
		if (_isVisible == value) return;
		_isVisible = value;
		RaisePropertyChanged(L"IsVisible");
	}

	bool BooleanValue() const noexcept {
		return _value != 0.0;
	}

	void BooleanValue(bool value) {
		// A row's non-applicable binding must never coerce another parameter type.
		const double next = value ? 1.0 : 0.0;
		if (!_isBoolean || !_isEnabled || _value == next) return;
		_value = next;
		RaisePropertyChanged(L"BooleanValue");
		if (isDisclosure) RaisePropertyChanged(L"DisclosureGlyph");
	}

	bool IsEnabled() const noexcept { return _isEnabled; }
	void IsEnabled(bool value) {
		if (_isEnabled == value) return;
		_isEnabled = value;
		RaisePropertyChanged(L"IsEnabled");
	}

	int32_t ChoiceValue() const noexcept {
		return std::isfinite(_value) && _value >= std::numeric_limits<int32_t>::min() &&
			_value <= std::numeric_limits<int32_t>::max() ? static_cast<int32_t>(_value) :
			std::numeric_limits<int32_t>::min();
	}

	void ChoiceValue(int32_t value) {
		if (!_isChoice || !_isEnabled) return;
		const auto choice = std::find(_choiceValues.begin(), _choiceValues.end(), value);
		if (choice != _choiceValues.end() && !IsChoiceAllowed(int32_t(choice - _choiceValues.begin()))) {
			RaisePropertyChanged(L"ChoiceIndex");
			return;
		}
		if (!_isEnabled || std::find(_choiceValues.begin(), _choiceValues.end(), value) ==
			_choiceValues.end() || _value == value) {
			return;
		}
		_value = value;
		RaisePropertyChanged(L"ChoiceValue");
		RaisePropertyChanged(L"ChoiceIndex");
	}

	int32_t ChoiceIndex() const noexcept {
		if (!std::isfinite(_value) || _value != std::floor(_value) ||
			_value < std::numeric_limits<int32_t>::min() || _value > std::numeric_limits<int32_t>::max()) return -1;
		const auto it = std::find(
			_choiceValues.begin(), _choiceValues.end(), ChoiceValue());
		return it == _choiceValues.end() ? -1 :
			static_cast<int32_t>(std::distance(_choiceValues.begin(), it));
	}

	void ChoiceIndex(int32_t index) {
		if (!_isChoice || !_isEnabled || index < 0 || static_cast<size_t>(index) >= _choiceValues.size()) {
			return;
		}
		ChoiceValue(_choiceValues[index]);
	}

	IObservableVector<hstring> Choices() const noexcept {
		return _choices;
	}
	void ChoiceAvailability(std::vector<bool> allowed, hstring reason) {
		_choiceAllowed = std::move(allowed); _choiceReason = std::move(reason);
	}
	bool IsChoiceAllowed(int32_t index) const noexcept {
		return index >= 0 && size_t(index) < _choiceValues.size() &&
			(_choiceAllowed.empty() || _choiceAllowed[size_t(index)]);
	}
	hstring ChoiceReason(int32_t index) const { return IsChoiceAllowed(index) ? hstring() : _choiceReason; }
	const std::vector<int>& ChoiceValues() const noexcept { return _choiceValues; }
	Magpie::ScalingModeParameter Companion() const noexcept { return _companion; }
	void Companion(Magpie::ScalingModeParameter value) { _companion = std::move(value); }
	bool HasCompanion() const noexcept { return bool(_companion); }

	double Value() const noexcept {
		return _value;
	}

	void Value(double value) {
		if (!IsFloat() || !_isEnabled) return;
		if (_descriptor && _descriptor->inputStep > 0)
			value = ::Magpie::NormalizeEffectParameterInput(*_descriptor, float(value));
		if (!_isEnabled || _value == value) return;
		_value = value;
		RaisePropertyChanged(L"Value");
		RaisePropertyChanged(L"ValueText");
		RaisePropertyChanged(L"EditableValue");
		RaisePropertyChanged(L"SliderValue");
	}
	void ConfigureDescriptor(const ::Magpie::EffectParameterDesc& value) { _descriptor = value; }
	bool IsPreciseInput() const noexcept { return _descriptor && _descriptor->inputStep > 0; }
	bool IsLegacyNumeric() const noexcept { return !IsPreciseInput(); }
	hstring EditableValue() const noexcept { return ValueText(); }
	void EditableValue(hstring const& value);
	hstring InputError() const noexcept { return _inputError; }
	bool HasInputError() const noexcept { return !_inputError.empty(); }
	double SliderMinimum() const noexcept { return IsPreciseInput() ? 0 : _minimum; }
	double SliderMaximum() const noexcept {
		return IsPreciseInput() ? double(::Magpie::GetEffectParameterInteractionTicks(*_descriptor).size() - 1) : _maximum;
	}
	double SliderStep() const noexcept { return IsPreciseInput() ? 1 : _step; }
	double SliderValue() const noexcept {
		if (!IsPreciseInput()) return _value;
		int tick = 0, maximum = 0;
		::Magpie::GetEffectParameterTicks(*_descriptor, float(_value), tick, maximum);
		return tick;
	}
	void SliderValue(double value) {
		if (!IsFloat() || !_isEnabled || !std::isfinite(value)) return;
		if (!IsPreciseInput()) { Value(value); return; }
		// Binding initialization must not quantize an existing non-grid value.
		if (value == SliderValue()) return;
		Value(::Magpie::GetEffectParameterValueFromTick(*_descriptor, int(std::lround(value)), int(SliderMaximum())));
	}
	void StepValue(bool up, bool coarse = false) {
		if (IsPreciseInput()) Value(::Magpie::StepEffectParameter(*_descriptor, float(_value), up, coarse));
	}

	double DefaultValue() const noexcept { return _defaultValue; }
	void ResetToDefault() {
		if (IsFloat() && _isEnabled && _isVisible) Value(_defaultValue);
	}

	void SynchronizeValue(float value) {
		if (_value == value) return;
		_value = value;
		if (_isBoolean) {
			RaisePropertyChanged(L"BooleanValue");
			if (isDisclosure) RaisePropertyChanged(L"DisclosureGlyph");
		}
		else if (_isChoice) {
			RaisePropertyChanged(L"ChoiceValue");
			RaisePropertyChanged(L"ChoiceIndex");
		} else {
			RaisePropertyChanged(L"Value");
			RaisePropertyChanged(L"ValueText");
			RaisePropertyChanged(L"EditableValue");
			RaisePropertyChanged(L"SliderValue");
		}
	}

	hstring ValueText() const noexcept;

	hstring Label() const noexcept {
		return _label;
	}
	hstring Tooltip() const noexcept { return _tooltip; }
	void Tooltip(hstring value) {
		if (_tooltip == value) return;
		_tooltip = std::move(value);
		RaisePropertyChanged(L"Tooltip");
	}

	bool HasDescriptionLine() const noexcept {
		return std::wstring_view(_label).find(L'\n') != std::wstring_view::npos;
	}

	bool HasNoDescriptionLine() const noexcept {
		return !HasDescriptionLine();
	}

	hstring PrimaryLabel() const {
		const std::wstring_view label(_label);
		const size_t lineBreak = label.find(L'\n');
		return lineBreak == std::wstring_view::npos ?
			_label : hstring(label.substr(0, lineBreak));
	}

	hstring DescriptionLabel() const {
		const std::wstring_view label(_label);
		const size_t lineBreak = label.find(L'\n');
		return lineBreak == std::wstring_view::npos ?
			hstring() : hstring(label.substr(lineBreak + 1));
	}

	double Minimum() const noexcept {
		return _minimum;
	}

	double Maximum() const noexcept {
		return _maximum;
	}

	double Step() const noexcept {
		return _step;
	}

private:
	const uint32_t _index;
	const hstring _label;
	hstring _tooltip;
	const bool _isBoolean = false;
	const bool _isChoice = false;
	bool _isVisible = true;
	bool _isEnabled = true;
	const double _minimum = 0.0;
	const double _maximum = 1.0;
	const double _step = 1.0;
	const double _defaultValue = 0.0;
	double _value;
	IObservableVector<hstring> _choices{ nullptr };
	std::vector<int> _choiceValues;
	std::vector<bool> _choiceAllowed;
	hstring _choiceReason;
	Magpie::ScalingModeParameter _companion{ nullptr };
	std::optional<::Magpie::EffectParameterDesc> _descriptor;
	hstring _inputError;
};

struct EffectParameterGroupViewModel :
		EffectParameterGroupViewModelT<EffectParameterGroupViewModel>,
		wil::notify_property_changed_base<EffectParameterGroupViewModel> {
	EffectParameterGroupViewModel(
		hstring label,
		std::vector<IInspectable>&& params
	);

	hstring Label() const noexcept { return _label; }
	bool HasLabel() const noexcept { return !_label.empty(); }
	bool IsVisible() const noexcept { return _isVisible; }
	bool ShowLeadingSeparator() const noexcept { return _showLeadingSeparator; }
	IVector<IInspectable> Params() const noexcept { return _params; }
	void RefreshVisibility();
	void ShowLeadingSeparator(bool value);
	void UpdateParameters(const std::vector<IInspectable>& parameters);

private:
	hstring _label;
	IVector<IInspectable> _params{ nullptr };
	bool _isVisible = true;
	bool _showLeadingSeparator = false;
};

struct EffectParametersViewModel : EffectParametersViewModelT<EffectParametersViewModel>,
		wil::notify_property_changed_base<EffectParametersViewModel> {
	EffectParametersViewModel(uint32_t scalingModeIdx, uint32_t effectIdx);
	bool IsOKLab() const noexcept;
	int32_t OKSection() const noexcept { return _okSection; }
	int32_t OKColor() const noexcept { return _okColor; }
	int32_t OKSlot() const noexcept { return _okSlot; }
	int32_t OKCurve() const noexcept { return _okCurve; }
	int32_t OKPreset() const noexcept { return _okPreset; }
	void OKSection(int32_t value);
	void OKColor(int32_t value);
	void OKSlot(int32_t value);
	void OKCurve(int32_t value);
	void OKPreset(int32_t value) noexcept { if(value>=0)_okPreset=std::clamp(value,0,5); }
	int32_t OKWheel() const noexcept { return _okWheel; }
	void OKWheel(int32_t value);
	bool OKWheelVisible() const noexcept { return IsOKLab() && _okSection==4; }
	IVector<hstring> OKWheels() const;
	Media::ImageSource OKWheelImage();
	double OKWheelX() const;
	double OKWheelY() const;
	void OKWheelPressed(IInspectable const&, Input::PointerRoutedEventArgs const&);
	void OKWheelMoved(IInspectable const&, Input::PointerRoutedEventArgs const&);
	void OKWheelReleased(IInspectable const&, Input::PointerRoutedEventArgs const&);
	bool OKMixerVisible() const noexcept { return IsOKLab() && _okSection==3; }
	bool OKSelectionVisible() const noexcept { return IsOKLab() && _okSection==5; }
	bool OKCurveVisible() const noexcept { return IsOKLab() && _okSection==8; }
	bool OKLutVisible() const noexcept { return IsOKLab() && _okSection==13; }
	IVector<hstring> OKSections() const;
	IVector<hstring> OKColors() const;
	IVector<hstring> OKSlots() const;
	IVector<hstring> OKCurves() const;
	IVector<hstring> OKPresets() const;
	hstring OKStatus() const noexcept { return _okStatus; }
	Media::PointCollection OKCurvePoints() const;
	Media::Geometry OKCurveGeometry() const;
	void OKApplyPreset();
	void OKResetGroup();
	void OKResetAll();
	void OKStoreA();
	void OKSwapAB();
	void OKUndo();
	winrt::fire_and_forget OKImportLut();
	winrt::fire_and_forget OKExportLut();
	void OKClearLut();
	void OKCurvePressed(IInspectable const&, Input::PointerRoutedEventArgs const&);
	void OKCurveMoved(IInspectable const&, Input::PointerRoutedEventArgs const&);
	void OKCurveReleased(IInspectable const&, Input::PointerRoutedEventArgs const&);

	uint32_t ScalingModeIdx() const noexcept {
		return _scalingModeIdx;
	}

	void ScalingModeIdx(uint32_t value) noexcept {
		_scalingModeIdx = value;
	}

	uint32_t EffectIdx() const noexcept {
		return _effectIdx;
	}

	void EffectIdx(uint32_t value) noexcept {
		_effectIdx = value;
	}

	IVector<IInspectable> Groups() const noexcept {
		return _groups;
	}
	double ContentWidth() const noexcept { return _contentWidth; }
	double ContentMaxHeight() const noexcept { return _contentMaxHeight; }
	bool HasOKContextTools() const noexcept {
		return OKMixerVisible() || OKSelectionVisible() || OKWheelVisible() || OKCurveVisible() || OKLutVisible();
	}
	hstring ValidationMessage() const noexcept { return _validationMessage; }
	bool HasValidationMessage() const noexcept { return !_validationMessage.empty(); }

	void UpdateLayoutSize(double availableWidth, double availableHeight);

private:
	bool _IsRemoved() const noexcept;

	void _ScalingModeParameter_PropertyChanged(
		IInspectable const& sender,
		PropertyChangedEventArgs const& args
	);

	phmap::flat_hash_map<std::wstring, float>& _Data() const;
	void _RefreshVisibleGroups();
	void _RefreshConditionalVisibility();

	IVector<IInspectable> _groups{ nullptr };
	std::vector<com_ptr<EffectParameterGroupViewModel>> _groupImpls;
	std::vector<com_ptr<EffectParameterGroupViewModel>> _sourceGroups;
	std::vector<uint32_t> _visibleParameters;
	std::vector<com_ptr<ScalingModeParameter>> _parameterImpls;
	::Magpie::Event<uint32_t, uint32_t>::EventRevoker _parameterChangedRevoker;
	::Magpie::Event<>::EventRevoker _runtimeStatusChangedRevoker;
	bool _synchronizing = false;
	hstring _validationMessage;
	void _SynchronizeParameters();
	void _RefreshRuntimeResolution();

	double _contentWidth = 260.0;
	double _contentMaxHeight = 600.0;

	uint32_t _scalingModeIdx;
	uint32_t _effectIdx;
	const ::Magpie::EffectInfo* _effectInfo = nullptr;
	int32_t _okSection=0,_okColor=0,_okSlot=0,_okCurve=0,_okPreset=0,_okDragPoint=-1;
	int32_t _okWheel=0;
	bool _okWheelDragging=false;
	Media::ImageSource _okWheelImage{nullptr};
	void _OKNotifyVisuals();
	hstring _okStatus;
	std::vector<float> _okA, _okUndo;
	std::vector<float> _OKSnapshot() const;
	void _OKApply(const std::vector<float>& values, bool remember=true);
	void _OKRefreshEditor();
	void _OKRefreshSourceGroup();
	static void _LazySaveAppSettings();
};

}
