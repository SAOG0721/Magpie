#include "pch.h"
#include "NgxRuntimeGuard.h"
#include "DLSSNRFilter.h"
#include "DLSSNRColorShader.h"
#include "DLSSNRDetailShader.h"
#include "DLSSNRDetailParameters.h"
#include "DLSSNRParameters.h"
#include "DLSSNRChainCache.h"
#include "DLSSNRPerformanceState.h"
#include "DLSSNRSparseState.h"
#include "DLSSNRSparseShader.h"
#include "DLSSNREnhancementPool.h"
#include "DLSSNRSubmissionWorker.h"
#include "DLSSNRGpuTimer.h"
#include "DLSSNRPerformanceShader.h"
#include "DLSSNRTemporal.h"
#include "FrameTrace.h"
#include "DeviceResources.h"
#include "DirectXHelper.h"
#include "Logger.h"
#include "NgxD3D12Core.h"
#include "Win32Helper.h"
#include "FrameGuidanceD3D12Interop.h"
#include "FrameGuidancePerformance.h"
#include "NativeBackendTiming.h"
#include "OpticalFlowSettings.h"
#include "StrHelper.h"
#include <winver.h>
#pragma comment(lib, "version.lib")

namespace Magpie {

DLSSNRSettings ParseDLSSNRSettings(const EffectOption& option, bool hdrEnabled) noexcept {
	auto getParameter = [&](std::string_view name, float defaultValue) noexcept {
		auto it = option.parameters.find(std::string(name));
		return it != option.parameters.end() && std::isfinite(it->second)
			? it->second : defaultValue;
	};
	auto getClamped = [&](std::string_view name, float defaultValue,
		float minimum, float maximum) noexcept {
		return std::clamp(getParameter(name, defaultValue), minimum, maximum);
	};
	// The caller supplies the resolved automatic boundary, never a saved UI choice.
	return DLSSNRSettings{
		.modelPreset = static_cast<int>(getClamped("modelPreset", 0.f, 0.f, 2.f)),
		.processingMode = DLSSNRMode(getParameter),
		.residualTransfer = getParameter("residualTransfer", 0.f) == 1.f ?
			DLSSNRResidualTransfer::PerPixel : DLSSNRResidualTransfer::InPlace,
		.enableInputResolutionScaling = true,
		.inputResolutionPercent = static_cast<uint32_t>(std::clamp(
			static_cast<int>(std::lround(
				getClamped("inputResolutionPercent", 100.0f, 25.f, 100.f))), 25, 100)),
		.sparseUpdateInterval = DLSSNRSparseUpdateInterval(getParameter),
		.residualMultiplier = getClamped("residualMultiplier", 1.0f, 0.0f, 2.0f),
		.residualSaturation = getClamped("residualSaturation", 1.0f, 0.0f, 2.0f),
		.residualLightness = getClamped("residualLightness", 1.0f, 0.0f, 2.0f),
		.shadowStructureMultiplier = getClamped(
			"shadowStructureMultiplier", 1.0f, 0.0f, 2.0f),
		.residualMidtoneStrength = getClamped("residualMidtoneStrength", 1.f, 0.f, 2.f),
		.reflectionGlowMultiplier = getClamped(
			"reflectionGlowMultiplier", 1.0f, 0.0f, 2.0f),
		.residualHueProtection = getClamped("residualHueProtection", 0.f, 0.f, 1.f),
		.residualChromaProtection = getClamped("residualChromaProtection", 0.f, 0.f, 1.f),
		.residualDarkProtection = getClamped("residualDarkProtection", 0.f, 0.f, 1.f),
		.residualHighlightProtection = getClamped("residualHighlightProtection", 0.f, 0.f, 1.f),
		.residualLocalCompression = getClamped("residualLocalCompression", 0.f, 0.f, 1.f),
		.residualLowFrequencyGain = getClamped("residualLowFrequencyGain", 1.f, 0.f, 2.f),
		.residualDetailGain = getClamped("residualDetailGain", 1.f, 0.f, 2.f),
		.residualChromaTemporalStrength = 0.f,
		.residualDebugView = static_cast<int>(std::lround(getClamped("residualDebugView", 0.f, 0.f, 7.f))),
		.style = std::clamp(static_cast<int>(std::lround(
			getParameter("style", 0.0f))), 0, 2),
		.intensity = getClamped("intensity", 1.0f, 0.0f, 2.0f),
		.localToneStrength = getClamped("localToneStrength", 1.0f, 0.0f, 2.0f),
		.localStructureStrength = getClamped(
			"localStructureStrength", 1.0f, 0.0f, 2.0f),
		.skinStructureStrength = getClamped(
			"skinStructureStrength", 0.0f, 0.0f, 2.0f),
		.useAutoMask = getParameter("useAutoMask", 0.0f) >= 0.5f,
		.uiCorrection = getParameter("uiCorrection", 0.0f) >= 0.5f,
		.motionRequest = ParseDlssOpticalFlowRequest(option),
		.experimentalHdr = DlssnrExperimentProtocol{ .enabled = hdrEnabled, .scale = 1.0f }
	};
}

}

#ifdef MP_ENABLE_DLSSNR
#include <d3d12.h>
#include <nvsdk_ngx.h>
#include <atomic>
#include <map>

namespace Magpie {

namespace {

void LogDlssnrStatus(std::string message, bool error = false) noexcept {
	if (error) {
		Logger::Get().Error(message);
	} else {
		Logger::Get().Info(message);
	}
	message.push_back('\n');
	OutputDebugStringA(message.c_str());
}

float ClampFinite(
	float value,
	float minimum,
	float maximum,
	float fallback
) noexcept {
	return std::isfinite(value) ? std::clamp(value, minimum, maximum) : fallback;
}

constexpr NVSDK_NGX_Feature FEATURE_DLSSNR =
	static_cast<NVSDK_NGX_Feature>(18);
constexpr unsigned long long DLSSNR_SIGNED_SNIPPET_APPLICATION_ID = 0x0876232Cull;
// Keep the unsupported Core Feature 18 route as an explicit diagnostic only.
// It must never run before the signed snippet in a production session.
constexpr bool ENABLE_CORE_FEATURE18_DIAGNOSTIC = false;

constexpr char PARAM_WIDTH[] = "DLSSNR.Width";
constexpr char PARAM_HEIGHT[] = "DLSSNR.Height";
constexpr char PARAM_INPUT_WIDTH[] = "DLSSNR.InputWidth";
constexpr char PARAM_INPUT_HEIGHT[] = "DLSSNR.InputHeight";
constexpr char PARAM_OUTPUT_WIDTH[] = "DLSSNR.OutputWidth";
constexpr char PARAM_OUTPUT_HEIGHT[] = "DLSSNR.OutputHeight";
constexpr char PARAM_OUTPUT_DOT_WIDTH[] = "DLSSNR.Output.Width";
constexpr char PARAM_OUTPUT_DOT_HEIGHT[] = "DLSSNR.Output.Height";
constexpr char PARAM_UPSCALING[] = "DLSSNR.Upscaling";
constexpr char PARAM_SCALE[] = "DLSSNR.Scale";
constexpr char PARAM_SCALING_RATIO[] = "DLSSNR.ScalingRatio";
constexpr char PARAM_SCALING_RATIO_CALLBACK[] = "DLSSNRComputeScalingRatioCallback";
constexpr char PARAM_PRESET[] = "DLSSNR.Hint.Render.Preset";
constexpr char PARAM_COLOR[] = "DLSSNR.Color";
constexpr char PARAM_OUTPUT[] = "DLSSNR.Output";
constexpr char PARAM_MVEC[] = "DLSSNR.MVec";
constexpr char PARAM_DEPTH[] = "DLSSNR.Depth";
constexpr char PARAM_MVEC_SCALE_X[] = "DLSSNR.MVecScaleX";
constexpr char PARAM_MVEC_SCALE_Y[] = "DLSSNR.MVecScaleY";
constexpr char PARAM_DEPTH_INVERTED[] = "DLSSNR.DepthInverted";
constexpr char PARAM_ENABLED[] = "DLSSNR.Enabled";
constexpr char PARAM_RESET[] = "DLSSNR.Reset";
constexpr char PARAM_STYLE[] = "DLSSNR.Style";
constexpr char PARAM_INTENSITY[] = "DLSSNR.Intensity";
constexpr char PARAM_LOCAL_TONE[] = "DLSSNR.LocalToneStrength";
constexpr char PARAM_LOCAL_STRUCTURE[] = "DLSSNR.LocalStructureStrength";
constexpr char PARAM_SKIN_STRUCTURE[] = "DLSSNR.SkinStructureStrength";
constexpr char PARAM_AUTO_MASK[] = "DLSSNR.UseAutoMask";
constexpr char PARAM_UI_CORRECTION[] = "DLSSNR.UICorrection";
constexpr char PARAM_INDICATOR_INVERT_X[] = "DLSS.Indicator.Invert.X.Axis";
constexpr char PARAM_INDICATOR_INVERT_Y[] = "DLSS.Indicator.Invert.Y.Axis";

struct ResourceParameters {
	const char* baseX;
	const char* baseY;
	const char* width;
	const char* height;
};

constexpr ResourceParameters RESOURCE_PARAMETERS[]{
	{ "DLSSNR.ColorSubrectBaseX", "DLSSNR.ColorSubrectBaseY",
		"DLSSNR.ColorSubrectWidth", "DLSSNR.ColorSubrectHeight" },
	{ "DLSSNR.OutputSubrectBaseX", "DLSSNR.OutputSubrectBaseY",
		"DLSSNR.OutputSubrectWidth", "DLSSNR.OutputSubrectHeight" },
	{ "DLSSNR.MVecSubrectBaseX", "DLSSNR.MVecSubrectBaseY",
		"DLSSNR.MVecSubrectWidth", "DLSSNR.MVecSubrectHeight" },
	{ "DLSSNR.DepthSubrectBaseX", "DLSSNR.DepthSubrectBaseY",
		"DLSSNR.DepthSubrectWidth", "DLSSNR.DepthSubrectHeight" }
};

constexpr char COLOR_CONVERT_HLSL[] = R"(
Texture2D<float4> InputColor : register(t0);
RWTexture2D<float4> OutputColor : register(u0);

[numthreads(8, 8, 1)]
void ConvertToRgba(uint3 tid : SV_DispatchThreadID) {
    uint width, height;
    OutputColor.GetDimensions(width, height);
    if (tid.x >= width || tid.y >= height) return;
    OutputColor[tid.xy] = InputColor.Load(int3(tid.xy, 0));
}
)";

constexpr char COLOR_DOWNSAMPLE_HLSL[] = R"(
Texture2D<float4> InputColor : register(t0);
RWTexture2D<float4> OutputColor : register(u0);

cbuffer ResampleParams : register(b0) {
    uint2 SourceExtent;
    uint2 TargetExtent;
    uint Padding0;
    float2 MotionScale;
    float ResidualMultiplier;
    float ResidualSaturation;
    float ResidualLightness;
    float ShadowStructureMultiplier;
    float ReflectionGlowMultiplier;
    float MidtoneStrength;
    float HueProtection;
    float DarkProtection;
    float HighlightProtection;
    float LocalCompression;
    float LowFrequencyGain;
    float DetailGain;
    uint DebugView;
    float ChromaProtection;
    uint FinalizeOutput;
    uint2 ColorPadding;
};

float Sinc(float x) {
    if (abs(x) < 1e-5) return 1.0;
    x *= 3.14159265358979323846;
    return sin(x) / x;
}

float Lanczos2(float x) {
    return abs(x) < 2.0 ? Sinc(x) * Sinc(x * 0.5) : 0.0;
}

[numthreads(8, 8, 1)]
void DownsampleColorVertical(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= SourceExtent.x || tid.y >= TargetExtent.y) return;
    float scale = float(TargetExtent.y) / float(SourceExtent.y);
    float position = (float(tid.y) + 0.5) / scale - 0.5;
    float support = 2.0 / scale;
    int first = int(ceil(position - support));
    int last = int(floor(position + support));
    float4 total = 0.0;
    float totalWeight = 0.0;
    [loop]
    for (int y = first; y <= last; ++y) {
        float weight = Lanczos2((float(y) - position) * scale);
        total += InputColor.Load(int3(tid.x,
            clamp(y, 0, int(SourceExtent.y) - 1), 0)) * weight;
        totalWeight += weight;
    }
    // Keep negative lobes in the shared FP16 intermediate, including alpha.
    OutputColor[tid.xy] = total / (abs(totalWeight) > 1e-6 ? totalWeight : 1.0);
}

[numthreads(8, 8, 1)]
void DownsampleColorHorizontal(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= TargetExtent)) return;
    float scale = float(TargetExtent.x) / float(SourceExtent.x);
    float position = (float(tid.x) + 0.5) / scale - 0.5;
    float support = 2.0 / scale;
    int first = int(ceil(position - support));
    int last = int(floor(position + support));
    float4 total = 0.0;
    float totalWeight = 0.0;
    [loop]
    for (int x = first; x <= last; ++x) {
        float weight = Lanczos2((float(x) - position) * scale);
        total += InputColor.Load(int3(
            clamp(x, 0, int(SourceExtent.x) - 1), tid.y, 0)) * weight;
        totalWeight += weight;
    }
    OutputColor[tid.xy] = total / (abs(totalWeight) > 1e-6 ? totalWeight : 1.0);
}
)";

constexpr char GUIDANCE_DOWNSAMPLE_HLSL[] = R"(
Texture2D<float2> InputMotion : register(t0);
Texture2D<float> InputDepth : register(t1);
Texture2D<float> InputConfidence : register(t2);
RWTexture2D<float2> OutputMotion : register(u0);
RWTexture2D<float> OutputDepth : register(u1);
RWTexture2D<float> OutputConfidence : register(u2);

cbuffer ResampleParams : register(b0) {
    uint2 SourceExtent;
    uint2 TargetExtent;
    uint Padding0;
    float2 MotionScale;
    float ResidualMultiplier;
    float ResidualSaturation;
    float ResidualLightness;
    float ShadowStructureMultiplier;
    float ReflectionGlowMultiplier;
    float MidtoneStrength;
    float HueProtection;
    float DarkProtection;
    float HighlightProtection;
    float LocalCompression;
    float LowFrequencyGain;
    float DetailGain;
    uint DebugView;
    float ChromaProtection;
    uint FinalizeOutput;
    uint2 ColorPadding;
};

[numthreads(8, 8, 1)]
void DownsampleGuidance(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= TargetExtent)) return;
    float2 sourceStart = float2(tid.xy) * float2(SourceExtent) /
        float2(TargetExtent);
    float2 sourceEnd = float2(tid.xy + 1) * float2(SourceExtent) /
        float2(TargetExtent);
    int2 first = int2(floor(sourceStart));
    int2 last = int2(ceil(sourceEnd));
    float2 motionTotal = 0.0;
    float2 weightedMotionTotal = 0.0;
    float confidenceTotal = 0.0;
    float totalWeight = 0.0;
    float closestDepth = 0.0;
    [loop]
    for (int y = first.y; y < last.y; ++y) {
        float weightY = max(0.0, min(sourceEnd.y, float(y + 1)) -
            max(sourceStart.y, float(y)));
        [loop]
        for (int x = first.x; x < last.x; ++x) {
            float weightX = max(0.0, min(sourceEnd.x, float(x + 1)) -
                max(sourceStart.x, float(x)));
            float weight = weightX * weightY;
            int2 sourcePixel = clamp(
                int2(x, y), int2(0, 0), int2(SourceExtent) - 1);
            float2 motion = InputMotion.Load(int3(sourcePixel, 0));
            float confidence = InputConfidence.Load(int3(sourcePixel, 0));
            motionTotal += motion * weight;
            weightedMotionTotal += motion * confidence * weight;
            confidenceTotal += confidence * weight;
            totalWeight += weight;
            // Relative inverse depth is conservative when the nearest
            // (largest) value is retained across the source footprint.
            closestDepth = max(
                closestDepth, InputDepth.Load(int3(sourcePixel, 0)));
        }
    }
    float2 motion = confidenceTotal > 1e-6 ?
        weightedMotionTotal / confidenceTotal :
        motionTotal / max(totalWeight, 1e-6);
    OutputMotion[tid.xy] = motion * MotionScale;
    OutputDepth[tid.xy] = closestDepth;
    OutputConfidence[tid.xy] = confidenceTotal / max(totalWeight, 1e-6);
}
)";

constexpr char RESIDUAL_PREPARE_HLSL[] = R"(
Texture2D<float4> ReducedColor : register(t0);
Texture2D<float4> ReducedDenoised : register(t1);
RWTexture2D<float4> ControlledResidual : register(u0);

cbuffer ResampleParams : register(b0) {
    uint2 SourceExtent;
    uint2 TargetExtent;
    uint Padding0;
    float2 MotionScale;
    float ResidualMultiplier;
    float ResidualSaturation;
    float ResidualLightness;
    float ShadowStructureMultiplier;
    float ReflectionGlowMultiplier;
    float MidtoneStrength;
    float HueProtection;
    float DarkProtection;
    float HighlightProtection;
    float LocalCompression;
    float LowFrequencyGain;
    float DetailGain;
    uint DebugView;
    float ChromaProtection;
    uint FinalizeOutput;
    uint2 ColorPadding;
};
)";

constexpr char RESIDUAL_HORIZONTAL_HLSL[] = R"(
Texture2D<float4> ControlledResidual : register(t0);
RWTexture2D<float4> HorizontalResidual : register(u0);

cbuffer ResampleParams : register(b0) {
    uint2 SourceExtent;
    uint2 TargetExtent;
    uint Padding0;
    float2 MotionScale;
    float ResidualMultiplier;
    float ResidualSaturation;
    float ResidualLightness;
    float ShadowStructureMultiplier;
    float ReflectionGlowMultiplier;
    float MidtoneStrength;
    float HueProtection;
    float DarkProtection;
    float HighlightProtection;
    float LocalCompression;
    float LowFrequencyGain;
    float DetailGain;
    uint DebugView;
    float ChromaProtection;
    uint FinalizeOutput;
    uint2 ColorPadding;
};

float CatmullRom(float x) {
    x = abs(x);
    if (x < 1.0) return ((1.5 * x - 2.5) * x) * x + 1.0;
    if (x < 2.0) return ((-0.5 * x + 2.5) * x - 4.0) * x + 2.0;
    return 0.0;
}

[numthreads(8, 8, 1)]
void UpsampleResidualHorizontal(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= SourceExtent.x || tid.y >= TargetExtent.y) return;
    if (SourceExtent.x == TargetExtent.x) {
        HorizontalResidual[tid.xy] =
            ControlledResidual.Load(int3(tid.xy, 0));
        return;
    }
    float reducedPosition = (float(tid.x) + 0.5) *
        float(TargetExtent.x) / float(SourceExtent.x) - 0.5;
    int center = int(floor(reducedPosition));
    float3 residual = 0.0;
    float totalWeight = 0.0;
    [unroll]
    for (int x = -1; x <= 2; ++x) {
        float weight = CatmullRom(reducedPosition - float(center + x));
        int sampleX = clamp(center + x, 0, int(TargetExtent.x) - 1);
        int3 samplePixel = int3(sampleX, tid.y, 0);
        residual += ControlledResidual.Load(samplePixel).rgb * weight;
        totalWeight += weight;
    }
    residual /= abs(totalWeight) > 1e-6 ? totalWeight : 1.0;
    HorizontalResidual[tid.xy] = float4(residual, 0.0);
}
)";

constexpr char RESIDUAL_VERTICAL_COMPOSITE_HLSL[] = R"(
Texture2D<float4> OriginalColor : register(t0);
Texture2D<float4> HorizontalResidual : register(t1);
RWTexture2D<float4> OutputColor : register(u0);

cbuffer ResampleParams : register(b0) {
    uint2 SourceExtent;
    uint2 TargetExtent;
    uint Padding0;
    float2 MotionScale;
    float ResidualMultiplier;
    float ResidualSaturation;
    float ResidualLightness;
    float ShadowStructureMultiplier;
    float ReflectionGlowMultiplier;
    float MidtoneStrength;
    float HueProtection;
    float DarkProtection;
    float HighlightProtection;
    float LocalCompression;
    float LowFrequencyGain;
    float DetailGain;
    uint DebugView;
    float ChromaProtection;
    uint FinalizeOutput;
    uint2 ColorPadding;
};

float CatmullRom(float x) {
    x = abs(x);
    if (x < 1.0) return ((1.5 * x - 2.5) * x) * x + 1.0;
    if (x < 2.0) return ((-0.5 * x + 2.5) * x - 4.0) * x + 2.0;
    return 0.0;
}

