#pragma once
#include <cmath>
#include <array>
#include <string>
#include <string_view>
#include <utility>

namespace Magpie {

inline constexpr float DLSSNR_MODE_VERSION = 2.f;

// Retired async values remain for internal implementation/tests, not parsing.
enum class DLSSNRProcessingMode {
	EveryFrame = 0, Alternating = 1, Pipeline = 2, SparseEconomy = 3
};
enum class DLSSNRResidualTransfer { InPlace, PerPixel };

// Fixed capture-age budget, independent of display/FG frames or completion.
inline constexpr unsigned DLSSNR_RESIDUAL_MAX_AGE_FRAMES = 1;

template<class GetValue>
DLSSNRProcessingMode DLSSNRMode(GetValue&& getValue) noexcept {
	const float value = getValue("processingMode", 0.f);
	return value == 1.f || value == 3.f ?
		DLSSNRProcessingMode::Alternating : DLSSNRProcessingMode::EveryFrame;
}

inline constexpr bool IsDLSSNRSparseProcessingMode(DLSSNRProcessingMode mode) noexcept {
	return mode == DLSSNRProcessingMode::SparseEconomy;
}

inline constexpr bool SupportsDLSSNRUpdateInterval(DLSSNRProcessingMode mode) noexcept {
	return mode == DLSSNRProcessingMode::Alternating || mode == DLSSNRProcessingMode::SparseEconomy;
}

// Both interval modes use accepted captures, independent of display/FG frames.
template<class GetValue>
unsigned DLSSNRSparseUpdateInterval(GetValue&& getValue) noexcept {
	const float value = getValue("sparseUpdateInterval", 2.f);
	return value == 2.f || value == 3.f || value == 4.f ? unsigned(value) : 2u;
}

inline bool IsDLSSNRDisplayParameter(std::string_view name) noexcept {
	return name == "residualShowAdvanced" || name == "performanceShowAdvanced";
}

inline bool IsDLSSNRPerformanceParameter(std::string_view name) noexcept {
	return name == "resolutionControl" || name == "inputResolutionPercent" ||
		name == "processingMode" || name == "opticalFlowMethod" ||
		name == "amdOpticalFlowMode" || name == "nvidiaOpticalFlowQuality" ||
		name == "residualTransfer" || name == "sparseUpdateInterval";
}

template<class GetValue>
bool IsDLSSNRChoiceEnabled(std::string_view parameter, int value, GetValue&& getValue) noexcept {
	const auto mode = DLSSNRMode(getValue);
	if (IsDLSSNRSparseProcessingMode(mode)) {
		if (parameter == "resolutionControl" && value != 1) return false;
		if (parameter == "residualTransfer" && value != 1) return false;
		if (parameter == "opticalFlowMethod" && value == 0) return false;
	}
	const float flow = getValue("opticalFlowMethod", 0.f);
	if (flow == 1.f || flow == 2.f) return true;
	return !((parameter == "residualTransfer" && value == 1) ||
		(parameter == "antiFlicker" && value >= 2 && value <= 4));
}

// Turning optical flow off also resolves the controls that require motion.
// Keep unknown enum values visible to validation instead of reinterpreting them.
template<class GetValue, class SetValue>
bool NormalizeDLSSNRFlowDependencies(GetValue&& getValue, SetValue&& setValue) {
	if (getValue("opticalFlowMethod", 0.f) != 0.f) return false;
	bool changed = false;
	const float antiFlicker = getValue("antiFlicker", 0.f);
	if (antiFlicker == 2.f || antiFlicker == 3.f || antiFlicker == 4.f) {
		setValue("antiFlicker", 1.f);
		changed = true;
	}
	if (getValue("residualTransfer", 0.f) == 1.f) {
		setValue("residualTransfer", 0.f);
		changed = true;
	}
	return changed;
}

// Validate the whole draft before it reaches any live or restart transaction.
// Mode selection is a user action, not a load-time migration: preserve explicit
// saved choices and allow subsequent edits to the reuse/flow controls.
template<class GetValue, class SetValue>
bool NormalizeDLSSNRParameterEdit(std::string_view parameter, GetValue&& getValue, SetValue&& setValue) {
	if (parameter == "opticalFlowMethod") return NormalizeDLSSNRFlowDependencies(getValue, setValue);
	const auto mode = DLSSNRMode(getValue);
	if (parameter != "processingMode" || mode == DLSSNRProcessingMode::EveryFrame) return false;
	setValue("residualTransfer", 1.f);
	if (IsDLSSNRSparseProcessingMode(mode)) setValue("resolutionControl", 1.f);
	if (getValue("opticalFlowMethod", 0.f) == 0.f) {
		setValue("opticalFlowMethod", 1.f);
		setValue("amdOpticalFlowMode", 1.f);
	}
	return true;
}

template<class GetValue>
bool IsDLSSNRPerformanceConfigurationValid(GetValue&& getValue) noexcept {
	const float model = getValue("modelPreset", 0.f);
	if (!(model == 0.f || model == 1.f || model == 2.f)) return false;
	const float resolutionControl = getValue("resolutionControl", 1.f);
	if (!(resolutionControl == 0.f || resolutionControl == 1.f)) return false;
	const float mode = getValue("processingMode", 0.f);
	const float transfer = getValue("residualTransfer", 0.f);
	const float flow = getValue("opticalFlowMethod", 0.f);
	const float antiFlicker = getValue("antiFlicker", 0.f);
	const float amdQuality = getValue("amdOpticalFlowMode", 1.f);
	const float nvidiaQuality = getValue("nvidiaOpticalFlowQuality", 2.f);
	if (!(mode == 0.f || mode == 1.f) ||
		!(transfer == 0.f || transfer == 1.f) ||
		!(flow == 0.f || flow == 1.f || flow == 2.f) ||
		!std::isfinite(antiFlicker) || antiFlicker < 0 || antiFlicker > 4 ||
		antiFlicker != std::floor(antiFlicker)) return false;
	if (flow == 1.f && !(amdQuality == 0.f || amdQuality == 1.f)) return false;
	if (flow == 2.f && (!std::isfinite(nvidiaQuality) || nvidiaQuality < 1 ||
		nvidiaQuality > 5 || nvidiaQuality != std::floor(nvidiaQuality))) return false;
	if (IsDLSSNRSparseProcessingMode(DLSSNRMode(getValue))) {
		if (resolutionControl != 1.f || transfer != 1.f || flow == 0.f) return false;
	}
	if (SupportsDLSSNRUpdateInterval(DLSSNRMode(getValue))) {
		const float interval = getValue("sparseUpdateInterval", 2.f);
		if (!(interval == 2.f || interval == 3.f || interval == 4.f)) return false;
	}
	return (mode == 0.f || IsDLSSNRChoiceEnabled("residualTransfer", int(transfer), getValue)) &&
		IsDLSSNRChoiceEnabled("antiFlicker", int(antiFlicker), getValue);
}

template<class Option>
bool IsDLSSNRPerformanceOptionValid(const Option& option) noexcept {
	if (option.name != "DLSSNR\\DLSSNR_AI_Filter") return true;
	return IsDLSSNRPerformanceConfigurationValid([&](std::string_view name, float fallback) {
		const auto it = option.parameters.find(std::string(name));
		return it == option.parameters.end() ? fallback : it->second;
	});
}

// Gate live edits only on controls that require a rebuilt performance session.
template<class Option>
bool SameDLSSNRPerformanceControls(const Option& a, const Option& b) noexcept {
	constexpr std::array<std::pair<std::string_view, float>, 8> controls{{
		{"modelPreset", 0.f},
		{"resolutionControl", 1.f},
		{"processingMode", 0.f}, {"opticalFlowMethod", 0.f}, {"amdOpticalFlowMode", 1.f},
		{"nvidiaOpticalFlowQuality", 2.f}, {"residualTransfer", 0.f},
		{"sparseUpdateInterval", 2.f}
	}};
	const auto modeValue = a.parameters.find("processingMode");
	const float mode = modeValue == a.parameters.end() ? 0.f : modeValue->second;
	for (const auto& [name, fallback] : controls) {
		if (name == "sparseUpdateInterval" && mode != 1.f && mode != 3.f) continue;
		const auto left = a.parameters.find(std::string(name)), right = b.parameters.find(std::string(name));
		if ((left == a.parameters.end() ? fallback : left->second) !=
			(right == b.parameters.end() ? fallback : right->second)) return false;
	}
	return true;
}

}
