#pragma once
#include "NativeEffectBackend.h"

namespace Magpie {

class DeviceResources;

// Experimental FSR 3.1.5 / FSR 4.1.1 upscaler running on D3D12 through
// resources shared with Magpie's D3D11 renderer. Frame generation is omitted.
class FSR3ZeroMVUpscaler final : public NativeEffectBackend {
public:
	struct Impl;

	FSR3ZeroMVUpscaler();
	FSR3ZeroMVUpscaler(const FSR3ZeroMVUpscaler&) = delete;
	FSR3ZeroMVUpscaler& operator=(const FSR3ZeroMVUpscaler&) = delete;
	~FSR3ZeroMVUpscaler() override;

	bool Initialize(DeviceResources& resources, ID3D11Texture2D* input,
		ID3D11Texture2D* output, bool enableOpticalFlow = false,
		bool enableJitter = false, bool useFsr4 = false) noexcept;
	bool Resize(DeviceResources& resources, ID3D11Texture2D* input,
		ID3D11Texture2D* output) noexcept override;
	bool Draw(const NativeEffectDrawContext& context) noexcept override;

private:
	struct ImplDeleter { void operator()(Impl* impl) const noexcept; };
	std::unique_ptr<Impl, ImplDeleter> _impl;
	bool _enableOpticalFlow = false;
	bool _enableJitter = false;
	bool _useFsr4 = false;
};

}
