#pragma once

#include "FrameUpdateReason.h"
#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <d3d11.h>
#include <dxgicommon.h>

namespace Magpie {

enum class HdrColorPrimaries : uint8_t { Unknown, Rec709, Rec2020, P3D65 };
enum class HdrTransferFunction : uint8_t { Unknown, Linear, SRGB, PQ, HLG };
enum class HdrColorRange : uint8_t {
    Unknown,
    Full,
    Limited,
    SceneLinear,
    DisplayReferred
};

enum class HdrAlphaMode : uint8_t {
    Unknown,
    Preserve,
    ForceOpaque,
    Premultiplied
};

enum class HdrEvidenceLevel : uint8_t {
    None,
    PublicApiContract,
    ReferenceImplementation,
    CommunityExperiment,
    LocalValidation
};

enum class HdrAdapterProfile : uint8_t {
    DirectFP16, BoundedHDR, SDRCompatible, ConditionalFP16, Unknown, PresentationTerminal
};

// Every HDR texture crossing a Magpie module boundary carries one of these
// roles. RawCapture is confined to the capture front end and is never a
// renderer/effect/presentation input.
enum class HdrFrameStage : uint8_t {
    Unknown,
    RawCapture,
    CanonicalInput,
    EffectLocalInput,
    EffectLocalOutput,
    CanonicalOutput,
    PublishedOutput,
    PresentedOutput,
    GeneratedOutput,
};

struct HdrMetadata {
    float maxMasteringLuminanceNits = 0.0f;
    float minMasteringLuminanceNits = 0.0f;
    float maxContentLightLevelNits = 0.0f;
    float maxFrameAverageLightLevelNits = 0.0f;
    std::array<float, 8> displayPrimaries{};
    std::array<float, 2> whitePoint{};
    bool HasLuminanceRange() const noexcept;
    bool operator==(const HdrMetadata&) const noexcept = default;
};

struct ColorDescription {
    DXGI_COLOR_SPACE_TYPE dxgiColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    HdrColorPrimaries primaries = HdrColorPrimaries::Unknown;
    HdrTransferFunction transfer = HdrTransferFunction::Unknown;
    HdrColorRange range = HdrColorRange::Unknown;
    // Canonical scRGB reference white. Windows scRGB defines 1.0 as 80 nit.
    float referenceWhiteNits = 80.0f;
    // Display SDR white level used when SDR content is embedded in HDR.
    float sdrWhiteNits = 80.0f;
    float displayPeakNits = 1000.0f;
    bool isSceneReferred = false;
    bool isPreExposed = false;
    bool isInferred = false;
    bool displayHdrEnabled = false;
    float preExposure = 1.0f;
    HdrMetadata metadata{};
    bool IsValid() const noexcept;
    bool operator==(const ColorDescription&) const noexcept = default;
};

struct HdrFrameMetadata {
    uint64_t frameId = 0;
    uint64_t captureSequence = 0;
    uint64_t resourceGeneration = 0;
    int64_t timestamp100ns = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    DXGI_FORMAT sourceFormat = DXGI_FORMAT_UNKNOWN;
    ColorDescription color{};
    HdrFrameStage stage = HdrFrameStage::Unknown;
    std::string routeId;
    bool generated = false;
    bool valid = false;
	uint64_t parameterRevision = 0;
	uint64_t previousFrameId = 0;
	uint32_t frameIndex = 0, frameMultiplier = 1;
	// Optional CPU observation at backend acceptance of a NEW captured frame.
	// Separate clock/provenance from timestamp100ns; not game render time and
	// never refreshed by publication retries, generated frames or UI redraws.
	int64_t captureAdmissionTime100ns = 0;
	// Effect history epoch and non-capture redraw travel with the published color.
	uint64_t outputHistoryRevision = 0;
	bool contentRedraw = false;
	FrameUpdateReason updateReason = FrameUpdateReason::NewCapture;
	// UI paper white travels with processed/published pixels. Legacy native SDR
	// mapping applies exposure; paired restoration does not. Zero is unspecified.
	float overlayReferenceWhiteNits = 0;
    bool IsValid() const noexcept;
};

struct HdrFrame {
    ID3D11Texture2D* texture = nullptr;
    HdrFrameMetadata metadata{};
    DXGI_FORMAT workingFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    bool IsCanonical() const noexcept;

    HdrFrame WithStage(HdrFrameStage newStage, std::string route = {}) const {
        HdrFrame result = *this;
        result.metadata.stage = newStage;
        result.metadata.routeId = std::move(route);
        return result;
    }
};

struct HdrAdapterProtocol {
    HdrAdapterProfile profile = HdrAdapterProfile::Unknown;
    DXGI_FORMAT inputFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT outputFormat = DXGI_FORMAT_UNKNOWN;
    HdrTransferFunction inputTransfer = HdrTransferFunction::Unknown;
    HdrTransferFunction outputTransfer = HdrTransferFunction::Unknown;
    bool preservesAlpha = true;
    bool isPresentationTerminal = false;
};

// A protocol route describes one way to carry an effect across the canonical
// HDR boundary. Routes are stored as structured data and are never selected by
// effect-name string comparisons. The route intentionally does not encode a
// color-space guess from its texture format; callers must set hdrNative and the
// transfer/range fields from verified evidence.
struct HdrFormatRoute {
    std::string effectId;
    std::string optionId;
    DXGI_FORMAT inputFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT outputFormat = DXGI_FORMAT_UNKNOWN;
    HdrTransferFunction inputTransfer = HdrTransferFunction::Unknown;
    HdrTransferFunction outputTransfer = HdrTransferFunction::Unknown;
    HdrColorRange inputRange = HdrColorRange::Unknown;
    HdrColorRange outputRange = HdrColorRange::Unknown;
    HdrAlphaMode alphaMode = HdrAlphaMode::Unknown;
    HdrEvidenceLevel evidenceLevel = HdrEvidenceLevel::None;
    bool hdrNative = false;
    HdrAdapterProfile adapterProfile = HdrAdapterProfile::Unknown;
    bool defaultForHdr = false;
    bool defaultForSdr = false;
    float normalizationScale = 1.0f;

    std::string Id() const;

    // Structural validity: a route must be identifiable and must name real
    // input/output formats plus an explicit adapter profile and alpha rule.
    // Unknown evidence routes may still be represented for diagnostics but are
    // not considered accepted HDR routes.
    bool IsValid() const noexcept;

    bool IsAccepted() const noexcept {
        return IsValid();
    }

    // True only when the route explicitly declares HDR semantics. FP16 alone
    // and R8/R10 format acceptance alone never make this flag true.
    bool IsHdrNative() const noexcept {
        return IsValid() && hdrNative;
    }

    // Routes that do not directly carry HDR semantics but are accepted through
    // an explicit bounded/SDR/conditional adapter.
    bool IsHdrAdapter() const noexcept {
        return IsValid() && !hdrNative && adapterProfile != HdrAdapterProfile::Unknown &&
            adapterProfile != HdrAdapterProfile::PresentationTerminal;
    }

    bool IsPresentationTerminal() const noexcept {
        return adapterProfile == HdrAdapterProfile::PresentationTerminal;
    }
};

}
