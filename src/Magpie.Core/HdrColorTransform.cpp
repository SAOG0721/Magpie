#include "pch.h"
#include "HdrColorTransform.h"

#include <algorithm>
#include <cmath>

namespace Magpie {

namespace {

// Anchored-shoulder curve family for bounded HDR routes (DLSSNR FP16):
//   x <= 1            identity (SDR content passes through bit-exact)
//   1 < x <= peak     anchored Reinhard shoulder f(x) = x / (1 + k(x - 1))
//   x > peak          linear tail with slope f'(peak)
// The anchor fixes f(peak) = highlightTarget so k = (peak - T) / (T(peak - 1)).
// k in (0, 1) keeps f strictly increasing on the shoulder and the tail slope
// f'(peak) = (1 - k) T^2 / peak^2 stays positive, so highlights never invert
// and the paired inverse has bounded gain everywhere.
// SDR-compatible routes cannot use this family: identity below white forces
// f(1) = 1 and monotonicity then forbids T < 1, while UNORM8 storage has no
// room above 1. SDR routes use the replicate contract instead: identity and
// saturate (see MapHdrToSdr/MapSdrToHdr).
struct ShoulderCoefficients {
    float peak = 1.0f;      // normalized peak: hdrPeakNits / sdrWhiteNits
    float target = 1.0f;    // f(peak) by design, always > 1
    float k = 0.0f;         // shoulder strength in (0, 1)
    float tailSlope = 1.0f; // f'(peak)
};

constexpr float IdentityEpsilon = 1e-6f;

// Design target f(peak) for the bounded FP16 route: keeps the DLSSNR model
// input in the experimentally validated band (scale 2 ~ baseline, 4.5 ~
// blowout). Public constant mirrors HdrColorTransform's constexpr member.
constexpr float BoundedRouteHighlightTarget = HdrColorTransform::BoundedRouteHighlightTarget;

ShoulderCoefficients BuildShoulder(
    const HdrTransformParameters& parameters,
    float highlightTarget
) noexcept {
    ShoulderCoefficients result;
    result.peak = std::max(
        parameters.hdrPeakNits / std::max(parameters.sdrWhiteNits, 1e-4f), 1.0f);
    // Degenerate monitors report MaxLuminance <= SDR white (e.g. 80 <= 360).
    // There is no HDR headroom then; the curve must degrade to the plain
    // identity instead of dividing by (peak - 1) == 0 and seeding NaN into
    // every downstream conversion.
    if (result.peak <= 1.0f + IdentityEpsilon) {
        result.target = 1.0f;
        result.k = 0.0f;
        result.tailSlope = 1.0f;
        return result;
    }
    result.target = std::clamp(highlightTarget, 1.0f + IdentityEpsilon, result.peak);
    result.k = std::clamp(
        (result.peak - result.target) /
            (result.target * (result.peak - 1.0f)),
        IdentityEpsilon, 1.0f - IdentityEpsilon);
    result.tailSlope = (1.0f - result.k) * result.target * result.target /
        (result.peak * result.peak);
    return result;
}

float ApplyShoulder(float x, const ShoulderCoefficients& c) noexcept {
    const float value = std::max(x, 0.0f);
    if (value <= 1.0f) return value;
    if (value <= c.peak) {
        return value / (1.0f + c.k * (value - 1.0f));
    }
    return c.target + c.tailSlope * (value - c.peak);
}

float InvertShoulder(float y, const ShoulderCoefficients& c) noexcept {
    if (y <= 1.0f) return y;
    if (y <= c.target) {
        // Algebraic inverse of the anchored Reinhard shoulder.
        return y * (1.0f - c.k) / std::max(1.0f - c.k * y, 1e-6f);
    }
    return c.peak + (y - c.target) / std::max(c.tailSlope, 1e-6f);
}

// PQ/HLG transfer constants (BT.2100).
constexpr float PQM1 = 2610.0f / 16384.0f;
constexpr float PQM2 = 2523.0f / 32.0f;
constexpr float PQC1 = 3424.0f / 4096.0f;
constexpr float PQC2 = 2413.0f / 128.0f;
constexpr float PQC3 = 2392.0f / 128.0f;
constexpr float PQMaxNits = 10000.0f;

float Clamp01(float value) noexcept { return std::clamp(value, 0.0f, 1.0f); }

float DecodePq(float value) noexcept {
    const float normalized = std::pow(Clamp01(value), 1.0f / PQM2);
    const float numerator = std::max(normalized - PQC1, 0.0f);
    const float denominator = PQC2 - PQC3 * normalized;
    return denominator > 0.0f ? std::pow(numerator / denominator, 1.0f / PQM1) * PQMaxNits : 0.0f;
}

float EncodePq(float value) noexcept {
    const float normalized = std::pow(std::max(value, 0.0f) / PQMaxNits, PQM1);
    return Clamp01(std::pow((PQC1 + PQC2 * normalized) / (1.0f + PQC3 * normalized), PQM2));
}

float DecodeHlg(float value) noexcept {
    constexpr float a = 0.17883277f;
    constexpr float b = 1.0f - 4.0f * a;
    const float c = 0.5f - a * std::log(4.0f * a);
    const float encoded = Clamp01(value);
    return encoded <= 0.5f ? (encoded * encoded) / 3.0f : (std::exp((encoded - c) / a) + b) / 12.0f;
}

float EncodeHlg(float value) noexcept {
    constexpr float a = 0.17883277f;
    constexpr float b = 1.0f - 4.0f * a;
    const float c = 0.5f - a * std::log(4.0f * a);
    const float linear = std::max(value, 0.0f);
    return Clamp01(linear <= 1.0f / 12.0f
        ? std::sqrt(3.0f * linear) : a * std::log(12.0f * linear - b) + c);
}
}

bool HdrTransformParameters::IsValid() const noexcept {
    return std::isfinite(exposure) && exposure > 0.0f &&
        std::isfinite(referenceWhiteNits) && referenceWhiteNits == 80.0f &&
        std::isfinite(sdrWhiteNits) && sdrWhiteNits > 0.0f &&
        std::isfinite(hdrPeakNits) && hdrPeakNits >= referenceWhiteNits &&
        std::isfinite(shoulder) && shoulder > 0.0f;
}

HdrTransformParameters HdrColorTransform::ForFrame(const ColorDescription& color) noexcept {
    HdrTransformParameters parameters;
    if (!color.IsValid()) return parameters;
    parameters.referenceWhiteNits = 80.0f;
    parameters.sdrWhiteNits = color.sdrWhiteNits;
    parameters.hdrPeakNits = std::max({
        color.displayPeakNits, 80.0f, color.sdrWhiteNits });
    parameters.exposure = color.isPreExposed ? color.preExposure : 1.0f;
    return parameters;
}

HdrTransformConstants HdrColorTransform::PrepareConstants(
    const HdrTransformParameters& parameters,
    HdrTransferFunction inputTransfer,
    HdrTransferFunction outputTransfer
) noexcept {
    const HdrTransformParameters valid = parameters.IsValid() ? parameters : HdrTransformParameters{};
    return { valid.exposure, 1.0f / valid.exposure, valid.sdrWhiteNits / valid.hdrPeakNits,
        valid.hdrPeakNits, valid.shoulder, static_cast<uint32_t>(inputTransfer),
        static_cast<uint32_t>(outputTransfer) };
}

float HdrColorTransform::DecodeTransfer(float value, HdrTransferFunction transfer) noexcept {
    switch (transfer) {
    case HdrTransferFunction::SRGB: return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
    case HdrTransferFunction::PQ: return DecodePq(value);
    case HdrTransferFunction::HLG: return DecodeHlg(value);
    case HdrTransferFunction::Linear:
    case HdrTransferFunction::Unknown:
    default: return value;
    }
}

float HdrColorTransform::EncodeTransfer(float value, HdrTransferFunction transfer) noexcept {
    switch (transfer) {
    case HdrTransferFunction::SRGB: return value <= 0.0031308f ? value * 12.92f : 1.055f * std::pow(std::max(value, 0.0f), 1.0f / 2.4f) - 0.055f;
    case HdrTransferFunction::PQ: return EncodePq(value);
    case HdrTransferFunction::HLG: return EncodeHlg(value);
    case HdrTransferFunction::Linear:
    case HdrTransferFunction::Unknown:
    default: return value;
    }
}

float HdrColorTransform::MapHdrToSdr(float value, const HdrTransformParameters& parameters) noexcept {
    // SDR-compatible replicate contract: identity below the SDR white point
    // (the captured SDR game frame reaches the backend bit-exact) and plain
    // saturation above it. There is no shoulder: identity forces f(1) = 1 and
    // UNORM8 storage leaves no code space for a highlight target above 1.
    const HdrTransformParameters valid = parameters.IsValid() ? parameters : HdrTransformParameters{};
    const float referenceWhiteScale = valid.sdrWhiteNits / 80.0f;
    const float normalized = std::max(value, 0.0f) * valid.exposure / referenceWhiteScale;
    return std::min(normalized, 1.0f);
}

float HdrColorTransform::MapSdrToHdr(float value, const HdrTransformParameters& parameters) noexcept {
    // Paired inverse of the replicate contract. Values saturated to 1.0
    // restore to the frame's HDR headroom level in the normalized domain
    // (peak/sdrWhite), then scale back by the white point. The previous
    // formula multiplied by the white point twice for saturated pixels
    // (peak/80 * sdrWhite/80 = 20.25 for a 360-nit display), pushing whites
    // to 1620 nit — the source of the yellow-tinted highlight leak.
    const HdrTransformParameters valid = parameters.IsValid() ? parameters : HdrTransformParameters{};
    const float referenceWhiteScale = valid.sdrWhiteNits / 80.0f;
    const float mapped = std::clamp(value, 0.0f, 1.0f);
    const float normalized = mapped >= 1.0f
        ? std::max(valid.hdrPeakNits, valid.sdrWhiteNits) / valid.sdrWhiteNits
        : mapped;
    return normalized * referenceWhiteScale / valid.exposure;
}

float HdrColorTransform::EncodeBoundedHdr(float value, const HdrTransformParameters& parameters) noexcept {
    const HdrTransformParameters valid = parameters.IsValid() ? parameters : HdrTransformParameters{};
    const ShoulderCoefficients curve = BuildShoulder(valid, BoundedRouteHighlightTarget);
    return ApplyShoulder(std::max(value, 0.0f), curve);
}

float HdrColorTransform::DecodeBoundedHdr(float value, const HdrTransformParameters& parameters) noexcept {
    const HdrTransformParameters valid = parameters.IsValid() ? parameters : HdrTransformParameters{};
    const ShoulderCoefficients curve = BuildShoulder(valid, BoundedRouteHighlightTarget);
    return InvertShoulder(std::max(value, 0.0f), curve);
}

HdrColorTransform::ShoulderCurve HdrColorTransform::BuildShoulderCurve(
    const HdrTransformParameters& parameters,
    float highlightTarget
) noexcept {
    const ShoulderCoefficients coefficients = BuildShoulder(parameters, highlightTarget);
    return ShoulderCurve{
        coefficients.peak, coefficients.target, coefficients.k, coefficients.tailSlope };
}

float HdrColorTransform::ApplyShoulderCurve(float value, const ShoulderCurve& curve) noexcept {
    return ApplyShoulder(value, { curve.peak, curve.target, curve.k, curve.tailSlope });
}

float HdrColorTransform::InvertShoulderCurve(float value, const ShoulderCurve& curve) noexcept {
    return InvertShoulder(value, { curve.peak, curve.target, curve.k, curve.tailSlope });
}

HdrColor HdrColorTransform::Transform(
    const HdrColor& color,
    HdrTransferFunction inputTransfer,
    HdrTransferFunction outputTransfer,
    const HdrTransformParameters& parameters
) noexcept {
    HdrColor result = color;
    for (size_t channel = 0; channel < 3; ++channel) {
        result[channel] = EncodeTransfer(DecodeTransfer(color[channel], inputTransfer), outputTransfer);
    }
    result[3] = parameters.preserveAlpha ? color[3] : 1.0f;
    return result;
}

}
