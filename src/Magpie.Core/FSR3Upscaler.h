#pragma once
#include "NativeEffectBackend.h"
#include "GroupBEffectProtocol.h"

namespace Magpie {

class DeviceResources;

// Experimental FSR 3.1.5 / FSR 4.1.1 upscaler running on D3D12 through
// resources shared with Magpie's D3D11 renderer. Frame generation is omitted.
class FSR3Upscaler final : public NativeEffectBackend {
public:
	struct Impl;

	FSR3Upscaler();
	FSR3Upscaler(const FSR3Upscaler&) = delete;
	FSR3Upscaler& operator=(const FSR3Upscaler&) = delete;
	~FSR3Upscaler() override;

	bool Initialize(DeviceResources& resources, ID3D11Texture2D* input,
		ID3D11Texture2D* output, MotionVectorRequest motionRequest = {}, bool useFsr4 = false) noexcept;
	bool Resize(DeviceResources& resources, ID3D11Texture2D* input,
		ID3D11Texture2D* output) noexcept override;
	bool Draw(const NativeEffectDrawContext& context) noexcept override;
	void SetFsrHdrProtocol(const FsrHdrProtocol& protocol) noexcept {
		_hdrProtocol = protocol;
	}
	FrameGuidanceRequirements GetFrameGuidanceRequirements() const noexcept override {
		FrameGuidanceRequirements result{ .zero = true };
		result.Add(_motionRequest);
		return result;
	}

	EffectParameterRestartReason GetParameterRestartReason(std::string_view) const noexcept override {
		return EffectParameterRestartReason::FrameGuidance;
	}

private:
	struct ImplDeleter { void operator()(Impl* impl) const noexcept; };
	MotionVectorRequest _motionRequest{};
	[[maybe_unused]] std::unique_ptr<Impl, ImplDeleter> _impl;
	[[maybe_unused]] bool _useFsr4 = false;
	FsrHdrProtocol _hdrProtocol{};
};

}
