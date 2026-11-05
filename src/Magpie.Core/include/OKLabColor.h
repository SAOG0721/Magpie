#pragma once
#include <array>
#include <cmath>
#include <atomic>
#include <algorithm>

namespace Magpie::OKLab {
using Color = std::array<float,3>;
inline Color ToLab(Color rgb) noexcept {
    const double r=rgb[0],g=rgb[1],b=rgb[2];
    const double l=std::cbrt(.4122214708*r+.5363325363*g+.0514459929*b);
    const double m=std::cbrt(.2119034982*r+.6806995451*g+.1073969566*b);
    const double s=std::cbrt(.0883024619*r+.2817188376*g+.6299787005*b);
    return {float(.2104542553*l+.7936177850*m-.0040720468*s),
        float(1.9779984951*l-2.4285922050*m+.4505937099*s),
        float(.0259040371*l+.7827717662*m-.8086757660*s)};
}
inline float Decode(float value) noexcept {
    const float a=std::abs(value);
    return std::copysign(a<=.04045f?a/12.92f:std::pow((a+.055f)/1.055f,2.4f),value);
}
inline Color PreviewColor(float a,float b) noexcept {
    const double l=.7+.3963377774*a+.2158037573*b;
    const double m=.7-.1055613458*a-.0638541728*b;
    const double s=.7-.0894841775*a-1.2914855480*b;
    Color rgb{float(4.0767416621*l*l*l-3.3077115913*m*m*m+.2309699292*s*s*s),
        float(-1.2684380046*l*l*l+2.6097574011*m*m*m-.3413193965*s*s*s),
        float(-.0041960863*l*l*l-.7034186147*m*m*m+1.7076147010*s*s*s)};
    for(auto& v:rgb)v=std::clamp(v<=.0031308f?v*12.92f:1.055f*std::pow(v,1.f/2.4f)-.055f,0.f,1.f);
    return rgb;
}
inline std::array<float,2> WheelValue(float x,float y,float previousHue) noexcept {
    const float length=std::hypot(x,y);
    return {length<1e-6f?previousHue:std::fmod(std::atan2(y,x)*57.2957795f+360.f,360.f),
        std::min(length,1.f)*.1f};
}
struct PickResult {
    // The backend publishes data once, then completion with release semantics.
    Color lab{};
    std::atomic<int> completion{0}; // 0 pending, 1 ready, -1 failed
};
}
