#pragma once
#include <algorithm>
#include <cstdint>

namespace Magpie::DlssgSm86Policy {

enum class Route { Unsupported, NativeFamily, NewerPtx };

// Compute capability is obtained from the CUDA device matching the DXGI LUID,
// never from an adapter display name. Future architectures require validation.
constexpr Route SelectRoute(int major, int minor) noexcept {
	if ((major == 7 && minor == 5) || (major == 8 && minor == 6)) return Route::NativeFamily;
	if ((major == 8 && minor == 9) || (major == 12 && minor == 0)) return Route::NewerPtx;
	return Route::Unsupported;
}

// A missing legacy MFG key can still provide 2x. A returned zero is an explicit
// no-generation capability and must not be promoted to one generated frame.
constexpr uint32_t MaxMultiplier(bool querySucceeded, uint32_t generatedFrames) noexcept {
	return querySucceeded ? std::min(generatedFrames, 3u) + 1u : 2u;
}

constexpr uint32_t EffectiveMultiplier(uint32_t requested, uint32_t maximum) noexcept {
	return std::min(std::clamp(requested, 2u, 4u), maximum);
}

}
