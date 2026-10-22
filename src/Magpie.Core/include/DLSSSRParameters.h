#pragma once
#include <cmath>
#include <string>
#include <string_view>

namespace Magpie {

// Stable NGX request values, independent of the display order. A-F retain the
// historical API values even where a newer SDK no longer names those enums.
// Sources: NVIDIA-RTX/Streamline v2.7.32 include/sl_dlss.h (A-F) and
// NVIDIA/DLSS include/nvsdk_ngx_defs.h (J-M). No Streamline dependency.
inline constexpr int DLSS_SR_DEFAULT_PRESET = 11; // K

inline int NormalizeDLSSSRPreset(float value) noexcept {
	if (!std::isfinite(value) || value != std::floor(value)) return DLSS_SR_DEFAULT_PRESET;
	if ((value >= 0 && value <= 6) || (value >= 10 && value <= 13))
		return static_cast<int>(value);
	return DLSS_SR_DEFAULT_PRESET;
}

template<class Option>
int ParseDLSSSRPreset(const Option& option) noexcept {
	const auto it = option.parameters.find("renderPreset");
	return it == option.parameters.end() ? DLSS_SR_DEFAULT_PRESET : NormalizeDLSSSRPreset(it->second);
}

inline std::string_view DLSSSRPresetName(int value) noexcept {
	switch (value) {
	case 0: return "Auto";
	case 1: return "A";
	case 2: return "B";
	case 3: return "C";
	case 4: return "D";
	case 5: return "E";
	case 6: return "F";
	case 10: return "J";
	case 11: return "K";
	case 12: return "L";
	case 13: return "M";
	default: return "Unknown";
	}
}

}