float3 CompositeColor(uint2 pixel, out float4 storedOriginal) {
    storedOriginal = OriginalColor.Load(int3(pixel, 0));
    // A typed BGRA SRV already returns logical RGBA components.
    float3 original = storedOriginal.rgb;
    float3 residual = 0.0;
    if (SourceExtent.y == TargetExtent.y) {
        residual = HorizontalResidual.Load(int3(pixel, 0)).rgb;
    } else {
        float reducedPosition = (float(pixel.y) + 0.5) *
            float(TargetExtent.y) / float(SourceExtent.y) - 0.5;
        int center = int(floor(reducedPosition));
        residual = 0.0;
        float totalWeight = 0.0;
        [unroll]
        for (int y = -1; y <= 2; ++y) {
            float weight = CatmullRom(reducedPosition - float(center + y));
            int sampleY = clamp(center + y, 0, int(TargetExtent.y) - 1);
            residual += HorizontalResidual.Load(
                int3(pixel.x, sampleY, 0)).rgb * weight;
            totalWeight += weight;
        }
        residual /= abs(totalWeight) > 1e-6 ? totalWeight : 1.0;
    }
    float3 color = DebugView != 0 ? saturate(residual) : original + residual;
    if (DebugView == 6) {
        float gamutScale;
        color = FinalizeSDRColor(original+residual,gamutScale);
        color = gamutScale;
    }
    if (FinalizeOutput && DebugView == 0) color = FinalizeSDRColor(color);
    return color;
}
[numthreads(8, 8, 1)]
void CompositeResidualVertical(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= SourceExtent)) return;
    float4 original;
    float3 color = CompositeColor(tid.xy, original);
    OutputColor[tid.xy] = float4(color, original.a);
}
[numthreads(8, 8, 1)]
void ExtractControlledResidualVertical(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= SourceExtent)) return;
    float4 original;
    float3 color = CompositeColor(tid.xy, original);
    // Sparse used an FP16 full-colour intermediate before subtracting the
    // original. Keep that quantization, including debug and non-finite values.
    float3 storedColor = f16tof32(f32tof16(color));
    OutputColor[tid.xy] = float4(storedColor - original.rgb, 0);
}
)";

inline const std::string RESIDUAL_PREPARE_SHADER = std::string(DLSSNR_COLOR_HLSL) +
	RESIDUAL_PREPARE_HLSL + std::string(DLSSNR_DETAIL_HLSL);
inline const std::string RESIDUAL_VERTICAL_COMPOSITE_SHADER = std::string(DLSSNR_COLOR_HLSL) + RESIDUAL_VERTICAL_COMPOSITE_HLSL;

struct ResampleConstants {
	uint32_t sourceWidth = 0;
	uint32_t sourceHeight = 0;
	uint32_t targetWidth = 0;
	uint32_t targetHeight = 0;
	uint32_t padding0 = 0;
	float motionScaleX = 1.0f;
	float motionScaleY = 1.0f;
	float residualMultiplier = 1.0f;
	float residualSaturation = 1.0f;
	float residualLightness = 1.0f;
	float shadowStructureMultiplier = 1.0f;
	float reflectionGlowMultiplier = 1.0f;
	float midtoneStrength = 1.0f;
	float hueProtection = 0;
	float darkProtection = 0;
	float highlightProtection = 0;
	float localCompression = 0;
	float lowFrequencyGain = 1;
	float detailGain = 1;
	uint32_t debugView = 0;
	float chromaProtection = 0;
	uint32_t finalizeOutput = 1;
	uint32_t padding1 = 0, padding2 = 0;
	bool operator==(const ResampleConstants&) const = default;
};
static_assert(sizeof(ResampleConstants) == 96);

bool NGXSucceeded(NVSDK_NGX_Result result) noexcept {
	return NVSDK_NGX_SUCCEED(result);
}

struct TimingSummary {
	double average = 0.0;
	double p95 = 0.0;
	double p99 = 0.0;
	double maximum = 0.0;
	size_t count = 0;
};

struct TimingWindow {
	static constexpr size_t CAPACITY = 120;
	std::array<double, CAPACITY> values{};
	size_t count = 0;
	size_t next = 0;

	void Add(double value) noexcept {
		values[next] = value;
		next = (next + 1) % CAPACITY;
		count = std::min(count + 1, CAPACITY);
	}

	TimingSummary Summarize() const noexcept {
		TimingSummary result{ .count = count };
		if (!count) return result;
		std::array<double, CAPACITY> sorted{};
		std::copy_n(values.begin(), count, sorted.begin());
		std::sort(sorted.begin(), sorted.begin() + count);
		double total = 0.0;
		for (size_t i = 0; i < count; ++i) total += sorted[i];
		result.average = total / static_cast<double>(count);
		result.p95 = sorted[std::min(count - 1, (count * 95 + 99) / 100 - 1)];
		result.p99 = sorted[std::min(count - 1, (count * 99 + 99) / 100 - 1)];
		result.maximum = sorted[count - 1];
		return result;
	}
};

template <typename T>
T GetExport(HMODULE module, const char* name) noexcept {
	return reinterpret_cast<T>(GetProcAddress(module, name));
}

}

struct DLSSNRFilter::Impl {
	DlssNrFailure failure;
	std::string libraryInfo;
	static constexpr uint32_t COMMAND_SLOT_COUNT = 4;
	static constexpr uint32_t TIMESTAMP_STRIDE = 8; // chain plus three pass pairs
	struct ShaderSet {
		std::map<std::string, winrt::com_ptr<ID3D11ComputeShader>> shaders;
	};
	std::shared_ptr<ShaderSet> shaders;
	std::vector<std::unique_ptr<Impl>> laterPasses;
	DLSSNRChainCache cache;
	uint64_t cachedInputHistoryRevision = 0;
	FrameGuidanceView cachedGuidance{};
	FrameGuidanceView preparedGuidance{};
	bool guidancePrepared = false;
	bool residualConstantsValid = false;
	ResampleConstants cachedResidualConstants{};
	winrt::com_ptr<ID3D11Buffer> residualConstants11;
	struct CommandSlot {
		winrt::com_ptr<ID3D12CommandAllocator> allocator;
		winrt::com_ptr<ID3D12GraphicsCommandList> commandList;
		uint64_t completionValue = 0;
		uint32_t timestampQuery = 0;
		FrameGuidanceFrameId timestampFrameId = 0;
		bool timestampPending = false;
		uint32_t timestampFirstPass = 0;
		uint32_t timestampPassCount = 0;
	};

	using SnippetInitExtFn = NVSDK_NGX_Result(NVSDK_CONV*)(
		unsigned long long, const wchar_t*, ID3D12Device*, NVSDK_NGX_Version,
		const NVSDK_NGX_Parameter*);
	using CreateFeatureFn = NVSDK_NGX_Result(NVSDK_CONV*)(
		ID3D12GraphicsCommandList*, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*,
		NVSDK_NGX_Handle**);
	using EvaluateFeatureFn = NVSDK_NGX_Result(NVSDK_CONV*)(
		ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*,
		const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback);
	using ReleaseFeatureFn = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Handle*);
	using ShutdownFn = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12Device*);
	using GetModuleFileNameWFn = DWORD(WINAPI*)(HMODULE, LPWSTR, DWORD);

	~Impl();

	ID3D11Device5* device11 = nullptr;
	ID3D11DeviceContext4* context11 = nullptr;
	NgxD3D12Core* coreOwner = nullptr;
	winrt::com_ptr<ID3D12Device> device12;
	winrt::com_ptr<ID3D12CommandQueue> queue12;
	winrt::com_ptr<ID3D12CommandAllocator> allocator12;
	winrt::com_ptr<ID3D12GraphicsCommandList> commandList12;
	std::array<CommandSlot, COMMAND_SLOT_COUNT> commandSlots;
	uint32_t nextCommandSlot = 0;
	winrt::com_ptr<ID3D11Texture2D> sharedInput11;
	winrt::com_ptr<ID3D11Texture2D> sharedOutput11;
	winrt::com_ptr<ID3D11ShaderResourceView> inputSrv11;
	ID3D11Texture2D* inputViewTexture = nullptr; // owned by inputSrv11
	winrt::com_ptr<ID3D11ShaderResourceView> sharedInputSrv11;
	winrt::com_ptr<ID3D11ShaderResourceView> sharedOutputSrv11;
	winrt::com_ptr<ID3D11UnorderedAccessView> sharedInputUav11;
	winrt::com_ptr<ID3D11ComputeShader> colorConvertShader11;
	winrt::com_ptr<ID3D11ComputeShader> colorDownsampleVerticalShader11;
	winrt::com_ptr<ID3D11ComputeShader> colorDownsampleHorizontalShader11;
	winrt::com_ptr<ID3D11ComputeShader> guidanceDownsampleShader11;
	winrt::com_ptr<ID3D11ComputeShader> residualPrepareShader11;
	winrt::com_ptr<ID3D11ComputeShader> residualHorizontalShader11;
	winrt::com_ptr<ID3D11ComputeShader> residualVerticalCompositeShader11;
	winrt::com_ptr<ID3D11ComputeShader> residualVerticalExtractShader11;
	winrt::com_ptr<ID3D11Buffer> resampleConstants11;
	winrt::com_ptr<ID3D11Texture2D> reducedMotion11;
	winrt::com_ptr<ID3D11Texture2D> reducedDepth11;
	winrt::com_ptr<ID3D11Texture2D> reducedConfidence11;
	winrt::com_ptr<ID3D11UnorderedAccessView> reducedMotionUav11;
	winrt::com_ptr<ID3D11UnorderedAccessView> reducedDepthUav11;
	winrt::com_ptr<ID3D11UnorderedAccessView> reducedConfidenceUav11;
	winrt::com_ptr<ID3D11ShaderResourceView> guidanceMotionSrv11;
	winrt::com_ptr<ID3D11ShaderResourceView> guidanceDepthSrv11;
	winrt::com_ptr<ID3D11ShaderResourceView> guidanceConfidenceSrv11;
	ID3D11Texture2D* guidanceMotion11 = nullptr;
	ID3D11Texture2D* guidanceDepth11 = nullptr;
	ID3D11Texture2D* guidanceConfidence11 = nullptr;
	winrt::com_ptr<ID3D11Texture2D> resampleIntermediate11;
	winrt::com_ptr<ID3D11Texture2D> controlledResidual11;
	winrt::com_ptr<ID3D11ShaderResourceView> controlledResidualSrv11;
	winrt::com_ptr<ID3D11UnorderedAccessView> controlledResidualUav11;
	winrt::com_ptr<ID3D11ShaderResourceView> resampleIntermediateSrv11;
	winrt::com_ptr<ID3D11UnorderedAccessView> resampleIntermediateUav11;
	winrt::com_ptr<ID3D11Texture2D> compositeOutput11;
	winrt::com_ptr<ID3D11UnorderedAccessView> compositeOutputUav11;
	winrt::com_ptr<ID3D12Resource> sharedInput12;
	winrt::com_ptr<ID3D12Resource> sharedOutput12;
	std::shared_ptr<FrameGuidanceD3D12Interop> guidanceInterop;
	winrt::com_ptr<ID3D11Fence> fence11;
	winrt::com_ptr<ID3D12Fence> fence12;
	wil::unique_event_nothrow fenceEvent;
	winrt::com_ptr<ID3D12QueryHeap> timestampQueryHeap;
	winrt::com_ptr<ID3D12Resource> timestampReadback;
	uint64_t timestampFrequency = 0;
	NVSDK_NGX_Parameter* parameters = nullptr;
	NVSDK_NGX_Handle* feature = nullptr;
	// Shared runtime/hook owner survives until the last feature is released.
	std::shared_ptr<Impl> snippetSession;
	// Owned by the shared signed-NR session, independent of per-feature
	// D3D11/D3D12 interop fences. Only Evaluate submissions signal this fence.
	winrt::com_ptr<ID3D12Fence> snippetEvaluateFence;
	std::mutex snippetEvaluateMutex;
	uint64_t snippetEvaluateValue = 0;
	bool snippetEvaluateFaulted = false;
	HMODULE snippetModule = nullptr;
	SnippetInitExtFn snippetInitExt = nullptr;
	CreateFeatureFn snippetCreateFeature = nullptr;
	EvaluateFeatureFn snippetEvaluateFeature = nullptr;
	ReleaseFeatureFn snippetReleaseFeature = nullptr;
	ShutdownFn snippetShutdown = nullptr;
	void** snippetGetModuleFileNameIatSlot = nullptr;
	uint64_t fenceValue = 0;
	uint64_t pendingInferenceValue = 0;
	uint64_t initializationValue = 0;
	std::chrono::steady_clock::time_point initializationSubmittedAt{};
	double initializationWaitMs = 0;
	bool initializationObserved = false;
	uint64_t evaluateCount = 0;
	uint64_t evaluateSuccessCount = 0;
	uint64_t evaluateFailureCount = 0;
	TimingWindow slotWaitTimings;
	TimingWindow inputPrepareTimings;
	TimingWindow guidancePrepareTimings;
	TimingWindow evaluateCpuTimings;
	TimingWindow submitTimings;
	TimingWindow evaluateGpuTimings;
	double lastEvaluateGpuMs = 0;
	uint64_t evaluateGpuSamples = 0;
	TimingWindow passGpuTimings;
	DLSSNRGpuTimer foregroundTimer;
	winrt::com_ptr<ID3D11Fence> retirementFence;
	uint64_t retirementValue = 0;
	FrameGuidanceFrameId lastGuidanceResetFrameId =
		std::numeric_limits<FrameGuidanceFrameId>::max();
	uint64_t evaluateParameterRevision = 0;
	uint64_t duplicateFrameReuseCount = 0;
	uint32_t sourceWidth = 0;
	uint32_t sourceHeight = 0;
	uint32_t width = 0;
	uint32_t height = 0;
	bool convertInputToRgba = false;
	bool experimentalHdrPath = false;
	float experimentalHdrScale = 1.0f;
	bool useResolutionScaling = false;
	bool coreRegistered = false;
	bool snippetInitialized = false;
	bool snippetCallerHookInstalled = false;
	bool useSignedSnippet = false;
	bool resetHistory = true;
	bool residualParametersDirty = false;
	bool finalizeOutput = true;
	int modelPreset = 0;
	bool disabled = false;
};

struct DLSSNRFilter::ResolutionChange {
	std::unique_ptr<DLSSNRFilter> full, retiredFull;
	std::unique_ptr<DLSSNRFilter> hotFull;
	std::vector<DLSSNRSettings> settings;
	std::chrono::steady_clock::time_point startedAt{};
	double preparationMs = 0;
	bool pending = false;
	bool fullChanged = false;
	bool cacheEnabled = false, warming = false, cacheHit = false;
	bool retiredFullCacheable = false;
};

struct DLSSNRFilter::Submission {
	struct Job {
		Impl* impl = nullptr;
		uint32_t slot = 0;
		size_t first = 0, count = 0;
		std::array<DLSSNRSettings, 3> settings;
		std::array<uint64_t, 3> revisions{}, evaluateNumbers{};
		std::array<bool, 3> resetHistory{}, guidanceReset{};
		FrameGuidanceView guidance{}, sourceGuidance{};
		uint64_t frame = 0, inputRevision = 0, historyRevision = 0;
		uint64_t inputReady = 0, outputReady = 0;
		HANDLE completionEvent = nullptr;
		double waitMs = 0, inputMs = 0, guidanceMs = 0;
		bool foregroundQueue = false;
		// Written only by Execute; the owner acquires completion before reading.
		DlssNrFailure failure;
		std::array<bool, 3> attempted{}, evaluated{};
		double evaluateMs = 0, submitMs = 0;
		bool submitted = false;
		uint32_t completedPassCount = 0;
	};
	static void Wake(uintptr_t owner) noexcept {
		PostThreadMessageW(static_cast<DWORD>(owner), WM_NULL, 0, 0);
	}
	static void Execute(Job& job) noexcept;
	bool Complete(DLSSNRFilter& owner) noexcept;
	// The worker is destroyed/joined before its immutable job and GPU references.
	Job job;
	DLSSNRSubmissionWorker worker{Wake, GetCurrentThreadId()};
	bool pending = false;
};

struct DLSSNRFilter::Performance {
	struct Texture {
		winrt::com_ptr<ID3D11Texture2D> texture;
		winrt::com_ptr<ID3D11ShaderResourceView> srv;
		winrt::com_ptr<ID3D11UnorderedAccessView> uav;
		bool Create(ID3D11Device* device, uint32_t width, uint32_t height, DXGI_FORMAT format,
			bool shared = false) noexcept {
			texture = DirectXHelper::CreateTexture2D(device, format, width, height,
				D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, D3D11_USAGE_DEFAULT,
				shared ? D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE : 0);
			return texture && SUCCEEDED(device->CreateShaderResourceView(texture.get(), nullptr, srv.put())) &&
				SUCCEEDED(device->CreateUnorderedAccessView(texture.get(), nullptr, uav.put()));
		}
	};
	struct MotionStep {
		Texture motion, confidence;
		uint64_t sequence = 0;
		bool valid = false;
		bool confidenceAvailable = false;
	};
	Impl composition;
	Texture validity, generated;
	struct Result {
		Texture enhancement, reference;
		std::chrono::steady_clock::time_point submittedAt{};
		bool readyObserved = false, adopted = false;
	};
	std::array<Result, DLSSNREnhancementPool::Capacity> results;
	DLSSNREnhancementPool pool;
	winrt::com_ptr<ID3D11Fence> poolFence;
	uint64_t poolFenceValue = 0, consumedVersion = 0;
	size_t pendingSlot = DLSSNREnhancementPool::None;
	winrt::com_ptr<ID3D11Texture2D> latestInput, latestPreparedInput;
	FrameGuidanceView latestZero{};
	winrt::com_ptr<ID3D11Texture2D> latestDepth;
	winrt::com_ptr<ID3D11Fence> latestDepthFence;
	HdrFrameMetadata latestInputMetadata{}, latestOutputMetadata{};
	uint64_t latestSequence = 0, latestEpoch = 0, latestFrame = 0, latestPreviousFrame = 0;
	uint64_t latestRevision = 0, latestHistoryRevision = 0;
	bool latestIsNewCapture = false, latestWaiting = false;
	uint64_t replacedRequests = 0, poolBusy = 0, fullCompleted = 0, fullPublished = 0, qualityUpdates = 0;
	uint64_t completionToObserve = 0;
	uint64_t unusedExpired = 0, adoptedResults = 0;
	uint64_t fullReadySamples = 0, fullAdoptionSamples = 0, coverageCount = 0;
	double fullReadyMs = 0, fullAdoptionMs = 0;
	std::chrono::steady_clock::time_point submittedAt{};
	double fullTurnaroundMs = 0;
	Texture pendingMotion, pendingConfidence;
	Texture pendingDepth, zeroMotion, zeroConfidence;
	// Residual reuse needs at most three steps; successive full NR observations
	// can be four accepted captures apart and require all four for NGX history.
	std::array<MotionStep, 4> motion;
	winrt::com_ptr<ID3D11SamplerState> sampler;
	winrt::com_ptr<ID3D11Buffer> constants, invalid, invalidReadback;
	winrt::com_ptr<ID3D11UnorderedAccessView> invalidUav;
	winrt::com_ptr<ID3D11ComputeShader> publishShader, composeShader, checkShader, motionShader, coverageShader;
	struct CoverageSample { winrt::com_ptr<ID3D11Buffer> buffer; winrt::com_ptr<ID3D11Query> done; bool pending = false; };
	std::array<CoverageSample, 3> coverageSamples;
	double enhancementCoverage = -1, zeroEnhancementFraction = -1;
	DLSSNRPerformanceState state;
	DLSSNRPostOutputState postOutput;
	uint64_t poolRetryFence = 0;
	uint64_t pipelineFallbacks = 0;
	uint64_t pendingSequence = 0, pendingEpoch = 0;
	uint64_t readySequence = 0, readyEpoch = 0;
	uint64_t lastInferenceSequence = 0, lastInferenceEpoch = 0;
	uint64_t fullSubmissions = 0;
	uint64_t sameSpecRecoveries = 0, continuousReuseOutputs = 0;
	uint64_t lateDiscards = 0, epochDiscards = 0, noHistoryFrames = 0;
	uint64_t missingMotionFrames = 0, expiredHistoryFrames = 0, historyResetFrames = 0;
	uint64_t outputRevision = 0;
	uint64_t motionGeneration = 0, depthGeneration = 0, confidenceGeneration = 0;
	FrameGuidanceRegion guidanceRegion{};
	bool pending = false;
	bool compositeDirty = true;
	wil::unique_event_nothrow completionEvent;
	HANDLE registeredWait = nullptr;
	DWORD ownerThread = 0;
	~Performance() {
		if (registeredWait) UnregisterWaitEx(registeredWait, INVALID_HANDLE_VALUE);
	}
	struct Constants {
		uint32_t width, height, sourceWidth, sourceHeight;
		uint32_t age = 0, reproject = 0, hasHistory = 0;
		uint32_t processingMode = uint32_t(DLSSNRProcessingMode::EveryFrame);
		uint32_t confidenceBits = 0, padding[3]{};
	};
	static_assert(sizeof(Constants) == 48);

