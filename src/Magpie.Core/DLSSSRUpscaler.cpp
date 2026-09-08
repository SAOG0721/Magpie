#include "pch.h"
#include "DLSSSRUpscaler.h"
#include "DeviceResources.h"
#include "DirectXHelper.h"
#include "Logger.h"

#ifdef MP_ENABLE_DLSS_SR
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>

namespace Magpie {

static bool NGXSucceeded(NVSDK_NGX_Result result) noexcept {
	return NVSDK_NGX_SUCCEED(result);
}

DLSSSRUpscaler::~DLSSSRUpscaler() {
	_Reset();
}

void DLSSSRUpscaler::_Reset() noexcept {
	if (_feature) {
		NVSDK_NGX_D3D11_ReleaseFeature(static_cast<NVSDK_NGX_Handle*>(_feature));
		_feature = nullptr;
	}
	if (_parameters) {
		NVSDK_NGX_D3D11_DestroyParameters(static_cast<NVSDK_NGX_Parameter*>(_parameters));
		_parameters = nullptr;
	}
	if (_ngxInitialized) {
		NVSDK_NGX_D3D11_Shutdown1(_device);
		_ngxInitialized = false;
	}

	_zeroDepthUav = nullptr;
	_zeroDepth = nullptr;
	_biasCurrentColorMaskUav = nullptr;
	_biasCurrentColorMask = nullptr;
	_zeroMotionVectorsUav = nullptr;
	_zeroMotionVectors = nullptr;
	_device = nullptr;
	_d3dDC = nullptr;
	_resetHistory = true;
	_settings = {};
	_lastGuidanceBinding = UINT8_MAX;
	_lastGuidanceResetFrameId =
		std::numeric_limits<FrameGuidanceFrameId>::max();
}

bool DLSSSRUpscaler::Initialize(
	DeviceResources& deviceResources,
	ID3D11Texture2D* input,
	ID3D11Texture2D* output,
	const DLSSSRSettings& settings
) noexcept {
	_Reset();
	_settings = settings;
	_device = deviceResources.GetD3DDevice();
	_d3dDC = deviceResources.GetD3DDC();

	D3D11_TEXTURE2D_DESC inputDesc{};
	D3D11_TEXTURE2D_DESC outputDesc{};
	input->GetDesc(&inputDesc);
	output->GetDesc(&outputDesc);
	if (inputDesc.Width > outputDesc.Width || inputDesc.Height > outputDesc.Height) {
		Logger::Get().Error("DLSS SR only supports upscaling");
		_Reset();
		return false;
	}

	_zeroMotionVectors = DirectXHelper::CreateTexture2D(
		_device,
		DXGI_FORMAT_R16G16_FLOAT,
		inputDesc.Width,
		inputDesc.Height,
		D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS
	);
	_zeroDepth = DirectXHelper::CreateTexture2D(
		_device,
		DXGI_FORMAT_R32_FLOAT,
		inputDesc.Width,
		inputDesc.Height,
		D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS
	);
	_biasCurrentColorMask = DirectXHelper::CreateTexture2D(
		_device,
		DXGI_FORMAT_R8_UNORM,
		inputDesc.Width,
		inputDesc.Height,
		D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS
	);
	if (!_zeroMotionVectors || !_zeroDepth || !_biasCurrentColorMask) {
		Logger::Get().Error("Create DLSS SR auxiliary textures failed");
		_Reset();
		return false;
	}
	HRESULT hr = _device->CreateUnorderedAccessView(
		_zeroMotionVectors.get(), nullptr, _zeroMotionVectorsUav.put());
	if (SUCCEEDED(hr)) {
		hr = _device->CreateUnorderedAccessView(
			_zeroDepth.get(), nullptr, _zeroDepthUav.put());
	}
	if (SUCCEEDED(hr)) {
		hr = _device->CreateUnorderedAccessView(
			_biasCurrentColorMask.get(), nullptr, _biasCurrentColorMaskUav.put());
	}
	if (FAILED(hr)) {
		Logger::Get().ComError("Create DLSS SR UAV failed", hr);
		_Reset();
		return false;
	}

	// A stable custom project identifier is permitted by NGX when an NVIDIA
	// application ID has not been assigned to an integration.
	NVSDK_NGX_Result result = NVSDK_NGX_D3D11_Init_with_ProjectID(
		"7c134ab9-9677-4af5-a2b2-bca943350861",
		NVSDK_NGX_ENGINE_TYPE_CUSTOM,
		"Magpie-DLSS-SR-2",
		L"logs",
		_device
	);
	if (!NGXSucceeded(result)) {
		Logger::Get().Error(fmt::format("NVSDK_NGX_D3D11_Init failed ({:#x})", (uint32_t)result));
		_Reset();
		return false;
	}
	_ngxInitialized = true;

	NVSDK_NGX_Parameter* parameters = nullptr;
	result = NVSDK_NGX_D3D11_GetCapabilityParameters(&parameters);
	if (!NGXSucceeded(result) || !parameters) {
		Logger::Get().Error(fmt::format("NVSDK_NGX_D3D11_GetCapabilityParameters failed ({:#x})", (uint32_t)result));
		_Reset();
		return false;
	}
	_parameters = parameters;

	// Preset J remains the established default for this captured-frame path.
	// Real optical flow improves temporal input but is not engine motion.
	NVSDK_NGX_Parameter_SetI(
		parameters,
		NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced,
		NVSDK_NGX_DLSS_Hint_Render_Preset_J
	);

	NVSDK_NGX_DLSS_Create_Params createParams{
		.Feature{
			.InWidth = inputDesc.Width,
			.InHeight = inputDesc.Height,
			.InTargetWidth = outputDesc.Width,
			.InTargetHeight = outputDesc.Height,
			.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_Balanced
		},
		.InFeatureCreateFlags = int(
			NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
			NVSDK_NGX_DLSS_Feature_Flags_AutoExposure |
			(_hdrProtocol.hdrColorInput
				? NVSDK_NGX_DLSS_Feature_Flags_IsHDR
				: NVSDK_NGX_DLSS_Feature_Flags_None)),
		.InEnableOutputSubrects = false
	};

	NVSDK_NGX_Handle* feature = nullptr;
	result = NGX_D3D11_CREATE_DLSS_EXT(_d3dDC, &feature, parameters, &createParams);
	if (!NGXSucceeded(result) || !feature) {
		Logger::Get().Error(fmt::format("NGX_D3D11_CREATE_DLSS_EXT failed ({:#x})", (uint32_t)result));
		_Reset();
		return false;
	}
	_feature = feature;

	Logger::Get().Info(fmt::format(
		"DLSS SR initialized (Balanced, preset J): {}x{} -> {}x{}, "
		"requestedMotion={} quality={}, depth=zero-contract",
		inputDesc.Width, inputDesc.Height, outputDesc.Width, outputDesc.Height,
		uint32_t(_settings.motionRequest.method), _settings.motionRequest.quality));
	return true;
}

FrameGuidanceRequirements
DLSSSRUpscaler::GetFrameGuidanceRequirements() const noexcept {
	FrameGuidanceRequirements result{ .zero = true };
	result.Add(_settings.motionRequest);
	return result;
}

bool DLSSSRUpscaler::Resize(
	DeviceResources& deviceResources,
	ID3D11Texture2D* input,
	ID3D11Texture2D* output
) noexcept {
	const DLSSSRSettings settings = _settings;
	return Initialize(deviceResources, input, output, settings);
}

bool DLSSSRUpscaler::Draw(const NativeEffectDrawContext& context) noexcept {
	ID3D11Texture2D* input = context.input;
	ID3D11Texture2D* output = context.output;
	if (!_feature || !_parameters || !_zeroMotionVectorsUav || !_zeroDepthUav ||
		!_biasCurrentColorMaskUav) {
		return false;
	}

	static constexpr float ZERO[4]{};
	static constexpr float BIAS_CURRENT_COLOR[4]{ 0.5f,0.5f,0.5f,0.5f };
	ID3D11Texture2D* motionVectors = _zeroMotionVectors.get();
	ID3D11Texture2D* depth = _zeroDepth.get();
	bool guidanceReset = false;
	bool realMotion = false;

	D3D11_TEXTURE2D_DESC inputDesc{};
	input->GetDesc(&inputDesc);
	const FrameGuidanceExtent inputExtent{ inputDesc.Width, inputDesc.Height };
	const FrameGuidanceView guidance = SelectFrameGuidanceChannels(
		context.frameGuidance, context.zeroFrameGuidance,
		context.frameId, inputExtent,
		_settings.motionRequest.method != OpticalFlowMethod::None);
	if (guidance.IsValidFor(context.frameId, inputExtent)) {
		depth = guidance.depth.texture;
		if (_settings.motionRequest.method != OpticalFlowMethod::None) {
			motionVectors = guidance.motion.texture;
			realMotion = !guidance.motion.metadata.isZero;
		}
		guidanceReset = guidance.requiresHistoryReset;
		for (const FrameGuidanceResource* resource :
			{ &guidance.motion, &guidance.depth }) {
			if (resource->metadata.sync.fence && resource->metadata.sync.value &&
				FAILED(_d3dDC->Wait(
					resource->metadata.sync.fence,
					resource->metadata.sync.value))) {
				Logger::Get().Warn("DLSS SR Frame Guidance producer wait failed; using Zero");
				motionVectors = _zeroMotionVectors.get();
				depth = _zeroDepth.get();
				realMotion = false;
				guidanceReset = true;
				break;
			}
		}
	}

	const uint8_t binding = uint8_t(realMotion) |
		(uint8_t(_settings.motionRequest.method != OpticalFlowMethod::None) << 1);
	const bool bindingChanged = _lastGuidanceBinding != UINT8_MAX &&
		binding != _lastGuidanceBinding;
	if (binding != _lastGuidanceBinding) {
		Logger::Get().Info(fmt::format(
			"DLSS SR guidance frameId={}: requested motion={}, "
			"bound motion={} depth=zero, fallback={}",
			context.frameId,
			_settings.motionRequest.method != OpticalFlowMethod::None,
			realMotion ? "real" : "zero",
			(!realMotion && _settings.motionRequest.method != OpticalFlowMethod::None) ? "zero" : "none"));
	}
	guidanceReset = bindingChanged ||
		(guidanceReset && _lastGuidanceResetFrameId != context.frameId);

	if (motionVectors == _zeroMotionVectors.get()) {
		_d3dDC->ClearUnorderedAccessViewFloat(_zeroMotionVectorsUav.get(), ZERO);
	}
	if (depth == _zeroDepth.get()) {
		_d3dDC->ClearUnorderedAccessViewFloat(_zeroDepthUav.get(), ZERO);
	}
	_d3dDC->ClearUnorderedAccessViewFloat(_biasCurrentColorMaskUav.get(), BIAS_CURRENT_COLOR);

	NVSDK_NGX_D3D11_DLSS_Eval_Params evalParams{};
	evalParams.Feature.InSharpness = 0.3f;
	evalParams.Feature.pInColor = input;
	evalParams.Feature.pInOutput = output;
	evalParams.pInDepth = depth;
	evalParams.pInMotionVectors = motionVectors;
	evalParams.pInBiasCurrentColorMask = _biasCurrentColorMask.get();
	evalParams.InJitterOffsetX = 0.0f;
	evalParams.InJitterOffsetY = 0.0f;
	evalParams.InRenderSubrectDimensions = { inputDesc.Width, inputDesc.Height };
	evalParams.InReset = _resetHistory || guidanceReset ? 1 : 0;
	evalParams.InMVScaleX = 1.0f;
	evalParams.InMVScaleY = 1.0f;
	evalParams.InPreExposure = _hdrProtocol.preExposure;
	evalParams.InExposureScale = _hdrProtocol.exposure;

	const NVSDK_NGX_Result result = NGX_D3D11_EVALUATE_DLSS_EXT(
		_d3dDC,
		static_cast<NVSDK_NGX_Handle*>(_feature),
		static_cast<NVSDK_NGX_Parameter*>(_parameters),
		&evalParams
	);
	if (!NGXSucceeded(result)) {
		Logger::Get().Error(fmt::format("NGX_D3D11_EVALUATE_DLSS_EXT failed ({:#x})", (uint32_t)result));
		return false;
	}

	_resetHistory = false;
	_lastGuidanceBinding = binding;
	if (guidanceReset) _lastGuidanceResetFrameId = context.frameId;
	return true;
}

}

#else

namespace Magpie {

DLSSSRUpscaler::~DLSSSRUpscaler() = default;
void DLSSSRUpscaler::_Reset() noexcept {}

bool DLSSSRUpscaler::Initialize(
	DeviceResources&, ID3D11Texture2D*, ID3D11Texture2D*,
	const DLSSSRSettings&) noexcept {
	Logger::Get().Error("DLSS SR is disabled at build time");
	return false;
}

FrameGuidanceRequirements
DLSSSRUpscaler::GetFrameGuidanceRequirements() const noexcept { return {}; }

bool DLSSSRUpscaler::Resize(DeviceResources&, ID3D11Texture2D*, ID3D11Texture2D*) noexcept {
	return false;
}

bool DLSSSRUpscaler::Draw(const NativeEffectDrawContext&) noexcept {
	return false;
}

}

#endif
