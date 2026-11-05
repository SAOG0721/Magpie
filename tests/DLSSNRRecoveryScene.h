#pragma once

// Deterministic input and conservative input-change oracle for the isolated
// recovery benchmark. This is not an optical-flow or NR-response oracle.
// All surfaces are stationary between events. The occluder changes between two
// positions at an event; its changed areas are rejected rather than assigning
// false zero motion to a moving surface. GPU and CPU use identical byte colors.
//
// Capture IDs start at 1. The first event is warmup+period+2, then every period.
// With warmup=60 and period=24, events are 86, 110, 134, ... . A quality dump
// covering 121..144 includes both sides of event 134, which is off the K3 NR
// source phase 1 mod 3. Each event leaves period-1 captures for recovery.
// Across an event, affected pixels remain invalid until the chosen NR source
// belongs to the target's event interval, including when a periodic image has
// returned to an earlier appearance. This prevents stale-history resurrection.
// The HLSL interface uses uint frame IDs; keep GPU runs within UINT32_MAX.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace Magpie::RecoveryScene {

enum class Kind : uint32_t { Static = 0, Occlusion = 1, Lighting = 2, Cut = 3, Ui = 4 };

inline constexpr uint32_t DefaultPeriod = 24;
inline constexpr uint32_t EventPhase = 2;
inline constexpr uint32_t ValidityHalo = 16;

struct Rect {
    uint32_t x = 0, y = 0, width = 0, height = 0;
    uint32_t Right() const noexcept { return x + width; }
    uint32_t Bottom() const noexcept { return y + height; }
    uint64_t Area() const noexcept { return uint64_t(width) * height; }
    bool Contains(uint32_t px, uint32_t py) const noexcept {
        return px >= x && py >= y && px < Right() && py < Bottom();
    }
};

struct Rects {
    std::array<Rect, 2> values{};
    uint32_t count = 0;
};

struct State {
    uint64_t epoch = 1;
    uint64_t eventSerial = 0;
    uint32_t variant = 0;
    bool event = false;
    bool globalEvent = false;
};

struct Mapping {
    // Source texture pixel centers, suitable for dividing by texture extent.
    float sourceX = 0, sourceY = 0;
    bool valid = false;
    bool globalInvalid = false;
};

struct RgbBytes { uint32_t r, g, b; };

inline Kind ParseKind(std::string_view name) {
    if (name == "static") return Kind::Static;
    if (name == "occlusion") return Kind::Occlusion;
    if (name == "lighting") return Kind::Lighting;
    if (name == "cut") return Kind::Cut;
    if (name == "ui") return Kind::Ui;
    throw std::invalid_argument("scene must be static, occlusion, lighting, cut or ui");
}

inline const char* Name(Kind kind) noexcept {
    switch (kind) {
    case Kind::Static: return "static";
    case Kind::Occlusion: return "occlusion";
    case Kind::Lighting: return "lighting";
    case Kind::Cut: return "cut";
    case Kind::Ui: return "ui";
    }
    return "unknown";
}

inline uint64_t EventSerial(Kind kind, uint64_t frame, uint32_t warmup,
    uint32_t period = DefaultPeriod) noexcept {
    const uint64_t start = uint64_t(warmup) + EventPhase;
    return kind == Kind::Static || frame <= start ? 0 :
        (frame - start) / std::max(1u, period);
}

inline State GetState(Kind kind, uint64_t frame, uint32_t warmup,
    uint32_t period = DefaultPeriod) noexcept {
    const uint64_t serial = EventSerial(kind, frame, warmup, period);
    const bool event = serial > 0 && frame > 0 &&
        serial != EventSerial(kind, frame - 1, warmup, period);
    return { kind == Kind::Cut ? serial + 1 : 1, serial, uint32_t(serial & 3),
        event, event && (kind == Kind::Lighting || kind == Kind::Cut) };
}