	bool MotionInputs(uint64_t origin, ID3D11ShaderResourceView** srvs, Constants& values) noexcept {
		if (origin > state.sequence || state.sequence-origin > motion.size()) return false;
		values.age = uint32_t(state.sequence-origin);
		values.confidenceBits = 0;
		for (uint32_t i=0; i<values.age; ++i) {
			const auto& step = motion[(state.sequence-i)%motion.size()];
			if (!step.valid || step.sequence != state.sequence-i) return false;
			srvs[3+i] = step.motion.srv.get();
			srvs[7+i] = step.confidence.srv.get();
			if (step.confidenceAvailable) values.confidenceBits |= 1u<<i;
		}
		return true;
	}
	void Dispatch(ID3D11ComputeShader* shader, const Constants& values,
		ID3D11ShaderResourceView* const* srvs, ID3D11UnorderedAccessView* const* uavs, uint32_t pixelsPerGroup = 8) noexcept {
		auto* dc = composition.context11;
		dc->UpdateSubresource(constants.get(), 0, nullptr, &values, 0, 0);
		auto* cb = constants.get(); auto* sample = sampler.get();
		dc->CSSetShader(shader, nullptr, 0); dc->CSSetConstantBuffers(0, 1, &cb);
		dc->CSSetSamplers(0, 1, &sample); dc->CSSetShaderResources(0, 12, srvs);
		dc->CSSetUnorderedAccessViews(0, 4, uavs, nullptr);
		dc->Dispatch((values.width+pixelsPerGroup-1)/pixelsPerGroup, (values.height+pixelsPerGroup-1)/pixelsPerGroup, 1);
		ID3D11ShaderResourceView* emptySrvs[12]{};
		ID3D11UnorderedAccessView* emptyUavs[4]{};
		dc->CSSetShaderResources(0, 12, emptySrvs); dc->CSSetUnorderedAccessViews(0, 4, emptyUavs, nullptr);
		dc->CSSetShader(nullptr, nullptr, 0);
	}
};

#include "DLSSNRSparse.h"

namespace {

std::atomic<void*> g_snippetCallerHookOwner = nullptr;
std::atomic<HMODULE> g_snippetCallerModule = nullptr;
std::atomic<DLSSNRFilter::Impl::GetModuleFileNameWFn>
	g_snippetOriginalGetModuleFileNameW = nullptr;

NVSDK_NGX_Result CallCreateFeatureSafely(
	DLSSNRFilter::Impl::CreateFeatureFn function,
	ID3D12GraphicsCommandList* commandList,
	NVSDK_NGX_Feature featureId,
	NVSDK_NGX_Parameter* parameters,
	NVSDK_NGX_Handle** feature,
	DWORD* sehCode
) noexcept {
	return NgxRuntimeGuard::Invoke([&]() {
		return function(commandList, featureId, parameters, feature);
	}, NVSDK_NGX_Result_FAIL_PlatformError, sehCode);
}

NVSDK_NGX_Result CallEvaluateFeatureSafely(
	DLSSNRFilter::Impl::EvaluateFeatureFn function,
	ID3D12GraphicsCommandList* commandList,
	const NVSDK_NGX_Handle* feature,
	const NVSDK_NGX_Parameter* parameters,
	DWORD* sehCode
) noexcept {
	return NgxRuntimeGuard::Invoke([&]() {
		return function(commandList, feature, parameters, nullptr);
	}, NVSDK_NGX_Result_FAIL_PlatformError, sehCode);
}

NVSDK_NGX_Result CallReleaseFeatureSafely(
	DLSSNRFilter::Impl::ReleaseFeatureFn function,
	NVSDK_NGX_Handle* feature,
	DWORD* sehCode
) noexcept {
	return NgxRuntimeGuard::Invoke([&]() {
		return function(feature);
	}, NVSDK_NGX_Result_FAIL_PlatformError, sehCode);
}

NVSDK_NGX_Result CallShutdownSafely(
	DLSSNRFilter::Impl::ShutdownFn function,
	ID3D12Device* device,
	DWORD* sehCode
) noexcept {
	return NgxRuntimeGuard::Invoke([&]() {
		return function(device);
	}, NVSDK_NGX_Result_FAIL_PlatformError, sehCode);
}

NVSDK_NGX_Result CallSnippetInitSafely(
	DLSSNRFilter::Impl::SnippetInitExtFn function,
	const wchar_t* applicationDataPath,
	ID3D12Device* device,
	DWORD* sehCode
) noexcept {
	return NgxRuntimeGuard::Invoke([&]() {
		return function(
			DLSSNR_SIGNED_SNIPPET_APPLICATION_ID, applicationDataPath,
			device, NVSDK_NGX_Version_API, nullptr);
	}, NVSDK_NGX_Result_FAIL_PlatformError, sehCode);
}

DWORD WINAPI SnippetGetModuleFileNameW(
	HMODULE module,
	LPWSTR filename,
	DWORD size
) noexcept {
	if (module == g_snippetCallerModule.load(std::memory_order_acquire)) {
		constexpr wchar_t AUTHORIZED_CALLER[] = L"nvngx.dll";
		constexpr DWORD AUTHORIZED_CALLER_LENGTH = ARRAYSIZE(AUTHORIZED_CALLER) - 1;
		if (!filename || !size) {
			SetLastError(ERROR_INSUFFICIENT_BUFFER);
			return 0;
		}
		if (size <= AUTHORIZED_CALLER_LENGTH) {
			if (size > 1) {
				std::memcpy(filename, AUTHORIZED_CALLER, (size - 1) * sizeof(wchar_t));
			}
			filename[size - 1] = L'\0';
			SetLastError(ERROR_INSUFFICIENT_BUFFER);
			return size;
		}
		std::memcpy(filename, AUTHORIZED_CALLER, sizeof(AUTHORIZED_CALLER));
		return AUTHORIZED_CALLER_LENGTH;
	}

	const auto original =
		g_snippetOriginalGetModuleFileNameW.load(std::memory_order_acquire);
	if (original) return original(module, filename, size);
	SetLastError(ERROR_INVALID_FUNCTION);
	return 0;
}

template <typename T>
void* FunctionAddress(T function) noexcept {
	void* result = nullptr;
	static_assert(sizeof(function) == sizeof(result));
	std::memcpy(&result, &function, sizeof(result));
	return result;
}

void** FindImportedFunctionSlot(HMODULE module, const char* functionName) noexcept {
	if (!module || !functionName) return nullptr;
	auto* base = reinterpret_cast<std::byte*>(module);
	const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
	if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return nullptr;
	const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE ||
		nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
		return nullptr;
	}

	const auto& directory =
		nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	if (!directory.VirtualAddress || !directory.Size ||
		directory.VirtualAddress >= nt->OptionalHeader.SizeOfImage ||
		directory.Size > nt->OptionalHeader.SizeOfImage ||
		directory.VirtualAddress > nt->OptionalHeader.SizeOfImage - directory.Size) {
		return nullptr;
	}

	auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
		base + directory.VirtualAddress);
	const auto* descriptorEnd = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(
		base + directory.VirtualAddress + directory.Size);
	for (; descriptor < descriptorEnd && descriptor->Name; ++descriptor) {
		if (descriptor->Name >= nt->OptionalHeader.SizeOfImage) continue;
		const char* libraryName = reinterpret_cast<const char*>(base + descriptor->Name);
		if (_stricmp(libraryName, "KERNEL32.dll") != 0 &&
			_stricmp(libraryName, "api-ms-win-core-libraryloader-l1-2-0.dll") != 0 &&
			_stricmp(libraryName, "api-ms-win-core-libraryloader-l1-1-0.dll") != 0) {
			continue;
		}
		if (!descriptor->OriginalFirstThunk || !descriptor->FirstThunk) continue;
		auto* nameThunk = reinterpret_cast<IMAGE_THUNK_DATA64*>(
			base + descriptor->OriginalFirstThunk);
		auto* addressThunk = reinterpret_cast<IMAGE_THUNK_DATA64*>(
			base + descriptor->FirstThunk);
		for (; nameThunk->u1.AddressOfData; ++nameThunk, ++addressThunk) {
			if (IMAGE_SNAP_BY_ORDINAL64(nameThunk->u1.Ordinal)) continue;
			const uint32_t nameRva = static_cast<uint32_t>(nameThunk->u1.AddressOfData);
			if (nameRva >= nt->OptionalHeader.SizeOfImage) return nullptr;
			const auto* import = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + nameRva);
			if (std::strcmp(reinterpret_cast<const char*>(import->Name), functionName) == 0) {
				return reinterpret_cast<void**>(&addressThunk->u1.Function);
			}
		}
	}
	return nullptr;
}

bool InstallSnippetCallerCompatibility(DLSSNRFilter::Impl& impl) noexcept {
	impl.snippetGetModuleFileNameIatSlot =
		FindImportedFunctionSlot(impl.snippetModule, "GetModuleFileNameW");
	if (!impl.snippetGetModuleFileNameIatSlot) {
		Logger::Get().Error(
			"DLSSNR signed snippet has no GetModuleFileNameW import");
		return false;
	}

	void* expectedOwner = nullptr;
	if (!g_snippetCallerHookOwner.compare_exchange_strong(
		expectedOwner, &impl, std::memory_order_acq_rel)) {
		Logger::Get().Error(
			"DLSSNR signed snippet caller compatibility is already owned by another session");
		return false;
	}

	void* hookAddress = FunctionAddress(&SnippetGetModuleFileNameW);
	HMODULE callerModule = nullptr;
	if (!GetModuleHandleExW(
		GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
			GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		reinterpret_cast<LPCWSTR>(hookAddress), &callerModule)) {
		g_snippetCallerHookOwner.store(nullptr, std::memory_order_release);
		Logger::Get().Win32Error(
			"Resolve DLSSNR signed snippet caller module failed");
		return false;
	}

	DWORD oldProtection = 0;
	if (!VirtualProtect(
		impl.snippetGetModuleFileNameIatSlot, sizeof(void*),
		PAGE_READWRITE, &oldProtection)) {
		g_snippetCallerHookOwner.store(nullptr, std::memory_order_release);
		Logger::Get().Win32Error(
			"Make DLSSNR signed snippet IAT writable failed");
		return false;
	}

	g_snippetCallerModule.store(callerModule, std::memory_order_release);
	void* original = InterlockedExchangePointer(
		reinterpret_cast<void* volatile*>(impl.snippetGetModuleFileNameIatSlot),
		hookAddress);
	DLSSNRFilter::Impl::GetModuleFileNameWFn originalFunction = nullptr;
	std::memcpy(&originalFunction, &original, sizeof(original));
	g_snippetOriginalGetModuleFileNameW.store(
		originalFunction, std::memory_order_release);
	impl.snippetCallerHookInstalled = true;

	DWORD ignoredProtection = 0;
	if (!VirtualProtect(
		impl.snippetGetModuleFileNameIatSlot, sizeof(void*),
		oldProtection, &ignoredProtection)) {
		Logger::Get().Win32Error(
			"Restore DLSSNR signed snippet IAT protection failed");
	}
	FlushInstructionCache(
		GetCurrentProcess(), impl.snippetGetModuleFileNameIatSlot, sizeof(void*));
	if (!originalFunction) {
		Logger::Get().Error(
			"DLSSNR signed snippet GetModuleFileNameW import was null");
		return false;
	}
	return true;
}

bool RestoreSnippetCallerCompatibility(DLSSNRFilter::Impl& impl) noexcept {
	if (!impl.snippetCallerHookInstalled) return true;
	const auto original =
		g_snippetOriginalGetModuleFileNameW.load(std::memory_order_acquire);
	DWORD oldProtection = 0;
	if (!impl.snippetGetModuleFileNameIatSlot || !VirtualProtect(
			impl.snippetGetModuleFileNameIatSlot, sizeof(void*),
			PAGE_READWRITE, &oldProtection)) {
		Logger::Get().Win32Error(
			"Restore DLSSNR signed snippet caller IAT failed");
		return false;
	}

	InterlockedExchangePointer(
		reinterpret_cast<void* volatile*>(impl.snippetGetModuleFileNameIatSlot),
		FunctionAddress(original));
	DWORD ignoredProtection = 0;
	VirtualProtect(
		impl.snippetGetModuleFileNameIatSlot, sizeof(void*),
		oldProtection, &ignoredProtection);
	FlushInstructionCache(
		GetCurrentProcess(), impl.snippetGetModuleFileNameIatSlot, sizeof(void*));
	impl.snippetCallerHookInstalled = false;
	impl.snippetGetModuleFileNameIatSlot = nullptr;
	g_snippetOriginalGetModuleFileNameW.store(nullptr, std::memory_order_release);
	g_snippetCallerModule.store(nullptr, std::memory_order_release);
	void* expectedOwner = &impl;
	g_snippetCallerHookOwner.compare_exchange_strong(
		expectedOwner, nullptr, std::memory_order_acq_rel);
	return true;
}

}

static bool WaitForFence(DLSSNRFilter::Impl& impl, uint64_t value) noexcept {
	const uint64_t completed = impl.fence12->GetCompletedValue();
	if (completed == std::numeric_limits<uint64_t>::max()) {
		Logger::Get().Error("DLSSNR device removed while waiting for a fence");
		return false;
	}
	if (!value || completed >= value) {
		return true;
	}
	if (!impl.fenceEvent) {
		Logger::Get().Error("DLSSNR fence event is unavailable");
		return false;
	}
	ResetEvent(impl.fenceEvent.get());
	const HRESULT hr = impl.fence12->SetEventOnCompletion(
		value, impl.fenceEvent.get());
	if (FAILED(hr)) {
		Logger::Get().ComError("Set DLSSNR fence event failed", hr);
		return false;
	}
	impl.fenceEvent.wait();
	return impl.fence12->GetCompletedValue() != std::numeric_limits<uint64_t>::max();
}

static bool WaitForQueue(DLSSNRFilter::Impl& impl) noexcept {
	const uint64_t value = ++impl.fenceValue;
	HRESULT hr = impl.queue12->Signal(impl.fence12.get(), value);
	if (FAILED(hr)) {
		Logger::Get().ComError("Signal DLSSNR D3D12 fence failed", hr);
		return false;
	}
	return WaitForFence(impl, value);
}

static HRESULT SubmitDlssnrEvaluation(DLSSNRFilter::Impl& impl,
	ID3D12CommandList* list, uint64_t outputReady) noexcept {
	// Scope is Evaluate execution. Feature creation retains its initialization
	// fence, and ReleaseFeature retains the owning queue's existing drain.
	// These lifecycle calls are not covered by an Evaluate completion ticket.
	auto* session = impl.useSignedSnippet ? impl.snippetSession.get() : nullptr;
	if (session) {
		// Execute holds the session lock across recording and ticket reservation.
		if (!session->snippetEvaluateFence || session->snippetEvaluateFaulted ||
			session->snippetEvaluateValue == UINT64_MAX) return E_UNEXPECTED;
		// The input-ready signal has already been enqueued and flushed on
		// D3D11 before reaching here, ahead of this job's D3D11 output wait.
		// Every predecessor ticket is signalled by an earlier submission, so
		// this GPU-only dependency cannot wait on a future input-ready signal.
		if (session->snippetEvaluateValue) {
			const auto hr = impl.queue12->Wait(session->snippetEvaluateFence.get(), session->snippetEvaluateValue);
			if (FAILED(hr)) { session->snippetEvaluateFaulted = true; return hr; }
		}
	}
	ID3D12CommandList* lists[]{list};
	impl.queue12->ExecuteCommandLists(1, lists);
	if (session) {
		const auto next = session->snippetEvaluateValue+1;
		const auto hr = impl.queue12->Signal(session->snippetEvaluateFence.get(), next);
		if (FAILED(hr)) { session->snippetEvaluateFaulted = true; return hr; }
		session->snippetEvaluateValue = next;
	}
	const auto hr = impl.queue12->Signal(impl.fence12.get(), outputReady);
	if (FAILED(hr) && session) session->snippetEvaluateFaulted = true;
	return hr;
}

static void CollectGpuTiming(
	DLSSNRFilter::Impl& impl,
	DLSSNRFilter::Impl::CommandSlot& slot
) noexcept {
	if (!slot.timestampPending || !impl.timestampReadback ||
		!impl.timestampFrequency) return;
	const size_t offset = size_t(slot.timestampQuery) * sizeof(uint64_t);
	const D3D12_RANGE readRange{ offset, offset + sizeof(uint64_t) * DLSSNRFilter::Impl::TIMESTAMP_STRIDE };
	void* mapped = nullptr;
	const HRESULT hr = impl.timestampReadback->Map(0, &readRange, &mapped);
	if (FAILED(hr) || !mapped) {
		Logger::Get().ComError("Read DLSSNR GPU timestamp failed", hr);
		slot.timestampPending = false;
		return;
	}
	const auto* timestamps = reinterpret_cast<const uint64_t*>(
		static_cast<const uint8_t*>(mapped) + offset);
	if (timestamps[1] >= timestamps[0]) {
		const double gpuMs = double(timestamps[1] - timestamps[0]) * 1000.0 /
			double(impl.timestampFrequency);
		impl.evaluateGpuTimings.Add(gpuMs);
		impl.lastEvaluateGpuMs = gpuMs;
		++impl.evaluateGpuSamples;
		FrameGuidancePerformance::PublishDlssnrGpuTiming(gpuMs);
	}
	for (uint32_t i = slot.timestampFirstPass; i < slot.timestampPassCount; ++i) {
		const size_t q = 2 + size_t(i) * 2;
		if (timestamps[q + 1] >= timestamps[q]) {
			auto& pass = i ? *impl.laterPasses[i - 1] : impl;
			const double ms = double(timestamps[q + 1] - timestamps[q]) * 1000.0 / double(impl.timestampFrequency);
			pass.passGpuTimings.Add(ms);
			if constexpr (NativeBackendTiming::Enabled)
				Logger::Get().Info(fmt::format("DLSSNR GPU pass {}: frame={} ms={:.3f}", i + 1, slot.timestampFrameId, ms));
		}
	}
	const D3D12_RANGE writtenRange{ 0, 0 };
	impl.timestampReadback->Unmap(0, &writtenRange);
	slot.timestampPending = false;
}

static std::string FormatTimingSummary(
	std::string_view name,
	const TimingSummary& value
) {
	return fmt::format(
		"{}[n={} avg={:.3f} p95={:.3f} p99={:.3f} max={:.3f}]",
		name, value.count, value.average, value.p95, value.p99, value.maximum);
}

DLSSNRFilter::Impl::~Impl() {
	const auto releaseStart = std::chrono::steady_clock::now();
	if (queue12 && fence12) {
		WaitForQueue(*this);
	}
	const double waitMs = std::chrono::duration<double, std::milli>(
		std::chrono::steady_clock::now()-releaseStart).count();
	// Release all child features before the runtime and Core owner.
	laterPasses.clear();
	if (feature) {
		const auto featureReleaseStart = std::chrono::steady_clock::now();
		DWORD sehCode = 0;
		const auto function = useSignedSnippet ? snippetReleaseFeature :
			static_cast<ReleaseFeatureFn>(&NVSDK_NGX_D3D12_ReleaseFeature);
		const NVSDK_NGX_Result result = function ?
			CallReleaseFeatureSafely(function, feature, &sehCode) :
			NVSDK_NGX_Result_FAIL_NotInitialized;
		if (sehCode) {
			Logger::Get().Warn(fmt::format(
				"DLSSNR ReleaseFeature raised SEH {:#x}", sehCode));
		} else if (!NGXSucceeded(result)) {
			Logger::Get().Warn(fmt::format(
				"DLSSNR ReleaseFeature failed ({:#x})", (uint32_t)result));
		}
		feature = nullptr;
		Logger::Get().Info(fmt::format("DLSSNR feature release: size={}x{} queueWaitMs={:.3f} releaseMs={:.3f}",
			width, height, waitMs, std::chrono::duration<double, std::milli>(
				std::chrono::steady_clock::now()-featureReleaseStart).count()));
	}
	if (parameters) {
		if (!coreOwner || !coreOwner->DestroyParameters(parameters, "DLSSNR")) {
			Logger::Get().Warn("DLSSNR shared Core parameter destruction failed");
		}
		parameters = nullptr;
	}
	snippetSession.reset();
	if (snippetInitialized && snippetShutdown && device12) {
		DWORD sehCode = 0;
		const NVSDK_NGX_Result result =
			CallShutdownSafely(snippetShutdown, device12.get(), &sehCode);
		if (sehCode) {
			Logger::Get().Warn(fmt::format(
				"DLSSNR signed snippet Shutdown1 raised SEH {:#x}", sehCode));
		} else if (!NGXSucceeded(result)) {
			Logger::Get().Warn(fmt::format(
				"DLSSNR signed snippet Shutdown1 failed ({:#x})", (uint32_t)result));
		}
		snippetInitialized = false;
	}
	const bool callerCompatibilityRestored =
		RestoreSnippetCallerCompatibility(*this);
	if (snippetModule) {
		if (callerCompatibilityRestored && !NgxRuntimeGuard::IsFaulted()) {
			if (!FreeLibrary(snippetModule)) {
				Logger::Get().Win32Error(
				"Release DLSSNR signed snippet DLL failed");
			}
		} else {
			Logger::Get().Warn(
				"DLSSNR signed snippet DLL retained after NGX fault or caller IAT restoration failure");
		}
		snippetModule = nullptr;
	}
	if (coreRegistered && coreOwner) {
		coreOwner->Release("DLSSNR");
		coreRegistered = false;
	}
}

