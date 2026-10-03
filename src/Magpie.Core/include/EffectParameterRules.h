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
		effect == "XeSSFG\\XeSS_FrameGeneration";
}

// Restrict built-in UI rules to their owning effects. Custom effects may use
// the same parameter names with completely different meanings.
template<class GetValue>
bool IsEffectParameterVisible(std::string_view effect, std::string_view parameter,
	GetValue&& getValue) noexcept {
	if (effect == "DLSSNR\\DLSSNR_AI_Filter" &&
		(parameter == "residualChromaTemporalStrength" ||
		 DLSSNRParameterPass(parameter) > DLSSNRPassCount(getValue))) return false;
	if (HasOpticalFlowSelection(effect)) {
		const float method = getValue("opticalFlowMethod", effect == "XeSSFG\\XeSS_FrameGeneration" ? 1.0f : 0.0f);
		if (parameter == "amdOpticalFlowMode") return method == 1.0f;
		if (parameter == "nvidiaOpticalFlowQuality") return method == 2.0f;
	}
	if (effect == "DLSSNR\\DLSSNR_AI_Filter") {
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
	if (effect == "AMDNR\\AMDNR_AI_Filter" && parameter == "skinStructureStrength")
		return getValue("useAutoMask",1.f) != 0;
	if (IsFrameRateFilterEffect(effect)) {
		if (parameter == "frameRateMode") return !frontEdgeSyncEnabled;
		if (parameter == "targetFrameRate") {
			return !UsesFrontEdgeSyncFrameRate(frontEdgeSyncEnabled, getValue("frameRateMode", 0.0f));
		}
	}
	return true;
}

}
