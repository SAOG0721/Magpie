#pragma once
#include "EffectDesc.h"
#include "NvidiaVfxParameters.h"
#include <string>
#include <type_traits>
#include <algorithm>

namespace Magpie {

inline void FilterNvidiaVfxVsrChoices(std::string_view effect, EffectParameterDesc& parameter, int category) {
	if (effect != NVIDIA_VFX_VSR_ID || parameter.name != "strength" || category != 2) return;
	std::erase_if(parameter.choices, [](const auto& choice) { return choice.value != 1 && choice.value != 3; });
}

inline constexpr std::string_view NVIDIA_VFX_VSR_STATE_PREFIX = "__nvidiaVfx";

template<class Char> std::basic_string<Char> NvidiaVfxStateKey(int category, std::string_view suffix) {
	const std::string key = std::string(NVIDIA_VFX_VSR_STATE_PREFIX) + std::to_string(category) + "." + std::string(suffix);
	return { key.begin(), key.end() };
}

inline bool IsNvidiaVfxVsrStateKey(std::string_view name) noexcept {
	if (!name.starts_with(NVIDIA_VFX_VSR_STATE_PREFIX)) return false;
	name.remove_prefix(NVIDIA_VFX_VSR_STATE_PREFIX.size());
	if (name.size() < 3 || name[0] < '0' || name[0] > '4' || name[1] != '.') return false;
	const int category = name[0] - '0';
	const auto suffix = name.substr(2);
	return suffix == "tier" || (!NvidiaVfxSameSize(category) &&
		(suffix == "scalingType" || suffix == "scaleX" || suffix == "scaleY"));
}

// Persist the per-family choices in the same parameter map used by config
// import/export. The hidden keys are state, never vendor parameters or controls.
template<class Effect> void ChangeNvidiaVfxVsrCategory(Effect& effect, int next) {
	using Key = typename std::remove_reference_t<decltype(effect.parameters)>::key_type;
	using Char = typename Key::value_type;
	const Key categoryKey{ Char('c'), Char('a'), Char('t'), Char('e'), Char('g'), Char('o'), Char('r'), Char('y') };
	const Key strengthKey{ Char('s'), Char('t'), Char('r'), Char('e'), Char('n'), Char('g'), Char('t'), Char('h') };
	const auto categoryIt = effect.parameters.find(categoryKey);
	const int previous = NvidiaVfxCategory(categoryIt == effect.parameters.end() ? 0.f : categoryIt->second);
	next = NvidiaVfxCategory(float(next));
	if (previous == next) return;
	const auto strengthIt = effect.parameters.find(strengthKey);
	effect.parameters[NvidiaVfxStateKey<Char>(previous, "tier")] = float(NvidiaVfxTier(previous,
		strengthIt == effect.parameters.end() ? 1.f : strengthIt->second));
	if (!NvidiaVfxSameSize(previous)) {
		effect.parameters[NvidiaVfxStateKey<Char>(previous, "scalingType")] = float(effect.scalingType);
		effect.parameters[NvidiaVfxStateKey<Char>(previous, "scaleX")] = effect.scale.first;
		effect.parameters[NvidiaVfxStateKey<Char>(previous, "scaleY")] = effect.scale.second;
	}
	const auto read = [&](std::string_view suffix, float fallback) {
		const auto it = effect.parameters.find(NvidiaVfxStateKey<Char>(next, suffix));
		return it != effect.parameters.end() && std::isfinite(it->second) ? it->second : fallback;
	};
	effect.parameters[categoryKey] = float(next);
	effect.parameters[strengthKey] = float(NvidiaVfxTier(next, read("tier", 1.f)));
	if (NvidiaVfxSameSize(next)) {
		effect.scalingType = decltype(effect.scalingType)(0);
		effect.scale = { 1.f, 1.f };
	} else {
		const float type = read("scalingType", 1.f);
		effect.scalingType = decltype(effect.scalingType)(type >= 0 && type <= 3 && std::floor(type) == type ? int(type) : 1);
		const float x = read("scaleX", 1.f), y = read("scaleY", 1.f);
		effect.scale = { x > 0 ? x : 1.f, y > 0 ? y : 1.f };
	}
}

} // namespace Magpie