static bool CreateSharedTexture(
	DLSSNRFilter::Impl& impl,
	const D3D11_TEXTURE2D_DESC& sourceDesc,
	bool allowUav,
	winrt::com_ptr<ID3D11Texture2D>& texture11,
	winrt::com_ptr<ID3D12Resource>& texture12
) noexcept {
	D3D11_TEXTURE2D_DESC desc = sourceDesc;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.CPUAccessFlags = 0;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE |
		(allowUav ? D3D11_BIND_UNORDERED_ACCESS : 0);
	desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED |
		D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

	HRESULT hr = impl.device11->CreateTexture2D(&desc, nullptr, texture11.put());
	if (FAILED(hr)) {
		impl.failure = DlssNrApiFailure(DlssNrStage::Initialize, DlssNrCodeKind::HResult, static_cast<uint32_t>(hr), "Create DLSSNR shared D3D11 texture failed");
		Logger::Get().ComError("Create DLSSNR shared D3D11 texture failed", hr);
		return false;
	}

	winrt::com_ptr<IDXGIResource1> dxgiResource;
	hr = texture11->QueryInterface(IID_PPV_ARGS(dxgiResource.put()));
	if (FAILED(hr)) {
		impl.failure = DlssNrApiFailure(DlssNrStage::Initialize, DlssNrCodeKind::HResult, static_cast<uint32_t>(hr), "Query DLSSNR shared IDXGIResource1 failed");
		Logger::Get().ComError("Query DLSSNR shared IDXGIResource1 failed", hr);
		return false;
	}
	HANDLE rawHandle = nullptr;
	hr = dxgiResource->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &rawHandle);
	if (FAILED(hr)) {
		impl.failure = DlssNrApiFailure(DlssNrStage::Initialize, DlssNrCodeKind::HResult, static_cast<uint32_t>(hr), "Create DLSSNR texture shared handle failed");
		Logger::Get().ComError("Create DLSSNR texture shared handle failed", hr);
		return false;
	}
	wil::unique_handle handle(rawHandle);
	hr = impl.device12->OpenSharedHandle(handle.get(), IID_PPV_ARGS(texture12.put()));
	if (FAILED(hr)) {
		impl.failure = DlssNrApiFailure(DlssNrStage::Initialize, DlssNrCodeKind::HResult, static_cast<uint32_t>(hr), "Open DLSSNR texture in D3D12 failed");
		Logger::Get().ComError("Open DLSSNR texture in D3D12 failed", hr);
		return false;
	}
	return true;
}

static bool CreateComputeShader(
	DLSSNRFilter::Impl& impl,
	std::string_view source,
	const char* entryPoint,
	const char* sourceName,
	winrt::com_ptr<ID3D11ComputeShader>& shader
) noexcept {
	static std::map<ID3D11Device5*, std::weak_ptr<DLSSNRFilter::Impl::ShaderSet>> sharedShaders;
	if (!impl.shaders) {
		impl.shaders = sharedShaders[impl.device11].lock();
		if (!impl.shaders) {
			impl.shaders = std::make_shared<DLSSNRFilter::Impl::ShaderSet>();
			sharedShaders[impl.device11] = impl.shaders;
		}
	}
	const auto cached = impl.shaders->shaders.find(sourceName);
	if (cached != impl.shaders->shaders.end()) {
		shader = cached->second;
		return true;
	}
	winrt::com_ptr<ID3DBlob> shaderBlob;
	if (!DirectXHelper::CompileComputeShader(
		source, entryPoint, shaderBlob.put(), sourceName)) {
		return false;
	}
	const HRESULT hr = impl.device11->CreateComputeShader(
		shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize(), nullptr,
		shader.put());
	if (FAILED(hr)) {
		Logger::Get().ComError(fmt::format(
			"Create {} compute shader failed", sourceName), hr);
		return false;
	}
	impl.shaders->shaders.emplace(sourceName, shader);
	return true;
}

static bool CreateCompositeOutput(
	DLSSNRFilter::Impl& impl,
	ID3D11Texture2D* input,
	ID3D11Texture2D* output,
	const D3D11_TEXTURE2D_DESC& outputDesc
) noexcept {
	impl.finalizeOutput = outputDesc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT;
	// The final pass reads the original input. Only use the output directly
	// when it is a separate UAV-capable resource; otherwise retain the copy path.
	if (input != output && (outputDesc.BindFlags & D3D11_BIND_UNORDERED_ACCESS)) {
		winrt::com_ptr<ID3D11UnorderedAccessView> outputUav;
		if (SUCCEEDED(impl.device11->CreateUnorderedAccessView(
			output, nullptr, outputUav.put()))) {
			impl.compositeOutput11.copy_from(output);
			impl.compositeOutputUav11 = std::move(outputUav);
			return true;
		}
	}
	D3D11_TEXTURE2D_DESC compositeDesc = outputDesc;
	compositeDesc.Usage = D3D11_USAGE_DEFAULT;
	compositeDesc.CPUAccessFlags = 0;
	compositeDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
	compositeDesc.MiscFlags = 0;
	HRESULT hr = impl.device11->CreateTexture2D(
		&compositeDesc, nullptr, impl.compositeOutput11.put());
	if (SUCCEEDED(hr)) {
		hr = impl.device11->CreateUnorderedAccessView(
			impl.compositeOutput11.get(), nullptr, impl.compositeOutputUav11.put());
	}
	if (FAILED(hr)) {
		impl.failure = DlssNrApiFailure(DlssNrStage::Initialize, DlssNrCodeKind::HResult, static_cast<uint32_t>(hr), "Create DLSSNR residual composite output failed");
		Logger::Get().ComError("Create DLSSNR residual composite output failed", hr);
		return false;
	}
	return true;
}

static bool CreateResolutionScalingResources(
	DLSSNRFilter::Impl& impl,
	ID3D11Texture2D* input,
	ID3D11Texture2D* output,
	const D3D11_TEXTURE2D_DESC& outputDesc
) noexcept {
	HRESULT hr = impl.device11->CreateShaderResourceView(
		input, nullptr, impl.inputSrv11.put());
	if (SUCCEEDED(hr)) {
		hr = impl.device11->CreateShaderResourceView(
			impl.sharedInput11.get(), nullptr, impl.sharedInputSrv11.put());
	}
	if (SUCCEEDED(hr)) {
		hr = impl.device11->CreateShaderResourceView(
			impl.sharedOutput11.get(), nullptr, impl.sharedOutputSrv11.put());
	}
	if (SUCCEEDED(hr)) {
		hr = impl.device11->CreateUnorderedAccessView(
			impl.sharedInput11.get(), nullptr, impl.sharedInputUav11.put());
	}
	if (FAILED(hr)) {
		impl.failure = DlssNrApiFailure(DlssNrStage::Initialize, DlssNrCodeKind::HResult, static_cast<uint32_t>(hr), "Create DLSSNR resolution scaling color views failed");
		Logger::Get().ComError("Create DLSSNR resolution scaling color views failed", hr);
		return false;
	}
	if (!CreateCompositeOutput(impl, input, output, outputDesc)) return false;
	const bool reduced = impl.width != impl.sourceWidth || impl.height != impl.sourceHeight;
	if (reduced) {
		impl.resampleIntermediate11 = DirectXHelper::CreateTexture2D(
			impl.device11, DXGI_FORMAT_R16G16B16A16_FLOAT,
			impl.sourceWidth, impl.height,
			D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
		if (!impl.resampleIntermediate11) {
			Logger::Get().Error(
				"Create DLSSNR horizontal residual texture failed");
			return false;
		}
		hr = impl.device11->CreateShaderResourceView(
			impl.resampleIntermediate11.get(), nullptr,
			impl.resampleIntermediateSrv11.put());
		if (SUCCEEDED(hr)) {
			hr = impl.device11->CreateUnorderedAccessView(
				impl.resampleIntermediate11.get(), nullptr,
				impl.resampleIntermediateUav11.put());
		}
		if (FAILED(hr)) {
			impl.failure = DlssNrApiFailure(DlssNrStage::Initialize, DlssNrCodeKind::HResult, static_cast<uint32_t>(hr), "Create DLSSNR horizontal residual views failed");
			Logger::Get().ComError("Create DLSSNR horizontal residual views failed", hr);
			return false;
		}
	}

	constexpr UINT GUIDANCE_BIND_FLAGS =
		D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
	impl.controlledResidual11 = DirectXHelper::CreateTexture2D(
		impl.device11, DXGI_FORMAT_R16G16B16A16_FLOAT, impl.width, impl.height,
		D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
	if (!impl.controlledResidual11) {
		Logger::Get().Error("Create DLSSNR controlled residual texture failed");
		return false;
	}
	hr = impl.device11->CreateShaderResourceView(
		impl.controlledResidual11.get(), nullptr, impl.controlledResidualSrv11.put());
	if (SUCCEEDED(hr)) {
		hr = impl.device11->CreateUnorderedAccessView(
			impl.controlledResidual11.get(), nullptr, impl.controlledResidualUav11.put());
	}
	if (FAILED(hr)) {
		impl.failure = DlssNrApiFailure(DlssNrStage::Initialize, DlssNrCodeKind::HResult, static_cast<uint32_t>(hr), "Create DLSSNR controlled residual views failed");
		Logger::Get().ComError("Create DLSSNR controlled residual views failed", hr);
		return false;
	}
	if (reduced) {
		constexpr UINT GUIDANCE_MISC_FLAGS =
			D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
		impl.reducedMotion11 = DirectXHelper::CreateTexture2D(
			impl.device11, DXGI_FORMAT_R16G16_FLOAT, impl.width, impl.height,
			GUIDANCE_BIND_FLAGS, D3D11_USAGE_DEFAULT, GUIDANCE_MISC_FLAGS);
		impl.reducedDepth11 = DirectXHelper::CreateTexture2D(
			impl.device11, DXGI_FORMAT_R32_FLOAT, impl.width, impl.height,
			GUIDANCE_BIND_FLAGS, D3D11_USAGE_DEFAULT, GUIDANCE_MISC_FLAGS);
		impl.reducedConfidence11 = DirectXHelper::CreateTexture2D(
			impl.device11, DXGI_FORMAT_R8_UNORM, impl.width, impl.height,
			GUIDANCE_BIND_FLAGS, D3D11_USAGE_DEFAULT, GUIDANCE_MISC_FLAGS);
		if (!impl.reducedMotion11 || !impl.reducedDepth11 ||
			!impl.reducedConfidence11) {
			Logger::Get().Error(
				"Create DLSSNR reduced Frame Guidance textures failed");
			return false;
		}
		hr = impl.device11->CreateUnorderedAccessView(
			impl.reducedMotion11.get(), nullptr, impl.reducedMotionUav11.put());
		if (SUCCEEDED(hr)) {
			hr = impl.device11->CreateUnorderedAccessView(
				impl.reducedDepth11.get(), nullptr, impl.reducedDepthUav11.put());
		}
		if (SUCCEEDED(hr)) {
			hr = impl.device11->CreateUnorderedAccessView(
				impl.reducedConfidence11.get(), nullptr,
				impl.reducedConfidenceUav11.put());
		}
		if (FAILED(hr)) {
			impl.failure = DlssNrApiFailure(DlssNrStage::Initialize, DlssNrCodeKind::HResult, static_cast<uint32_t>(hr), "Create DLSSNR reduced Frame Guidance UAVs failed");
			Logger::Get().ComError("Create DLSSNR reduced Frame Guidance UAVs failed", hr);
			return false;
		}
	}

	D3D11_BUFFER_DESC constantsDesc{};
	constantsDesc.ByteWidth = sizeof(ResampleConstants);
	constantsDesc.Usage = D3D11_USAGE_DEFAULT;
	constantsDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	const ResampleConstants constants{
		.sourceWidth = impl.sourceWidth, .sourceHeight = impl.sourceHeight,
		.targetWidth = impl.width, .targetHeight = impl.height,
		.motionScaleX = float(impl.width) / float(impl.sourceWidth),
		.motionScaleY = float(impl.height) / float(impl.sourceHeight)
	};
	const D3D11_SUBRESOURCE_DATA data{ &constants, 0, 0 };
	hr = impl.device11->CreateBuffer(
		&constantsDesc, &data, impl.resampleConstants11.put());
	if (SUCCEEDED(hr)) hr = impl.device11->CreateBuffer(
		&constantsDesc, nullptr, impl.residualConstants11.put());
	if (FAILED(hr)) {
		impl.failure = DlssNrApiFailure(DlssNrStage::Initialize, DlssNrCodeKind::HResult, static_cast<uint32_t>(hr), "Create DLSSNR resample constants failed");
		Logger::Get().ComError("Create DLSSNR resample constants failed", hr);
		return false;
	}

	return (!reduced || (CreateComputeShader(
			impl, COLOR_DOWNSAMPLE_HLSL, "DownsampleColorVertical",
			"DLSSNRColorDownsampleVertical", impl.colorDownsampleVerticalShader11) &&
		CreateComputeShader(impl, COLOR_DOWNSAMPLE_HLSL, "DownsampleColorHorizontal",
			"DLSSNRColorDownsampleHorizontal", impl.colorDownsampleHorizontalShader11) &&
		CreateComputeShader(impl, GUIDANCE_DOWNSAMPLE_HLSL, "DownsampleGuidance",
			"DLSSNRGuidanceDownsample", impl.guidanceDownsampleShader11))) &&
		(!impl.convertInputToRgba || CreateComputeShader(impl, COLOR_CONVERT_HLSL,
			"ConvertToRgba", "DLSSNRColorConvert", impl.colorConvertShader11)) &&
		CreateComputeShader(impl, RESIDUAL_PREPARE_SHADER, "PrepareResidual",
			"DLSSNRResidualPrepare", impl.residualPrepareShader11) &&
		(impl.sourceWidth == impl.width || CreateComputeShader(
			impl, RESIDUAL_HORIZONTAL_HLSL, "UpsampleResidualHorizontal",
			"DLSSNRResidualHorizontal", impl.residualHorizontalShader11)) &&
		CreateComputeShader(impl, RESIDUAL_VERTICAL_COMPOSITE_SHADER,
			"CompositeResidualVertical", "DLSSNRResidualVerticalComposite",
			impl.residualVerticalCompositeShader11);
}

static bool UpdateGuidanceResources(
	DLSSNRFilter::Impl& impl,
	const FrameGuidanceView& view,
	FrameGuidanceFrameId frameId
) noexcept {
	return impl.guidanceInterop && impl.guidanceInterop->Update(
		view, frameId, { impl.width, impl.height });
}

static void SetSubrect(
	NVSDK_NGX_Parameter* parameters,
	const ResourceParameters& resource,
	FrameGuidanceRegion region
) {
	parameters->Set(resource.baseX, region.x);
	parameters->Set(resource.baseY, region.y);
	parameters->Set(resource.width, region.width);
	parameters->Set(resource.height, region.height);
}

static NVSDK_NGX_Result NVSDK_CONV SetScalingRatioCallback(
	NVSDK_NGX_Parameter* parameters
) noexcept {
	__try {
		if (!parameters) return NVSDK_NGX_Result_FAIL_InvalidParameter;
		parameters->Set(PARAM_SCALING_RATIO, 1.0f);
		return NVSDK_NGX_Result_Success;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return NVSDK_NGX_Result_FAIL_PlatformError;
	}
}

static void SetCreateParametersUnsafe(DLSSNRFilter::Impl& impl) {
	impl.parameters->Set(PARAM_WIDTH, impl.width);
	impl.parameters->Set(PARAM_HEIGHT, impl.height);
	impl.parameters->Set(PARAM_INPUT_WIDTH, impl.width);
	impl.parameters->Set(PARAM_INPUT_HEIGHT, impl.height);
	impl.parameters->Set(PARAM_OUTPUT_WIDTH, impl.width);
	impl.parameters->Set(PARAM_OUTPUT_HEIGHT, impl.height);
	impl.parameters->Set(PARAM_OUTPUT_DOT_WIDTH, impl.width);
	impl.parameters->Set(PARAM_OUTPUT_DOT_HEIGHT, impl.height);
	impl.parameters->Set(PARAM_UPSCALING, 0u);
	impl.parameters->Set(PARAM_SCALE, 1.0f);
	impl.parameters->Set(PARAM_SCALING_RATIO, 1.0f);
	impl.parameters->Set(
		PARAM_SCALING_RATIO_CALLBACK,
		FunctionAddress(&SetScalingRatioCallback));
	impl.parameters->Set(PARAM_PRESET, impl.modelPreset);
	impl.parameters->Set(NVSDK_NGX_Parameter_Width, impl.width);
	impl.parameters->Set(NVSDK_NGX_Parameter_Height, impl.height);
	impl.parameters->Set(
		NVSDK_NGX_Parameter_PerfQualityValue,
		static_cast<int>(NVSDK_NGX_PerfQuality_Value_Balanced));
	impl.parameters->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
	impl.parameters->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
}

static bool SetCreateParametersSafely(
	DLSSNRFilter::Impl& impl,
	DWORD* sehCode
) noexcept {
	return NgxRuntimeGuard::Invoke([&]() {
		SetCreateParametersUnsafe(impl);
		return true;
	}, false, sehCode);
}

static std::string DescribeLoadedSnippet(HMODULE module) {
	std::wstring buffer(32768, L'\0');
	const DWORD length = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
	if (!length || length >= buffer.size()) return "Loaded DLSSNR path/version: unavailable";
	buffer.resize(length);
	std::string result = "Loaded DLSSNR: " + StrHelper::UTF16ToUTF8(buffer);
	DWORD unused = 0;
	const DWORD size = GetFileVersionInfoSizeW(buffer.c_str(), &unused);
	std::vector<uint8_t> data(size);
	VS_FIXEDFILEINFO* version = nullptr;
	UINT bytes = 0;
	if (size && GetFileVersionInfoW(buffer.c_str(), 0, size, data.data()) &&
		VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&version), &bytes) &&
		bytes >= sizeof(VS_FIXEDFILEINFO) && version->dwSignature == 0xFEEF04BD)
		result += fmt::format(" / FileVersion: {}.{}.{}.{}", HIWORD(version->dwFileVersionMS),
			LOWORD(version->dwFileVersionMS), HIWORD(version->dwFileVersionLS), LOWORD(version->dwFileVersionLS));
	else result += " / FileVersion: unavailable";
	return result;
}