inline Rect Intersect(Rect a, Rect b) noexcept {
    const uint32_t x = std::max(a.x, b.x), y = std::max(a.y, b.y);
    const uint32_t right = std::min(a.Right(), b.Right());
    const uint32_t bottom = std::min(a.Bottom(), b.Bottom());
    return { x, y, right > x ? right - x : 0, bottom > y ? bottom - y : 0 };
}

inline Rect PercentRect(uint32_t width, uint32_t height, uint32_t x0,
    uint32_t y0, uint32_t x1, uint32_t y1) noexcept {
    const auto scale = [](uint32_t extent, uint32_t percent) {
        return uint32_t(uint64_t(extent) * percent / 100);
    };
    const uint32_t x = scale(width, x0), y = scale(height, y0);
    return { x, y, scale(width, x1) - x, scale(height, y1) - y };
}

inline Rect Expand(Rect rect, uint32_t width, uint32_t height,
    uint32_t halo = ValidityHalo) noexcept {
    const uint32_t x = rect.x > halo ? rect.x - halo : 0;
    const uint32_t y = rect.y > halo ? rect.y - halo : 0;
    const uint32_t right = uint32_t(std::min(uint64_t(width), uint64_t(rect.Right()) + halo));
    const uint32_t bottom = uint32_t(std::min(uint64_t(height), uint64_t(rect.Bottom()) + halo));
    return { x, y, right > x ? right - x : 0, bottom > y ? bottom - y : 0 };
}

// Regions whose input contents can change. Both occluder positions are listed;
// the UI's full panel is conservative even though some panel pixels stay fixed.
inline Rects ChangedRects(Kind kind, uint32_t width, uint32_t height) noexcept {
    Rects result;
    if (kind == Kind::Occlusion) {
        result.values[0] = PercentRect(width, height, 18, 28, 38, 58);
        result.values[1] = PercentRect(width, height, 58, 28, 78, 58);
        result.count = 2;
    } else if (kind == Kind::Ui) {
        result.values[0] = PercentRect(width, height, 60, 10, 91, 30);
        result.count = 1;
    } else if (kind == Kind::Lighting || kind == Kind::Cut) {
        result.values[0] = { 0, 0, width, height };
        result.count = 1;
    }
    return result;
}

// A 16px halo rejects a little neighboring context. It is not a proven bound on
// the neural network's receptive field; quality comparisons must include the
// unchanged ROI too, rather than scoring only accepted or changed pixels.
inline Rects AffectedRects(Kind kind, uint32_t width, uint32_t height) noexcept {
    auto result = ChangedRects(kind, width, height);
    if (kind == Kind::Occlusion || kind == Kind::Ui)
        for (uint32_t i = 0; i < result.count; ++i)
            result.values[i] = Expand(result.values[i], width, height);
    return result;
}

// Excludes bottom 20% (private runtime indicator) and a 16px border, with a
// reduced border on tiny inputs. Rect coordinates are x/y/width/height.
inline Rect MetricsRect(uint32_t width, uint32_t height) noexcept {
    const uint32_t borderX = std::min(16u, width / 8);
    const uint32_t borderY = std::min(16u, height / 8);
    const uint32_t bottom = uint32_t(uint64_t(height) * 4 / 5);
    return { borderX, borderY, width - 2 * borderX,
        bottom > 2 * borderY ? bottom - 2 * borderY : 0 };
}

inline uint64_t UnionArea(Rects rects, Rect clip) noexcept {
    for (uint32_t i = 0; i < rects.count; ++i)
        rects.values[i] = Intersect(rects.values[i], clip);
    uint64_t area = rects.count > 0 ? rects.values[0].Area() : 0;
    if (rects.count > 1) area += rects.values[1].Area() -
        Intersect(rects.values[0], rects.values[1]).Area();
    return area;
}

inline bool CrossedEvent(Kind kind, uint64_t source, uint64_t target,
    uint32_t warmup, uint32_t period = DefaultPeriod) noexcept {
    return EventSerial(kind, source, warmup, period) !=
        EventSerial(kind, target, warmup, period);
}

