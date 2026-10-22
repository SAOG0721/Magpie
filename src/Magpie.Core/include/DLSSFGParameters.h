#pragma once
#include <cmath>
#include <cstdint>

namespace Magpie {
// Stable effect values, independent of display order or SDK enum ordinals.
enum class DLSSFGRenderPreset : uint32_t { Auto = 0, A = 1, B = 2, C = 3 };
inline DLSSFGRenderPreset ParseDLSSFGRenderPreset(float value) noexcept {
    if (!std::isfinite(value) || value < 0 || value > 3 || std::floor(value) != value)
        return DLSSFGRenderPreset::Auto;
    return static_cast<DLSSFGRenderPreset>(static_cast<uint32_t>(value));
}
constexpr const char* DLSSFGRenderPresetName(DLSSFGRenderPreset preset) noexcept {
    switch (preset) {
    case DLSSFGRenderPreset::A: return "A";
    case DLSSFGRenderPreset::B: return "B";
    case DLSSFGRenderPreset::C: return "C";
    default: return "Auto";
    }
}
// These application requests are resolved at Feature creation. INI remains
// Auto; NVIDIA driver profile overrides can take precedence over both keys.
inline constexpr const char* DLSSFGUIRecompositionKey = "DLSSG.UserInterfaceRecompositionEnabled";
inline constexpr const char* DLSSFGLiteBlendingKey = "DLSSG.LiteCandidateBlendingEnabled";
template<class SetBool>
bool SubmitDLSSFGRenderPreset(DLSSFGRenderPreset preset, SetBool&& setBool) {
    if (preset == DLSSFGRenderPreset::Auto) return true;
    return setBool(DLSSFGUIRecompositionKey, preset == DLSSFGRenderPreset::B) &&
        setBool(DLSSFGLiteBlendingKey, preset == DLSSFGRenderPreset::C);
}
}