static bool InitializeSignedSnippetSession(
	DLSSNRFilter::Impl& impl,
	const std::filesystem::path& applicationDirectory
) noexcept {
	const std::filesystem::path dllPath =
		applicationDirectory / L"nvngx_dlssnr.dll";
	const DWORD attributes = GetFileAttributesW(dllPath.c_str());
	const DWORD pathError = attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_SUCCESS;
	if (attributes == INVALID_FILE_ATTRIBUTES && (pathError == ERROR_FILE_NOT_FOUND || pathError == ERROR_PATH_NOT_FOUND)) {
		impl.failure = { DlssNrStage::LoadLibrary, DlssNrReason::LibraryMissing, DlssNrCodeKind::Win32, pathError, "GetFileAttributesW(nvngx_dlssnr.dll)" };
		Logger::Get().Error("DLSSNR library does not exist at the application path");
		return false;
	}
	impl.snippetModule = LoadLibraryExW(
		dllPath.c_str(), nullptr,
		LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
	if (!impl.snippetModule) {
		const DWORD code = GetLastError();
		impl.failure = DlssNrApiFailure(DlssNrStage::LoadLibrary, DlssNrCodeKind::Win32, code, "LoadLibraryExW(nvngx_dlssnr.dll)");
		if (impl.failure.reason == DlssNrReason::Unknown) impl.failure.reason = DlssNrReason::LibraryLoadFailed;
		Logger::Get().Error(fmt::format("Load signed nvngx_dlssnr.dll failed: Win32={}", code));
		return false;
	}

	impl.libraryInfo = DescribeLoadedSnippet(impl.snippetModule);
	Logger::Get().Info(impl.libraryInfo);
	impl.snippetInitExt = GetExport<DLSSNRFilter::Impl::SnippetInitExtFn>(
		impl.snippetModule, "NVSDK_NGX_D3D12_Init_Ext");
	impl.snippetCreateFeature = GetExport<DLSSNRFilter::Impl::CreateFeatureFn>(
		impl.snippetModule, "NVSDK_NGX_D3D12_CreateFeature");
	impl.snippetEvaluateFeature = GetExport<DLSSNRFilter::Impl::EvaluateFeatureFn>(
		impl.snippetModule, "NVSDK_NGX_D3D12_EvaluateFeature");
	impl.snippetReleaseFeature = GetExport<DLSSNRFilter::Impl::ReleaseFeatureFn>(
		impl.snippetModule, "NVSDK_NGX_D3D12_ReleaseFeature");
	impl.snippetShutdown = GetExport<DLSSNRFilter::Impl::ShutdownFn>(
		impl.snippetModule, "NVSDK_NGX_D3D12_Shutdown1");
	if (!impl.snippetInitExt || !impl.snippetCreateFeature ||
		!impl.snippetEvaluateFeature || !impl.snippetReleaseFeature ||
		!impl.snippetShutdown) {
		impl.failure = { DlssNrStage::LoadLibrary, DlssNrReason::MissingExports, DlssNrCodeKind::None, 0, "Resolve required NGX exports" };
		Logger::Get().Error("DLSSNR signed snippet exports are incomplete");
		return false;
	}
	impl.failure = { DlssNrStage::InitializeLibrary, DlssNrReason::Unknown, DlssNrCodeKind::None, 0, "Install snippet caller compatibility" };
	if (!InstallSnippetCallerCompatibility(impl)) return false;

	DWORD sehCode = 0;
	const NVSDK_NGX_Result result = CallSnippetInitSafely(
		impl.snippetInitExt, applicationDirectory.c_str(),
		impl.device12.get(), &sehCode);
	impl.failure = DlssNrApiFailure(DlssNrStage::InitializeLibrary, sehCode ? DlssNrCodeKind::Seh : DlssNrCodeKind::Ngx,
		sehCode ? sehCode : static_cast<uint32_t>(result), "NVSDK_NGX_D3D12_Init_Ext");
	if (sehCode) {
		Logger::Get().Error(fmt::format(
			"DLSSNR signed snippet Init_Ext raised SEH {:#x}", sehCode));
		return false;
	}
	if (!NGXSucceeded(result)) {
		Logger::Get().Error(fmt::format(
			"DLSSNR signed snippet Init_Ext failed ({:#x})", (uint32_t)result));
		return false;
	}
	impl.snippetInitialized = true;
	const auto hr = impl.device12->CreateFence(0, D3D12_FENCE_FLAG_NONE,
		IID_PPV_ARGS(impl.snippetEvaluateFence.put()));
	if (FAILED(hr)) {
		impl.failure = DlssNrApiFailure(DlssNrStage::Initialize, DlssNrCodeKind::HResult,
			static_cast<uint32_t>(hr), "Create signed NR Evaluate order fence");
		return false;
	}
	impl.useSignedSnippet = true;
	return true;
}

// Creation and destruction are serialized on the renderer backend thread.
// Init/Shutdown and the DLL caller hook belong to the runtime, not each feature.
static bool InitializeSignedSnippet(
	DLSSNRFilter::Impl& impl,
	const std::filesystem::path& applicationDirectory
) noexcept {
	static std::weak_ptr<DLSSNRFilter::Impl> sharedSession;
	auto session = sharedSession.lock();
	if (session && session->device12.get() != impl.device12.get()) {
		Logger::Get().Error("DLSSNR snippet runtime is active on another device");
		return false;
	}
	if (!session) {
		session = std::make_shared<DLSSNRFilter::Impl>();
		session->device12 = impl.device12;
		if (!InitializeSignedSnippetSession(*session, applicationDirectory)) {
			impl.failure = session->failure;
			impl.libraryInfo = session->libraryInfo;
			return false;
		}
		sharedSession = session;
	}
	impl.libraryInfo = session->libraryInfo;
	impl.snippetCreateFeature = session->snippetCreateFeature;
	impl.snippetEvaluateFeature = session->snippetEvaluateFeature;
	impl.snippetReleaseFeature = session->snippetReleaseFeature;
	impl.useSignedSnippet = true;
	impl.snippetSession = std::move(session);
	return true;
}

static void SetEvaluateParametersUnsafe(
	DLSSNRFilter::Impl& impl,
	const DLSSNRSettings& settings,
	const FrameGuidanceView& guidance,
	bool resetHistory
) {
	impl.parameters->Set(PARAM_COLOR, impl.sharedInput12.get());
	impl.parameters->Set(PARAM_OUTPUT, impl.sharedOutput12.get());
	impl.parameters->Set(PARAM_MVEC, impl.guidanceInterop->Motion());
	impl.parameters->Set(PARAM_DEPTH, impl.guidanceInterop->Depth());
	const FrameGuidanceRegion full = FrameGuidanceRegion::Full(
		{ impl.width, impl.height });
	SetSubrect(impl.parameters, RESOURCE_PARAMETERS[0], full);
	SetSubrect(impl.parameters, RESOURCE_PARAMETERS[1], full);
	SetSubrect(impl.parameters, RESOURCE_PARAMETERS[2],
		guidance.motion.metadata.validRegion);
	SetSubrect(impl.parameters, RESOURCE_PARAMETERS[3],
		guidance.depth.metadata.validRegion);
	impl.parameters->Set(PARAM_MVEC_SCALE_X, 1.0f);
	impl.parameters->Set(PARAM_MVEC_SCALE_Y, 1.0f);
	impl.parameters->Set(PARAM_DEPTH_INVERTED, 1);
	impl.parameters->Set(PARAM_INDICATOR_INVERT_X, 0);
	impl.parameters->Set(PARAM_INDICATOR_INVERT_Y, 0);
	impl.parameters->Set(PARAM_ENABLED, 1);
	impl.parameters->Set(
		PARAM_RESET, resetHistory ? 1 : 0);
	impl.parameters->Set(PARAM_STYLE, settings.style);
	impl.parameters->Set(PARAM_INTENSITY, settings.intensity);
	impl.parameters->Set(PARAM_LOCAL_TONE, settings.localToneStrength);
	impl.parameters->Set(PARAM_LOCAL_STRUCTURE, settings.localStructureStrength);
	impl.parameters->Set(PARAM_SKIN_STRUCTURE, settings.skinStructureStrength);
	impl.parameters->Set(PARAM_AUTO_MASK, settings.useAutoMask ? 1 : 0);
	impl.parameters->Set(PARAM_UI_CORRECTION, settings.uiCorrection ? 1 : 0);
}

static bool SetEvaluateParametersSafely(
	DLSSNRFilter::Impl& impl,
	const DLSSNRSettings& settings,
	const FrameGuidanceView& guidance,
	bool resetHistory,
	DWORD* sehCode
) noexcept {
	return NgxRuntimeGuard::Invoke([&]() {
		SetEvaluateParametersUnsafe(impl, settings, guidance, resetHistory);
		return true;
	}, false, sehCode);
}

static bool PrepareInput(
	DLSSNRFilter::Impl& impl,
	ID3D11Texture2D* input
) noexcept {
	if ((impl.useResolutionScaling || impl.convertInputToRgba) && impl.inputViewTexture != input) {
		winrt::com_ptr<ID3D11ShaderResourceView> view;
		if (FAILED(impl.device11->CreateShaderResourceView(input, nullptr, view.put()))) return false;
		impl.inputSrv11 = std::move(view);
		impl.inputViewTexture = input;
	}
	if (impl.useResolutionScaling &&
		(impl.width != impl.sourceWidth || impl.height != impl.sourceHeight)) {
		ID3D11ShaderResourceView* srv = impl.inputSrv11.get();
		ID3D11UnorderedAccessView* uav = impl.resampleIntermediateUav11.get();
		ID3D11Buffer* constantBuffer = impl.resampleConstants11.get();
		impl.context11->CSSetShader(
			impl.colorDownsampleVerticalShader11.get(), nullptr, 0);
		impl.context11->CSSetShaderResources(0, 1, &srv);
		impl.context11->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		impl.context11->CSSetConstantBuffers(0, 1, &constantBuffer);
		impl.context11->Dispatch(
			(impl.sourceWidth + 7) / 8, (impl.height + 7) / 8, 1);
		ID3D11ShaderResourceView* nullSrv = nullptr;
		ID3D11UnorderedAccessView* nullUav = nullptr;
		ID3D11Buffer* nullBuffer = nullptr;
		impl.context11->CSSetShaderResources(0, 1, &nullSrv);
		impl.context11->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
        // The first pass UAV becomes the second pass SRV only after unbinding.
        srv = impl.resampleIntermediateSrv11.get();
        uav = impl.sharedInputUav11.get();
        impl.context11->CSSetShader(impl.colorDownsampleHorizontalShader11.get(), nullptr, 0);
        impl.context11->CSSetShaderResources(0, 1, &srv);
        impl.context11->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
        impl.context11->Dispatch((impl.width + 7) / 8, (impl.height + 7) / 8, 1);
        impl.context11->CSSetShaderResources(0, 1, &nullSrv);
        impl.context11->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
		impl.context11->CSSetConstantBuffers(0, 1, &nullBuffer);
		impl.context11->CSSetShader(nullptr, nullptr, 0);
		return true;
	}
	if (!impl.convertInputToRgba) {
		impl.context11->CopyResource(impl.sharedInput11.get(), input);
		return true;
	}
	ID3D11ShaderResourceView* srv = impl.inputSrv11.get();
	ID3D11UnorderedAccessView* uav = impl.sharedInputUav11.get();
	impl.context11->CSSetShader(impl.colorConvertShader11.get(), nullptr, 0);
	impl.context11->CSSetShaderResources(0, 1, &srv);
	impl.context11->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
	impl.context11->Dispatch((impl.width + 7) / 8, (impl.height + 7) / 8, 1);
	ID3D11ShaderResourceView* nullSrv = nullptr;
	ID3D11UnorderedAccessView* nullUav = nullptr;
	impl.context11->CSSetShaderResources(0, 1, &nullSrv);
	impl.context11->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
	impl.context11->CSSetShader(nullptr, nullptr, 0);
	return true;
}

static bool UpdateGuidanceShaderResources(
	DLSSNRFilter::Impl& impl,
	const FrameGuidanceView& guidance
) noexcept {
	if (impl.guidanceMotion11 == guidance.motion.texture &&
		impl.guidanceDepth11 == guidance.depth.texture &&
		impl.guidanceConfidence11 == guidance.confidence.texture &&
		impl.guidanceMotionSrv11 && impl.guidanceDepthSrv11 &&
		impl.guidanceConfidenceSrv11) {
		return true;
	}

	winrt::com_ptr<ID3D11ShaderResourceView> motionSrv;
	winrt::com_ptr<ID3D11ShaderResourceView> depthSrv;
	winrt::com_ptr<ID3D11ShaderResourceView> confidenceSrv;
	HRESULT hr = impl.device11->CreateShaderResourceView(
		guidance.motion.texture, nullptr, motionSrv.put());
	if (SUCCEEDED(hr)) {
		hr = impl.device11->CreateShaderResourceView(
			guidance.depth.texture, nullptr, depthSrv.put());
	}
	if (SUCCEEDED(hr)) {
		hr = impl.device11->CreateShaderResourceView(
			guidance.confidence.texture, nullptr, confidenceSrv.put());
	}
	if (FAILED(hr)) {
		Logger::Get().ComError(
			"Create DLSSNR Frame Guidance downsample SRVs failed", hr);
		return false;
	}
	impl.guidanceMotionSrv11 = std::move(motionSrv);
	impl.guidanceDepthSrv11 = std::move(depthSrv);
	impl.guidanceConfidenceSrv11 = std::move(confidenceSrv);
	impl.guidanceMotion11 = guidance.motion.texture;
	impl.guidanceDepth11 = guidance.depth.texture;
	impl.guidanceConfidence11 = guidance.confidence.texture;
	return true;
}

static bool PrepareReducedGuidance(
	DLSSNRFilter::Impl& impl,
	const FrameGuidanceView& guidance
) noexcept {
	if (!UpdateGuidanceShaderResources(impl, guidance)) return false;
	ID3D11ShaderResourceView* srvs[]{
		impl.guidanceMotionSrv11.get(),
		impl.guidanceDepthSrv11.get(),
		impl.guidanceConfidenceSrv11.get()
	};
	ID3D11UnorderedAccessView* uavs[]{
		impl.reducedMotionUav11.get(),
		impl.reducedDepthUav11.get(),
		impl.reducedConfidenceUav11.get()
	};
	ID3D11Buffer* constantBuffer = impl.resampleConstants11.get();
	impl.context11->CSSetShader(
		impl.guidanceDownsampleShader11.get(), nullptr, 0);
	impl.context11->CSSetShaderResources(0, ARRAYSIZE(srvs), srvs);
	impl.context11->CSSetUnorderedAccessViews(
		0, ARRAYSIZE(uavs), uavs, nullptr);
	impl.context11->CSSetConstantBuffers(0, 1, &constantBuffer);
	impl.context11->Dispatch(
		(impl.width + 7) / 8, (impl.height + 7) / 8, 1);
	ID3D11ShaderResourceView* nullSrvs[ARRAYSIZE(srvs)]{};
	ID3D11UnorderedAccessView* nullUavs[ARRAYSIZE(uavs)]{};
	ID3D11Buffer* nullBuffer = nullptr;
	impl.context11->CSSetShaderResources(
		0, ARRAYSIZE(nullSrvs), nullSrvs);
	impl.context11->CSSetUnorderedAccessViews(
		0, ARRAYSIZE(nullUavs), nullUavs, nullptr);
	impl.context11->CSSetConstantBuffers(0, 1, &nullBuffer);
	impl.context11->CSSetShader(nullptr, nullptr, 0);
	return true;
}

static FrameGuidanceRegion ScaleGuidanceRegion(
	FrameGuidanceRegion region,
	FrameGuidanceExtent source,
	FrameGuidanceExtent target
) noexcept {
	const uint64_t sourceRight = uint64_t(region.x) + region.width;
	const uint64_t sourceBottom = uint64_t(region.y) + region.height;
	const uint32_t left = static_cast<uint32_t>(
		uint64_t(region.x) * target.width / source.width);
	const uint32_t top = static_cast<uint32_t>(
		uint64_t(region.y) * target.height / source.height);
	const uint32_t right = static_cast<uint32_t>(std::min<uint64_t>(
		target.width,
		(sourceRight * target.width + source.width - 1) / source.width));
	const uint32_t bottom = static_cast<uint32_t>(std::min<uint64_t>(
		target.height,
		(sourceBottom * target.height + source.height - 1) / source.height));
	return { left, top, right - left, bottom - top };
}

static FrameGuidanceView MakeReducedGuidance(
	DLSSNRFilter::Impl& impl,
	const FrameGuidanceView& source
) noexcept {
	const FrameGuidanceExtent sourceExtent{
		impl.sourceWidth, impl.sourceHeight };
	const FrameGuidanceExtent targetExtent{ impl.width, impl.height };
	auto reducedMetadata = [&](const FrameGuidanceMetadata& metadata) {
		FrameGuidanceMetadata result = metadata;
		result.sourceExtent = targetExtent;
		result.validRegion = ScaleGuidanceRegion(
			metadata.validRegion, sourceExtent, targetExtent);
		result.sync = {};
		return result;
	};
	FrameGuidanceView result = source;
	result.motion = {
		.texture = impl.reducedMotion11.get(),
		.format = DXGI_FORMAT_R16G16_FLOAT,
		.metadata = reducedMetadata(source.motion.metadata)
	};
	result.depth = {
		.texture = impl.reducedDepth11.get(),
		.format = DXGI_FORMAT_R32_FLOAT,
		.metadata = reducedMetadata(source.depth.metadata)
	};
	result.confidence = {
		.texture = impl.reducedConfidence11.get(),
		.format = DXGI_FORMAT_R8_UNORM,
		.metadata = reducedMetadata(source.confidence.metadata)
	};
	return result;
}

static bool CompositeResidual(
	DLSSNRFilter::Impl& impl,
	ID3D11Texture2D* output,
	ID3D11ShaderResourceView* reducedDenoised,
	const DLSSNRSettings& settings,
	ID3D11UnorderedAccessView* residualOutput = nullptr,
	ID3D11ShaderResourceView* pairedReducedColor = nullptr,
	ID3D11ShaderResourceView* pairedOriginalColor = nullptr
) noexcept {
	// Even at 100%, input-resolution adjustment is an explicit request to use
	// residual reconstruction.  Bypassing the compute passes at equal extents
	// would silently ignore the residual controls. Prepare them at the native
	// NR extent before interpolation; equal-width input skips the horizontal pass.
	const ResampleConstants constants{
		.sourceWidth = impl.sourceWidth,
		.sourceHeight = impl.sourceHeight,
		.targetWidth = impl.width,
		.targetHeight = impl.height,
		.motionScaleX = float(impl.width) / float(impl.sourceWidth),
		.motionScaleY = float(impl.height) / float(impl.sourceHeight),
		.residualMultiplier = settings.residualMultiplier,
		.residualSaturation = settings.residualSaturation,
		.residualLightness = settings.residualLightness,
		.shadowStructureMultiplier = settings.shadowStructureMultiplier,
		.reflectionGlowMultiplier = settings.reflectionGlowMultiplier,
		.midtoneStrength = settings.residualMidtoneStrength,
		.hueProtection = settings.residualHueProtection,
		.darkProtection = settings.residualDarkProtection,
		.highlightProtection = settings.residualHighlightProtection,
		.localCompression = settings.residualLocalCompression,
		.lowFrequencyGain = settings.residualLowFrequencyGain,
		.detailGain = settings.residualDetailGain,
		.debugView = static_cast<uint32_t>(settings.residualDebugView),
		.chromaProtection = settings.residualChromaProtection,
		.finalizeOutput = impl.finalizeOutput ? 1u : 0u
	};
	if (!impl.residualConstantsValid || impl.cachedResidualConstants != constants) {
		impl.context11->UpdateSubresource(
			impl.residualConstants11.get(), 0, nullptr, &constants, 0, 0);
		impl.cachedResidualConstants = constants;
		impl.residualConstantsValid = true;
	}
	ID3D11ShaderResourceView* prepareSrvs[]{
		pairedReducedColor ? pairedReducedColor : impl.sharedInputSrv11.get(), reducedDenoised
	};
	ID3D11UnorderedAccessView* prepareUav = impl.controlledResidualUav11.get();
	ID3D11Buffer* constantBuffer = impl.residualConstants11.get();
	impl.context11->CSSetShader(
		impl.residualPrepareShader11.get(), nullptr, 0);
	impl.context11->CSSetShaderResources(
		0, ARRAYSIZE(prepareSrvs), prepareSrvs);
	impl.context11->CSSetUnorderedAccessViews(
		0, 1, &prepareUav, nullptr);
	impl.context11->CSSetConstantBuffers(0, 1, &constantBuffer);
	impl.context11->Dispatch(
		(impl.width + 7) / 8, (impl.height + 7) / 8, 1);
	ID3D11ShaderResourceView* nullPrepareSrvs[ARRAYSIZE(prepareSrvs)]{};
	ID3D11UnorderedAccessView* nullUav = nullptr;
	impl.context11->CSSetShaderResources(
		0, ARRAYSIZE(nullPrepareSrvs), nullPrepareSrvs);
	impl.context11->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);

	ID3D11ShaderResourceView* verticalResidual = impl.controlledResidualSrv11.get();
	if (impl.sourceWidth != impl.width) {
		ID3D11ShaderResourceView* horizontalSrv = impl.controlledResidualSrv11.get();
		ID3D11UnorderedAccessView* horizontalUav = impl.resampleIntermediateUav11.get();
		impl.context11->CSSetShader(impl.residualHorizontalShader11.get(), nullptr, 0);
		impl.context11->CSSetShaderResources(0, 1, &horizontalSrv);
		impl.context11->CSSetUnorderedAccessViews(0, 1, &horizontalUav, nullptr);
		impl.context11->Dispatch((impl.sourceWidth + 7) / 8, (impl.height + 7) / 8, 1);
		ID3D11ShaderResourceView* nullSrv = nullptr;
		impl.context11->CSSetShaderResources(0, 1, &nullSrv);
		impl.context11->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
		verticalResidual = impl.resampleIntermediateSrv11.get();
	}
	ID3D11ShaderResourceView* verticalSrvs[]{
		pairedOriginalColor ? pairedOriginalColor : impl.inputSrv11.get(), verticalResidual
	};
	ID3D11UnorderedAccessView* compositeUav = residualOutput ? residualOutput :
		impl.compositeOutputUav11.get();
	impl.context11->CSSetShader(residualOutput ? impl.residualVerticalExtractShader11.get() :
		impl.residualVerticalCompositeShader11.get(), nullptr, 0);
	impl.context11->CSSetShaderResources(
		0, ARRAYSIZE(verticalSrvs), verticalSrvs);
	impl.context11->CSSetUnorderedAccessViews(
		0, 1, &compositeUav, nullptr);
	impl.context11->Dispatch(
		(impl.sourceWidth + 7) / 8, (impl.sourceHeight + 7) / 8, 1);
	ID3D11ShaderResourceView* nullVerticalSrvs[ARRAYSIZE(verticalSrvs)]{};
	ID3D11Buffer* nullBuffer = nullptr;
	impl.context11->CSSetShaderResources(
		0, ARRAYSIZE(nullVerticalSrvs), nullVerticalSrvs);
	impl.context11->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
	impl.context11->CSSetConstantBuffers(0, 1, &nullBuffer);
	impl.context11->CSSetShader(nullptr, nullptr, 0);
	if (!residualOutput && output != impl.compositeOutput11.get()) {
		impl.context11->CopyResource(output, impl.compositeOutput11.get());
	}
	// Offscreen inference leaves edits dirty until a real consumer uploads
	// them. Sparse can rebuild its immutable pair during another inference.
	impl.residualParametersDirty = false;
	return true;
}

