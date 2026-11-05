#pragma once
#include <string_view>
#include <string>
#include "DLSSNRColorShader.h"

namespace Magpie {
inline const std::string DLSSNR_TEMPORAL_SHADER = std::string(DLSSNR_COLOR_HLSL) + R"hlsl(
Texture2D<float4> Input : register(t0);
Texture2D<float4> Base : register(t1);
Texture2D<float4> Raw : register(t2);
Texture2D<float4> History : register(t3);
Texture2D<float4> PreviousGuide : register(t4);
Texture2D<float2> Motion : register(t5);
Texture2D<float4> LowResidual : register(t6);
Texture2D<float4> LowGuide : register(t7);
Texture2D<float> ReuseValidity : register(t8);
RWTexture2D<float4> Output : register(u0);
RWTexture2D<float4> NextHistory : register(u1);
RWTexture2D<float4> NextGuide : register(u2);
SamplerState LinearClamp : register(s0);
cbuffer Settings : register(b0) {
    uint2 Size; uint UseMotion; uint Hdr;
    float HistoryWeight; uint Route; uint2 LowSize;
    uint4 Region; // x, y, exclusive right, exclusive bottom
    float ChromaStrength; uint EnforceZero; uint UseReuseValidity; uint BypassHistory;
};
float4 FinalTemporalColor(float4 color) {
    float3 result = color.rgb;
    [branch] if (Hdr) result = clamp(color.rgb,-65504,65504);
    else result = FinalizeSDRColor(color.rgb);
    return float4(result,color.a);
}
float3 Guide(float3 c) { return Hdr ? c / (1 + abs(c)) : c; }
bool Inside(int2 p) { return all(p >= int2(Region.xy)) && all(p < int2(Region.zw)); }
bool PreviousPosition(int2 p, out float2 previous) {
    previous = p;
    if (!Inside(p)) return false;
    if (UseMotion) {
        float2 mv = Motion.Load(int3(p, 0));
        if (!all(isfinite(mv))) return false;
        previous += mv;
    }
    // Validate the entire bilinear footprint before a history/guide read.
    return all(previous >= float2(Region.xy)) &&
        all(previous <= float2(Region.zw) - 1);
}
// G: signed half-resolution observations, upsampled with original-color guidance.
// Return support explicitly: a failed reconstruction must not publish history.
bool Reconstruct(int2 p, float3 color, out float3 residual) {
    int2 a = clamp(int2(floor(float2(p)*.5-.25)), 0, int2(LowSize)-1);
    int2 b = clamp(int2(floor(float2(p)*.5-.25))+1, 0, int2(LowSize)-1);
    float2 ca = min(float2(a)*2+.5, float2(Size)-1);
    float2 cb = min(float2(b)*2+.5, float2(Size)-1);
    float2 f = saturate((float2(p)-ca)/max(cb-ca, 1));
    residual = 0;
    float mass = 0;
    [unroll] for (int y=0; y<2; ++y) {
        [unroll] for (int x=0; x<2; ++x) {
            int2 n = int2(x ? b.x : a.x, y ? b.y : a.y);
            float4 r = LowResidual.Load(int3(n,0));
            float4 g = LowGuide.Load(int3(n,0));
            if (!all(isfinite(r)) || !all(isfinite(g)) || r.a < .999 || g.a < .999) continue;
            float3 delta = abs(color-g.rgb);
            float accept = 1-smoothstep(.02,.10,max(delta.r,max(delta.g,delta.b)));
            float w = (x ? f.x : 1-f.x)*(y ? f.y : 1-f.y)*accept;
            if (w <= 0) continue;
            residual += w*r.rgb; mass += w;
        }
    }
    if (mass < .05) return false;
    residual /= mass;
    return true;
}
// G validates individual history taps before interpolation to limit edge leaks.
bool GatherHistory(float2 previous, float3 color, out float4 result) {
    int2 a = int2(floor(previous));
    float2 f = frac(previous);
    result = 0;
    float mass = 0;
    [unroll] for (int y=0; y<2; ++y) {
        [unroll] for (int x=0; x<2; ++x) {
            int2 n = a+int2(x,y);
            float w = (x ? f.x : 1-f.x)*(y ? f.y : 1-f.y);
            if (w <= 0 || !Inside(n)) continue;
            float4 g = PreviousGuide.Load(int3(n,0));
            float4 h = History.Load(int3(n,0));
            if (!all(isfinite(g)) || !all(isfinite(h)) || g.a < .999 || h.a < .999) continue;
            float3 delta = abs(color-g.rgb);
            w *= 1-smoothstep(.025,.10,max(delta.r,max(delta.g,delta.b)));
            if (w <= 0) continue;
            result += w*h; mass += w;
        }
    }
    if (mass < .25) return false;
    result /= mass;
    return true;
}
[numthreads(8,8,1)]
void main(uint3 id : SV_DispatchThreadID) {
    int2 p = id.xy;
    if (any(id.xy >= Size)) return;
    float4 original = Input.Load(int3(p,0));
    float4 base = Base.Load(int3(p,0));
    float4 raw = Raw.Load(int3(p,0));
    bool finiteInput = all(isfinite(original));
    bool finiteCurrent = finiteInput && all(isfinite(base)) && all(isfinite(raw));
    NextGuide[p] = finiteInput ? float4(Guide(original.rgb),1) : 0;
    if (!finiteCurrent) {
        NextHistory[p] = 0;
        Output[p] = all(isfinite(base)) ? base : (finiteInput ? original : float4(0,0,0,1));
        return;
    }
    if (BypassHistory) {
        NextHistory[p] = 0;
        Output[p] = FinalTemporalColor(raw);
        return;
    }
    float3 current = raw.rgb - base.rgb;
    // Invalid or only partially trusted enhancement must not be resurrected
    // through the post-filter. Current-frame lightweight NR remains intact.
    float reuse = 1;
    if ((UseReuseValidity & 1) != 0) {
        float validity = ReuseValidity.SampleLevel(LinearClamp,(p+.5)/Size,0);
        bool sparse = (UseReuseValidity & 2) != 0;
        if (!isfinite(validity) || (sparse ? validity <= 0 : validity < .999)) {
            NextHistory[p] = 0;
            Output[p] = FinalTemporalColor(raw);
            return;
        }
        if (sparse) reuse = saturate(validity);
    }
    // Sparse streams transport a repeated observation with weight=1; a new NR
    // observation uses its own elapsed time. Soft confidence cannot resurrect
    // a completely rejected correction. Legacy flags 0/1 retain their gate.
    float historyWeight = HistoryWeight * reuse;
    // New SDR detail mode has an exact zero-current contract. Do not retain a
    // prior correction at pixels where the total correction has vanished.
    if (EnforceZero && all(current == 0)) {
        NextHistory[p] = 0;
        Output[p] = FinalTemporalColor(raw);
        return;
    }
    float3 detail = 0;
    if (Route == 4) {
        float3 low;
        if (!Reconstruct(p, Guide(original.rgb), low)) {
            NextHistory[p] = 0;
            Output[p] = FinalTemporalColor(raw);
            return;
        }
        detail = current-low; // Exact current high-frequency complement, not temporally filtered.
        current = low;
    }
    // F stores conditional amplitude in RGB and 1+support in alpha. Missing
    // observations update support only; they never average zeros into amplitude.
    float3 delta = abs(Guide(raw.rgb)-Guide(base.rgb));
    float observed = smoothstep(.005,.02,max(delta.r,max(delta.g,delta.b)));
    float support = observed;
    float3 result = current;
    float2 previous;
    if (historyWeight > 0 && PreviousPosition(p, previous)) {
        float error = 0;
        float maximumError = 0;
        bool valid = true;
        float3 lo = current, hi = current;
        [unroll] for (int y=-1; y<=1; ++y) {
            [unroll] for (int x=-1; x<=1; ++x) {
                int2 n = p + int2(x,y);
                float2 previousN;
                if (!PreviousPosition(n, previousN)) { valid = false; continue; }
                // Reject patch support crossing a motion discontinuity.
                if (UseMotion && any(abs((previousN-n)-(previous-p)) > 2)) { valid = false; continue; }
                float4 inputN = Input.Load(int3(n,0));
                float4 old = PreviousGuide.SampleLevel(LinearClamp, (previousN+.5)/Size, 0);
                float3 residualN = Raw.Load(int3(n,0)).rgb - Base.Load(int3(n,0)).rgb;
                if (!all(isfinite(inputN)) || !all(isfinite(old)) || old.a < .999 ||
                    !all(isfinite(residualN))) { valid = false; continue; }
                float3 delta = abs(Guide(inputN.rgb) - old.rgb);
                float e = max(delta.r, max(delta.g, delta.b));
                error += e / 9;
                maximumError = max(maximumError, e);
                lo = min(lo, residualN); hi = max(hi, residualN);
            }
        }
        // Raw-color patch matching is the initial B.1 baseline: no residual blur.
        float q = (1-smoothstep(.008, .04, error)) *
                  (1-smoothstep(.025, .10, maximumError));
        if (valid && q > 0) {
            float4 old = 0;
            bool historyValid = true;
            if (Route == 4) historyValid = GatherHistory(previous, Guide(original.rgb), old);
            else old = History.SampleLevel(LinearClamp, (previous+.5)/Size, 0);
            if (historyValid && all(isfinite(old)) && old.a >= .999) {
                // Include the center and permit a stable correction to decay even
                // when the current residual vanishes. A tight variance box would
                // erase the very on/off history this filter needs to stabilize.
                float3 margin = .02 + q * abs(old.rgb);
                float3 safe = clamp(old.rgb, lo-margin, hi+margin);
                if (Route == 3) {
                    float oldSupport = saturate(old.a-1);
                    // 60 ms attack / 180 ms release, derived from the same capture
                    // interval as the 80 ms amplitude EMA. q is applied once.
                    float memory = pow(abs(historyWeight), .08/(observed > oldSupport ? .06 : .18))*q;
                    support = lerp(observed, oldSupport, memory);
                    if (oldSupport <= .001) result = current;
                    else if (observed > 0 && dot(current, old.rgb) < 0) {
                        // An opposite direction starts a new support episode.
                        support = observed*(1-pow(abs(historyWeight),.08/.06)*q);
                        result = current;
                    } else {
                        float update = (1-historyWeight*q)*observed;
                        result = lerp(safe, current, update);
                    }
                } else result = lerp(current, safe, historyWeight*q);
                if (!Hdr && ChromaStrength > 0) {
                    // Reuse the same validated history, footprint and confidence.
                    // Existing luma result is retained; additional memory only
                    // affects Lab chroma. Signed RGB history remains the storage.
                    float3 nowLab = RGBToLab(base.rgb+detail+(Route == 3 ? support*result : result));
                    float3 oldLab = RGBToLab(base.rgb+detail+safe);
                    float extra = ChromaStrength*historyWeight*q*(Route == 3 ? min(observed,support) : 1);
                    nowLab.yz = lerp(nowLab.yz,oldLab.yz,extra);
                    float gamutScale;
                    float3 extraResidual = MapLab(nowLab,gamutScale)-base.rgb-detail;
                    if (Route == 3 && support > 1e-6) result = extraResidual/support;
                    else if (Route != 3) result = extraResidual;
                }
            }
        }
    }
    // FP16 history is signed; its alpha marks a valid observation, not opacity.
    NextHistory[p] = float4(clamp(result,-65504,65504),Route == 3 ? 1+support : 1);
    float3 color = base.rgb + detail + (Route == 3 ? support*result : result);
    Output[p] = FinalTemporalColor(float4(color,raw.a));
}
)hlsl";

