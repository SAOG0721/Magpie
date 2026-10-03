#pragma once
#include "NativeEffectBackend.h"
#include <memory>
namespace Magpie {
// SDR-only, synchronous, same-resolution lmxxf runtime adapter.
class AmdLmxxfNRFilter final : public NativeEffectBackend {
public:
	AmdLmxxfNRFilter();
	~AmdLmxxfNRFilter();
	bool Initialize(DeviceResources&, ID3D11Texture2D*, ID3D11Texture2D*, const EffectOption&) noexcept;
	bool Resize(DeviceResources&, ID3D11Texture2D*, ID3D11Texture2D*) noexcept override;
	bool Draw(const NativeEffectDrawContext&) noexcept override;
	bool Drain() noexcept override;
	EffectParameterApplyMode GetParameterApplyMode(std::string_view) const noexcept override;
	bool ApplyLiveParameters(const EffectOption&, std::span<const std::string>) noexcept override;

private:
	struct Settings {
		float style = 1, intensity = 1, tone = 1, structure = 1, skin = 1;
		bool autoMask = true;
		uint32_t passes = 1;
	};
	static bool ReadSettings(const EffectOption&, Settings&) noexcept;
	Settings _settings;
	struct Impl;
	std::unique_ptr<Impl> _impl;
};
} // namespace Magpie