DLSSNRFilter::DLSSNRFilter() = default;
DLSSNRFilter::~DLSSNRFilter() { Drain(); _submission.reset(); _sparse.reset(); }

FrameGuidanceRequirements
DLSSNRFilter::GetFrameGuidanceRequirements() const noexcept {
	if (!_impl || _impl->disabled) return {};
	FrameGuidanceRequirements result{ .zero = true };
	result.Add(_settings.motionRequest);
	return result;
}

EffectParameterApplyMode DLSSNRFilter::GetParameterApplyMode(
	std::string_view parameterName
) const noexcept {
	if (!_impl || _impl->disabled) return EffectParameterApplyMode::Unavailable;
	if (IsDLSSNRDisplayParameter(parameterName))
		return EffectParameterApplyMode::Live;
	if (_settings.experimentalHdr.enabled &&
		(parameterName == "enableInputResolutionScaling" || parameterName == "inputResolutionPercent" ||
		 IsDLSSNRResidualParameter(parameterName))) return EffectParameterApplyMode::Restricted;
	if (parameterName == "style" || parameterName == "intensity" ||
		parameterName == "localToneStrength" ||
		parameterName == "localStructureStrength" ||
		parameterName == "skinStructureStrength" ||
		parameterName == "useAutoMask" || parameterName == "uiCorrection") {
		return EffectParameterApplyMode::Live;
	}
	if (IsDLSSNRResidualParameter(parameterName)) {
		return EffectParameterApplyMode::Live;
	}
	return EffectParameterApplyMode::RestartRequired;
}

EffectParameterRestartReason DLSSNRFilter::GetParameterRestartReason(
	std::string_view parameterName
) const noexcept {
	if (IsOpticalFlowParameter(parameterName)) {
		return EffectParameterRestartReason::FrameGuidance;
	}
	return EffectParameterRestartReason::ResourceRecreation;
}

static bool SameNRSettings(const DLSSNRSettings& a, const DLSSNRSettings& b) noexcept {
	return a.style == b.style && a.intensity == b.intensity &&
		a.localToneStrength == b.localToneStrength && a.localStructureStrength == b.localStructureStrength &&
		a.skinStructureStrength == b.skinStructureStrength && a.useAutoMask == b.useAutoMask &&
		a.uiCorrection == b.uiCorrection;
}

bool DLSSNRFilter::ApplyLiveParameters(
	const EffectOption& option, std::span<const std::string> names
) noexcept {
	if (!_impl) return false;
	std::vector<DLSSNRSettings> candidates;
	for (size_t i = 0; i < _passSettings.size(); ++i) {
		auto candidate = ParseDLSSNRSettings(DLSSNRPassOption(option, static_cast<int>(i + 1)),
			_settings.experimentalHdr.enabled);
		if (candidate.modelPreset != _settings.modelPreset ||
			candidate.inputResolutionPercent != _settings.inputResolutionPercent ||
			candidate.processingMode != _settings.processingMode ||
			candidate.residualTransfer != _settings.residualTransfer ||
			candidate.motionRequest != _settings.motionRequest ||
			(SupportsDLSSNRUpdateInterval(_settings.processingMode) &&
				candidate.sparseUpdateInterval != _settings.sparseUpdateInterval)) return false;
		candidates.push_back(candidate);
	}
	for (const auto& name : names) {
		const size_t pass = DLSSNRParameterPass(name) - 1;
		if (pass >= _passSettings.size()) continue; // persisted hidden setting
		const auto base = DLSSNRBaseParameter(name);
		if (GetParameterApplyMode(base) != EffectParameterApplyMode::Live) return false;
	}
	const auto& post = candidates.front();
	const bool residualChanged = post.residualMultiplier != _settings.residualMultiplier ||
		post.residualSaturation != _settings.residualSaturation || post.residualLightness != _settings.residualLightness ||
		post.shadowStructureMultiplier != _settings.shadowStructureMultiplier ||
		post.reflectionGlowMultiplier != _settings.reflectionGlowMultiplier ||
		post.residualMidtoneStrength != _settings.residualMidtoneStrength ||
		post.residualHueProtection != _settings.residualHueProtection ||
		post.residualChromaProtection != _settings.residualChromaProtection ||
		post.residualDarkProtection != _settings.residualDarkProtection ||
		post.residualHighlightProtection != _settings.residualHighlightProtection ||
		post.residualLocalCompression != _settings.residualLocalCompression ||
		post.residualLowFrequencyGain != _settings.residualLowFrequencyGain ||
		post.residualDetailGain != _settings.residualDetailGain ||
		post.residualChromaTemporalStrength != _settings.residualChromaTemporalStrength ||
		post.residualDebugView != _settings.residualDebugView;
	bool inferenceChanged = false;
	for (size_t i = 0; i < _passSettings.size(); ++i)
		inferenceChanged |= !SameNRSettings(candidates[i], _passSettings[i]);
	// Display/residual edits do not touch the worker's immutable NGX settings.
	// An actual NGX parameter edit first takes back feature/history ownership.
	if (!ServiceInferenceSubmission(inferenceChanged)) return false;
	UpdateSparseParameters(inferenceChanged, residualChanged);
	UpdatePerformanceParameters(names, inferenceChanged);
	for (size_t i = 0; i < _passSettings.size(); ++i) {
		if (SameNRSettings(candidates[i], _passSettings[i])) continue;
		Impl& pass = i ? *_impl->laterPasses[i - 1] : *_impl;
		++pass.evaluateParameterRevision;
		for (size_t j = i; j < _passSettings.size(); ++j)
			(j ? *_impl->laterPasses[j - 1] : *_impl).resetHistory = true;
	}
	_passSettings = std::move(candidates);
	_settings = _passSettings.front();
	_impl->residualParametersDirty |= residualChanged;
	if (std::ranges::any_of(names, [](const auto& name) {
		return !IsDLSSNRDisplayParameter(name);
	}))
		InvalidateHotResolution();
	return true;
}

bool DLSSNRFilter::Initialize(
	DeviceResources& resources,
	NgxD3D12Core& ngxCore,
	ID3D11Texture2D* input,
	ID3D11Texture2D* output,
	const DLSSNRSettings& settings
) noexcept {
	return InitializeChain(resources, ngxCore, input, output, { &settings, 1 });
}

bool DLSSNRFilter::InitializeChain(DeviceResources& resources, NgxD3D12Core& ngxCore,
	ID3D11Texture2D* input, ID3D11Texture2D* output,
	std::span<const DLSSNRSettings> passes) noexcept {
	return !passes.empty() && IsDLSSNRSparseProcessingMode(passes.front().processingMode)
		? InitializeSparse(resources, ngxCore, input, output, passes)
		: InitializeChainResources(resources, ngxCore, input, output, passes, false);
}

