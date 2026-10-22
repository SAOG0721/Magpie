#pragma once
#include "FrameGuidanceTypes.h"
#include "DLSSFGParameters.h"

namespace Magpie {

class DeviceResources;
class NgxD3D12Core;
class ReflexController;

struct DLSSFrameGenerationSettings {
	uint32_t multiplier = 2;
	DLSSFGRenderPreset renderPreset = DLSSFGRenderPreset::Auto;
	MotionVectorRequest motionRequest{};
};

// Experimental DLSS Frame Generation adapter. It consumes final effect-chain
// color plus Renderer-owned guidance from the same captured base frame.
class DLSSFrameGenerator {
public:
	struct Impl;
	using PublishCallback = std::function<bool(ID3D11Texture2D*, uint64_t)>;

	DLSSFrameGenerator();
	DLSSFrameGenerator(const DLSSFrameGenerator&) = delete;
	DLSSFrameGenerator& operator=(const DLSSFrameGenerator&) = delete;
	~DLSSFrameGenerator();

	bool Initialize(
		DeviceResources& resources,
		NgxD3D12Core& ngxCore,
		ID3D11Texture2D* input,
		FrameGuidanceExtent guidanceExtent,
		const DLSSFrameGenerationSettings& settings
	) noexcept;
	bool Resize(
		DeviceResources& resources,
		NgxD3D12Core& ngxCore,
		ID3D11Texture2D* input,
		FrameGuidanceExtent guidanceExtent
	) noexcept;
	bool Draw(
		ID3D11Texture2D* input,
		FrameGuidanceFrameId frameId,
		const FrameGuidanceView& guidance,
		const FrameGuidanceView& zeroGuidance,
		const PublishCallback& publishGeneratedFrame) noexcept;
	void RequestHistoryReset() noexcept;
	void SetReflexController(ReflexController* controller) noexcept;
	bool Drain() noexcept;

	FrameGuidanceRequirements GetFrameGuidanceRequirements() const noexcept;
	const DLSSFrameGenerationSettings& Settings() const noexcept {
		return _requestedSettings;
	}
	uint32_t Multiplier() const noexcept;
	uint32_t MaxSupportedMultiplier() const noexcept;

private:
	// 仅在 MP_ENABLE_DLSS_FRAME_GENERATION 构建中使用；无 SDK 的 CI 构建里 ClangCL -Werror 会报未使用
	[[maybe_unused]] std::unique_ptr<Impl> _impl;
	DLSSFrameGenerationSettings _requestedSettings{};
};

}
