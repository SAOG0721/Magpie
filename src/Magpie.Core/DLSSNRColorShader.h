#pragma once
#include <string_view>

namespace Magpie {
// Oklab matrices by Bjorn Ottosson, public domain:
// https://bottosson.github.io/posts/oklab/ (2021-01-25 matrices).
// Shared by residual controls and output temporal filtering; SDR stored RGB.
inline constexpr std::string_view DLSSNR_COLOR_HLSL = R"hlsl(
float DecodeSDR(float v) {
    float a = abs(v);
    return sign(v) * (a <= .04045 ? a / 12.92 : pow((a + .055) / 1.055, 2.4));
}
float EncodeSDR(float v) {
    float a = abs(v);
    return sign(v) * (a <= .0031308 ? a * 12.92 : 1.055 * pow(a, 1.0 / 2.4) - .055);
}
float3 DecodeRGB(float3 c) { return float3(DecodeSDR(c.r),DecodeSDR(c.g),DecodeSDR(c.b)); }
float3 EncodeRGB(float3 c) { return float3(EncodeSDR(c.r),EncodeSDR(c.g),EncodeSDR(c.b)); }
float3 LinearToLab(float3 c) {
    float3 lms = float3(dot(c,float3(.4122214708,.5363325363,.0514459929)),
        dot(c,float3(.2119034982,.6806995451,.1073969566)),
        dot(c,float3(.0883024619,.2817188376,.6299787005)));
    lms = sign(lms)*pow(abs(lms),1.0/3.0);
    return float3(dot(lms,float3(.2104542553,.7936177850,-.0040720468)),
        dot(lms,float3(1.9779984951,-2.4285922050,.4505937099)),
        dot(lms,float3(.0259040371,.7827717662,-.8086757660)));
}
float3 LabToLinear(float3 c) {
    float3 v = float3(c.x+.3963377774*c.y+.2158037573*c.z,
        c.x-.1055613458*c.y-.0638541728*c.z,
        c.x-.0894841775*c.y-1.2914855480*c.z);
    v = v*v*v;
    return float3(dot(v,float3(4.0767416621,-3.3077115913,.2309699292)),
        dot(v,float3(-1.2684380046,2.6097574011,-.3413193965)),
        dot(v,float3(-.0041960863,-.7034186147,1.7076147010)));
}
float3 RGBToLab(float3 c) { return LinearToLab(DecodeRGB(c)); }
// Value-continuous constant-L, constant-hue ray mapping. A tiny tolerance only
// absorbs matrix round-trip error; final UNORM clamp is a numerical guard.
bool InSDRGamut(float3 c) { return all(c >= -1e-6) && all(c <= 1.000001); }
float3 MapLab(float3 lab, out float scale) {
    lab.x = saturate(lab.x);
    float3 rgb = LabToLinear(lab);
    scale = 1;
    if (!InSDRGamut(rgb)) {
        float lo = 0, hi = 1;
        [loop] for (int k=0;k<24;++k) {
            float mid = (lo+hi)*.5;
            float3 trial = LabToLinear(float3(lab.x,lab.yz*mid));
            if (InSDRGamut(trial)) lo = mid; else hi = mid;
        }
        scale = lo;
        rgb = LabToLinear(float3(lab.x,lab.yz*lo));
    }
    return saturate(EncodeRGB(rgb));
}
// The only SDR output boundary. In-gamut RGB keeps its exact values; signed
// candidates are mapped after interpolation and optional temporal filtering.
float3 FinalizeSDRColor(float3 color, out float scale) {
    scale = 1;
    if (!all(isfinite(color))) { scale = 0; return 0; }
    if (all(color >= 0) && all(color <= 1)) return color;
    return MapLab(RGBToLab(color),scale);
}
float3 FinalizeSDRColor(float3 color) {
    float scale;
    return FinalizeSDRColor(color,scale);
}
)hlsl";
}
