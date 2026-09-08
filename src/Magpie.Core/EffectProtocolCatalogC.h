#pragma once

#include "HdrAdapterDispatcher.h"

// Group-C effect protocol declarations.  These functions deliberately keep
// each effect family separate so a future Renderer bridge can select a route
// without a shared generic HDR processor or effect-name switch.
namespace Magpie::EffectProtocolC {

inline HdrFormatRoute SdrRoute(
	std::string_view effect,
	std::string_view option,
	DXGI_FORMAT format,
	HdrAlphaMode alpha = HdrAlphaMode::ForceOpaque
) {
	return HdrFormatRoute{
		.effectId = std::string(effect),
		.optionId = std::string(option),
		.inputFormat = format,
		.outputFormat = format,
		.inputTransfer = HdrTransferFunction::SRGB,
		.outputTransfer = HdrTransferFunction::SRGB,
		.inputRange = HdrColorRange::Full,
		.outputRange = HdrColorRange::Full,
		.alphaMode = alpha,
		.evidenceLevel = HdrEvidenceLevel::ReferenceImplementation,
		.hdrNative = false,
		.adapterProfile = HdrAdapterProfile::SDRCompatible,
		.defaultForHdr = true,
		.defaultForSdr = true
	};
}

inline HdrFormatRoute ConditionalFp16Route(
	std::string_view effect,
	std::string_view option,
	HdrAlphaMode alpha = HdrAlphaMode::ForceOpaque
) {
	return HdrFormatRoute{
		.effectId = std::string(effect),
		.optionId = std::string(option),
		.inputFormat = DXGI_FORMAT_R16G16B16A16_FLOAT,
		.outputFormat = DXGI_FORMAT_R16G16B16A16_FLOAT,
		.inputTransfer = HdrTransferFunction::Linear,
		.outputTransfer = HdrTransferFunction::Linear,
		.inputRange = HdrColorRange::SceneLinear,
		.outputRange = HdrColorRange::SceneLinear,
		.alphaMode = alpha,
		.evidenceLevel = HdrEvidenceLevel::ReferenceImplementation,
		.hdrNative = false,
		.adapterProfile = HdrAdapterProfile::ConditionalFP16,
		.defaultForHdr = false,
		.defaultForSdr = false
	};
}

inline HdrFormatRoutes NNEDI3() {
	return { SdrRoute("NNEDI3", "luma-r16", DXGI_FORMAT_R8G8B8A8_UNORM) };
}

inline HdrFormatRoutes PixelArt() {
	return { SdrRoute("Pixel Art", "rgb-thresholds", DXGI_FORMAT_R8G8B8A8_UNORM) };
}

inline HdrFormatRoutes RAVU() {
	return { SdrRoute("RAVU", "variant-boundary", DXGI_FORMAT_R8G8B8A8_UNORM) };
}

inline HdrFormatRoutes RTXVideoVsr() {
	HdrFormatRoute route = SdrRoute(
		"RTXVideoVSR", "VSR-BGRA8-U8-sRGB-255", DXGI_FORMAT_B8G8R8A8_UNORM);
	// NvCVImage receives display-referred sRGB code values in the 0..255
	// domain. The adapter performs transfer encoding before the native call.
	route.inputTransfer = HdrTransferFunction::SRGB;
	route.outputTransfer = HdrTransferFunction::SRGB;
	return { route };
}

inline HdrFormatRoutes RTXVideoDenoiser() {
	return { SdrRoute("RTXVideoDenoise", "Denoise-BGRA8-U8", DXGI_FORMAT_B8G8R8A8_UNORM) };
}

inline HdrFormatRoutes RTXVideoHdr() {
	// Product-level SDR->HDR10 behavior has no verified callable texture route.
	return {};
}

inline HdrFormatRoutes Sharpen() {
	return { SdrRoute("Sharpen", "normalized-rgb", DXGI_FORMAT_R8G8B8A8_UNORM) };
}

inline HdrFormatRoutes SMAA() {
	return { SdrRoute("SMAA", "rgba-edge-blend", DXGI_FORMAT_R8G8B8A8_UNORM) };
}

inline HdrFormatRoutes Xbrz() {
	return { SdrRoute("xBRZ", "rgba-u8", DXGI_FORMAT_R8G8B8A8_UNORM) };
}

inline HdrFormatRoutes XeSS() {
	HdrFormatRoute hdr10{
		.effectId = "XeSS",
		.optionId = "HDR10-R10G10B10A2",
		.inputFormat = DXGI_FORMAT_R10G10B10A2_UNORM,
		.outputFormat = DXGI_FORMAT_R10G10B10A2_UNORM,
		.inputTransfer = HdrTransferFunction::PQ,
		.outputTransfer = HdrTransferFunction::PQ,
		.inputRange = HdrColorRange::DisplayReferred,
		.outputRange = HdrColorRange::DisplayReferred,
		.alphaMode = HdrAlphaMode::ForceOpaque,
		.evidenceLevel = HdrEvidenceLevel::PublicApiContract,
		.hdrNative = false,
		.adapterProfile = HdrAdapterProfile::BoundedHDR,
		.defaultForHdr = true,
		.defaultForSdr = false
	};
	HdrFormatRoute fp16 = hdr10;
	fp16.optionId = "R16G16B16A16_FLOAT";
	fp16.inputFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
	fp16.outputFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
	fp16.inputTransfer = HdrTransferFunction::Linear;
	fp16.outputTransfer = HdrTransferFunction::Linear;
	fp16.inputRange = HdrColorRange::SceneLinear;
	fp16.outputRange = HdrColorRange::SceneLinear;
	fp16.hdrNative = true;
	fp16.adapterProfile = HdrAdapterProfile::DirectFP16;
	fp16.defaultForHdr = true;
	hdr10.defaultForHdr = false;
	HdrFormatRoute r11 = fp16;
	r11.optionId = "R11G11B10_FLOAT";
	r11.inputFormat = DXGI_FORMAT_R11G11B10_FLOAT;
	r11.outputFormat = DXGI_FORMAT_R11G11B10_FLOAT;
	r11.defaultForHdr = false;
	HdrFormatRoute r8 = SdrRoute("XeSS", "R8G8B8A8_UNORM", DXGI_FORMAT_R8G8B8A8_UNORM);
	r8.evidenceLevel = HdrEvidenceLevel::PublicApiContract;
	return { hdr10, fp16, r11, r8 };
}

inline HdrFormatRoutes XeSSFG() {
	return { HdrFormatRoute{
		.effectId = "XeSSFG",
		.optionId = "canonical-FP16-marker",
		.inputFormat = DXGI_FORMAT_R16G16B16A16_FLOAT,
		.outputFormat = DXGI_FORMAT_R16G16B16A16_FLOAT,
		.inputTransfer = HdrTransferFunction::Linear,
		.outputTransfer = HdrTransferFunction::Linear,
		.inputRange = HdrColorRange::SceneLinear,
		.outputRange = HdrColorRange::SceneLinear,
		.alphaMode = HdrAlphaMode::Preserve,
		.evidenceLevel = HdrEvidenceLevel::PublicApiContract,
		.hdrNative = false,
		.adapterProfile = HdrAdapterProfile::PresentationTerminal,
		.defaultForHdr = true,
		.defaultForSdr = false
	} };
}

inline HdrFormatRoutes DLSSFG() {
	return { HdrFormatRoute{
		.effectId = "DLSSFG",
		.optionId = "presentation-terminal-runtime-format",
		.inputFormat = DXGI_FORMAT_R16G16B16A16_FLOAT,
		.outputFormat = DXGI_FORMAT_R16G16B16A16_FLOAT,
		.inputTransfer = HdrTransferFunction::Linear,
		.outputTransfer = HdrTransferFunction::Linear,
		.inputRange = HdrColorRange::SceneLinear,
		.outputRange = HdrColorRange::SceneLinear,
		.alphaMode = HdrAlphaMode::Preserve,
		.evidenceLevel = HdrEvidenceLevel::CommunityExperiment,
		.hdrNative = true,
		.adapterProfile = HdrAdapterProfile::PresentationTerminal,
		.defaultForHdr = true,
		.defaultForSdr = false
	} };
}

inline HdrFormatRoutes FSR3FG() {
	return { HdrFormatRoute{
		.effectId = "FSR3FG",
		.optionId = "presentation-terminal-runtime-format",
		.inputFormat = DXGI_FORMAT_R16G16B16A16_FLOAT,
		.outputFormat = DXGI_FORMAT_R16G16B16A16_FLOAT,
		.inputTransfer = HdrTransferFunction::Linear,
		.outputTransfer = HdrTransferFunction::Linear,
		.inputRange = HdrColorRange::SceneLinear,
		.outputRange = HdrColorRange::SceneLinear,
		.alphaMode = HdrAlphaMode::Preserve,
		.evidenceLevel = HdrEvidenceLevel::PublicApiContract,
		.hdrNative = false,
		.adapterProfile = HdrAdapterProfile::PresentationTerminal,
		.defaultForHdr = true,
		.defaultForSdr = false
	} };
}

inline HdrFormatRoutes NvidiaOpticalFlow() {
	// Optical flow produces auxiliary vectors, so this route records the input
	// boundary only. The vector output remains R16G16_FLOAT/S10.5-decoded data.
	HdrFormatRoute route = SdrRoute(
		"NVIDIA Optical Flow", "ABGR8-input-S10.5-output", DXGI_FORMAT_R8G8B8A8_UNORM,
		HdrAlphaMode::ForceOpaque);
	route.evidenceLevel = HdrEvidenceLevel::PublicApiContract;
	return { route };
}

inline HdrFormatRoutes GetGroupCHdrRoutes(std::string_view effectGroup) {
    if (effectGroup == "NNEDI3") return NNEDI3();
    if (effectGroup == "Pixel Art") return PixelArt();
    if (effectGroup == "RAVU") return RAVU();
    if (effectGroup == "RTXVideoVSR") return RTXVideoVsr();
    if (effectGroup == "RTXVideoDenoise") return RTXVideoDenoiser();
    if (effectGroup == "Sharpen") return Sharpen();
    if (effectGroup == "SMAA") return SMAA();
    if (effectGroup == "xBRZ") return Xbrz();
    if (effectGroup == "XeSS") return XeSS();
    if (effectGroup == "XeSSFG") return XeSSFG();
    if (effectGroup == "DLSSFG") return DLSSFG();
    if (effectGroup == "FSR3FG") return FSR3FG();
    if (effectGroup == "NVIDIA Optical Flow") return NvidiaOpticalFlow();
    return {};
}

} // namespace Magpie::EffectProtocolC
