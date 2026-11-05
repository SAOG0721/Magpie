#include "pch.h"
#include "EffectParametersViewModel.h"
#if __has_include("EffectParameterGroupViewModel.g.cpp")
#include "EffectParameterGroupViewModel.g.cpp"
#endif
#if __has_include("ScalingModeParameter.g.cpp")
#include "ScalingModeParameter.g.cpp"
#endif
#if __has_include("EffectParametersViewModel.g.cpp")
#include "EffectParametersViewModel.g.cpp"
#endif
#include "StrHelper.h"
#include "AppSettings.h"
#include "ScalingModesService.h"
#include "ScalingMode.h"
#include "EffectsService.h"
#include "EffectDesc.h"
#include "EffectParameterValue.h"
#include "EffectParameterRules.h"
#include "EffectParameterPopupLayout.h"
#include "HdrParameterPolicy.h"
#include "NvidiaVfxParameterPolicy.h"
#include "ScalingService.h"
#include "EffectParameterLocalization.h"
#include "App.h"
#include "EffectPickerModel.h"
#include "ToastService.h"
#include "OKLabParameters.h"
#include "ParameterItems.h"
#include "ErrorService.h"

using namespace Magpie;

namespace winrt::Magpie::implementation {

EffectParameterGroupViewModel::EffectParameterGroupViewModel(
	hstring label,
	std::vector<IInspectable>&& params
) : _label(std::move(label)) {
	_params = single_threaded_observable_vector(std::move(params));
	RefreshVisibility();
}

void EffectParameterGroupViewModel::UpdateParameters(const std::vector<IInspectable>& parameters) {
	SynchronizeParameterItems(_params, parameters);
	RefreshVisibility();
}

void EffectParameterGroupViewModel::RefreshVisibility() {
	bool visible = false;
	if (_params) {
		for (const IInspectable& item : _params) {
			if (get_self<ScalingModeParameter>(
				item.as<Magpie::ScalingModeParameter>())->IsVisible()) {
				visible = true;
				break;
			}
		}
	}
	if (_isVisible == visible) return;
	_isVisible = visible;
	RaisePropertyChanged(L"IsVisible");
}

void EffectParameterGroupViewModel::ShowLeadingSeparator(bool value) {
	if (_showLeadingSeparator == value) return;
	_showLeadingSeparator = value;
	RaisePropertyChanged(L"ShowLeadingSeparator");
}

// 限制保存频率
// 1 秒内没有新的调用才执行保存
void EffectParametersViewModel::_LazySaveAppSettings() {
	// Debounce entirely on the UI dispatcher. A thread-pool delay used to read
	// lastInvokeTime concurrently with parameter edits on the UI thread.
	static const auto timer = [] {
		auto value = App::Get().Dispatcher().CreateTimer();
		value.Interval(std::chrono::seconds(1));
		value.IsRepeating(false);
		value.Tick([](auto const&, auto const&) { AppSettings::Get().SaveAsync(); });
		return value;
	}();
	timer.Stop();
	timer.Start();
}

EffectParametersViewModel::EffectParametersViewModel(uint32_t scalingModeIdx, uint32_t effectIdx)
	: _scalingModeIdx(scalingModeIdx), _effectIdx(effectIdx)
{
	ScalingMode& scalingMode = ScalingModesService::Get().GetScalingMode(_scalingModeIdx);
	_effectInfo = EffectsService::Get().GetEffect(scalingMode.effects[_effectIdx].name);
	const bool isDlssnr = scalingMode.effects[_effectIdx].name ==
		L"DLSSNR\\DLSSNR_AI_Filter";

	phmap::flat_hash_map<std::wstring, float>& params = _Data();

	struct GroupBuilder {
		std::string name;
		std::vector<IInspectable> params;
	};
	std::vector<GroupBuilder> groupBuilders;
	auto getGroup = [&](const std::string& name) -> std::vector<IInspectable>& {
		auto it = std::ranges::find(groupBuilders, name, &GroupBuilder::name);
		if (it == groupBuilders.end()) {
			it = groupBuilders.emplace(groupBuilders.end(), GroupBuilder{ name });
		}
		return it->params;
	};
	auto displayParams = _effectInfo->params;
	EffectParameterLocalization::Localize(StrHelper::UTF16ToUTF8(_effectInfo->name), displayParams);
	com_ptr<ScalingModeParameter> residualToggle;
	std::array<com_ptr<ScalingModeParameter>, 3> passMaskToggles;
	for (uint32_t i = 0, size = (uint32_t)displayParams.size(); i < size; ++i) {
		EffectParameterDesc param = displayParams[i];
		const auto category = params.find(L"category");
		FilterNvidiaVfxVsrChoices(StrHelper::UTF16ToUTF8(_effectInfo->name), param,
			NvidiaVfxCategory(category == params.end() ? 0.f : category->second));

		std::optional<float> paramValue;
		if (_effectInfo->name == L"Color\\SDR_to_HDR" && param.name == "nativeWhiteNits")
			paramValue = NativeHdrWhiteValue(params, std::wstring(L"nativeWhiteNits"), std::wstring(L"whiteNits"), std::wstring(L"exposure"));
		{
			auto it = params.find(StrHelper::UTF8ToUTF16(param.name));
			if (it != params.end()) {
				paramValue = it->second;
			}
		}
		const float defaultValue = param.constant.index() == 0
			? std::get<0>(param.constant).defaultValue
			: static_cast<float>(std::get<1>(param.constant).defaultValue);
		const float normalizedValue = NormalizeEffectParameterValue(
			param, paramValue.value_or(defaultValue));

		if (IsChoiceEffectParameter(param)) {
			std::vector<std::pair<int, hstring>> choices;
			choices.reserve(param.choices.size());
			for (const EffectParameterChoice& choice : param.choices) {
				choices.emplace_back(choice.value,
					hstring(StrHelper::UTF8ToUTF16(choice.label)));
			}
			auto paramItem = make_self<ScalingModeParameter>(
				i,
				hstring(StrHelper::UTF8ToUTF16(
					param.label.empty() ? param.name : param.label)),
				isDlssnr && (IsDLSSNRPerformanceParameter(param.name) || param.name == "antiFlicker" || param.name == "modelPreset")
					? double(paramValue.value_or(defaultValue)) : double(normalizedValue),
				std::move(choices));
			paramItem->PropertyChanged({
				this, &EffectParametersViewModel::_ScalingModeParameter_PropertyChanged });
			_parameterImpls.push_back(paramItem);
			getGroup(param.group).push_back(*paramItem);
		} else if (param.constant.index() == 0) {
			const EffectConstant<float>& constant = std::get<0>(param.constant);
			auto paramItem = make_self<ScalingModeParameter>(
				i,
				hstring(StrHelper::UTF8ToUTF16(param.label.empty() ? param.name : param.label)),
				normalizedValue,
				constant.minValue,
				constant.maxValue,
				constant.step,
				defaultValue
			);
			paramItem->ConfigureDescriptor(param);
			paramItem->PropertyChanged({
				this, &EffectParametersViewModel::_ScalingModeParameter_PropertyChanged });

			_parameterImpls.push_back(paramItem);
			getGroup(param.group).push_back(*paramItem);
		} else {
			const EffectConstant<int>& constant = std::get<1>(param.constant);
			if (constant.minValue == 0 && constant.maxValue == 1 && constant.step == 1) {
				auto paramItem = make_self<ScalingModeParameter>(
					i,
					hstring(StrHelper::UTF8ToUTF16(param.label.empty() ? param.name : param.label)),
					std::abs(normalizedValue) > FLOAT_EPSILON<float>
				);
				paramItem->PropertyChanged({
					this, &EffectParametersViewModel::_ScalingModeParameter_PropertyChanged });
				_parameterImpls.push_back(paramItem);
				const size_t pass = DLSSNRParameterPass(param.name)-1;
				const auto baseName = DLSSNRBaseParameter(param.name);
				if (isDlssnr && baseName == "uiCorrection" && passMaskToggles[pass]) {
					passMaskToggles[pass]->Companion(*paramItem);
				} else if (isDlssnr && param.name == "performanceShowAdvanced" && residualToggle) {
					residualToggle->Companion(*paramItem);
				} else {
					getGroup(param.group).push_back(*paramItem);
					if (isDlssnr && param.name == "residualShowAdvanced") residualToggle = paramItem;
					if (isDlssnr && baseName == "useAutoMask") passMaskToggles[pass] = paramItem;
				}
			} else {
				auto paramItem = make_self<ScalingModeParameter>(
					i,
					hstring(StrHelper::UTF8ToUTF16(param.label.empty() ? param.name : param.label)),
					normalizedValue,
					(float)constant.minValue,
					(float)constant.maxValue,
					(float)constant.step,
					defaultValue
				);
				paramItem->ConfigureDescriptor(param);
				paramItem->PropertyChanged({
					this, &EffectParametersViewModel::_ScalingModeParameter_PropertyChanged });

				_parameterImpls.push_back(paramItem);
				getGroup(param.group).push_back(*paramItem);
			}
		}
	}
	std::vector<IInspectable> groups;
	groups.reserve(groupBuilders.size());
	_sourceGroups.reserve(groupBuilders.size());
	for (GroupBuilder& builder : groupBuilders) {
		if (!isDlssnr && ClassifyFrameGenerationEffect(std::wstring_view(_effectInfo->name)) ==
			FrameGenerationEffectKind::None) {
			std::stable_partition(
				builder.params.begin(), builder.params.end(),
				[](const IInspectable& item) {
					return get_self<ScalingModeParameter>(
						item.as<Magpie::ScalingModeParameter>())->IsBoolean();
				});
		}
		auto group = make_self<EffectParameterGroupViewModel>(
			hstring(StrHelper::UTF8ToUTF16(builder.name)),
			std::move(builder.params));
		groups.push_back(*group);
		_sourceGroups.push_back(std::move(group));
	}
	if (!groups.empty()) {
		_groups = single_threaded_vector(std::move(groups));
	}
	_SynchronizeParameters();
	if (isDlssnr) {
		_runtimeStatusChangedRevoker = ErrorService::Get().Changed(auto_revoke,
			[this] { _RefreshRuntimeResolution(); });
	}
	if (IsOKLab()) { _OKRefreshSourceGroup(); _RefreshVisibleGroups(); }
	_parameterChangedRevoker = ScalingModesService::Get().EffectParametersChanged(
		auto_revoke, [this](uint32_t mode, uint32_t effect) {
			if (_IsRemoved() || mode != _scalingModeIdx) return;
			if (effect == _effectIdx) _SynchronizeParameters();
			else { _RefreshConditionalVisibility(); _RefreshVisibleGroups(); }
		});
}

void EffectParametersViewModel::UpdateLayoutSize(double availableWidth, double availableHeight) {
	_contentWidth = std::max(availableWidth, 1.0);
	RaisePropertyChanged(L"ContentWidth");
	_contentMaxHeight = GetEffectParameterContentHeight(availableHeight, IsOKLab());
	RaisePropertyChanged(L"ContentMaxHeight");
	_RefreshVisibleGroups();
}

void EffectParametersViewModel::_RefreshVisibleGroups() {
	// Conditional rows must not recreate surviving controls while a ComboBox is
	// committing a selection. Keep the group and row collections observable and
	// update only inserted/removed items, preserving the native popup/focus owner.
	std::vector<uint32_t> signature;
	for (const auto& group : _sourceGroups) {
		signature.push_back(UINT32_MAX);
		for (const auto& item : group->Params()) {
			const auto parameter = get_self<ScalingModeParameter>(item.as<Magpie::ScalingModeParameter>());
			if (parameter->IsVisible()) signature.push_back(parameter->Index());
		}
	}
	if (signature == _visibleParameters) return;
	_visibleParameters = std::move(signature);
	std::vector<IInspectable> groups;
	const bool initialize = _groupImpls.empty();
	_groupImpls.resize(_sourceGroups.size());
	for (size_t index = 0; index < _sourceGroups.size(); ++index) {
		const auto& source = _sourceGroups[index];
		std::vector<IInspectable> visible;
		for (const auto& item : source->Params()) {
			if (get_self<ScalingModeParameter>(item.as<Magpie::ScalingModeParameter>())->IsVisible()) visible.push_back(item);
		}
		auto& group = _groupImpls[index];
		if (!group || group->Label() != source->Label()) {
			group = make_self<EffectParameterGroupViewModel>(source->Label(), std::move(visible));
		} else group->UpdateParameters(visible);
		if (group->Params().Size() == 0) continue;
		group->ShowLeadingSeparator(!groups.empty());
		groups.push_back(*group);
	}
	if (initialize) {
		_groups = single_threaded_observable_vector(std::move(groups));
		RaisePropertyChanged(L"Groups");
	} else SynchronizeParameterItems(_groups, groups);
}

// 应确保被删除后依然处于合法的状态，调用任何方法都不会崩溃，见 ScalingModeItem::_IsRemoved
bool EffectParametersViewModel::_IsRemoved() const noexcept {
	return _scalingModeIdx == std::numeric_limits<uint32_t>::max() ||
		_effectIdx == std::numeric_limits<uint32_t>::max();
}

void EffectParametersViewModel::_ScalingModeParameter_PropertyChanged(
	IInspectable const& sender,
	PropertyChangedEventArgs const& args
) {
	if (_IsRemoved() || _synchronizing ||
		(args.PropertyName() != L"Value" &&
		 args.PropertyName() != L"BooleanValue" &&
		 args.PropertyName() != L"ChoiceValue")) {
		return;
	}

	const auto parameter = sender.try_as<Magpie::ScalingModeParameter>();
	if (!parameter) return;
	ScalingModeParameter* paramImpl = get_self<ScalingModeParameter>(parameter);
	const auto expectedProperty = paramImpl->IsBoolean() ? L"BooleanValue" :
		paramImpl->IsChoice() ? L"ChoiceValue" : L"Value";
	if (args.PropertyName() != expectedProperty || !paramImpl->IsEnabled()) return;
	if (paramImpl->Index() >= _effectInfo->params.size()) return;
	TraceParameterFlyout("parameter.edit.begin", uintptr_t(paramImpl), paramImpl->Index(),
		paramImpl->IsChoice() ? paramImpl->ChoiceValue() : 0);
	auto traceEnd = wil::scope_exit([paramImpl] {
		TraceParameterFlyout("parameter.edit.end", uintptr_t(paramImpl));
	});
	const std::string& effectName = _effectInfo->params[paramImpl->Index()].name;
	if (IsOKLab()) _okUndo = _OKSnapshot();
	if (_effectInfo->name == L"RTXVideo\\RTXVideo_VSR" && effectName == "category") {
		auto& item = ScalingModesService::Get().GetScalingMode(_scalingModeIdx).effects[_effectIdx];
		ChangeNvidiaVfxVsrCategory(item, paramImpl->ChoiceValue());
		_SynchronizeParameters();
	} else {
	_Data()[StrHelper::UTF8ToUTF16(effectName)] = paramImpl->IsBoolean()
		? static_cast<float>(paramImpl->BooleanValue())
		: paramImpl->IsChoice()
			? static_cast<float>(paramImpl->ChoiceValue())
			: static_cast<float>(paramImpl->Value());
		if (_effectInfo->name == L"DLSSNR\\DLSSNR_AI_Filter") {
			auto& values = _Data();
			if (NormalizeDLSSNRParameterEdit(effectName, [&](std::string_view name, float fallback) {
				const auto it = values.find(StrHelper::UTF8ToUTF16(name));
				return it == values.end() ? fallback : it->second;
			}, [&](std::string_view name, float value) { values[StrHelper::UTF8ToUTF16(name)] = value; })) {
				_SynchronizeParameters();
			}
		}
	}
	_RefreshConditionalVisibility();
	_RefreshVisibleGroups();
	ScalingService::Get().EffectParameterEdited(_scalingModeIdx, _effectIdx, effectName,
		_Data().at(StrHelper::UTF8ToUTF16(effectName)));
	ScalingModesService::Get().EffectParametersChanged.Invoke(_scalingModeIdx, _effectIdx);

	_LazySaveAppSettings();
	if (IsOKLab()) _OKNotifyVisuals();
}

void EffectParametersViewModel::_RefreshConditionalVisibility() {
	const std::string effect = StrHelper::UTF16ToUTF8(_effectInfo->name);
	const auto& values = _Data();
	std::vector<EffectOption> options;
	for (const auto& item : ScalingModesService::Get().GetScalingMode(_scalingModeIdx).effects)
		options.push_back(static_cast<EffectOption>(item));
	const auto hdrPlan = BuildHdrComponentPlan(options, ScalingModesService::Get().GetScalingMode(_scalingModeIdx).legacyHdrCaptureIntent);
	auto getValue = [&](std::string_view name, float fallback) {
		if (name == "__hdrHasPair") return hdrPlan.error == HdrComponentError::None && _effectIdx < hdrPlan.stages.size() &&
			hdrPlan.stages[_effectIdx].pairIndex != size_t(-1) ? 1.f : 0.f;
		const auto it = values.find(StrHelper::UTF8ToUTF16(name));
		if (it != values.end()) return it->second;
		const auto descriptor = std::ranges::find(_effectInfo->params, name, &EffectParameterDesc::name);
		if (descriptor == _effectInfo->params.end()) return fallback;
		return descriptor->constant.index() == 0 ? std::get<0>(descriptor->constant).defaultValue : float(std::get<1>(descriptor->constant).defaultValue);
	};
	const bool frontEdgeSyncEnabled = false;
	const bool wasSynchronizing = std::exchange(_synchronizing, true);
	auto reset = wil::scope_exit([this, wasSynchronizing] { _synchronizing = wasSynchronizing; });
	const bool dlssnr = effect == "DLSSNR\\DLSSNR_AI_Filter";
	const hstring validation = dlssnr && !IsDLSSNRPerformanceConfigurationValid(getValue)
		? to_hstring(EffectParameterLocalization::Resource(L"EffectParam_DLSSNR_DLSSNR_AI_Filter_InvalidConfiguration")) : hstring();
	if (_validationMessage != validation) {
		_validationMessage = validation;
		RaisePropertyChanged(L"ValidationMessage"); RaisePropertyChanged(L"HasValidationMessage");
	}
	for (const auto& parameter : _parameterImpls) {
		const auto& name = _effectInfo->params[parameter->Index()].name;
		if (effect == NVIDIA_VFX_VSR_ID && name == "strength") {
			auto descriptor = _effectInfo->params[parameter->Index()];
			std::vector<EffectParameterDesc> localized{ std::move(descriptor) };
			EffectParameterLocalization::Localize(effect, localized);
			FilterNvidiaVfxVsrChoices(effect, localized.front(), NvidiaVfxCategory(getValue("category", 0.f)));
			std::vector<std::pair<int, hstring>> choices;
			for (const auto& choice : localized.front().choices)
				choices.emplace_back(choice.value, to_hstring(choice.label));
			parameter->_RebuildChoices(choices);
			parameter->SynchronizeValue(float(NvidiaVfxTier(NvidiaVfxCategory(getValue("category", 0.f)), getValue("strength", 1.f))));
		}
		if (IsFrameRateFilterEffect(effect) && name == "frameRateMode") {
			parameter->SynchronizeValue(UsesFrontEdgeSyncFrameRate(frontEdgeSyncEnabled,
				getValue(name, 0.0f)) ? 0.0f : 1.0f);
		}
		const bool visible = IsOKLab() ? OKLab::Visible(name, _okSection, _okColor, _okSlot, _okCurve) :
			IsEffectParameterVisible(effect, name, getValue);
		parameter->IsVisible(visible);
		parameter->IsEnabled(visible && IsEffectParameterEnabled(effect, name, frontEdgeSyncEnabled, getValue));
		if (dlssnr && parameter->IsChoice()) {
			std::vector<bool> allowed;
			for (int value : parameter->ChoiceValues()) allowed.push_back(IsDLSSNRChoiceEnabled(name, value, getValue));
			parameter->ChoiceAvailability(std::move(allowed), to_hstring(
				EffectParameterLocalization::DLSSNRChoiceReason(name, getValue)));
		}
		if (IsOKLab() && !visible) continue;
		auto display = _effectInfo->params[parameter->Index()];
		std::vector<EffectParameterDesc> localized{ std::move(display) };
		EffectParameterLocalization::Localize(effect, localized);
		auto tooltip = EffectParameterLocalization::Tooltip(effect, localized.front(), parameter->IsEnabled());
		if (dlssnr && parameter->IsChoice() && !parameter->IsChoiceAllowed(parameter->ChoiceIndex()))
			tooltip = parameter->ChoiceIndex() < 0 ? EffectParameterLocalization::Resource(
				L"EffectParam_DLSSNR_DLSSNR_AI_Filter_InvalidConfiguration") : to_string(parameter->ChoiceReason(parameter->ChoiceIndex()));
		if (name == "mode" && hdrPlan.error == HdrComponentError::None && _effectIdx < hdrPlan.stages.size() &&
			hdrPlan.stages[_effectIdx].pairIndex != size_t(-1))
			tooltip += '\n' + EffectParameterLocalization::HdrPairSource(hdrPlan.stages[_effectIdx].pairIndex);
		parameter->Tooltip(to_hstring(tooltip));
	}
	for (const auto& group : _sourceGroups) {
		group->RefreshVisibility();
	}
	_RefreshRuntimeResolution();
}

void EffectParametersViewModel::_RefreshRuntimeResolution() {
	if (_IsRemoved() || _effectInfo->name != L"DLSSNR\\DLSSNR_AI_Filter") return;
	const auto& modes = AppSettings::Get().ScalingModes();
	if (_scalingModeIdx >= modes.size() || _effectIdx >= modes[_scalingModeIdx].effects.size()) return;
	const auto& mode = modes[_scalingModeIdx];
	const auto& effect = mode.effects[_effectIdx];
	if (effect.name != _effectInfo->name) return;
	const auto modeValue = effect.parameters.find(L"resolutionControl");
	const bool adaptive = modeValue == effect.parameters.end() || modeValue->second == 0.f;
	const auto stored = effect.parameters.find(L"inputResolutionPercent");
	float displayed = stored == effect.parameters.end() ? 100.f : stored->second;
	const auto& errors = ErrorService::Get();
	const auto& context = errors.DlssNrContext();
	// A status belongs to the running group and effect identity, not whichever
	// settings page happens to be open when its notification reaches the UI.
	if (adaptive && context.scalingModeIdentity.lock() == mode.runtimeIdentity &&
		_effectIdx < context.effectIdentities.size() &&
		context.effectIdentities[_effectIdx].lock() == effect.runtimeIdentity) {
		if (const auto session = errors.DlssNrSession()) {
			const auto snapshot = session->Read();
			if (_effectIdx < snapshot.effects.size()) {
				const auto& status = snapshot.effects[_effectIdx];
				if (status.state == DlssNrState::Running && status.activeInputResolutionPercent >= 25 &&
					status.activeInputResolutionPercent <= 100)
					displayed = float(status.activeInputResolutionPercent);
			}
		}
	}
	const bool wasSynchronizing = std::exchange(_synchronizing, true);
	auto reset = wil::scope_exit([this, wasSynchronizing] { _synchronizing = wasSynchronizing; });
	for (const auto& parameter : _parameterImpls) {
		if (_effectInfo->params[parameter->Index()].name == "inputResolutionPercent") {
			// Only the read-only view changes. The stored manual percentage remains
			// available when switching back to manual control.
			parameter->SynchronizeValue(displayed);
			break;
		}
	}
}

void EffectParametersViewModel::_SynchronizeParameters() {
	const auto& modes = AppSettings::Get().ScalingModes();
	if (_scalingModeIdx >= modes.size() || _effectIdx >= modes[_scalingModeIdx].effects.size()) return;
	const auto& name = modes[_scalingModeIdx].effects[_effectIdx].name;
	if (name != _effectInfo->name) return;
	_synchronizing = true;
	auto reset = wil::scope_exit([this] { _synchronizing = false; });
	const auto& values = _Data();
	for (const auto& parameter : _parameterImpls) {
		const auto& descriptor = _effectInfo->params[parameter->Index()];
		const auto it = values.find(StrHelper::UTF8ToUTF16(descriptor.name));
		float fallback = descriptor.constant.index() == 0 ?
			std::get<0>(descriptor.constant).defaultValue : float(std::get<1>(descriptor.constant).defaultValue);
		if (name == L"Color\\SDR_to_HDR" && descriptor.name == "nativeWhiteNits")
			fallback = NativeHdrWhiteValue(values, std::wstring(L"nativeWhiteNits"), std::wstring(L"whiteNits"), std::wstring(L"exposure"));
		const float value = it == values.end() ? fallback : it->second;
		const bool preserveDraft = name == L"DLSSNR\\DLSSNR_AI_Filter" &&
			(IsDLSSNRPerformanceParameter(descriptor.name) || descriptor.name == "antiFlicker" || descriptor.name == "modelPreset");
		parameter->SynchronizeValue(preserveDraft ? value : NormalizeEffectParameterValue(descriptor, value));
	}
	_RefreshConditionalVisibility();
	_RefreshVisibleGroups();
	if (IsOKLab()) _OKNotifyVisuals();
}

phmap::flat_hash_map<std::wstring, float>& EffectParametersViewModel::_Data() const {
	ScalingMode& scalingMode = ScalingModesService::Get().GetScalingMode(_scalingModeIdx);
	return scalingMode.effects[_effectIdx].parameters;
}

hstring ScalingModeParameter::ValueText() const noexcept {
	return App::DoubleFormatter().FormatDouble(_value);
}

void ScalingModeParameter::EditableValue(hstring const& text) {
	if (!IsFloat() || !_isEnabled || !IsPreciseInput() || text == ValueText()) return;
	const auto parsed = App::DoubleFormatter().as<Windows::Globalization::NumberFormatting::INumberParser>().ParseDouble(text);
	const double minimum = std::max(_minimum, double(_descriptor->editMinimum));
	const float candidate = parsed ? float(parsed.Value()) : std::numeric_limits<float>::quiet_NaN();
	// Compare in the stored float domain: a typed decimal 0.1 must match the
	// float minimum, which is slightly greater than the parsed double 0.1.
	if (!parsed || !std::isfinite(candidate) || candidate < float(minimum) || candidate > float(_maximum) ||
		(_descriptor->constant.index() == 1 && parsed.Value() != std::floor(parsed.Value()))) {
		_inputError = to_hstring(::Magpie::EffectParameterLocalization::Resource(L"HdrParameter_InvalidInput",
			"Enter a number in the shown range; integer parameters require whole numbers."));
	} else {
		_inputError = {};
		Value(parsed.Value());
	}
	RaisePropertyChanged(L"InputError");
	RaisePropertyChanged(L"HasInputError");
}

}
