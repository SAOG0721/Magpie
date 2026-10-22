#pragma once
#include "NativeEffectBackend.h"
#include "GroupBEffectProtocol.h"
#include "DLSSSRParameters.h"

namespace Magpie {

class DeviceResources;
class NgxFeatureDiscovery;

struct DLSSSRSettings {
	MotionVectorRequest motionRequest{};
	int renderPreset = DLSS_SR_DEFAULT_PRESET;
};

// DLSS SR adapter for captured colour frames, with shared optical flow
// and a zero-depth contract.
class DLSSSRUpscaler final : public NativeEffectBackend {
public:
	DLSSSRUpscaler();
	DLSSSRUpscaler(const DLSSSRUpscaler&) = delete;
	DLSSSRUpscaler& operator=(const DLSSSRUpscaler&) = delete;
	~DLSSSRUpscaler() override;

	bool Initialize(
		DeviceResources& deviceResources,
		ID3D11Texture2D* input,
		ID3D11Texture2D* output,
		const DLSSSRSettings& settings = {}
	) noexcept;

	FrameGuidanceRequirements GetFrameGuidanceRequirements() const noexcept override;
	EffectParameterApplyMode GetParameterApplyMode(
		std::string_view /*parameterName*/
	) const noexcept override {
		return EffectParameterApplyMode::RestartRequired;
	}
	EffectParameterRestartReason GetParameterRestartReason(
		std::string_view parameterName
	) const noexcept override {
		return parameterName == "opticalFlowMethod" ||
			parameterName == "amdOpticalFlowMode" || parameterName == "nvidiaOpticalFlowQuality"
			? EffectParameterRestartReason::FrameGuidance
			: EffectParameterRestartReason::NativeBackend;
	}

	bool Resize(
		DeviceResources& deviceResources,
		ID3D11Texture2D* input,
		ID3D11Texture2D* output
	) noexcept override;

	bool Draw(const NativeEffectDrawContext& context) noexcept override;
	void SetDlssHdrProtocol(const FsrHdrProtocol& protocol) noexcept { _hdrProtocol = protocol; }

private:
	void _Reset() noexcept;

	// 以下字段仅在 MP_ENABLE_DLSS_SR 构建中使用；无 SDK 的 CI 构建里
	// ClangCL -Werror、-Wunused-private-field 会报错
	[[maybe_unused]] ID3D11Device5* _device = nullptr;
	[[maybe_unused]] ID3D11DeviceContext4* _d3dDC = nullptr;
	[[maybe_unused]] winrt::com_ptr<ID3D11Texture2D> _zeroMotionVectors;
	[[maybe_unused]] winrt::com_ptr<ID3D11UnorderedAccessView> _zeroMotionVectorsUav;
	[[maybe_unused]] winrt::com_ptr<ID3D11Texture2D> _zeroDepth;
	[[maybe_unused]] winrt::com_ptr<ID3D11UnorderedAccessView> _zeroDepthUav;
	[[maybe_unused]] winrt::com_ptr<ID3D11Texture2D> _biasCurrentColorMask;
	[[maybe_unused]] winrt::com_ptr<ID3D11UnorderedAccessView> _biasCurrentColorMaskUav;
	[[maybe_unused]] void* _parameters = nullptr;
	[[maybe_unused]] void* _feature = nullptr;
	[[maybe_unused]] bool _ngxInitialized = false;
	[[maybe_unused]] std::unique_ptr<NgxFeatureDiscovery> _featureDiscovery;
	[[maybe_unused]] bool _resetHistory = true;
	[[maybe_unused]] DLSSSRSettings _settings{};
	[[maybe_unused]] uint8_t _lastGuidanceBinding = UINT8_MAX;
	[[maybe_unused]] FrameGuidanceFrameId _lastGuidanceResetFrameId =
		std::numeric_limits<FrameGuidanceFrameId>::max();
	FsrHdrProtocol _hdrProtocol{};
};

}
