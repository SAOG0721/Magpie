#pragma once
#include "NativeEffectBackend.h"
#include <memory>

namespace Magpie {
class DeviceResources;

// Post-chain residual history. Never feeds an accumulated image back into NGX.
class DLSSNRTemporal {
public:
	DLSSNRTemporal() noexcept;
	~DLSSNRTemporal();
	bool Initialize(DeviceResources& resources, ID3D11Texture2D* input,
		ID3D11Texture2D* base, ID3D11Texture2D* raw, ID3D11Texture2D* output,
		int mode, bool hdr, float timeMs) noexcept;
	void Reset() noexcept;
	void SetTimeConstant(float timeMs) noexcept;
	void ConfigureDetail(float chromaStrength, bool enforceZero) noexcept;
	bool Draw(const NativeEffectDrawContext& context) noexcept;
private:
	struct Impl;
	std::unique_ptr<Impl> _impl;
};
}
