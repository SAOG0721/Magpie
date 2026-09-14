#pragma once
#include "NativeEffectBackend.h"
#include "GroupBEffectProtocol.h"

namespace Magpie {

class DeviceResources;
class NgxD3D12Core;

struct DLSSNRSettings {
	bool enableInputResolutionScaling = false;
	uint32_t inputResolutionPercent = 100;
	float residualMultiplier = 1.0f;
	float residualSaturation = 1.0f;
	float residualLightness = 1.0f;
	float shadowStructureMultiplier = 1.0f;
	float reflectionGlowMultiplier = 1.0f;
	int style = 0;
	float intensity = 1.0f;
	float localToneStrength = 1.0f;
	float localStructureStrength = 1.0f;
	float skinStructureStrength = 0.0f;
	// Number of chained DLSSNR evaluations per frame. Layer N reads layer N-1.
	int layerCount = 1;
	bool useAutoMask = false;
	bool uiCorrection = false;
	MotionVectorRequest motionRequest{};
	// Experimental FP16 path. SDR RGBA8 remains the default.
	DlssnrExperimentProtocol experimentalHdr{};
};

DLSSNRSettings ParseDLSSNRSettings(const EffectOption& option, bool hdrEnabled = false) noexcept;

// Experimental same-resolution DLSS neural filter. Magpie only owns the
// composited colour frame. Motion uses shared optical flow when selected;
// depth and unavailable motion use explicit zero guides.
class DLSSNRFilter final : public NativeEffectBackend {
public:
	struct Impl;

	DLSSNRFilter();
	DLSSNRFilter(const DLSSNRFilter&) = delete;
	DLSSNRFilter& operator=(const DLSSNRFilter&) = delete;
	~DLSSNRFilter() override;

	FrameGuidanceRequirements GetFrameGuidanceRequirements() const noexcept override;
	bool Drain() noexcept override;
	EffectParameterApplyMode GetParameterApplyMode(
		std::string_view parameterName
	) const noexcept override;
	EffectParameterRestartReason GetParameterRestartReason(
		std::string_view parameterName
	) const noexcept override;
	bool ApplyLiveParameters(
		const EffectOption& option,
		std::span<const std::string> parameterNames
	) noexcept override;

	bool Initialize(
		DeviceResources& resources,
		NgxD3D12Core& ngxCore,
		ID3D11Texture2D* input,
		ID3D11Texture2D* output,
		const DLSSNRSettings& settings
	) noexcept;

	bool Resize(
		DeviceResources& resources,
		ID3D11Texture2D* input,
		ID3D11Texture2D* output
	) noexcept override;

	bool Draw(const NativeEffectDrawContext& context) noexcept override;

private:
	// 仅在 MP_ENABLE_DLSSNR 构建中使用；无 SDK 的 CI 构建里 ClangCL -Werror 会报未使用
	[[maybe_unused]] std::unique_ptr<Impl> _impl;
	[[maybe_unused]] DLSSNRSettings _settings;
	[[maybe_unused]] NgxD3D12Core* _ngxCore = nullptr;
};

}
