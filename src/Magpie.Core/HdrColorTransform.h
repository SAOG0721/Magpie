#pragma once

#include "HdrFrame.h"
#include <array>

namespace Magpie {

using HdrColor = std::array<float, 4>;

struct HdrTransformParameters {
    float exposure = 1.0f;
    // Canonical scRGB reference white is fixed by the global HDR protocol.
    float referenceWhiteNits = 80.0f;
    float sdrWhiteNits = 80.0f;
    float hdrPeakNits = 1000.0f;
    float shoulder = 1.0f;
    bool preserveAlpha = true;
    bool IsValid() const noexcept;
};

struct HdrTransformConstants {
    float exposure = 1.0f;
    float inverseExposure = 1.0f;
    float sdrWhiteScale = 0.08f;
    float hdrPeakNits = 1000.0f;
    float shoulder = 1.0f;
    uint32_t inputTransfer = static_cast<uint32_t>(HdrTransferFunction::Linear);
    uint32_t outputTransfer = static_cast<uint32_t>(HdrTransferFunction::Linear);
};

class HdrColorTransform {
public:
    static HdrTransformParameters ForFrame(const ColorDescription& color) noexcept;
    static HdrTransformConstants PrepareConstants(
        const HdrTransformParameters& parameters,
        HdrTransferFunction inputTransfer,
        HdrTransferFunction outputTransfer
    ) noexcept;
    static float DecodeTransfer(float value, HdrTransferFunction transfer) noexcept;
    static float EncodeTransfer(float value, HdrTransferFunction transfer) noexcept;
    static float MapHdrToSdr(float value, const HdrTransformParameters& parameters) noexcept;
    static float MapSdrToHdr(float value, const HdrTransformParameters& parameters) noexcept;
    // Bounded FP16 routes (DLSSNR experimental path) use the anchored
    // shoulder family: identity below the SDR white, Reinhard shoulder up to
    // the peak, linear tail beyond; strictly monotone and invertible.
    static float EncodeBoundedHdr(float value, const HdrTransformParameters& parameters) noexcept;
    static float DecodeBoundedHdr(float value, const HdrTransformParameters& parameters) noexcept;
    // Shoulder coefficients derived from the parameters and the route target:
    // f(x<=1)=x, anchored Reinhard up to peak, linear tail beyond.
    struct ShoulderCurve {
        float peak = 1.0f;      // hdrPeakNits / sdrWhiteNits (>= 1)
        float target = 1.0f;    // f(peak) by design, always > 1
        float k = 0.0f;         // shoulder strength in (0, 1)
        float tailSlope = 1.0f; // f'(peak)
    };
    static ShoulderCurve BuildShoulderCurve(
        const HdrTransformParameters& parameters,
        float highlightTarget
    ) noexcept;
    static float ApplyShoulderCurve(float value, const ShoulderCurve& curve) noexcept;
    static float InvertShoulderCurve(float value, const ShoulderCurve& curve) noexcept;
    static constexpr float BoundedRouteHighlightTarget = 2.5f;
    static HdrColor Transform(
        const HdrColor& color,
        HdrTransferFunction inputTransfer,
        HdrTransferFunction outputTransfer,
        const HdrTransformParameters& parameters
    ) noexcept;
};

}