// Apply the same predicate to the previous actual NR source of every full/lite
// feature and the reference. Zero MV does not assert temporal correspondence
// across changed surfaces. Scene cut additionally advances epoch.
inline bool NeedsNrReset(Kind kind, uint64_t previousNrSource, uint64_t target,
    uint32_t warmup, uint32_t period = DefaultPeriod) noexcept {
    return previousNrSource == 0 || previousNrSource > target ||
        CrossedEvent(kind, previousNrSource, target, warmup, period);
}

inline Mapping EvaluateMapping(Kind kind, uint64_t source, uint64_t target,
    uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t warmup,
    uint32_t period = DefaultPeriod) noexcept {
    Mapping result{ float(x) + .5f, float(y) + .5f, false, false };
    if (source == 0 || source > target || x >= width || y >= height) {
        result.globalInvalid = true;
        return result;
    }
    if (!CrossedEvent(kind, source, target, warmup, period)) {
        result.valid = true;
        return result;
    }
    result.globalInvalid = kind == Kind::Lighting || kind == Kind::Cut;
    result.valid = true;
    const auto affected = AffectedRects(kind, width, height);
    for (uint32_t i = 0; i < affected.count; ++i)
        if (affected.values[i].Contains(x, y)) { result.valid = false; break; }
    return result;
}

inline double EvaluateInvalidFractionInRect(Kind kind, uint64_t source,
    uint64_t target, uint32_t width, uint32_t height, uint32_t warmup,
    uint32_t period, Rect clip) noexcept {
    clip = Intersect(clip, { 0, 0, width, height });
    if (clip.Area() == 0) return 0;
    if (source == 0 || source > target) return 1;
    if (!CrossedEvent(kind, source, target, warmup, period)) return 0;
    return double(UnionArea(AffectedRects(kind, width, height), clip)) / double(clip.Area());
}

inline double EvaluateInvalidFraction(Kind kind, uint64_t source,
    uint64_t target, uint32_t width, uint32_t height, uint32_t warmup,
    uint32_t period = DefaultPeriod) noexcept {
    return EvaluateInvalidFractionInRect(kind, source, target, width, height,
        warmup, period, { 0, 0, width, height });
}

inline RgbBytes BackgroundBytes(uint32_t x, uint32_t y, uint32_t variant) noexcept {
    uint32_t hash = x * 1664525u ^ y * 1013904223u ^ variant * 374761393u;
    hash ^= hash >> 16; hash *= 2246822519u; hash ^= hash >> 13;
    const uint32_t grain = (hash >> 24) & 15u;
    const uint32_t tile = ((x / 31) ^ (y / 23)) & 1u;
    const uint32_t stripe = ((x / 7) + (y / 11)) & 1u;
    RgbBytes color{
        32 + (x * 3 + y + grain * 3 + tile * 71) % 168,
        40 + (x + y * 2 + grain * 2 + stripe * 59) % 160,
        36 + (x * 2 + y * 3 + grain * 4 + tile * 37) % 172 };
    if (variant & 1u) color = { 255 - color.b, 255 - color.r, 255 - color.g };
    if (variant & 2u) color = { color.g, color.b, color.r };
    return color;
}

