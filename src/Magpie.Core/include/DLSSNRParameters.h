#pragma once
#include "DLSSNRDetailParameters.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <string_view>

namespace Magpie {

inline constexpr float DLSSNR_ANTI_FLICKER_TIME_DEFAULT_MS = 80.f;

inline float NormalizeDLSSNRAntiFlickerTimeMs(float value) noexcept {
	if (!std::isfinite(value)) return DLSSNR_ANTI_FLICKER_TIME_DEFAULT_MS;
	return std::round(std::clamp(value, 5.f, 200.f) / 5.f) * 5.f;
}

template<class GetValue>
float DLSSNRAntiFlickerTimeMs(GetValue&& getValue) noexcept {
	return NormalizeDLSSNRAntiFlickerTimeMs(getValue("antiFlickerTimeMs", DLSSNR_ANTI_FLICKER_TIME_DEFAULT_MS));
}

inline constexpr std::array<std::string_view, 7> DLSSNR_PASS_PARAMETERS{
	"style", "intensity", "localToneStrength", "localStructureStrength",
	"skinStructureStrength", "useAutoMask", "uiCorrection"
};

inline int DLSSNRParameterPass(std::string_view name) noexcept {
	if (name.starts_with("pass2_")) return 2;
	if (name.starts_with("pass3_")) return 3;
	return 1;
}

inline std::string_view DLSSNRBaseParameter(std::string_view name) noexcept {
	return DLSSNRParameterPass(name) == 1 ? name : name.substr(6);
}

// A time-constant edit affects the next capture, not the already accumulated
// output. Do not dirty the current frame or reset downstream temporal history.
inline bool DLSSNRParameterChangesCurrentOutput(std::string_view name, int activePasses) noexcept {
	return name != "antiFlickerTimeMs" && name != "residualShowAdvanced" &&
		DLSSNRParameterPass(name) <= activePasses;
}

template<class GetValue>
int DLSSNRPassCount(GetValue&& getValue) noexcept {
	const float value = getValue("multiPass", 1.0f);
	if (!std::isfinite(value) || value < 0.5f || value >= 3.5f) return 1;
	return std::max(1, static_cast<int>(std::lround(value)));
}

template<class GetValue>
int DLSSNRAntiFlickerMode(GetValue&& getValue) noexcept {
	const float value = getValue("antiFlicker", 0.0f);
	if (!std::isfinite(value) || value < 0 || value > 4 || value != std::floor(value)) return 0;
	return static_cast<int>(value);
}

// Pass 1 keeps the existing keys for old configurations. Missing later-pass
// keys are erased, allowing the filter parser to use factory defaults.
template<class Option>
Option DLSSNRPassOption(const Option& option, int pass) {
	Option result = option;
	if (pass > 1) {
		const std::string prefix = "pass" + std::to_string(pass) + "_";
		for (const auto name : DLSSNR_PASS_PARAMETERS) {
			const auto it = option.parameters.find(prefix + std::string(name));
			if (it == option.parameters.end()) result.parameters.erase(std::string(name));
			else result.parameters[std::string(name)] = it->second;
		}
	}
	return result;
}

}
