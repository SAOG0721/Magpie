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
		int initValue,
		std::vector<std::pair<int, hstring>> choices
	) : _index(index), _label(std::move(label)), _isChoice(true),
		_value(initValue) {
		_RebuildChoices(choices);
		assert(ChoiceIndex() >= 0 &&
			static_cast<uint32_t>(ChoiceIndex()) < _choices.Size());
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
		_value = value ? 1.0 : 0.0;
		RaisePropertyChanged(L"BooleanValue");
	}

	bool IsEnabled() const noexcept { return _isEnabled; }
	void IsEnabled(bool value) {
		if (_isEnabled == value) return;
		_isEnabled = value;
		RaisePropertyChanged(L"IsEnabled");
	}

	int32_t ChoiceValue() const noexcept {
		return static_cast<int32_t>(std::lround(_value));
	}

	void ChoiceValue(int32_t value) {
		if (!_isEnabled || std::find(_choiceValues.begin(), _choiceValues.end(), value) ==
			_choiceValues.end() || ChoiceValue() == value) {
			return;
		}
		_value = value;
		RaisePropertyChanged(L"ChoiceValue");
		RaisePropertyChanged(L"ChoiceIndex");
	}

	int32_t ChoiceIndex() const noexcept {
		const auto it = std::find(
			_choiceValues.begin(), _choiceValues.end(), ChoiceValue());
		return it == _choiceValues.end() ? -1 :
			static_cast<int32_t>(std::distance(_choiceValues.begin(), it));
	}

	void ChoiceIndex(int32_t index) {
		if (index < 0 || static_cast<size_t>(index) >= _choiceValues.size()) {
			return;
		}
		ChoiceValue(_choiceValues[index]);
	}

	IObservableVector<hstring> Choices() const noexcept {
		return _choices;
	}

	double Value() const noexcept {
		return _value;
	}

	void Value(double value) {
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
		if (_isBoolean) RaisePropertyChanged(L"BooleanValue");
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
	double ColumnWidth() const noexcept { return _columnWidth; }
	bool ShowLeadingSeparator() const noexcept { return _showLeadingSeparator; }
	IVector<IInspectable> Params() const noexcept { return _params; }
	void RefreshVisibility();
	void ColumnWidth(double value);
	void ShowLeadingSeparator(bool value);

private:
	hstring _label;
	IVector<IInspectable> _params{ nullptr };
	bool _isVisible = true;
	bool _showLeadingSeparator = false;
	double _columnWidth = 260.0;
};

struct EffectParametersViewModel : EffectParametersViewModelT<EffectParametersViewModel>,
		wil::notify_property_changed_base<EffectParametersViewModel> {
	EffectParametersViewModel(uint32_t scalingModeIdx, uint32_t effectIdx);

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

	void UpdateLayoutSize(double availableWidth, double availableHeight);

private:
	bool _IsRemoved() const noexcept;

	void _ScalingModeParameter_PropertyChanged(
		IInspectable const& sender,
		PropertyChangedEventArgs const& args
	);

	phmap::flat_hash_map<std::wstring, float>& _Data() const;
	void _RefreshLayoutWidth();
	void _RefreshConditionalVisibility();

	IVector<IInspectable> _groups{ nullptr };
	std::vector<com_ptr<EffectParameterGroupViewModel>> _groupImpls;
	std::vector<com_ptr<ScalingModeParameter>> _parameterImpls;
	::Magpie::Event<uint32_t, uint32_t>::EventRevoker _parameterChangedRevoker;
	bool _synchronizing = false;
	void _SynchronizeParameters();

	double _availableLayoutWidth = std::numeric_limits<double>::infinity();
	double _contentWidth = 260.0;
	double _contentMaxHeight = 600.0;

	uint32_t _scalingModeIdx;
	uint32_t _effectIdx;
	const ::Magpie::EffectInfo* _effectInfo = nullptr;
};

}