inline RgbBytes ColorBytes(Kind kind, uint64_t frame, uint32_t width,
    uint32_t height, uint32_t warmup, uint32_t period, uint32_t x, uint32_t y) noexcept {
    const auto state = GetState(kind, frame, warmup, period);
    auto color = BackgroundBytes(x, y, kind == Kind::Cut ? state.variant : 0);
    if (kind == Kind::Occlusion) {
        const auto rects = ChangedRects(kind, width, height);
        const auto rect = rects.values[state.eventSerial & 1u];
        if (rect.Contains(x, y)) {
            const uint32_t lx = x - rect.x, ly = y - rect.y;
            const bool edge = lx < 3 || ly < 3 || lx + 3 >= rect.width || ly + 3 >= rect.height;
            const uint32_t check = ((lx / 9) ^ (ly / 9)) & 1u;
            color = edge ? RgbBytes{ 245, 235, 40 } :
                (check ? RgbBytes{ 210, 30, 165 } : RgbBytes{ 24, 175, 200 });
        }
    } else if (kind == Kind::Lighting) {
        const uint32_t scale = state.variant == 0 ? 256 :
            state.variant == 1 ? 352 : state.variant == 2 ? 192 : 288;
        color = { std::min(255u, color.r * scale / 256),
            std::min(255u, color.g * scale / 256), std::min(255u, color.b * scale / 256) };
    } else if (kind == Kind::Ui) {
        const auto rect = ChangedRects(kind, width, height).values[0];
        if (rect.Contains(x, y)) {
            const uint32_t lx = x - rect.x, ly = y - rect.y;
            const uint32_t cellW = std::max(1u, rect.width / 8);
            const uint32_t cellH = std::max(1u, rect.height / 5);
            const uint32_t cx = lx / cellW, cy = ly / cellH;
            const bool edge = lx < 3 || ly < 3 || lx + 3 >= rect.width || ly + 3 >= rect.height;
            const bool glyph = cx > 0 && cx < 7 && cy > 0 && cy < 4 &&
                ((cx * 3 + cy * 5 + state.variant * 7) % 5 < 2);
            const bool bar = ly > rect.height * 4 / 5 &&
                lx < rect.width * (state.variant + 1) / 5;
            color = edge ? RgbBytes{ 100, 150, 210 } :
                glyph ? RgbBytes{ 230, 240, 250 } :
                bar ? RgbBytes{ 35, 205, 125 } : RgbBytes{ 20, 28, 42 };
        }
    }
    return color;
}

inline void FillPixels(Kind kind, uint64_t frame, uint32_t width,
    uint32_t height, uint32_t warmup, uint32_t period,
    std::vector<uint8_t>& pixels, bool bgra) {
    pixels.resize(size_t(width) * height * 4);
    for (uint32_t y = 0; y < height; ++y) for (uint32_t x = 0; x < width; ++x) {
        const auto color = ColorBytes(kind, frame, width, height, warmup, period, x, y);
        const size_t offset = (size_t(y) * width + x) * 4;
        pixels[offset] = uint8_t(bgra ? color.b : color.r);
        pixels[offset + 1] = uint8_t(color.g);
        pixels[offset + 2] = uint8_t(bgra ? color.r : color.b);
        pixels[offset + 3] = 255;
    }
}

// Prefer the GPU SceneColorBytes generator below for timed/power runs. These
// CPU functions are for reproducibility and small CPU/byte-consistency checks.
inline void FillRgba(Kind kind, uint64_t frame, uint32_t width,
    uint32_t height, uint32_t warmup, uint32_t period, std::vector<uint8_t>& pixels) {
    FillPixels(kind, frame, width, height, warmup, period, pixels, false);
}
inline void FillBgra(Kind kind, uint64_t frame, uint32_t width,
    uint32_t height, uint32_t warmup, uint32_t period, std::vector<uint8_t>& pixels) {
    FillPixels(kind, frame, width, height, warmup, period, pixels, true);
}

