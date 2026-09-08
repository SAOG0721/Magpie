#include "pch.h"
#include "HdrSurfaceAdapter.h"

#include "BackendDescriptorStore.h"
#include "DeviceResources.h"
#include "DirectXHelper.h"
#include "Logger.h"

namespace Magpie {

namespace {
constexpr char HLSL[] = R"(
cbuffer Transform : register(b0) {
    float exposure;
    float inverseExposure;
    float sdrWhiteScale;
    float hdrPeakNits;
    float shoulder;
    float referenceWhiteNits;
    float sdrWhiteNits;
    uint inputTransfer;
    uint outputTransfer;
    uint mode;
    uint preserveAlpha;
    float normalizationScale;
    float peak;
    float target;
    float shoulderK;
    float tailSlope;
};
Texture2D<float4> sourceTexture : register(t0);
RWTexture2D<float4> outputTexture : register(u0);

float DecodeSrgb(float value) {
    return value <= 0.04045 ? value / 12.92 : pow(max((value + 0.055) / 1.055, 0.0), 2.4);
}
float EncodeSrgb(float value) {
    value = saturate(value);
    return value <= 0.0031308 ? value * 12.92 : 1.055 * pow(value, 1.0 / 2.4) - 0.055;
}
// Extended sRGB: the standard OETF on [0, 1] (matching what the U8 route
// feeds the model bit-for-bit) and the same power curve continued above 1
// for HDR headroom. Monotone and exactly invertible on [0, inf).
float EncodeExtendedSrgb(float value) {
    value = max(value, 0.0);
    if (value <= 0.0031308) return value * 12.92;
    return 1.055 * pow(value, 1.0 / 2.4) - 0.055;
}
float DecodeExtendedSrgb(float value) {
    value = max(value, 0.0);
    if (value <= 0.04045) return value / 12.92;
    return pow((value + 0.055) / 1.055, 2.4);
}
float DecodeHlg(float value) {
    const float a = 0.17883277;
    const float b = 1.0 - 4.0 * a;
    const float c = 0.5 - a * log(4.0 * a);
    value = saturate(value);
    return value <= 0.5 ? (value * value) / 3.0 : (exp((value - c) / a) + b) / 12.0;
}
float EncodeHlg(float value) {
    const float a = 0.17883277;
    const float b = 1.0 - 4.0 * a;
    const float c = 0.5 - a * log(4.0 * a);
    value = max(value, 0.0);
    return saturate(value <= 1.0 / 12.0 ? sqrt(3.0 * value) : a * log(12.0 * value - b) + c);
}
float DecodePq(float value) {
    const float m1 = 2610.0 / 16384.0;
    const float m2 = 2523.0 / 32.0;
    const float c1 = 3424.0 / 4096.0;
    const float c2 = 2413.0 / 128.0;
    const float c3 = 2392.0 / 128.0;
    float p = pow(max(saturate(value), 0.0), 1.0 / m2);
    return pow(max(p - c1, 0.0) / max(c2 - c3 * p, 1e-6), 1.0 / m1) * 10000.0;
}
float EncodePq(float value) {
    const float m1 = 2610.0 / 16384.0;
    const float m2 = 2523.0 / 32.0;
    const float c1 = 3424.0 / 4096.0;
    const float c2 = 2413.0 / 128.0;
    const float c3 = 2392.0 / 128.0;
    float p = pow(max(value, 0.0) / 10000.0, m1);
    return saturate(pow((c1 + c2 * p) / (1.0 + c3 * p), m2));
}
float3 Rec709ToRec2020(float3 value) {
    return float3(
        dot(value, float3(0.6274040, 0.3292820, 0.0433136)),
        dot(value, float3(0.0690970, 0.9195400, 0.0113612)),
        dot(value, float3(0.0163916, 0.0880132, 0.8955950)));
}
float3 Rec2020ToRec709(float3 value) {
    return float3(
        dot(value, float3(1.6604910, -0.5876411, -0.0728499)),
        dot(value, float3(-0.1245505, 1.1328999, -0.0083494)),
        dot(value, float3(-0.0181508, -0.1005789, 1.1187297)));
}
float3 MapRec2020ToPqGamut(float3 value) {
    float minimum = min(value.r, min(value.g, value.b));
    if (minimum >= 0.0) return value;
    float luminance = max(dot(value, float3(0.2627, 0.6780, 0.0593)), 0.0);
    float chromaScale = luminance / max(luminance - minimum, 1e-6);
    return max(luminance + (value - luminance) * saturate(chromaScale), 0.0);
}
// Anchored-shoulder curve, GPU twin of HdrColorTransform's CPU family.
// x <= 1 identity; 1 < x <= peak: x / (1 + k(x - 1)); linear tail beyond.
// The constant buffer carries peak/target/k/tailSlope so CPU and GPU stay
// bit-comparable; highlights never invert and inverse gain stays bounded.
float ApplyShoulder(float value, float peak, float target, float k, float tailSlope) {
    value = max(value, 0.0);
    if (value <= 1.0) return value;
    if (value <= peak) return value / (1.0 + k * (value - 1.0));
    return target + tailSlope * (value - peak);
}
float InvertShoulder(float value, float peak, float target, float k, float tailSlope) {
    value = max(value, 0.0);
    if (value <= 1.0) return value;
    if (value <= target) return value * (1.0 - k) / max(1.0 - k * value, 1e-6);
    return peak + (value - target) / max(tailSlope, 1e-6);
}
float DecodeTransfer(float value, uint transfer) {
    if (transfer == 2) return DecodeSrgb(value);
    // PQ is absolute-display-referred. Canonical scRGB uses 80 nit as its
    // fixed reference; the monitor SDR white level is a separate parameter.
    if (transfer == 3) return DecodePq(value) / 80.0;
    if (transfer == 4) return DecodeHlg(value);
    return value;
}
float EncodeTransfer(float value, uint transfer) {
    if (transfer == 2) return EncodeSrgb(value);
    if (transfer == 3) return EncodePq(value * 80.0);
    if (transfer == 4) return EncodeHlg(value);
    return value;
}
float3 MapHdrToSdr(float3 value) {
    float referenceWhiteScale = max(sdrWhiteNits / 80.0, 1e-4);
    // Replicate contract for SDR-compatible backends: identity below the SDR
    // white point (captured SDR frames reach the backend bit-exact), plain
    // saturation above it. No shoulder: identity forces f(1)=1 and UNORM8
    // storage leaves no code space above 1.
    return saturate(max(value, 0.0) * exposure / referenceWhiteScale);
}
float3 MapSdrToHdr(float3 value) {
    float referenceWhiteScale = max(sdrWhiteNits / 80.0, 1e-4);
    float3 mapped = saturate(value);
    // Paired inverse of the replicate contract. Saturated whites restore to
    // the HDR headroom in the normalized domain (peak/sdrWhite), then scale
    // by the white point once. Multiplying by the white point twice pushed
    // whites to (peak/80)*(sdrWhite/80) = 20.25 on a 360-nit display.
    float peakHeadroom = max(hdrPeakNits, sdrWhiteNits) / max(sdrWhiteNits, 1e-4);
    float3 restored = float3(
        mapped.r >= 1.0 ? peakHeadroom : mapped.r,
        mapped.g >= 1.0 ? peakHeadroom : mapped.g,
        mapped.b >= 1.0 ? peakHeadroom : mapped.b);
    return restored * referenceWhiteScale * inverseExposure;
}
[numthreads(8, 8, 1)]
void Main(uint3 id : SV_DispatchThreadID) {
    uint width, height;
    outputTexture.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;
    float4 value = sourceTexture.Load(int3(id.xy, 0));
    float3 result;
    if (mode == 0) {
        float3 mapped = MapHdrToSdr(value.rgb);
        result = float3(
            EncodeTransfer(mapped.r, outputTransfer),
            EncodeTransfer(mapped.g, outputTransfer),
            EncodeTransfer(mapped.b, outputTransfer));
    } else if (mode == 1) {
        float3 decoded = float3(
            DecodeTransfer(value.r, inputTransfer),
            DecodeTransfer(value.g, inputTransfer),
            DecodeTransfer(value.b, inputTransfer));
        result = MapSdrToHdr(decoded);
	} else if (mode == 2) {
		// Canonical scRGB may use a display SDR-white scale (for example 4.5
		// for a 360-nit SDR white). Bounded backends operate in a normalized
		// domain: normalize against the frame's SDR white point, then encode
		// with the extended sRGB OETF. The model's training contract is sRGB-
		// encoded U8 input; feeding it linear values leaves mid-tones squeezed
		// into the bottom of its range (observed as a washed-out, hazy image).
		// The extended curve matches the standard OETF on [0,1] exactly and
		// continues the same power law for HDR headroom.
		// The consumption domain is [0, inf); clamp out-of-gamut negatives
		// before they reach the model (inverse mode 3 keeps full range).
		float3 normalized = max(value.rgb, 0.0) * normalizationScale /
			max(sdrWhiteNits / 80.0, 1e-4);
		result = float3(
			EncodeExtendedSrgb(normalized.r),
			EncodeExtendedSrgb(normalized.g),
			EncodeExtendedSrgb(normalized.b));
	} else if (mode == 3) {
		// Paired inverse of mode 2; full range on purpose.
		float3 normalized = float3(
			DecodeExtendedSrgb(value.r),
			DecodeExtendedSrgb(value.g),
			DecodeExtendedSrgb(value.b));
		result = normalized * (sdrWhiteNits / 80.0) / max(normalizationScale, 1e-4);
	} else if (mode == 5) {
		// Canonical scRGB is linear with 1.0 == 80 nit. HDR10 also requires
		// Rec.2020 primaries, so convert the canonical Rec.709 values first.
		float3 rec2020 = MapRec2020ToPqGamut(Rec709ToRec2020(value.rgb));
		result = float3(
			EncodePq(rec2020.r * 80.0),
			EncodePq(rec2020.g * 80.0),
			EncodePq(rec2020.b * 80.0));
	} else if (mode == 6) {
		float3 rec2020 = float3(
			DecodePq(value.r) / 80.0,
			DecodePq(value.g) / 80.0,
			DecodePq(value.b) / 80.0);
		result = Rec2020ToRec709(rec2020);
	} else {
		result = value.rgb;
	}
    outputTexture[id.xy] = float4(result, preserveAlpha != 0 ? value.a : 1.0);
}
)";

struct AdapterConstants {
    float exposure;
    float inverseExposure;
    float sdrWhiteScale;
    float hdrPeakNits;
    float shoulder;
    float referenceWhiteNits;
    float sdrWhiteNits;
    uint32_t inputTransfer;
    uint32_t outputTransfer;
    uint32_t mode;
    uint32_t preserveAlpha;
    float normalizationScale;
    float peak;
    float target;
    float shoulderK;
    float tailSlope;
    float _padding[12]{};
};
// D3D11 constant buffers must be a multiple of 16 bytes; the HLSL cbuffer
// reads 20 scalars (80 bytes), so the CPU copy pads to 112.
static_assert(sizeof(AdapterConstants) == 112, "HDR adapter constant buffer layout must match HLSL");
static_assert(sizeof(AdapterConstants) % 16 == 0, "constant buffer size must stay 16-byte aligned");
}