bool DLSSNRFilter::InitializeChainResources(DeviceResources& resources, NgxD3D12Core& ngxCore,
	ID3D11Texture2D* input, ID3D11Texture2D* output,
	std::span<const DLSSNRSettings> passes, bool deferInitialization) noexcept {
	const auto initializeStart = std::chrono::steady_clock::now();
	auto elapsedMs = [](auto start) {
		return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-start).count();
	};
	double resourcesMs = 0, runtimeMs = 0;
	std::array<double, 3> createMs{};
	_libraryInfo.clear();
	_failure = { DlssNrStage::Initialize, DlssNrReason::Unknown, DlssNrCodeKind::None, 0, "Validate DLSSNR inputs" };
	if (passes.empty() || passes.size() > 3 || !input || !output) return false;
	DLSSNRSettings settings = passes.front();
	settings.enableInputResolutionScaling = true;
	if (settings.modelPreset < 0 || settings.modelPreset > 2) {
		_failure.reason = DlssNrReason::Compatibility;
		_failure.operation = "Invalid DLSSNR model preset";
		return false;
	}
	if (settings.processingMode != DLSSNRProcessingMode::EveryFrame && settings.experimentalHdr.enabled) {
		_failure.reason = DlssNrReason::Compatibility;
		_failure.operation = "DLSSNR performance modes currently require SDR; HDR must be validated separately";
		return false;
	}
	if (!Drain()) return false;
	_submission.reset();
	_sparse.reset();
	_resolutionChange.reset();
	_performance.reset();
	for (const auto& pass : passes) {
		if (pass.modelPreset != settings.modelPreset ||
			pass.inputResolutionPercent != settings.inputResolutionPercent ||
			pass.motionRequest != settings.motionRequest ||
			pass.experimentalHdr.enabled != settings.experimentalHdr.enabled) return false;
	}
	_passSettings.assign(passes.begin(), passes.end());
	_settings = settings;
	_settings.residualMultiplier = ClampFinite(
		_settings.residualMultiplier, 0.0f, 2.0f, 1.0f);
	_settings.residualSaturation = ClampFinite(
		_settings.residualSaturation, 0.0f, 2.0f, 1.0f);
	_settings.residualLightness = ClampFinite(
		_settings.residualLightness, 0.0f, 2.0f, 1.0f);
	_settings.shadowStructureMultiplier = ClampFinite(
		_settings.shadowStructureMultiplier, 0.0f, 2.0f, 1.0f);
	_settings.residualMidtoneStrength = ClampFinite(_settings.residualMidtoneStrength, 0.f, 2.f, 1.f);
	_settings.reflectionGlowMultiplier = ClampFinite(
		_settings.reflectionGlowMultiplier, 0.0f, 2.0f, 1.0f);
	_settings.residualHueProtection = ClampFinite(_settings.residualHueProtection,0.f,1.f,0.f);
	_settings.residualChromaProtection = ClampFinite(_settings.residualChromaProtection,0.f,1.f,0.f);
	_settings.residualDarkProtection = ClampFinite(_settings.residualDarkProtection,0.f,1.f,0.f);
	_settings.residualHighlightProtection = ClampFinite(_settings.residualHighlightProtection,0.f,1.f,0.f);
	_settings.residualLocalCompression = ClampFinite(_settings.residualLocalCompression,0.f,1.f,0.f);
	_settings.residualLowFrequencyGain = ClampFinite(_settings.residualLowFrequencyGain,0.f,2.f,1.f);
	_settings.residualDetailGain = ClampFinite(_settings.residualDetailGain,0.f,2.f,1.f);
	_settings.residualChromaTemporalStrength = 0.f;
	_settings.residualDebugView = std::clamp(_settings.residualDebugView,0,7);
	_settings.intensity = ClampFinite(
		_settings.intensity, 0.0f, 2.0f, 1.0f);
	_settings.localToneStrength = ClampFinite(
		_settings.localToneStrength, 0.0f, 2.0f, 1.0f);
	_settings.localStructureStrength = ClampFinite(
		_settings.localStructureStrength, 0.0f, 2.0f, 1.0f);
	_passSettings.front() = _settings;
	for (auto& pass : _passSettings) {
		pass.enableInputResolutionScaling = true;
		pass.style = std::clamp(pass.style, 0, 2);
		pass.intensity = ClampFinite(pass.intensity, 0.0f, 2.0f, 1.0f);
		pass.localToneStrength = ClampFinite(pass.localToneStrength, 0.0f, 2.0f, 1.0f);
		pass.localStructureStrength = ClampFinite(pass.localStructureStrength, 0.0f, 2.0f, 1.0f);
		pass.skinStructureStrength = ClampFinite(pass.skinStructureStrength, 0.0f, 2.0f, 0.0f);
	}
	_settings = _passSettings.front();
	_ngxCore = &ngxCore;
	_impl.reset();
	FrameGuidancePerformance::ResetDlssnrGpuTiming();
	auto impl = std::make_unique<Impl>();
	impl->failure = _failure;
	const auto captureFailure = wil::scope_exit([&] { if (impl) { _failure = impl->failure; _libraryInfo = impl->libraryInfo; } });
	impl->device11 = resources.GetD3DDevice();
	impl->context11 = resources.GetD3DDC();
	impl->coreOwner = &ngxCore;

	D3D11_TEXTURE2D_DESC inputDesc{};
	D3D11_TEXTURE2D_DESC outputDesc{};
	input->GetDesc(&inputDesc);
	output->GetDesc(&outputDesc);
	const bool experimentalHdrPath = settings.experimentalHdr.enabled &&
		settings.experimentalHdr.IsVerifiedScale() &&
		inputDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT &&
		outputDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT;
	if (inputDesc.Width != outputDesc.Width || inputDesc.Height != outputDesc.Height) {
		Logger::Get().Error(fmt::format(
			"DLSSNR requires same-resolution input/output: {}x{} -> {}x{}",
			inputDesc.Width, inputDesc.Height, outputDesc.Width, outputDesc.Height));
		return false;
	}
	const bool supportedInput = inputDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
		inputDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM;
	const bool sdrOutput = outputDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
		outputDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT;
	if ((!supportedInput || !sdrOutput) &&
		!experimentalHdrPath) {
		Logger::Get().Error(fmt::format(
			"DLSSNR SDR path unsupported formats: input={}, output={}",
			(uint32_t)inputDesc.Format, (uint32_t)outputDesc.Format));
		return false;
	}
	impl->modelPreset = settings.modelPreset;
	impl->sourceWidth = inputDesc.Width;
	impl->sourceHeight = inputDesc.Height;
	impl->experimentalHdrPath = experimentalHdrPath;
	impl->experimentalHdrScale = settings.experimentalHdr.scale;
	// SDR always reconstructs the total residual, including at 100%. Its
	// optional FP16 output is encoded SDR pending the temporal/final gamut pass.
	// The experimental HDR route keeps its same-resolution color contract.
	impl->useResolutionScaling = !experimentalHdrPath;
	const uint32_t resolutionPercent = std::clamp(settings.inputResolutionPercent, 25u, 100u);
	impl->width = impl->useResolutionScaling ? std::max(
		1u, static_cast<uint32_t>(std::lround(
			double(inputDesc.Width) * double(resolutionPercent) / 100.0))) :
		inputDesc.Width;
	impl->height = impl->useResolutionScaling ? std::max(
		1u, static_cast<uint32_t>(std::lround(
			double(inputDesc.Height) * double(resolutionPercent) / 100.0))) :
		inputDesc.Height;
	impl->convertInputToRgba = inputDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM;

	auto coreFailure = [&](DlssNrStage stage) {
		const auto& failure = ngxCore.LastFailure();
		const auto kind = failure.kind == NgxCallFailure::Kind::Seh ? DlssNrCodeKind::Seh :
			failure.kind == NgxCallFailure::Kind::Ngx ? DlssNrCodeKind::Ngx :
			failure.kind == NgxCallFailure::Kind::HResult ? DlssNrCodeKind::HResult :
			failure.kind == NgxCallFailure::Kind::Win32 ? DlssNrCodeKind::Win32 : DlssNrCodeKind::None;
		return DlssNrApiFailure(stage, kind, failure.code, failure.operation);
	};
	impl->failure.operation = "Acquire NGX D3D12 Core";
	const auto acquireStart = std::chrono::steady_clock::now();
	if (!ngxCore.Acquire(resources, "DLSSNR")) {
		impl->failure = coreFailure(DlssNrStage::Initialize);
		return false;
	}
	runtimeMs += elapsedMs(acquireStart);
	const auto graphicsStart = std::chrono::steady_clock::now();
	impl->coreRegistered = true;
	impl->device12.copy_from(ngxCore.Device());
	impl->failure.operation = "Create DLSSNR graphics resources";
	HRESULT hr = S_OK;
	D3D12_COMMAND_QUEUE_DESC queueDesc{};
	queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	queueDesc.Priority = _foregroundQueue ? D3D12_COMMAND_QUEUE_PRIORITY_HIGH : D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
	hr = impl->device12->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(impl->queue12.put()));
	if (FAILED(hr) && _foregroundQueue) {
		queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
		hr = impl->device12->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(impl->queue12.put()));
		Logger::Get().Warn("DLSSNR foreground queue uses normal priority; latency requires measurement");
	}
	if (SUCCEEDED(hr)) {
		hr = impl->device12->CreateCommandAllocator(
			D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(impl->allocator12.put()));
	}
	if (SUCCEEDED(hr)) {
		hr = impl->device12->CreateCommandList(
			0, D3D12_COMMAND_LIST_TYPE_DIRECT, impl->allocator12.get(), nullptr,
			IID_PPV_ARGS(impl->commandList12.put()));
	}
	if (FAILED(hr)) {
		impl->failure = DlssNrApiFailure(DlssNrStage::Initialize, DlssNrCodeKind::HResult, static_cast<uint32_t>(hr), "Create DLSSNR D3D12 command objects failed");
		Logger::Get().ComError("Create DLSSNR D3D12 command objects failed", hr);
		return false;
	}

	D3D11_TEXTURE2D_DESC sharedDesc = outputDesc;
	sharedDesc.Format = experimentalHdrPath ?
		DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
	sharedDesc.Width = impl->width;
	sharedDesc.Height = impl->height;
	if (!CreateSharedTexture(*impl, sharedDesc, true,
		impl->sharedInput11, impl->sharedInput12) ||
		!CreateSharedTexture(*impl, sharedDesc, true,
			impl->sharedOutput11, impl->sharedOutput12)) {
		return false;
	}
	if (impl->useResolutionScaling) {
		if (!CreateResolutionScalingResources(
			*impl, input, output, outputDesc)) {
			return false;
		}
	} else if (impl->convertInputToRgba) {
		hr = impl->device11->CreateShaderResourceView(
			input, nullptr, impl->inputSrv11.put());
		if (SUCCEEDED(hr)) {
			hr = impl->device11->CreateUnorderedAccessView(
				impl->sharedInput11.get(), nullptr, impl->sharedInputUav11.put());
		}
		if (SUCCEEDED(hr) && !CreateComputeShader(*impl, COLOR_CONVERT_HLSL,
			"ConvertToRgba", "DLSSNRColorConvert", impl->colorConvertShader11)) hr = E_FAIL;
		if (FAILED(hr)) {
			impl->failure = DlssNrApiFailure(DlssNrStage::Initialize, DlssNrCodeKind::HResult, static_cast<uint32_t>(hr), "Create DLSSNR BGRA conversion resources failed");
			Logger::Get().ComError("Create DLSSNR BGRA conversion resources failed", hr);
			return false;
		}
	}

	hr = impl->device11->CreateFence(
		0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(impl->fence11.put()));
	if (FAILED(hr)) {
		impl->failure = DlssNrApiFailure(DlssNrStage::Initialize, DlssNrCodeKind::HResult, static_cast<uint32_t>(hr), "Create DLSSNR shared fence failed");
		Logger::Get().ComError("Create DLSSNR shared fence failed", hr);
		return false;
	}
	HANDLE rawFence = nullptr;
	hr = impl->fence11->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &rawFence);
	if (FAILED(hr)) {
		impl->failure = DlssNrApiFailure(DlssNrStage::Initialize, DlssNrCodeKind::HResult, static_cast<uint32_t>(hr), "Create DLSSNR fence shared handle failed");
		Logger::Get().ComError("Create DLSSNR fence shared handle failed", hr);
		return false;
	}
	wil::unique_handle fenceHandle(rawFence);
	hr = impl->device12->OpenSharedHandle(
		fenceHandle.get(), IID_PPV_ARGS(impl->fence12.put()));
	if (FAILED(hr)) {
		impl->failure = DlssNrApiFailure(DlssNrStage::Initialize, DlssNrCodeKind::HResult, static_cast<uint32_t>(hr), "Open DLSSNR shared fence in D3D12 failed");
		Logger::Get().ComError("Open DLSSNR shared fence in D3D12 failed", hr);
		return false;
	}
	hr = impl->fenceEvent.create();
	if (FAILED(hr)) {
		impl->failure = DlssNrApiFailure(DlssNrStage::Initialize, DlssNrCodeKind::HResult, static_cast<uint32_t>(hr), "Create reusable DLSSNR fence event failed");
		Logger::Get().ComError("Create reusable DLSSNR fence event failed", hr);
		return false;
	}

	{
		D3D12_QUERY_HEAP_DESC queryDesc{};
		queryDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
		queryDesc.Count = Impl::COMMAND_SLOT_COUNT * Impl::TIMESTAMP_STRIDE;
		hr = impl->device12->CreateQueryHeap(
			&queryDesc, IID_PPV_ARGS(impl->timestampQueryHeap.put()));
		D3D12_HEAP_PROPERTIES readbackHeap{};
		readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;
		readbackHeap.CreationNodeMask = 1;
		readbackHeap.VisibleNodeMask = 1;
		D3D12_RESOURCE_DESC readbackDesc{};
		readbackDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		readbackDesc.Width = uint64_t(queryDesc.Count) * sizeof(uint64_t);
		readbackDesc.Height = 1;
		readbackDesc.DepthOrArraySize = 1;
		readbackDesc.MipLevels = 1;
		readbackDesc.SampleDesc.Count = 1;
		readbackDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		if (SUCCEEDED(hr)) {
			hr = impl->device12->CreateCommittedResource(
				&readbackHeap, D3D12_HEAP_FLAG_NONE, &readbackDesc,
				D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
				IID_PPV_ARGS(impl->timestampReadback.put()));
		}
		if (SUCCEEDED(hr)) {
			hr = impl->queue12->GetTimestampFrequency(&impl->timestampFrequency);
		}
		if (FAILED(hr) || !impl->timestampFrequency) {
			Logger::Get().Warn(fmt::format(
				"DLSSNR GPU timestamp telemetry unavailable ({:#x})",
				static_cast<uint32_t>(hr)));
			impl->timestampQueryHeap = nullptr;
			impl->timestampReadback = nullptr;
			impl->timestampFrequency = 0;
		}
	}

	const std::filesystem::path applicationDirectory =
		Win32Helper::GetExePath().parent_path();
	DWORD sehCode = 0;
	NVSDK_NGX_Result result = NVSDK_NGX_Result_Success;
	resourcesMs += elapsedMs(graphicsStart);

	if constexpr (!ENABLE_CORE_FEATURE18_DIAGNOSTIC) {
		const auto runtimeStart = std::chrono::steady_clock::now();
		impl->failure = { DlssNrStage::InitializeLibrary, DlssNrReason::Unknown, DlssNrCodeKind::None, 0, "Acquire signed snippet session" };
		if (!InitializeSignedSnippet(*impl, applicationDirectory)) return false;
		runtimeMs += elapsedMs(runtimeStart);
	}

	const auto parametersStart = std::chrono::steady_clock::now();
	impl->failure = { DlssNrStage::CreateFeature, DlssNrReason::Unknown, DlssNrCodeKind::None, 0, "Allocate NGX parameters" };
	if (!ngxCore.AllocateParameters(&impl->parameters, "DLSSNR")) {
		impl->failure = coreFailure(DlssNrStage::CreateFeature);
		return false;
	}
	sehCode = 0;
	if (!SetCreateParametersSafely(*impl, &sehCode)) {
		impl->failure = DlssNrApiFailure(DlssNrStage::CreateFeature, DlssNrCodeKind::Seh, sehCode, "SetCreateParameters");
		Logger::Get().Error(fmt::format(
			"DLSSNR creation parameter setup raised SEH {:#x}", sehCode));
		return false;
	}

	const Impl::CreateFeatureFn createFeature =
		ENABLE_CORE_FEATURE18_DIAGNOSTIC ?
		static_cast<Impl::CreateFeatureFn>(&NVSDK_NGX_D3D12_CreateFeature) :
		impl->snippetCreateFeature;
	sehCode = 0;
	resourcesMs += elapsedMs(parametersStart);
	const auto firstCreateStart = std::chrono::steady_clock::now();
	result = CallCreateFeatureSafely(
		createFeature, impl->commandList12.get(), FEATURE_DLSSNR,
		impl->parameters, &impl->feature, &sehCode);
	createMs[0] = elapsedMs(firstCreateStart);
	if (sehCode) {
		Logger::Get().Error(fmt::format(
			"DLSSNR {} CreateFeature raised SEH {:#x}",
			ENABLE_CORE_FEATURE18_DIAGNOSTIC ? "Core diagnostic" : "signed snippet",
			sehCode));
		impl->failure = DlssNrApiFailure(DlssNrStage::CreateFeature, DlssNrCodeKind::Seh, sehCode, "CreateFeature(18), pass 1");
		return false;
	}
	if (!NGXSucceeded(result) || !impl->feature) {
		impl->failure = DlssNrApiFailure(DlssNrStage::CreateFeature, DlssNrCodeKind::Ngx, static_cast<uint32_t>(result), "CreateFeature(18), pass 1");
		Logger::Get().Error(fmt::format(
			"DLSSNR {} Feature 18 creation failed ({:#x})",
			ENABLE_CORE_FEATURE18_DIAGNOSTIC ? "Core diagnostic" : "signed snippet",
			(uint32_t)result));
		return false;
	}

	for (size_t i = 1; i < passes.size(); ++i) {
		const auto childResourcesStart = std::chrono::steady_clock::now();
		auto child = std::make_unique<Impl>();
		child->coreOwner = &ngxCore;
		child->device11 = impl->device11;
		child->device12 = impl->device12;
		child->width = impl->width;
		child->height = impl->height;
		child->modelPreset = settings.modelPreset;
		child->sharedInput11 = i == 1 ? impl->sharedOutput11 : impl->laterPasses.back()->sharedOutput11;
		child->sharedInput12 = i == 1 ? impl->sharedOutput12 : impl->laterPasses.back()->sharedOutput12;
		child->snippetSession = impl->snippetSession;
		child->snippetReleaseFeature = impl->snippetReleaseFeature;
		child->snippetEvaluateFeature = impl->snippetEvaluateFeature;
		child->useSignedSnippet = impl->useSignedSnippet;
		if (!CreateSharedTexture(*impl, sharedDesc, true,
			child->sharedOutput11, child->sharedOutput12)) return false;
		if (impl->useResolutionScaling && i + 1 == passes.size() && FAILED(impl->device11->CreateShaderResourceView(
			child->sharedOutput11.get(), nullptr, child->sharedOutputSrv11.put()))) return false;
		if (!ngxCore.AllocateParameters(&child->parameters, "DLSSNR")) {
			impl->failure = coreFailure(DlssNrStage::CreateFeature);
			return false;
		}
		sehCode = 0;
		if (!SetCreateParametersSafely(*child, &sehCode)) {
			impl->failure = DlssNrApiFailure(DlssNrStage::CreateFeature, DlssNrCodeKind::Seh, sehCode, "SetCreateParameters, later pass");
			return false;
		}
		resourcesMs += elapsedMs(childResourcesStart);
		const auto childCreateStart = std::chrono::steady_clock::now();
		result = CallCreateFeatureSafely(createFeature, impl->commandList12.get(), FEATURE_DLSSNR,
			child->parameters, &child->feature, &sehCode);
		createMs[i] = elapsedMs(childCreateStart);
		if (sehCode || !NGXSucceeded(result) || !child->feature) {
			impl->failure = DlssNrApiFailure(DlssNrStage::CreateFeature, sehCode ? DlssNrCodeKind::Seh : DlssNrCodeKind::Ngx,
				sehCode ? sehCode : static_cast<uint32_t>(result), fmt::format("CreateFeature(18), pass {}", i + 1));
			Logger::Get().Error(fmt::format("DLSSNR pass {} creation failed ({:#x}), SEH={:#x}",
				i + 1, static_cast<uint32_t>(result), sehCode));
			return false;
		}
		impl->laterPasses.push_back(std::move(child));
	}

	hr = impl->commandList12->Close();
	if (FAILED(hr)) {
		impl->failure = DlssNrApiFailure(DlssNrStage::Initialize, DlssNrCodeKind::HResult, static_cast<uint32_t>(hr), "Close DLSSNR initialization command list failed");
		Logger::Get().ComError("Close DLSSNR initialization command list failed", hr);
		return false;
	}
	ID3D12CommandList* lists[]{ impl->commandList12.get() };
	impl->queue12->ExecuteCommandLists(1, lists);
	impl->initializationValue = ++impl->fenceValue;
	impl->initializationSubmittedAt = std::chrono::steady_clock::now();
	if (FAILED(impl->queue12->Signal(impl->fence12.get(), impl->initializationValue)) ||
		(!deferInitialization && !WaitForFence(*impl, impl->initializationValue))) {
		return false;
	}
	impl->initializationObserved = !deferInitialization;
	if (!deferInitialization) impl->initializationWaitMs = elapsedMs(impl->initializationSubmittedAt);
	const auto finalResourcesStart = std::chrono::steady_clock::now();
	for (uint32_t i = 0; i < impl->commandSlots.size(); ++i) {
		Impl::CommandSlot& slot = impl->commandSlots[i];
		slot.timestampQuery = i * Impl::TIMESTAMP_STRIDE;
		hr = impl->device12->CreateCommandAllocator(
			D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(slot.allocator.put()));
		if (SUCCEEDED(hr)) {
			hr = impl->device12->CreateCommandList(
				0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot.allocator.get(), nullptr,
				IID_PPV_ARGS(slot.commandList.put()));
		}
		if (SUCCEEDED(hr)) hr = slot.commandList->Close();
		if (FAILED(hr)) {
			impl->failure = DlssNrApiFailure(DlssNrStage::Initialize, DlssNrCodeKind::HResult, static_cast<uint32_t>(hr), "Create DLSSNR command ring failed");
			Logger::Get().ComError("Create DLSSNR command ring failed", hr);
			return false;
		}
	}
	impl->guidanceInterop = std::make_shared<FrameGuidanceD3D12Interop>();
	if (!impl->guidanceInterop->Initialize(
		impl->device12.get(), impl->fence12.get())) {
		return false;
	}

	for (const auto& child : impl->laterPasses) child->guidanceInterop = impl->guidanceInterop;
	Logger::Get().Info(fmt::format("DLSSNR chain: passes={} queues=1 fences=1 slots={} intermediates={}x{} residual=total",
		passes.size(), Impl::COMMAND_SLOT_COUNT, impl->width, impl->height));

	LogDlssnrStatus(fmt::format(
		"DLSSNR STATUS: Feature=18 created=true path={} sourceSize={}x{} sourceFormat={} "
		"colorDownsample=lanczos2-aa residualControls=before-upsample residualUpsample=catmull-rom-4+4 inputSize={}x{} inputResolutionScaling={} inputResolutionPercent={} residualMultiplier={} "
		"residualSaturation={} residualLightness={} shadowStructureMultiplier={} "
		"reflectionGlowMultiplier={} requestedPreset={} resolvedPreset=unknown resolvedModel=unknown "
		"style={} intensity={} localTone={} localStructure={} skinStructure={} "
		"opticalFlowMethod={} opticalFlowQuality={} autoMask={} uiCorrection={} depth=zero-contract disabled=false "
		"experimentalHdrPath={} experimentalHdrScale={}",
		ENABLE_CORE_FEATURE18_DIAGNOSTIC ? "core-diagnostic" : "signed-snippet",
		impl->sourceWidth, impl->sourceHeight, static_cast<uint32_t>(inputDesc.Format),
		impl->width, impl->height,
		impl->useResolutionScaling, _settings.inputResolutionPercent,
		_settings.residualMultiplier,
		_settings.residualSaturation, _settings.residualLightness,
		_settings.shadowStructureMultiplier, _settings.reflectionGlowMultiplier, _settings.modelPreset,
		_settings.style,
		_settings.intensity, _settings.localToneStrength,
		_settings.localStructureStrength, _settings.skinStructureStrength,
		static_cast<uint32_t>(_settings.motionRequest.method),
		static_cast<uint32_t>(_settings.motionRequest.quality),
		_settings.useAutoMask, _settings.uiCorrection,
		impl->experimentalHdrPath, impl->experimentalHdrScale));
	_failure = {};
	_libraryInfo = impl->libraryInfo;
	if (!impl->foregroundTimer.Initialize(impl->device11) || FAILED(impl->device11->CreateFence(
		0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(impl->retirementFence.put())))) return false;
	resourcesMs += elapsedMs(finalResourcesStart);
	Logger::Get().Info(fmt::format(
		"DLSSNR initialize timings: size={}x{} passes={} resourcesMs={:.3f} runtimeMs={:.3f} createMs=[{:.3f},{:.3f},{:.3f}] initGpuWaitMs={:.3f} deferredGpuWait={} cpuPrepareMs={:.3f}",
		impl->width, impl->height, passes.size(), resourcesMs, runtimeMs,
		createMs[0], createMs[1], createMs[2], impl->initializationWaitMs,
		deferInitialization, elapsedMs(initializeStart)));
	_impl = std::move(impl);
	return settings.processingMode == DLSSNRProcessingMode::EveryFrame ||
		InitializePerformance(input, output);
}

bool DLSSNRFilter::Resize(
	DeviceResources& resources,
	ID3D11Texture2D* input,
	ID3D11Texture2D* output
) noexcept {
	const auto settings = _passSettings;
	return _ngxCore && InitializeChain(resources, *_ngxCore, input, output, settings);
}

bool DLSSNRFilter::Drain() noexcept {
	// CPU NGX calls must finish before queue drains or any resource destruction.
	// A failed submission still has to retire accepted GPU work during teardown.
	const bool submissionHealthy = ServiceInferenceSubmission(true);
	if (_sparse && !DrainSparse()) return false;
	if (_resolutionChange) {
		for (auto* branch : { _resolutionChange->full.get(), _resolutionChange->retiredFull.get(),
			_resolutionChange->hotFull.get() })
			if (branch && !branch->Drain()) return false;
	}
	if (_impl && _impl->queue12 && _impl->fence12 && !WaitForQueue(*_impl)) return false;
	if (_performance) {
		auto& p = *_performance;
		const auto value = ++p.poolFenceValue;
		wil::unique_event_nothrow retired;
		if (FAILED(retired.create()) || FAILED(p.composition.context11->Signal(p.poolFence.get(), value)) ||
			FAILED(p.poolFence->SetEventOnCompletion(value, retired.get()))) return false;
		p.composition.context11->Flush();
		// Stop/resize may wait; normal producer/consumer progress never does.
		if (WaitForSingleObject(retired.get(), INFINITE) != WAIT_OBJECT_0) return false;
	}
	return submissionHealthy;
}

static FrameGuidanceView SelectGuidance(
	const NativeEffectDrawContext& context,
	const DLSSNRSettings& settings,
	FrameGuidanceExtent extent
) noexcept {
	return SelectFrameGuidanceChannels(
		context.frameGuidance, context.zeroFrameGuidance,
		context.frameId, extent,
		settings.motionRequest.method != OpticalFlowMethod::None);
}

// Resource identity, generations, validity and synchronization are part of reuse.
static bool SameGuidance(const FrameGuidanceView& a, const FrameGuidanceView& b,
	bool constantContents = false) noexcept {
	if (a.motionDirection != b.motionDirection || a.motionUnit != b.motionUnit ||
		(!constantContents && a.requiresHistoryReset != b.requiresHistoryReset)) return false;
	const FrameGuidanceResource* left[]{ &a.motion, &a.depth, &a.confidence };
	const FrameGuidanceResource* right[]{ &b.motion, &b.depth, &b.confidence };
	for (size_t i = 0; i < 3; ++i) {
		const auto& x = left[i]->metadata;
		const auto& y = right[i]->metadata;
		if (left[i]->texture != right[i]->texture || left[i]->format != right[i]->format ||
			x.resourceGeneration != y.resourceGeneration || x.sourceExtent != y.sourceExtent ||
			x.validRegion != y.validRegion || x.valid != y.valid || x.isZero != y.isZero ||
			x.sync.fence != y.sync.fence || x.sync.value != y.sync.value) return false;
		if (!constantContents && (x.frameId != y.frameId || x.captureSequence != y.captureSequence ||
			x.timestamp100ns != y.timestamp100ns || x.requiresHistoryReset != y.requiresHistoryReset ||
			x.resetReason != y.resetReason)) return false;
	}
	return true;
}

static void TransitionColor(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
	D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) noexcept {
	D3D12_RESOURCE_BARRIER barrier{};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition = { resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after };
	list->ResourceBarrier(1, &barrier);
}

bool DLSSNRFilter::Draw(const NativeEffectDrawContext& context) noexcept {
	if (!_impl) return false;
	auto* dc = _impl->context11;
	{
		FrameTrace::NativeNrScope trace(FrameTrace::NativeNrStage::FilterTimingBegin, context.frameId);
		_impl->foregroundTimer.Poll(dc);
		if (context.isNewCaptureFrame) _impl->foregroundTimer.Begin(dc);
	}
	bool result = false;
	{
		FrameTrace::NativeNrScope trace(FrameTrace::NativeNrStage::FilterDraw, context.frameId);
		result = _sparse ? DrawSparse(context) : _performance ? DrawPerformance(context) : DrawInference(context);
	}
	if (context.isNewCaptureFrame) {
		FrameTrace::NativeNrScope trace(FrameTrace::NativeNrStage::FilterTimingEnd, context.frameId);
		_impl->foregroundTimer.End(dc);
	}
	HRESULT retired;
	{
		FrameTrace::NativeNrScope trace(FrameTrace::NativeNrStage::FilterRetirementSignal, context.frameId);
		retired = dc->Signal(_impl->retirementFence.get(), ++_impl->retirementValue);
		trace.Result(retired);
	}
	return SUCCEEDED(retired) && result;
}

