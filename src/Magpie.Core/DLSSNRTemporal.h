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
	void SetPerformanceHistory(uint64_t capture, uint64_t oldestSource, uint64_t epoch,
		unsigned maxAge, uint64_t outputRevision, ID3D11ShaderResourceView* validity) noexcept;
	// Target/source are real accepted-capture identities, relative to the pixels
	// in Draw's target context. Reprojection is not a new NR observation.
	// maxSourceAge=0 accepts only a source at the target; it never disables checks.
	void SetSparseHistory(uint64_t targetSequence, uint64_t oldestSourceSequence,
		uint64_t epoch, uint64_t observationRevision, unsigned maxSourceAge,
		uint64_t outputRevision, ID3D11ShaderResourceView* validity,
		int64_t observationTimestamp100ns) noexcept;
	bool Draw(const NativeEffectDrawContext& context, bool bypassHistory = false) noexcept;
private:
	struct Impl;
	std::unique_ptr<Impl> _impl;
};
}