// G's observations come from the exact post-chain difference, including every
// pass's controls, clipping and quantization. This does not change NGX inputs.
inline constexpr std::string_view DLSSNR_TEMPORAL_REDUCE_SHADER = R"hlsl(
Texture2D<float4> Input : register(t0);
Texture2D<float4> Base : register(t1);
Texture2D<float4> Raw : register(t2);
RWTexture2D<float4> Residual : register(u0);
RWTexture2D<float4> Guide : register(u1);
cbuffer Settings : register(b0) {
    uint2 Size; uint UseMotion; uint Hdr;
    float HistoryWeight; uint Route; uint2 LowSize;
    uint4 Region;
    float ChromaStrength; uint EnforceZero; uint UseReuseValidity; uint BypassHistory;
};
[numthreads(8,8,1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= LowSize)) return;
    float3 r = 0, g = 0;
    float count = 0;
    bool valid = true;
    [unroll] for (uint y=0; y<2; ++y) {
        [unroll] for (uint x=0; x<2; ++x) {
            uint2 p = id.xy*2+uint2(x,y);
            if (any(p >= Size)) continue; // Odd dimensions retain their last sample.
            if (any(p < Region.xy) || any(p >= Region.zw)) { valid = false; continue; }
            float4 i = Input.Load(int3(p,0));
            float4 b = Base.Load(int3(p,0));
            float4 o = Raw.Load(int3(p,0));
            if (!all(isfinite(i)) || !all(isfinite(b)) || !all(isfinite(o))) { valid = false; continue; }
            r += o.rgb-b.rgb;
            g += Hdr ? i.rgb/(1+abs(i.rgb)) : i.rgb;
            count += 1;
        }
    }
    if (!valid || count == 0) { Residual[id.xy] = 0; Guide[id.xy] = 0; return; }
    Residual[id.xy] = float4(clamp(r/count,-65504,65504),1);
    Guide[id.xy] = float4(g/count,1);
}
)hlsl";
}
