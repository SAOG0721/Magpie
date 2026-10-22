#pragma once
#include <string_view>
#include "FramePacingOptions.h"
#include "DLSSNRParameters.h"

namespace Magpie {

inline bool IsSuperResolutionEffect(std::string_view effect) noexcept {
	return effect == "DLSS\\DLSS_SR" || effect == "FSR2\\FSR2_SR" ||
		effect == "FSR3\\FSR3_SR" || effect == "FSR4\\FSR4_SR" || effect == "XeSS\\XeSS_SR";
}

inline bool HasOpticalFlowSelection(std::string_view effect) noexcept {
	return IsSuperResolutionEffect(effect) ||
		effect == "DLSSNR\\DLSSNR_AI_Filter" ||
		effect == "DLSSFG\\DLSS_FrameGeneration" ||
		effect == "Diagnostics\\FrameGuidance_Motion" ||
		effect == "Diagnostics\\FrameGuidance_Confidence" ||
		effect == "XeSSFG\\XeSS_FrameGeneration" ||
		effect == "FSR3FG\\FSR3_FrameGeneration";
}

// Restrict built-in UI rules to their owning effects. Custom effects may use
// the same parameter names with completely different meanings.
template<class GetValue>
bool IsEffectParameterVisible(std::string_view effect, std::string_view parameter,
	GetValue&& getValue) noexcept {
	if (effect == "Color\\HDR_to_SDR" && parameter == "peakNits")
		return getValue("mode", 0.f) == 0.f;
	if (effect == "Color\\SDR_to_HDR" && parameter == "nativeWhiteNits")
		return getValue("mode", 0.f) == 1.f ||
			(getValue("mode", 0.f) == 0.f && getValue("__hdrHasPair", 0.f) == 0.f);
	if (effect == "DLSSNR\\DLSSNR_AI_Filter" &&
		(parameter == "residualChromaTemporalStrength" ||
		 DLSSNRParameterPass(parameter) > DLSSNRPassCount(getValue))) return false;
	if (HasOpticalFlowSelection(effect)) {
		const float defaultMethod = (effect == "XeSSFG\\XeSS_FrameGeneration" ||
			effect == "FSR3FG\\FSR3_FrameGeneration") ? 1.0f : 0.0f;
		const float method = getValue("opticalFlowMethod", defaultMethod);
		if (parameter == "amdOpticalFlowMode") return method == 1.0f;
		if (parameter == "nvidiaOpticalFlowQuality") return method == 2.0f;
	}
	if (effect == "DLSSNR\\DLSSNR_AI_Filter") {
		if (parameter == "antiFlickerTimeMs") return DLSSNRAntiFlickerMode(getValue) != 0;
		if (parameter == "residualShowAdvanced")
			return getValue("enableInputResolutionScaling", 0.f) != 0;
		if (parameter == "inputResolutionPercent" || IsDLSSNRResidualParameter(parameter)) {
			if (getValue("enableInputResolutionScaling", 0.f) == 0) return false;
			if (IsDLSSNRAdvancedParameter(parameter))
				return getValue("residualShowAdvanced", 0.f) != 0;
		}
	}
	return true;
}

template<class GetValue>
bool IsEffectParameterEnabled(std::string_view effect, std::string_view parameter,
	bool frontEdgeSyncEnabled, GetValue&& getValue) noexcept {
	if (IsFrameRateFilterEffect(effect)) {
		if (parameter == "frameRateMode") return !frontEdgeSyncEnabled;
		if (parameter == "targetFrameRate") {
			return !UsesFrontEdgeSyncFrameRate(frontEdgeSyncEnabled, getValue("frameRateMode", 0.0f));
		}
	}
	return true;
}

}