bool HdrSurfaceAdapter::Initialize(
	DeviceResources& deviceResources,
	BackendDescriptorStore& descriptorStore
) noexcept {
	_deviceResources = &deviceResources;
	_descriptorStore = &descriptorStore;
	winrt::com_ptr<ID3DBlob> blob;
	if (!DirectXHelper::CompileComputeShader(HLSL, "Main", blob.put(), "HdrSurfaceAdapter", nullptr, {}, true)) {
		return false;
	}
	HRESULT hr = deviceResources.GetD3DDevice()->CreateComputeShader(
		blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, _shader.put());
	if (FAILED(hr)) {
		Logger::Get().ComError("创建 HDR 表面适配器 Compute Shader 失败", hr);
		return false;
	}
	const D3D11_BUFFER_DESC desc{
		.ByteWidth = sizeof(AdapterConstants),
		.Usage = D3D11_USAGE_DYNAMIC,
		.BindFlags = D3D11_BIND_CONSTANT_BUFFER,
		.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE
	};
	return SUCCEEDED(deviceResources.GetD3DDevice()->CreateBuffer(&desc, nullptr, _constants.put()));
}

bool HdrSurfaceAdapter::_Convert(
	ID3D11Texture2D* input,
	ID3D11Texture2D* output,
	const HdrTransformParameters& parameters,
	HdrTransferFunction transfer,
	bool hdrToSdr,
	uint32_t mode,
	float normalizationScale
) const noexcept {
	if (!_deviceResources || !_descriptorStore || !_shader || !_constants || !input || !output ||
		!parameters.IsValid()) return false;
	D3D11_TEXTURE2D_DESC inputDesc{}, outputDesc{};
	input->GetDesc(&inputDesc);
	output->GetDesc(&outputDesc);
	if (inputDesc.Width != outputDesc.Width || inputDesc.Height != outputDesc.Height) return false;
	ID3D11ShaderResourceView* inputSrv = _descriptorStore->GetShaderResourceView(input);
	ID3D11UnorderedAccessView* outputUav = _descriptorStore->GetUnorderedAccessView(output);
	if (!inputSrv || !outputUav) {
		Logger::Get().Error(fmt::format(
			"HDR adapter descriptors unavailable: inputSrv={} outputUav={} mode={} inputFormat={} outputFormat={}",
			inputSrv != nullptr, outputUav != nullptr, mode,
			static_cast<uint32_t>(inputDesc.Format), static_cast<uint32_t>(outputDesc.Format)));
		return false;
	}
	const HdrTransformConstants base = HdrColorTransform::PrepareConstants(
		parameters,
		hdrToSdr ? HdrTransferFunction::Linear : transfer,
		hdrToSdr ? transfer : HdrTransferFunction::Linear);
	// The GPU shoulder must mirror the CPU curve family exactly. Modes 2/3
	// (bounded) consume the curve; SDR modes (0/1) use the plain replicate
	// contract (identity + saturate) and ignore the curve coefficients.
	const HdrColorTransform::ShoulderCurve curve = HdrColorTransform::BuildShoulderCurve(
		parameters, HdrColorTransform::BoundedRouteHighlightTarget);
	const AdapterConstants constants{
		base.exposure, base.inverseExposure, base.sdrWhiteScale, base.hdrPeakNits,
		base.shoulder, parameters.referenceWhiteNits, parameters.sdrWhiteNits,
		base.inputTransfer, base.outputTransfer, mode,
		parameters.preserveAlpha ? 1u : 0u, normalizationScale,
		curve.peak, curve.target, curve.k, curve.tailSlope
	};
	D3D11_MAPPED_SUBRESOURCE mapped{};
	ID3D11DeviceContext4* context = _deviceResources->GetD3DDC();
	if (FAILED(context->Map(_constants.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
		Logger::Get().Error(fmt::format("HDR adapter constants map failed: mode={}", mode));
		return false;
	}
	memcpy(mapped.pData, &constants, sizeof(constants));
	context->Unmap(_constants.get(), 0);
	context->CSSetShader(_shader.get(), nullptr, 0);
	ID3D11Buffer* constantBuffer = _constants.get();
	context->CSSetConstantBuffers(0, 1, &constantBuffer);
	context->CSSetShaderResources(0, 1, &inputSrv);
	context->CSSetUnorderedAccessViews(0, 1, &outputUav, nullptr);
	context->Dispatch((outputDesc.Width + 7) / 8, (outputDesc.Height + 7) / 8, 1);
	Logger::Get().Info(fmt::format(
		"HDR adapter dispatch: mode={} input={} output={} white={:.3f} peak={:.3f} scale={:.3f}",
		mode, static_cast<uint32_t>(inputDesc.Format), static_cast<uint32_t>(outputDesc.Format),
		parameters.sdrWhiteNits, parameters.hdrPeakNits, normalizationScale));
	ID3D11ShaderResourceView* nullSrv = nullptr;
	ID3D11UnorderedAccessView* nullUav = nullptr;
	ID3D11Buffer* nullBuffer = nullptr;
	context->CSSetShaderResources(0, 1, &nullSrv);
	context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
	context->CSSetConstantBuffers(0, 1, &nullBuffer);
	context->CSSetShader(nullptr, nullptr, 0);
	return true;
}

bool HdrSurfaceAdapter::ConvertHdrToSdr(
	ID3D11Texture2D* input, ID3D11Texture2D* output,
	const HdrTransformParameters& parameters, HdrTransferFunction outputTransfer
) const noexcept {
	return _Convert(input, output, parameters, outputTransfer, true);
}

bool HdrSurfaceAdapter::ConvertSdrToHdr(
	ID3D11Texture2D* input, ID3D11Texture2D* output,
	const HdrTransformParameters& parameters, HdrTransferFunction inputTransfer
) const noexcept {
	return _Convert(input, output, parameters, inputTransfer, false, 1u);
}

bool HdrSurfaceAdapter::ConvertHdrToBounded(
	ID3D11Texture2D* input, ID3D11Texture2D* output,
	const HdrTransformParameters& parameters, float normalizationScale
) const noexcept {
	return _Convert(input, output, parameters, HdrTransferFunction::Linear,
		false, 2u, normalizationScale);
}

bool HdrSurfaceAdapter::ConvertBoundedToHdr(
	ID3D11Texture2D* input, ID3D11Texture2D* output,
	const HdrTransformParameters& parameters, float normalizationScale
) const noexcept {
	return _Convert(input, output, parameters, HdrTransferFunction::Linear,
		false, 3u, normalizationScale);
}

bool HdrSurfaceAdapter::ConvertHdrToScRgb(
	ID3D11Texture2D* input, ID3D11Texture2D* output,
	const HdrTransformParameters& parameters
) const noexcept {
	return _Convert(input, output, parameters, HdrTransferFunction::Linear,
		false, 4u, 1.0f);
}

bool HdrSurfaceAdapter::ConvertCanonicalToHdr10(
	ID3D11Texture2D* input, ID3D11Texture2D* output,
	const HdrTransformParameters& parameters
) const noexcept {
	return _Convert(input, output, parameters, HdrTransferFunction::PQ,
		false, 5u, 1.0f);
}

bool HdrSurfaceAdapter::ConvertHdr10ToCanonical(
	ID3D11Texture2D* input, ID3D11Texture2D* output,
	const HdrTransformParameters& parameters
) const noexcept {
	return _Convert(input, output, parameters, HdrTransferFunction::PQ,
		false, 6u, 1.0f);
}

}
