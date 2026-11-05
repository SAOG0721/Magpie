#pragma once
#include "EffectPickerModel.h"

namespace Magpie {

struct EffectHelper {
	static std::wstring_view GetDisplayName(std::wstring_view fullName) noexcept {
		if (fullName == L"OKLabImageAdjustment") return L"OKLab Image Adjustment";
		if (fullName == L"NvidiaVFX\\Upscale") return L"Nvidia VFX Upscale";
		if (fullName == L"NvidiaVFX\\VFG") return L"Nvidia VFX VFG";
		if (fullName == L"RTXVideo\\RTXVideo_HDR") return L"Nvidia VFX TrueHDR";
		const int rtxFamily = RTXVideoFamily(fullName);
		if (rtxFamily >= 0) return L"Nvidia VFX VSR";
		if (fullName == L"DLSSFG\\DLSS_FrameGeneration") return L"DLSS FrameGeneration";
		if (fullName == L"XeSSFG\\XeSS_FrameGeneration") return L"XeSS FrameGeneration";
		if (fullName == L"FSR3FG\\FSR3_FrameGeneration") return L"FSR3 FrameGeneration";
		size_t delimPos = fullName.find_last_of(L'\\');
		return delimPos != std::wstring::npos ? fullName.substr(delimPos + 1) : fullName;
	}
};

}
