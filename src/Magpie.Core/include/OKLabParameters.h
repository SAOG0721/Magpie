#pragma once
#include "OKLabParameterTable.h"
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace Magpie::OKLab {
inline constexpr std::string_view EffectId = "OKLabImageAdjustment";
inline constexpr std::string_view Sections[] = {"Basic", "Tone", "Color", "Mixer", "Wheels",
    "Selection", "Style", "Spatial", "Curves", "Mask", "Automatic", "Calibration", "Output", "Lut"};
inline const Parameter* Find(std::string_view name) noexcept {
    for (const auto& p : Parameters) if (p.name == name) return &p;
    return nullptr;
}
inline float Normalize(const Parameter& p, float value) noexcept {
    if (!std::isfinite(value)) return p.value;
    value = std::clamp(value, p.minimum, p.maximum);
    return p.integer ? std::round(value) : value;
}
template<class Get> float Value(std::string_view name, Get&& get) noexcept {
    const auto* p = Find(name);
    return p ? Normalize(*p, get(name, p->value)) : 0;
}
inline bool Visible(std::string_view name, int section, int color, int slot, int curve) noexcept {
    const auto* p = Find(name);
    if (!p || section < 0 || section >= int(std::size(Sections)) || p->group != Sections[section]) return false;
    if (name.starts_with("mix") && name != "mixerFeather") return name[3] == char('0' + color);
    if (name.starts_with("sel")) return name[3] == char('0' + slot);
    if (name.starts_with("curve")) return name[5] == char('0' + curve);
    return true;
}
// Compare actual processing controls; dormant editor coordinates and support
// settings must not turn the neutral fast path off.
template<class Get> bool Neutral(Get&& get) noexcept {
    auto v = [&](std::string_view name) { return Value(name, get); };
    if (v("amount") == 0) return true;
    for (const auto& p : Parameters) {
        const auto n = p.name;
        if (p.group == "Output") {
            if ((n == "gamutSoft" || n == "dither") && v(n) != 0) return false;
            continue;
        }
        if (n == "amount" || n == "pivot" || n == "balance" || n == "zoneWidth" ||
            n.ends_with("Protect") || n.starts_with("skin") || n == "mixerFeather" ||
            n == "lutDomain" || n == "autoLock" || n == "autoTarget" ||
            n == "autoLimit" || n == "autoSpeed" || n == "sceneThreshold") continue;
        if (p.group == "Mask") continue;
        if (p.group == "Selection") {
            if (n == "sel0Enabled" || n == "sel1Enabled") { if (v(n) != 0) return false; }
            continue;
        }
        if (p.group == "Curves") { if (n.ends_with("Enabled") && v(n) != 0) return false; continue; }
        if (p.group == "Style") { if (n == "styleMode" && v(n) != 0 && v("styleAmount") != 0) return false; continue; }
        if (p.group == "Spatial" && (n.ends_with("Radius") || n.ends_with("Threshold") ||
            n == "edgeProtect" || n == "noiseFloor" || n == "bloomSoftness")) continue;
        if (p.group == "Calibration" && (n == "sourceKelvin" || n == "targetKelvin" || n == "whiteTint")) continue;
        if (p.group == "Wheels" && n.ends_with("Hue")) continue;
        if (v(n) != p.value) return false;
    }
    return true;
}
template<class Set> void Defaults(Set&& set, std::string_view group = {}) {
    for (const auto& p : Parameters) if (group.empty() || p.group == group) set(p.name, p.value);
}
template<class Set> void Preset(int preset, Set&& set) {
    Defaults(set);
    switch (preset) {
    case 1: set("contrast", 1.08f); set("vibrance", .2f); set("shoulder", .2f); break;
    case 2: set("shoulder", .8f); set("highlights", -.2f); set("gamutSoft", .3f); break;
    case 3: set("wheel0Hue", 220.f); set("wheel0Chroma", .025f); set("wheel2Hue", 65.f); set("wheel2Chroma", .02f); break;
    case 4: set("styleMode", 1.f); break;
    case 5: set("styleMode", 2.f); set("style0Hue", 230.f); set("style2Hue", 65.f); set("blackOut", .04f); break;
    default: break;
    }
}
}
