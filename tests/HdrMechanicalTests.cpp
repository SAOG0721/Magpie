// Lightweight unit-style validation for the HDR mechanical slice.
//
// This file is intentionally kept independent of the UI/configuration projects
// so it can be compiled as a small console executable when a test runner is
// available. It validates protocol route data and the adapter dispatcher plan.
// Configuration round-trip and source-level invariants are covered by
// scripts\Run-HdrMechanicalValidation.ps1 and by Magpie's normal build.
//
// Suggested compile shape (after MSVC environment is set up):
//   cl /std:c++17 /EHsc /I src\Magpie.Core src\Magpie.Core\HdrFrame.cpp ^
//       src\Magpie.Core\HdrProtocol.cpp src\Magpie.Core\HdrColorTransform.cpp ^
//       src\Magpie.Core\HdrAdapterDispatcher.cpp tests\HdrMechanicalTests.cpp

#include "HdrAdapterDispatcher.h"
#include "EffectProtocolCatalogC.h"
#include "HdrColorTransform.h"
#include "HdrFrame.h"
#include "HdrProtocol.h"

#include <cmath>
#include <cstdio>
#include <iterator>
#include <string>
#include <vector>

using namespace Magpie;

namespace {

int g_failures = 0;

void Check(bool condition, const char* message) {
    if (!condition) {
        std::printf("FAIL: %s\n", message);
        ++g_failures;
    } else {
        std::printf("PASS: %s\n", message);
    }
}

HdrFormatRoute MakeRoute(HdrAdapterProfile profile) {
    HdrFormatRoute route;
    route.effectId = "TestEffect";
    route.optionId = profile == HdrAdapterProfile::Unknown ? "UnknownRoute" : "Route";
    route.inputFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    route.outputFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    route.inputTransfer = HdrTransferFunction::Linear;
    route.outputTransfer = HdrTransferFunction::Linear;
    route.inputRange = HdrColorRange::SceneLinear;
    route.outputRange = HdrColorRange::SceneLinear;
    route.alphaMode = HdrAlphaMode::Preserve;
    route.evidenceLevel = HdrEvidenceLevel::LocalValidation;
    route.adapterProfile = profile;
    route.hdrNative = profile == HdrAdapterProfile::DirectFP16 ||
        profile == HdrAdapterProfile::BoundedHDR ||
        profile == HdrAdapterProfile::ConditionalFP16;
    route.defaultForHdr = profile == HdrAdapterProfile::DirectFP16;
    route.defaultForSdr = profile == HdrAdapterProfile::SDRCompatible;
    return route;
}

void TestRouteStorageAndCategories() {
    HdrFormatRoute route = MakeRoute(HdrAdapterProfile::DirectFP16);
    Check(route.Id() == "TestEffect/Route", "route id combines effectId and optionId");
    Check(route.IsValid(), "complete route is valid");
    Check(route.IsHdrNative(), "explicit hdrNative route is HDR-native");
    Check(!route.IsHdrAdapter(), "hdrNative route is not an adapter-only route");

    HdrFormatRoute r8Sdr = MakeRoute(HdrAdapterProfile::SDRCompatible);
    r8Sdr.inputFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    r8Sdr.outputFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    r8Sdr.hdrNative = false;
    Check(r8Sdr.IsHdrAdapter(), "R8 SDRCompatible route is an adapter route");
    Check(!r8Sdr.IsHdrNative(), "R8 route is not automatically HDR-native");

    HdrFormatRoute fp16WithoutFlag = MakeRoute(HdrAdapterProfile::DirectFP16);
    fp16WithoutFlag.hdrNative = false;
    Check(!fp16WithoutFlag.IsHdrNative(), "FP16 route without explicit hdrNative is not HDR-native");

    HdrFormatRoute r10 = MakeRoute(HdrAdapterProfile::BoundedHDR);
    r10.inputFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
    r10.outputFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
    r10.inputTransfer = HdrTransferFunction::PQ;
    r10.outputTransfer = HdrTransferFunction::PQ;
    r10.hdrNative = true;
    Check(r10.IsHdrNative(), "R10 route is HDR-native only when PQ/BT.2100 semantics are explicit");

    HdrFormatRoutes routes{ MakeRoute(HdrAdapterProfile::SDRCompatible), MakeRoute(HdrAdapterProfile::DirectFP16) };
    Check(SelectDefaultHdrRoute(routes) != nullptr, "HDR mode selects a default HDR route");
    Check(SelectDefaultHdrRoute(routes)->adapterProfile == HdrAdapterProfile::DirectFP16,
        "HDR mode prefers DirectFP16 when marked defaultForHdr");
    Check(SelectDefaultSdrRoute(routes)->adapterProfile == HdrAdapterProfile::SDRCompatible,
        "SDR mode keeps SDRCompatible default");
    Check(GetAcceptedFormatRoutes(routes).size() == 2, "accepted route list contains valid routes");
    Check(GetHdrNativeFormatRoutes(routes).size() == 1, "hdrNative route list is explicit only");
    Check(GetHdrAdapterFormatRoutes(routes).size() == 1, "hdrAdapter route list contains SDR route");
}

void TestDispatcherProfiles() {
    HdrAdapterDispatcher dispatcher;
    ColorDescription color{};
    color.referenceWhiteNits = 80.0f;
    color.displayPeakNits = 1000.0f;

    const char* profileNames[] = {
        "DirectFP16", "BoundedHDR", "SDRCompatible",
        "ConditionalFP16", "Unknown", "PresentationTerminal"
    };
    const HdrAdapterProfile profiles[] = {
        HdrAdapterProfile::DirectFP16, HdrAdapterProfile::BoundedHDR,
        HdrAdapterProfile::SDRCompatible, HdrAdapterProfile::ConditionalFP16,
        HdrAdapterProfile::Unknown, HdrAdapterProfile::PresentationTerminal
    };

    for (size_t i = 0; i < std::size(profiles); ++i) {
        HdrFormatRoute route = MakeRoute(profiles[i]);
        const HdrAdapterPlan plan = dispatcher.BuildPlan(route, color);
        Check(plan.profile == profiles[i], profileNames[i]);
    }

    HdrFormatRoute direct = MakeRoute(HdrAdapterProfile::DirectFP16);
    HdrAdapterPlan plan = dispatcher.BuildPlan(direct, color);
    Check(plan.IsNonTerminalCanonical(), "DirectFP16 returns canonical FP16");
    Check(!plan.requiresSdrMapping && !plan.requiresBoundedMapping,
        "DirectFP16 does not introduce SDR mapping");

    HdrFormatRoute sdr = MakeRoute(HdrAdapterProfile::SDRCompatible);
    plan = dispatcher.BuildPlan(sdr, color);
    Check(plan.requiresSdrMapping, "SDRCompatible requires HDR-to-SDR mapping");
    Check(plan.forwardParameters.IsValid() && plan.inverseParameters.IsValid(),
        "SDRCompatible carries paired forward/inverse parameters");

    HdrFormatRoute bounded = MakeRoute(HdrAdapterProfile::BoundedHDR);
    plan = dispatcher.BuildPlan(bounded, color);
    Check(plan.requiresBoundedMapping, "BoundedHDR requires bounded encode/decode");
    Check(plan.forwardParameters.IsValid() && plan.inverseParameters.IsValid(),
        "BoundedHDR carries paired forward/inverse parameters");

    HdrFormatRoute unknown = MakeRoute(HdrAdapterProfile::Unknown);
    unknown.alphaMode = HdrAlphaMode::Unknown;
    plan = dispatcher.BuildPlan(unknown, color);
    Check(plan.usesFallback, "Unknown route selects fallback");
    Check(!plan.fallbackReason.empty(), "Unknown route records fallback reason");
    Check(plan.alphaMode == HdrAlphaMode::ForceOpaque, "Unknown fallback makes alpha explicit");

    HdrFormatRoute terminal = MakeRoute(HdrAdapterProfile::PresentationTerminal);
    plan = dispatcher.BuildPlan(terminal, color);
    Check(plan.isPresentationTerminal, "PresentationTerminal is terminal");
    Check(plan.canonicalOutputFormat == DXGI_FORMAT_UNKNOWN,
        "PresentationTerminal does not promise a normal canonical output");
}

void TestHdrToneMapRoundTrip() {
    HdrTransformParameters parameters{};
    parameters.sdrWhiteNits = 80.0f;
    parameters.hdrPeakNits = 1000.0f;
    parameters.shoulder = 1.0f;

    // The replicate contract is exact below the white point and collapses
    // everything above it to saturation; the white point itself maps to the
    // frame peak on the way back by design.
    const float samples[] = { 0.0f, 0.25f, 0.5f, 0.75f, 0.999f };
    float previous = -1.0f;
    for (float sample : samples) {
        const float mapped = HdrColorTransform::MapHdrToSdr(sample, parameters);
        Check(mapped >= previous, "HDR-to-SDR mapping is monotonic through highlights");
        previous = mapped;
        const float reconstructed = HdrColorTransform::MapSdrToHdr(mapped, parameters);
        Check(std::abs(reconstructed - sample) <= 1e-3f,
            "HDR-to-SDR-to-HDR round trip preserves in-range luminance");
    }

    parameters.exposure = 0.5f;
    const float mappedPeak = HdrColorTransform::MapHdrToSdr(12.5f, parameters);
    const float reconstructedPeak = HdrColorTransform::MapSdrToHdr(mappedPeak, parameters);
    Check(reconstructedPeak >= 11.0f,
        "saturated peak restores to the frame peak with exposure");
}

void TestShoulderCurveFamily() {
    HdrTransformParameters parameters{};
    parameters.sdrWhiteNits = 80.0f;
    parameters.hdrPeakNits = 1000.0f;
    parameters.exposure = 1.0f;

    // Identity below the SDR white point keeps SDR content bit-exact for
    // both the replicate contract (SDR routes) and the bounded shoulder.
    for (int i = 0; i <= 100; ++i) {
        const float x = static_cast<float>(i) / 100.0f;
        Check(HdrColorTransform::MapHdrToSdr(x, parameters) == x,
            "SDR-route mapping is the identity on [0, 1]");
        Check(HdrColorTransform::EncodeBoundedHdr(x, parameters) == x,
            "bounded mapping is the identity on [0, 1]");
    }

    // The replicate contract saturates above the white point and restores
    // saturated whites to the frame peak on the way back.
    Check(HdrColorTransform::MapHdrToSdr(5.0f, parameters) == 1.0f,
        "SDR-route mapping saturates HDR highlights");
    Check(HdrColorTransform::MapSdrToHdr(1.0f, parameters) >= 12.0f,
        "SDR-route inverse restores saturated whites to the frame peak");
    // A 360-nit display (sdrWhite == peak) must restore saturated whites to
    // the white point itself, never above it: the earlier formula produced
    // (peak/80)*(sdrWhite/80) = 20.25 canonical (1620 nit) and leaked yellow.
    {
        HdrTransformParameters display{};
        display.sdrWhiteNits = 360.0f;
        display.hdrPeakNits = 360.0f;
        Check(HdrColorTransform::MapSdrToHdr(1.0f, display) <= 360.0f / 80.0f + 1e-4f,
            "saturated white restores at or below the display white point");
    }
    const float roundTrip = HdrColorTransform::MapSdrToHdr(
        HdrColorTransform::MapHdrToSdr(0.37f, parameters), parameters);
    Check(std::abs(roundTrip - 0.37f) <= 1e-5f,
        "SDR-route round trip is exact below the white point");

    // Strict monotonicity through the shoulder, the anchor and the linear
    // tail of the bounded curve.
    const auto curve = HdrColorTransform::BuildShoulderCurve(
        parameters, HdrColorTransform::BoundedRouteHighlightTarget);
    const float step = curve.peak / 400.0f;
    float previous = -1.0f;
    for (int i = 0; i <= 900; ++i) {
        const float x = static_cast<float>(i) * step;
        const float y = HdrColorTransform::ApplyShoulderCurve(x, curve);
        Check(y > previous, "anchored shoulder is strictly increasing");
        previous = y;
    }
    // Anchor and paired inverse, including points on the linear tail.
    const float anchor = HdrColorTransform::ApplyShoulderCurve(curve.peak, curve);
    Check(std::abs(anchor - curve.target) <= 1e-4f,
        "shoulder maps the peak to its design target");
    const float tailPoints[] = { 0.0f, 0.5f, 1.0f, curve.peak, curve.peak * 1.5f };
    for (float x : tailPoints) {
        const float y = HdrColorTransform::ApplyShoulderCurve(x, curve);
        const float restored = HdrColorTransform::InvertShoulderCurve(y, curve);
        Check(std::abs(restored - x) <= 1e-3f * std::max(1.0f, x),
            "shoulder inverse restores the original value");
    }

    // Bounded target keeps the DLSSNR model input in the validated band.
    Check(HdrColorTransform::BoundedRouteHighlightTarget >= 2.0f &&
        HdrColorTransform::BoundedRouteHighlightTarget <= 3.0f,
        "bounded route target stays in the experimentally validated band");

    // Degenerate monitors report peak <= SDR white; the curve must degrade
    // to the plain identity instead of producing NaN (observed live: a
    // 80-nit MaxLuminance report with a 360-nit SDR white).
    {
        HdrTransformParameters degenerate{};
        degenerate.sdrWhiteNits = 360.0f;
        degenerate.hdrPeakNits = 80.0f;
        const auto identity = HdrColorTransform::BuildShoulderCurve(
            degenerate, HdrColorTransform::BoundedRouteHighlightTarget);
        Check(identity.peak == 1.0f && identity.k == 0.0f &&
            identity.tailSlope == 1.0f && !std::isnan(identity.k) &&
            !std::isnan(identity.tailSlope),
            "degenerate peak degrades to the identity curve without NaN");
        for (float x : { 0.0f, 1.0f, 4.5f, 12.5f }) {
            Check(HdrColorTransform::ApplyShoulderCurve(x, identity) == x &&
                HdrColorTransform::InvertShoulderCurve(x, identity) == x,
                "degenerate curve is the identity for every input");
        }
    }

    // Bounded route round trip through the DLSSNR band.
    const float boundedSamples[] = { 0.0f, 0.25f, 1.0f, 2.0f, 4.0f, 12.5f };
    for (float x : boundedSamples) {
        const float encoded = HdrColorTransform::EncodeBoundedHdr(x, parameters);
        Check(encoded <= HdrColorTransform::BoundedRouteHighlightTarget + 1e-3f,
            "bounded encoding keeps the model input inside the band");
        const float decoded = HdrColorTransform::DecodeBoundedHdr(encoded, parameters);
        Check(std::abs(decoded - x) <= 1e-3f * std::max(1.0f, x),
            "bounded encode/decode round trip restores the canonical value");
    }
}

void TestPresentationTerminalSelection() {
    HdrFormatRoute terminal = MakeRoute(HdrAdapterProfile::PresentationTerminal);
    terminal.defaultForHdr = true;
    const HdrFormatRoutes routes{ terminal };
    Check(SelectDefaultHdrRoute(routes) == &routes.front(),
        "HDR mode selects an explicitly default presentation terminal");
    Check(SelectDefaultSdrRoute(routes) == nullptr,
        "SDR mode does not select a presentation terminal");

    const HdrFormatRoutes fgRoutes[] = {
        EffectProtocolC::DLSSFG(),
        EffectProtocolC::XeSSFG(),
        EffectProtocolC::FSR3FG()
    };
    for (const HdrFormatRoutes& markerRoutes : fgRoutes) {
        const HdrFormatRoute* selected = SelectDefaultHdrRoute(markerRoutes);
        Check(selected && selected->IsPresentationTerminal(),
            "FG marker selects its presentation-terminal route in HDR mode");
        Check(selected && selected->inputFormat == DXGI_FORMAT_R16G16B16A16_FLOAT &&
            selected->outputFormat == DXGI_FORMAT_R16G16B16A16_FLOAT,
            "FG marker preserves the canonical FP16 working surface");
    }
}

}

int main() {
    TestRouteStorageAndCategories();
    TestDispatcherProfiles();
    TestHdrToneMapRoundTrip();
    TestShoulderCurveFamily();
    TestPresentationTerminalSelection();

    if (g_failures == 0) {
        std::printf("All HDR mechanical tests passed.\n");
        return 0;
    }

    std::printf("%d HDR mechanical test(s) failed.\n", g_failures);
    return 1;
}