inline constexpr const char* Hlsl = R"RECOVERY_SCENE(
uint SceneEventSerial(uint kind, uint frame, uint warmup, uint period) {
    return kind == 0 || frame <= warmup+2 ? 0 : (frame-warmup-2)/max(1u,period);
}
uint4 ScenePercentRect(uint width,uint height,uint x0,uint y0,uint x1,uint y1) {
    return uint4(width*x0/100,height*y0/100,width*x1/100,height*y1/100);
}
uint4 SceneChangedRect(uint kind,uint index,uint width,uint height) {
    if (kind==1) return index==0 ? ScenePercentRect(width,height,18,28,38,58) :
        ScenePercentRect(width,height,58,28,78,58);
    if (kind==4) return ScenePercentRect(width,height,60,10,91,30);
    return uint4(0,0,width,height);
}
uint4 SceneExpandRect(uint4 rect,uint width,uint height) {
    return uint4(rect.x>16 ? rect.x-16 : 0,rect.y>16 ? rect.y-16 : 0,
        min(width,rect.z+16),min(height,rect.w+16));
}
bool SceneContains(uint4 rect,uint2 p) {
    return p.x>=rect.x && p.y>=rect.y && p.x<rect.z && p.y<rect.w;
}
float2 SceneSourcePixel(uint2 p) { return float2(p)+.5; }
float SceneValidity(uint kind,uint sourceFrame,uint targetFrame,uint width,
    uint height,uint warmup,uint period,uint2 p) {
    if (sourceFrame==0 || sourceFrame>targetFrame || p.x>=width || p.y>=height) return 0;
    if (SceneEventSerial(kind,sourceFrame,warmup,period)==
        SceneEventSerial(kind,targetFrame,warmup,period)) return 1;
    if (kind==2 || kind==3) return 0;
    if (kind==1 && (SceneContains(SceneExpandRect(SceneChangedRect(kind,0,width,height),width,height),p) ||
        SceneContains(SceneExpandRect(SceneChangedRect(kind,1,width,height),width,height),p))) return 0;
    if (kind==4 && SceneContains(SceneExpandRect(SceneChangedRect(kind,0,width,height),width,height),p)) return 0;
    return 1;
}
uint3 SceneBackgroundBytes(uint2 p,uint variant) {
    uint hash=p.x*1664525u ^ p.y*1013904223u ^ variant*374761393u;
    hash^=hash>>16; hash*=2246822519u; hash^=hash>>13;
    uint grain=(hash>>24)&15u;
    uint tile=((p.x/31)^(p.y/23))&1u;
    uint stripe=((p.x/7)+(p.y/11))&1u;
    uint3 color=uint3(32+(p.x*3+p.y+grain*3+tile*71)%168,
        40+(p.x+p.y*2+grain*2+stripe*59)%160,
        36+(p.x*2+p.y*3+grain*4+tile*37)%172);
    if (variant&1u) color=uint3(255-color.b,255-color.r,255-color.g);
    if (variant&2u) color=color.gbr;
    return color;
}
uint3 SceneColorBytes(uint kind,uint frame,uint width,uint height,uint warmup,
    uint period,uint2 p) {
    uint serial=SceneEventSerial(kind,frame,warmup,period),variant=serial&3u;
    uint3 color=SceneBackgroundBytes(p,kind==3 ? variant : 0);
    if (kind==1) {
        uint4 rect=SceneChangedRect(kind,serial&1u,width,height);
        if (SceneContains(rect,p)) {
            uint2 local=p-rect.xy,extent=rect.zw-rect.xy;
            bool edge=local.x<3 || local.y<3 || local.x+3>=extent.x || local.y+3>=extent.y;
            uint check=((local.x/9)^(local.y/9))&1u;
            color=edge ? uint3(245,235,40) : (check ? uint3(210,30,165) : uint3(24,175,200));
        }
    } else if (kind==2) {
        uint scale=variant==0 ? 256 : variant==1 ? 352 : variant==2 ? 192 : 288;
        color=min(uint3(255,255,255),color*scale/256);
    } else if (kind==4) {
        uint4 rect=SceneChangedRect(kind,0,width,height);
        if (SceneContains(rect,p)) {
            uint2 local=p-rect.xy,extent=rect.zw-rect.xy;
            uint cellW=max(1u,extent.x/8),cellH=max(1u,extent.y/5);
            uint cx=local.x/cellW,cy=local.y/cellH;
            bool edge=local.x<3 || local.y<3 || local.x+3>=extent.x || local.y+3>=extent.y;
            bool glyph=cx>0 && cx<7 && cy>0 && cy<4 && ((cx*3+cy*5+variant*7)%5<2);
            bool bar=local.y>extent.y*4/5 && local.x<extent.x*(variant+1)/5;
            color=edge ? uint3(100,150,210) : glyph ? uint3(230,240,250) :
                bar ? uint3(35,205,125) : uint3(20,28,42);
        }
    }
    return color;
}
)RECOVERY_SCENE";

} // namespace Magpie::RecoveryScene