void DLSSNRFilter::Submission::Execute(Job& job) noexcept {
	Impl& impl = *job.impl;
	std::unique_lock<std::mutex> sessionLock;
	if (impl.useSignedSnippet && impl.snippetSession) {
		// Signed features share runtime CPU/GPU state. Preserve the original
		// record/Evaluate/submit order across workers, without a CPU fence wait.
		sessionLock = std::unique_lock(impl.snippetSession->snippetEvaluateMutex);
	}
	auto& slot = impl.commandSlots[job.slot];
	const size_t first = job.first, count = job.count;
	auto passAt = [&](size_t i) -> Impl& { return i ? *impl.laterPasses[i - 1] : impl; };
	auto fail = [&](std::string_view stage) noexcept {
		if (job.failure.operation.empty()) job.failure = {DlssNrStage::Evaluate, DlssNrReason::Unknown,
			DlssNrCodeKind::None, 0, std::string(stage)};
	};
	if (FAILED(impl.queue12->Wait(impl.fence12.get(), job.inputReady))) { fail("input-wait"); return; }
	HRESULT hr = slot.allocator->Reset();
	if (SUCCEEDED(hr)) hr = slot.commandList->Reset(slot.allocator.get(), nullptr);
	if (FAILED(hr)) { fail("list-reset"); return; }
	auto* list = slot.commandList.get();
	impl.guidanceInterop->Transition(list, D3D12_RESOURCE_STATE_COMMON,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	TransitionColor(list, passAt(first).sharedInput12.get(), D3D12_RESOURCE_STATE_COMMON,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	if (impl.timestampQueryHeap) list->EndQuery(impl.timestampQueryHeap.get(),
		D3D12_QUERY_TYPE_TIMESTAMP, slot.timestampQuery);
	const auto evaluateStart = NativeBackendTiming::Now();
	bool success = true;
	for (size_t i = first; i < count; ++i) {
		Impl& pass = passAt(i);
		TransitionColor(list, pass.sharedOutput12.get(), D3D12_RESOURCE_STATE_COMMON,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		const bool reset = job.guidanceReset[i];
		DWORD seh = 0;
		if (!SetEvaluateParametersSafely(pass, job.settings[i], job.guidance, job.resetHistory[i] || reset, &seh)) {
			job.failure = DlssNrApiFailure(DlssNrStage::Evaluate, DlssNrCodeKind::Seh, seh, "SetEvaluateParameters");
			list->Close();
			fail("parameters-seh"); return;
		}
		const uint32_t query = slot.timestampQuery + 2 + static_cast<uint32_t>(i) * 2;
		if (impl.timestampQueryHeap) list->EndQuery(impl.timestampQueryHeap.get(), D3D12_QUERY_TYPE_TIMESTAMP, query);
		const auto function = pass.useSignedSnippet ? pass.snippetEvaluateFeature :
			static_cast<Impl::EvaluateFeatureFn>(&NVSDK_NGX_D3D12_EvaluateFeature);
		const auto result = CallEvaluateFeatureSafely(function, list, pass.feature, pass.parameters, &seh);
		job.attempted[i] = true;
		const bool evaluated = !seh && NGXSucceeded(result);
		job.evaluated[i] = evaluated;
		if (!evaluated || job.evaluateNumbers[i] == 1) Logger::Get().Info(fmt::format(
			"DLSSNR pass {}: frame={} result={:#x} SEH={:#x}", i + 1, job.frame, static_cast<uint32_t>(result), seh));
		if (impl.timestampQueryHeap) {
			list->EndQuery(impl.timestampQueryHeap.get(), D3D12_QUERY_TYPE_TIMESTAMP, query + 1);
			list->ResolveQueryData(impl.timestampQueryHeap.get(), D3D12_QUERY_TYPE_TIMESTAMP, query, 2,
				impl.timestampReadback.get(), uint64_t(query) * sizeof(uint64_t));
		}
		TransitionColor(list, pass.sharedInput12.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
			D3D12_RESOURCE_STATE_COMMON);
		TransitionColor(list, pass.sharedOutput12.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			evaluated && i + 1 < count ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_COMMON);
		if (!evaluated) {
			job.failure = DlssNrApiFailure(DlssNrStage::Evaluate, seh ? DlssNrCodeKind::Seh : DlssNrCodeKind::Ngx,
				seh ? seh : static_cast<uint32_t>(result), fmt::format("EvaluateFeature(18), pass {}", i + 1));
			success = false; break;
		}
	}
	job.evaluateMs = NativeBackendTiming::ElapsedMilliseconds(evaluateStart);
	impl.guidanceInterop->Transition(list, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
	if (impl.timestampQueryHeap) {
		list->EndQuery(impl.timestampQueryHeap.get(), D3D12_QUERY_TYPE_TIMESTAMP, slot.timestampQuery + 1);
		list->ResolveQueryData(impl.timestampQueryHeap.get(), D3D12_QUERY_TYPE_TIMESTAMP, slot.timestampQuery, 2,
			impl.timestampReadback.get(), uint64_t(slot.timestampQuery) * sizeof(uint64_t));
	}
	const auto submitStart = NativeBackendTiming::Now();
	if (FAILED(list->Close())) { fail("list-close"); return; }
	// NGX-owned resources are shared across signed-NR features. A session
	// completion ticket orders Evaluate GPU execution across their queues;
	// it does not serialize unrelated NGX consumers or add a CPU wait.
	hr = SubmitDlssnrEvaluation(impl, list, job.outputReady);
	job.submitted = SUCCEEDED(hr);
	job.completedPassCount = success ? static_cast<uint32_t>(count) : static_cast<uint32_t>(first);
	job.submitMs = NativeBackendTiming::ElapsedMilliseconds(submitStart);
	if (FAILED(hr)) { fail("output-signal"); return; }
	if (job.completionEvent && FAILED(impl.fence12->SetEventOnCompletion(job.outputReady, job.completionEvent))) {
		fail("output-completion-event"); return;
	}
	if (!success) { fail("evaluate"); return; }
}

bool DLSSNRFilter::Submission::Complete(DLSSNRFilter& owner) noexcept {
	auto& impl = *job.impl;
	auto& slot = impl.commandSlots[job.slot];
	for (size_t i = job.first; i < job.count; ++i) {
		auto& pass = i ? *impl.laterPasses[i - 1] : impl;
		if (job.attempted[i]) {
			++pass.evaluateCount;
			if (job.evaluated[i]) ++pass.evaluateSuccessCount; else ++pass.evaluateFailureCount;
		}
		if (job.evaluated[i]) {
			if (job.guidanceReset[i]) pass.lastGuidanceResetFrameId = job.frame;
			pass.resetHistory = false;
		}
	}
	slot.timestampPending = job.submitted && impl.timestampQueryHeap != nullptr;
	slot.timestampFrameId = job.frame;
	slot.timestampFirstPass = static_cast<uint32_t>(job.first);
	slot.timestampPassCount = job.completedPassCount;
	slot.completionValue = job.submitted ? job.outputReady : 0;
	if (!job.failure.operation.empty()) {
		// A recording/queue failure may have no output signal at all. Teardown
		// drains accepted GPU work with a fresh queue ticket after CPU retirement.
		if (!job.submitted) impl.pendingInferenceValue = 0;
		if (owner._failure.operation.empty()) owner._failure = job.failure;
		impl.disabled = true;
		impl.cache.valid = false;
		LogDlssnrStatus(fmt::format("DLSSNR chain failed: frame={} stage={} rejecting complete output",
			job.frame, job.failure.operation), true);
		return false;
	}
	impl.cachedGuidance = job.sourceGuidance;
	impl.cachedInputHistoryRevision = job.historyRevision;
	impl.cache.Commit(job.frame, job.inputRevision, {job.revisions.data(), job.count});
	if constexpr (NativeBackendTiming::Enabled) {
		impl.slotWaitTimings.Add(job.waitMs);
		impl.inputPrepareTimings.Add(job.inputMs);
		impl.guidancePrepareTimings.Add(job.guidanceMs);
		impl.evaluateCpuTimings.Add(job.evaluateMs);
		impl.submitTimings.Add(job.submitMs);
		if (impl.evaluateCount <= 8 || impl.evaluateCount % 120 == 0) Logger::Get().Info(fmt::format(
			"DLSSNR chain timing: queue={} input={}x{} frame={} firstPass={} {} {} {} {} {} {}",
			job.foregroundQueue ? "lightweight" : "full", impl.width, impl.height,
			job.frame, job.first + 1, FormatTimingSummary("slotWait", impl.slotWaitTimings.Summarize()),
			FormatTimingSummary("inputPrepare", impl.inputPrepareTimings.Summarize()),
			FormatTimingSummary("guidancePrepare", impl.guidancePrepareTimings.Summarize()),
			FormatTimingSummary("evaluateCPU", impl.evaluateCpuTimings.Summarize()),
			FormatTimingSummary("submit", impl.submitTimings.Summarize()),
			FormatTimingSummary("evaluateGPU", impl.evaluateGpuTimings.Summarize())));
	}
	return true;
}

bool DLSSNRFilter::InferenceSubmissionPending() const noexcept {
	return _submission && _submission->pending;
}

bool DLSSNRFilter::ServiceInferenceSubmission(bool wait) noexcept {
	if (!InferenceSubmissionPending()) return true;
	auto& submission = *_submission;
	if (wait) submission.worker.Wait();
	else if (!submission.worker.TryTakeCompletion()) return true;
	submission.pending = false;
	return submission.Complete(*this);
}

bool DLSSNRFilter::DrawInference(const NativeEffectDrawContext& context,
	bool background, bool compositeOutput, ID3D11Texture2D* preparedInput) noexcept {
	if (!ServiceInferenceSubmission(true)) return false;
	if (!_impl || !_impl->feature || !_impl->parameters) {
		if (_failure.operation.empty()) _failure = { DlssNrStage::Evaluate, DlssNrReason::Unknown, DlssNrCodeKind::None, 0, "DLSSNR feature or parameters unavailable" };
		return false;
	}
	Impl& impl = *_impl;
	const size_t count = _passSettings.size();
	auto passAt = [&](size_t i) -> Impl& { return i ? *impl.laterPasses[i - 1] : impl; };
	Impl& final = passAt(count - 1);
	auto composite = [&]() noexcept {
		if (impl.useResolutionScaling) return CompositeResidual(impl, context.output,
			impl.disabled ? impl.sharedInputSrv11.get() : final.sharedOutputSrv11.get(), _settings);
		impl.context11->CopyResource(context.output,
			impl.disabled ? impl.sharedInput11.get() : final.sharedOutput11.get());
		return true;
	};
	auto fail = [&](std::string_view stage) noexcept {
		if (_failure.operation.empty()) _failure = { DlssNrStage::Evaluate, DlssNrReason::Unknown, DlssNrCodeKind::None, 0, std::string(stage) };
		impl.disabled = true;
		impl.cache.valid = false;
		LogDlssnrStatus(fmt::format("DLSSNR chain failed: frame={} stage={} rejecting complete output",
			context.frameId, stage), true);
		return false;
	};
	if (impl.disabled) {
		if (count > 1 || !Drain() || !PrepareInput(impl, context.input)) return false;
		return composite(); // preserve the legacy single-feature pass-through
	}
	FrameGuidanceView guidance = SelectGuidance(context, _settings,
		{ impl.sourceWidth, impl.sourceHeight });
	if (!guidance.IsValidFor(context.frameId, { impl.sourceWidth, impl.sourceHeight }))
		return fail("invalid-guidance");
	std::array<uint64_t, 3> revisions{};
	for (size_t i = 0; i < count; ++i) revisions[i] = passAt(i).evaluateParameterRevision;
	const std::span<const uint64_t> activeRevisions{ revisions.data(), count };
	const FrameGuidanceView sourceGuidance = guidance;
	const bool sameGuidance = !context.inputHistoryReset &&
		impl.cachedInputHistoryRevision == context.inputHistoryRevision &&
		SameGuidance(impl.cachedGuidance, guidance);
	const size_t first = impl.cache.FirstDirty(context.frameId, context.inputRevision,
		sameGuidance, activeRevisions);
	if (first == count) {
		if (impl.residualParametersDirty && compositeOutput) {
			if (!composite()) return fail("cached-residual-composite");
			impl.residualParametersDirty = false;
		}
		++impl.duplicateFrameReuseCount;
		return true;
	}
	const bool prepareColor = !impl.cache.valid || impl.cache.frame != context.frameId ||
		impl.cache.inputRevision != context.inputRevision;
	const bool resourcesChanged = impl.cache.valid && (
		impl.cachedGuidance.motion.texture != guidance.motion.texture ||
		impl.cachedGuidance.depth.texture != guidance.depth.texture ||
		impl.cachedGuidance.confidence.texture != guidance.confidence.texture ||
		impl.cachedGuidance.motion.metadata.resourceGeneration != guidance.motion.metadata.resourceGeneration ||
		impl.cachedGuidance.depth.metadata.resourceGeneration != guidance.depth.metadata.resourceGeneration ||
		impl.cachedGuidance.confidence.metadata.resourceGeneration != guidance.confidence.metadata.resourceGeneration ||
		impl.cachedGuidance.motion.metadata.validRegion != guidance.motion.metadata.validRegion);
	if (resourcesChanged || context.inputHistoryReset ||
		impl.cachedInputHistoryRevision != context.inputHistoryRevision ||
		!context.isNewCaptureFrame || (impl.cache.valid && (
			(impl.cache.frame == context.frameId && impl.cache.inputRevision != context.inputRevision) ||
			context.frameId < impl.cache.frame))) {
		for (size_t i = 0; i < count; ++i) passAt(i).resetHistory = true;
	}
	if (!context.isNewCaptureFrame) {
		// Perform the reuse/residual decision against the captured guidance first.
		// Only a real SDK re-evaluation needs Zero and reset for this old frame.
		guidance = SelectFrameGuidanceChannels(context.zeroFrameGuidance, context.zeroFrameGuidance,
			context.frameId, { impl.sourceWidth, impl.sourceHeight }, false);
		if (!guidance.IsValidFor(context.frameId, { impl.sourceWidth, impl.sourceHeight }))
			return fail("invalid-redraw-zero-guidance");
	}
	Impl::CommandSlot& slot = impl.commandSlots[impl.nextCommandSlot++ % Impl::COMMAND_SLOT_COUNT];
	const auto waitStart = NativeBackendTiming::Now();
	if (slot.completionValue && !WaitForFence(impl, slot.completionValue)) return fail("slot-wait");
	const double waitMs = NativeBackendTiming::ElapsedMilliseconds(waitStart);
	CollectGpuTiming(impl, slot);
	const auto inputStart = NativeBackendTiming::Now();
	if (prepareColor) {
		if (preparedInput) {
			// Only offscreen performance inference supplies this paired snapshot.
			// Copy to the full branch's immutable in-flight input before its fence.
			D3D11_TEXTURE2D_DESC source{}, target{};
			preparedInput->GetDesc(&source); impl.sharedInput11->GetDesc(&target);
			if (compositeOutput || source.Width != target.Width || source.Height != target.Height ||
				source.Format != target.Format || source.MipLevels != target.MipLevels ||
				source.ArraySize != target.ArraySize || source.SampleDesc.Count != target.SampleDesc.Count ||
				source.SampleDesc.Quality != target.SampleDesc.Quality) return fail("prepared-input-spec");
			impl.context11->CopyResource(impl.sharedInput11.get(), preparedInput);
		} else if (!PrepareInput(impl, context.input)) return fail("prepare-input");
	}
	const double inputMs = NativeBackendTiming::ElapsedMilliseconds(inputStart);
	const auto guidanceStart = NativeBackendTiming::Now();
	const bool reduceGuidance = impl.useResolutionScaling &&
		(impl.width != impl.sourceWidth || impl.height != impl.sourceHeight);
	if (!impl.guidanceInterop->WaitForProducer(impl.context11, guidance, reduceGuidance)) return fail("producer-wait");
	FrameGuidanceView reduced;
	const FrameGuidanceView* evaluateGuidance = &guidance;
	if (reduceGuidance) {
		// Zero resources are immutable constants (depth can be ONE). Resample their
		// actual contents once rather than guessing the clear value from isZero.
		const bool constant = guidance.motion.metadata.isZero && guidance.depth.metadata.isZero &&
			guidance.confidence.metadata.isZero;
		if (!impl.guidancePrepared || !SameGuidance(impl.preparedGuidance, guidance, constant)) {
			if (!PrepareReducedGuidance(impl, guidance)) return fail("guidance-downsample");
			impl.preparedGuidance = guidance;
			impl.guidancePrepared = true;
		}
		reduced = MakeReducedGuidance(impl, guidance);
		evaluateGuidance = &reduced;
	}
	if (!UpdateGuidanceResources(impl, *evaluateGuidance, context.frameId)) return fail("guidance-mapping");
	const double guidanceMs = NativeBackendTiming::ElapsedMilliseconds(guidanceStart);
	const uint64_t inputReady = ++impl.fenceValue;
	if (FAILED(impl.context11->Signal(impl.fence11.get(), inputReady))) return fail("input-signal");
	impl.context11->Flush();
	if (!_submission) _submission = std::make_unique<Submission>();
	auto& submission = *_submission;
	auto& job = submission.job;
	job = {};
	job.impl = &impl;
	job.slot = static_cast<uint32_t>(&slot - impl.commandSlots.data());
	job.first = first; job.count = count;
	job.guidance = *evaluateGuidance; job.sourceGuidance = sourceGuidance;
	job.frame = context.frameId; job.inputRevision = context.inputRevision;
	job.historyRevision = context.inputHistoryRevision;
	job.inputReady = inputReady; job.outputReady = ++impl.fenceValue;
	job.completionEvent = !background ? nullptr : _sparse ? _sparse->event.get() :
		_performance ? _performance->completionEvent.get() : nullptr;
	job.waitMs = waitMs; job.inputMs = inputMs; job.guidanceMs = guidanceMs;
	job.foregroundQueue = _foregroundQueue;
	for (size_t i = 0; i < count; ++i) {
		job.settings[i] = _passSettings[i]; job.revisions[i] = revisions[i];
		job.resetHistory[i] = passAt(i).resetHistory;
		job.guidanceReset[i] = evaluateGuidance->requiresHistoryReset && passAt(i).lastGuidanceResetFrameId != context.frameId;
		job.evaluateNumbers[i] = passAt(i).evaluateCount + 1;
	}
	// Reserve the fence on the owner, but do not let progress interpret it as a
	// submitted command list until the CPU completion has been acquired.
	impl.pendingInferenceValue = job.outputReady;
	impl.guidanceInterop->MarkSubmitted(job.outputReady);
	if (background) {
		submission.pending = true;
		if (!submission.worker.Submit([state = &submission]() noexcept { Submission::Execute(state->job); })) {
			submission.pending = false;
			return fail("cpu-worker-submit");
		}
		return true;
	}
	Submission::Execute(job);
	if (!submission.Complete(*this)) return false;
	if (FAILED(impl.context11->Wait(impl.fence11.get(), job.outputReady))) return fail("output-wait");
	if (compositeOutput) {
		if (!composite()) return fail("composite");
		impl.residualParametersDirty = false;
	}
	return true;
}

#include "DLSSNRPerformance.inl"
#include "DLSSNRSparse.inl"

bool DLSSNRFilter::IsHealthy() const noexcept {
	return _impl && !_impl->disabled;
}

}

#else

namespace Magpie {

struct DLSSNRFilter::Impl {};
struct DLSSNRFilter::Performance {};
struct DLSSNRFilter::Submission {};
struct DLSSNRFilter::ResolutionChange {};
struct DLSSNRFilter::Sparse {};
NativeEffectDrawContext DLSSNRFilter::OutputDrawContext(const NativeEffectDrawContext& context) const noexcept { return context; }
uint64_t DLSSNRFilter::OutputContentVersion() const noexcept { return 0; }
DLSSNRSparseOutputInfo DLSSNRFilter::GetSparseOutputInfo() const noexcept { return {}; }
bool DLSSNRFilter::HasReadyOutput() const noexcept { return false; }
uint64_t DLSSNRFilter::ReadyOutputVersion() const noexcept { return 0; }
void DLSSNRFilter::ServiceAsync() noexcept {}
bool DLSSNRFilter::ServiceInferenceSubmission(bool) noexcept { return true; }
bool DLSSNRFilter::InferenceSubmissionPending() const noexcept { return false; }
bool DLSSNRFilter::HasPendingPostOutputWork() const noexcept { return false; }
void DLSSNRFilter::MarkOutputPublished() noexcept {}
void DLSSNRFilter::ServiceAfterOutput() noexcept {}
bool DLSSNRFilter::IsGpuIdle() const noexcept { return true; }
bool DLSSNRFilter::CanPrepareResolution() const noexcept { return false; }
bool DLSSNRFilter::PrepareResolution(DeviceResources&, ID3D11Texture2D*, ID3D11Texture2D*, uint32_t) noexcept { return false; }
bool DLSSNRFilter::CommitPreparedResolution() noexcept { return false; }
void DLSSNRFilter::CancelPreparedResolution() noexcept {}
double DLSSNRFilter::ResolutionPreparationMs() const noexcept { return 0; }
bool DLSSNRFilter::PrewarmResolution(DeviceResources&, ID3D11Texture2D*, ID3D11Texture2D*, uint32_t) noexcept { return false; }
bool DLSSNRFilter::WarmPreparedResolution(const NativeEffectDrawContext&) noexcept { return true; }
bool DLSSNRFilter::GetHotResolution(uint32_t&) const noexcept { return false; }
bool DLSSNRFilter::HasHotResolution(uint32_t) const noexcept { return false; }
void DLSSNRFilter::InvalidateHotResolution() noexcept {}
DLSSNRPerformanceSnapshot DLSSNRFilter::GetPerformanceSnapshot() const noexcept { return {}; }
void DLSSNRFilter::ConfigureTemporalHistory(DLSSNRTemporal&) const noexcept {}
bool DLSSNRFilter::IsHealthy() const noexcept { return false; }
DLSSNRFilter::DLSSNRFilter() = default;
DLSSNRFilter::~DLSSNRFilter() = default;
FrameGuidanceRequirements
DLSSNRFilter::GetFrameGuidanceRequirements() const noexcept { return {}; }
bool DLSSNRFilter::Initialize(
	DeviceResources&, NgxD3D12Core&, ID3D11Texture2D*, ID3D11Texture2D*,
	const DLSSNRSettings&) noexcept {
	_failure = { DlssNrStage::Initialize, DlssNrReason::BuildDisabled, DlssNrCodeKind::None, 0, "MP_ENABLE_DLSSNR" };
	Logger::Get().Error("DLSSNR support is disabled at build time");
	return false;
}
bool DLSSNRFilter::InitializeChain(DeviceResources&, NgxD3D12Core&,
	ID3D11Texture2D*, ID3D11Texture2D*, std::span<const DLSSNRSettings>) noexcept {
	_failure = { DlssNrStage::Initialize, DlssNrReason::BuildDisabled, DlssNrCodeKind::None, 0, "MP_ENABLE_DLSSNR" };
	Logger::Get().Error("DLSSNR support is disabled at build time");
	return false;
}
bool DLSSNRFilter::Resize(
	DeviceResources&, ID3D11Texture2D*, ID3D11Texture2D*) noexcept {
	return false;
}
bool DLSSNRFilter::Drain() noexcept { return true; }
EffectParameterApplyMode DLSSNRFilter::GetParameterApplyMode(
	std::string_view) const noexcept {
	return EffectParameterApplyMode::RestartRequired;
}
EffectParameterRestartReason DLSSNRFilter::GetParameterRestartReason(
	std::string_view) const noexcept {
	return EffectParameterRestartReason::NativeBackend;
}
bool DLSSNRFilter::ApplyLiveParameters(
	const EffectOption&, std::span<const std::string>) noexcept {
	return false;
}
bool DLSSNRFilter::Draw(const NativeEffectDrawContext&) noexcept {
	return false;
}

}

#endif
