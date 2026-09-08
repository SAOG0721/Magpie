#include "pch.h"
#include "GroupBHdrRoutes.h"

namespace Magpie {

namespace {

HdrFormatRoute MakeRoute(
	std::string_view effect,
	std::string_view option,
	DXGI_FORMAT inputFormat,
	DXGI_FORMAT outputFormat,
	HdrAdapterProfile profile,
	HdrTransferFunction inputTransfer,
	HdrTransferFunction outputTransfer,
	HdrColorRange inputRange,
	HdrColorRange outputRange,
	HdrEvidenceLevel evidence,
	bool hdrNative,
	float normalizationScale = 1.0f
) {
	return HdrFormatRoute{
		.effectId = std::string(effect),
		.optionId = std::string(option),
		.inputFormat = inputFormat,
		.outputFormat = outputFormat,
		.inputTransfer = inputTransfer,
		.outputTransfer = outputTransfer,
		.inputRange = inputRange,
		.outputRange = outputRange,
		.alphaMode = HdrAlphaMode::ForceOpaque,
		.evidenceLevel = evidence,
		.hdrNative = hdrNative,
		.adapterProfile = profile,
		.defaultForHdr = true,
		.defaultForSdr = false,
		.normalizationScale = normalizationScale,
	};
}

HdrFormatRoute MakeSdr(std::string_view effect, std::string_view option) {
	return MakeRoute(effect, option, DXGI_FORMAT_R8G8B8A8_UNORM,
		DXGI_FORMAT_R8G8B8A8_UNORM, HdrAdapterProfile::SDRCompatible,
		HdrTransferFunction::SRGB, HdrTransferFunction::SRGB,
		HdrColorRange::Full, HdrColorRange::Full,
		HdrEvidenceLevel::PublicApiContract, false);
}

}

HdrFormatRoutes GetGroupBHdrRoutes(
	std::string_view effectGroup,
	bool experimentalDlssnr
) noexcept {
	if (effectGroup == "DLSSNR") {
		if (experimentalDlssnr) {
			return { MakeRoute("DLSSNR", "experimental-fp16",
				DXGI_FORMAT_R16G16B16A16_FLOAT,
				DXGI_FORMAT_R16G16B16A16_FLOAT,
				HdrAdapterProfile::BoundedHDR,
				HdrTransferFunction::SRGB,
				HdrTransferFunction::SRGB,
				HdrColorRange::Full,
				HdrColorRange::Full,
				HdrEvidenceLevel::LocalValidation,
				false) };
		}
		return { MakeSdr("DLSSNR", "sdr-r8") };
	}

	if (effectGroup == "DLSS" || effectGroup == "FSR2" ||
		effectGroup == "FSR3" || effectGroup == "FSR4" ||
		effectGroup == "NIS") {
		return { MakeRoute(effectGroup, "hdr-linear-fp16",
			DXGI_FORMAT_R16G16B16A16_FLOAT,
			DXGI_FORMAT_R16G16B16A16_FLOAT,
			HdrAdapterProfile::DirectFP16,
			HdrTransferFunction::Linear,
			HdrTransferFunction::Linear,
			HdrColorRange::SceneLinear,
			HdrColorRange::SceneLinear,
			HdrEvidenceLevel::PublicApiContract, true) };
	}

	if (effectGroup == "FSR") {
		return { MakeSdr("FSR", "sdr-srgb") };
	}

	return {};
}

}
