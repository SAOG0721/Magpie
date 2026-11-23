#pragma once
#include "NativeEffectBackend.h"

namespace Magpie {

class DeviceResources;

// Experimental colour-only XeSS-SR adapter. Magpie renders with D3D11, while
// the cross-vendor XeSS path is D3D12, so resources are shared between APIs.
class XeSSZeroMVUpscaler final : public NativeEffectBackend {
public:
	struct Impl;

	XeSSZeroMVUpscaler();
	XeSSZeroMVUpscaler(const XeSSZeroMVUpscaler&) = delete;
	XeSSZeroMVUpscaler& operator=(const XeSSZeroMVUpscaler&) = delete;
	~XeSSZeroMVUpscaler() override;

	bool Initialize(
		DeviceResources& deviceResources,
		ID3D11Texture2D* input,
		ID3D11Texture2D* output,
		bool enableOpticalFlow = false,
		bool enableJitter = false
	) noexcept;

	bool Resize(
		DeviceResources& deviceResources,
		ID3D11Texture2D* input,
		ID3D11Texture2D* output
	) noexcept override;

	bool Draw(const NativeEffectDrawContext& context) noexcept override;

private:
	struct ImplDeleter {
		void operator()(Impl* impl) const noexcept;
	};
	std::unique_ptr<Impl, ImplDeleter> _impl;
	bool _enableOpticalFlow = false;
	bool _enableJitter = false;
};

}
