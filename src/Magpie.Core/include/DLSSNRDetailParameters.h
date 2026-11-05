#pragma once
#include <array>
#include <algorithm>
#include <cmath>
#include <string_view>
#include "DLSSNRPerformanceParameters.h"

namespace Magpie {
inline constexpr std::array<std::string_view, 14> DLSSNR_RESIDUAL_PARAMETERS{
	"residualMultiplier", "residualSaturation", "residualLightness",
	"shadowStructureMultiplier", "residualMidtoneStrength", "reflectionGlowMultiplier",
	"residualHueProtection", "residualDarkProtection", "residualHighlightProtection",
	"residualChromaProtection",
	"residualLocalCompression", "residualLowFrequencyGain", "residualDetailGain",
	"residualDebugView"
};
inline bool IsDLSSNRResidualParameter(std::string_view name) noexcept {
	for (auto value : DLSSNR_RESIDUAL_PARAMETERS) if (value == name) return true;
	return false;
}
inline bool IsDLSSNRAdvancedParameter(std::string_view name) noexcept {
	return name == "residualHueProtection" || name == "residualDarkProtection" ||
		name == "residualHighlightProtection" || name == "residualLocalCompression" ||
		name == "residualLowFrequencyGain" || name == "residualDetailGain" ||
		name == "residualDebugView" || name == "residualChromaProtection";
}
// Retain the old enable key for import/save compatibility while making SDR
// residual reconstruction permanent. Preserve valid numeric adjustments.
template<class Map>
bool NormalizeDLSSNRDetailParameters(Map& values) {
	bool changed = values.erase(L"residualColorMode") != 0;
	changed |= values.erase(L"opticalFlowScaleOrder") != 0;
	// This internal key travels with save/export/copy data, never effect metadata.
	// Decode the old numbering before retiring async modes (v1 2 -> 0, 3 -> 1).
	// Unversioned mode 1 used a fixed two-capture cadence; its hidden interval
	// must not become active. Version 2 persists only the two supported modes.
	const auto version = values.find(L"__dlssnrModeVersion");
	const bool currentModes = version != values.end() && version->second == DLSSNR_MODE_VERSION;
	const bool contiguousModes = currentModes || (version != values.end() && version->second == 1.f);
	const auto mode = values.find(L"processingMode");
	const float oldMode = mode == values.end() ? 0.f : mode->second;
	const float contiguousMode = contiguousModes
		? (oldMode == 1.f || oldMode == 2.f || oldMode == 3.f ? oldMode : 0.f)
		: (oldMode == 1.f ? 1.f : oldMode == 3.f ? 2.f : oldMode == 5.f ? 3.f : 0.f);
	const float normalizedMode = contiguousMode == 1.f || contiguousMode == 3.f ? 1.f : 0.f;
	if (mode == values.end() || oldMode != normalizedMode) {
		values[L"processingMode"] = normalizedMode;
		changed = true;
	}
	const auto interval = values.find(L"sparseUpdateInterval");
	const float oldInterval = interval == values.end() ? 2.f : interval->second;
	const float normalizedInterval = !contiguousModes && oldMode == 1.f ? 2.f :
		(oldInterval == 2.f || oldInterval == 3.f || oldInterval == 4.f ? oldInterval : 2.f);
	if (interval == values.end() || oldInterval != normalizedInterval) {
		values[L"sparseUpdateInterval"] = normalizedInterval;
		changed = true;
	}
	if (!currentModes) {
		values[L"__dlssnrModeVersion"] = DLSSNR_MODE_VERSION;
		changed = true;
	}
	for (const auto name : {L"recoveryPolicy", L"bufferedDelayFrames", L"asyncResidualComposition"})
		changed |= values.erase(name) != 0;
	// Missing keys belong to older configurations: keep their manual setting.
	// New effects explicitly write Adaptive before passing through migration.
	if (!values.contains(L"resolutionControl")) {
		values[L"resolutionControl"] = 1.f;
		changed = true;
	}
	changed |= values.erase(L"residualChromaTemporalStrength") != 0;
	// The old selective protections have no equivalent global value. The new
	// control defaults to zero rather than silently protecting every color.
	for (const auto name : {L"residualShowColorZones", L"residualShadowChroma",
		L"residualMidtoneChroma", L"residualHighlightChroma", L"residualNeutralProtection",
		L"residualSkinProtection", L"residualMaxAgeFrames"}) changed |= values.erase(name) != 0;
	const auto enabled = values.find(L"enableInputResolutionScaling");
	if (enabled == values.end() || enabled->second != 1.f) {
		values[L"enableInputResolutionScaling"] = 1.f;
		changed = true;
	}
	const auto resolution = values.find(L"inputResolutionPercent");
	const float old = resolution == values.end() ? 100.f : resolution->second;
	const float normalized = std::isfinite(old) ? std::round(std::clamp(old, 25.f, 100.f)) : 100.f;
	if (resolution == values.end() || old != normalized) {
		values[L"inputResolutionPercent"] = normalized;
		changed = true;
	}
	const auto protection = values.find(L"residualShowProtection");
	if (protection != values.end()) {
		if (std::isfinite(protection->second) && protection->second != 0) {
			values[L"residualShowAdvanced"] = 1.f;
		}
		values.erase(L"residualShowProtection");
		changed = true;
	}
	for (const auto name : {L"residualShowAdvanced", L"performanceShowAdvanced"}) {
		const auto advanced = values.find(name);
		if (advanced != values.end()) {
			const float normalizedToggle = std::isfinite(advanced->second) && advanced->second != 0 ? 1.f : 0.f;
			changed |= advanced->second != normalizedToggle;
			advanced->second = normalizedToggle;
		}
	}
	return changed;
}
}
