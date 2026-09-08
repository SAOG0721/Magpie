#pragma once

#include "HdrFrame.h"

#include <array>
#include <cmath>

namespace Magpie {

// Effect-local protocol values for the DLSS/FSR/NIS/Optical Flow family.
// These fields deliberately stay separate from the renderer-wide HDR model:
// each SDK consumes a different transfer, range, and auxiliary-resource set.
enum class GroupBTransfer : uint8_t { Unspecified, Linear, SRGB, PQ };

struct FsrHdrProtocol {
	bool hdrColorInput = false;
	GroupBTransfer transfer = GroupBTransfer::Linear;
	float preExposure = 1.0f;
	float exposure = 1.0f;
	bool depthInverted = true;
	bool depthInfinite = true;
	bool useReactiveMask = true;
	bool useTransparencyMask = true;
};

struct NisHdrProtocol {
	enum class Mode : uint8_t { None, Linear, PQ };
	Mode mode = Mode::None;
	float linearMax = 12.5f;
};

struct AmdOpticalFlowHdrProtocol {
	GroupBTransfer transfer = GroupBTransfer::SRGB;
	std::array<float, 2> minMaxLuminance{ 0.0f, 1.0f };
};

struct DlssnrExperimentProtocol {
	bool enabled = false;
	// The FP16 route encodes the normalized color with the extended sRGB
	// OETF (matching the model's U8 training contract below the white point
	// and continuing the same power law for HDR headroom). No scale
	// selector: the normalization is fully derived from the frame metadata.
};

} // namespace Magpie
