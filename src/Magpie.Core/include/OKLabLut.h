#pragma once
#include <array>
#include <algorithm>
#include <cmath>
#include <locale>
#include <sstream>
#include <string>
#include <vector>

namespace Magpie::OKLab {
// Cube files use red-fastest indexing. Preserve every source node (2..65),
// packed as x=r+edge*b, y=g inside a fixed maximum-size GPU texture.
// File contents are embedded in the effect configuration for portable presets.
inline constexpr unsigned LutAtlasEdge=65, LutAtlasWidth=LutAtlasEdge*LutAtlasEdge;
struct Cube {
    unsigned size = 0;
    std::array<float,3> minimum{0,0,0}, maximum{1,1,1};
    std::vector<std::array<float,3>> values;
};
inline bool ParseCube(std::string_view text, Cube& result, std::string& error) {
    result = {}; error.clear();
    if (text.size() > 16*1024*1024) { error="LUT exceeds 16 MiB"; return false; }
    Cube cube;
    std::istringstream stream{std::string(text)};
    stream.imbue(std::locale::classic());
    std::string line;
    bool hasMin=false,hasMax=false;
    while (std::getline(stream,line)) {
        if (const auto comment=line.find('#');comment!=std::string::npos) line.resize(comment);
        std::istringstream row(line); row.imbue(std::locale::classic());
        std::string token; if (!(row>>token)) continue;
        if (token=="TITLE") continue;
        if (token=="LUT_3D_SIZE") {
            if(cube.size!=0 || !(row>>cube.size) || cube.size<2 || cube.size>65) { error="LUT_3D_SIZE must be 2..65"; return false; }
        } else if (token=="DOMAIN_MIN" || token=="DOMAIN_MAX") {
            auto& domain=token=="DOMAIN_MIN"?cube.minimum:cube.maximum;
            bool& seen=token=="DOMAIN_MIN"?hasMin:hasMax;
            if(seen || !cube.values.empty()) {error="Duplicate or late LUT domain";return false;}
            seen=true;
            for(float& x:domain) if(!(row>>x)||!std::isfinite(x)||std::abs(x)>65504) {error="Invalid LUT domain";return false;}
        } else {
            std::istringstream numbers(line); numbers.imbue(std::locale::classic());
            std::array<float,3> color{};
            for(float& x:color) if(!(numbers>>x)||!std::isfinite(x)||std::abs(x)>65504) {error="Expected a finite RGB triplet";return false;}
            if(numbers>>token) {error="Unexpected trailing LUT data";return false;}
            if(cube.size==0 || cube.values.size()>=size_t(cube.size)*cube.size*cube.size) {error="Unexpected LUT sample count";return false;}
            cube.values.push_back(color);
            continue;
        }
        if(row>>token) {error="Unexpected LUT header value";return false;}
    }
    if(cube.size==0 || cube.values.size()!=size_t(cube.size)*cube.size*cube.size) {error="Incomplete 3D LUT";return false;}
    for(unsigned i=0;i<3;++i) if(!(cube.maximum[i]-cube.minimum[i]>=1e-20f)) {error="LUT domain is empty or too narrow";return false;}
    result=std::move(cube);return true;
}
inline std::array<float,3> SampleCube(const Cube& cube,std::array<float,3> rgb) {
    if(cube.size==0)return rgb;
    std::array<unsigned,3> base{}; std::array<float,3> f{};
    for(unsigned k=0;k<3;++k) {
        float x=std::clamp((rgb[k]-cube.minimum[k])/(cube.maximum[k]-cube.minimum[k]),0.f,1.f)*(cube.size-1);
        base[k]=std::min(unsigned(x),cube.size-2);f[k]=x-base[k];
    }
    std::array<float,3> result{};
    for(unsigned b=0;b<2;++b)for(unsigned g=0;g<2;++g)for(unsigned r=0;r<2;++r) {
        float w=(r?f[0]:1-f[0])*(g?f[1]:1-f[1])*(b?f[2]:1-f[2]);
        const auto& c=cube.values[base[0]+r+cube.size*(base[1]+g+cube.size*(base[2]+b))];
        for(unsigned k=0;k<3;++k)result[k]+=w*c[k];
    }
    return result;
}
inline std::vector<std::array<float,4>> Atlas(const Cube& cube) {
    std::vector<std::array<float,4>> result(LutAtlasWidth*LutAtlasEdge);
    const unsigned edge=cube.size?cube.size:2;
    for(unsigned b=0;b<edge;++b)for(unsigned g=0;g<edge;++g)for(unsigned r=0;r<edge;++r) {
        const auto c=cube.size?cube.values[r+edge*(g+edge*b)]:std::array<float,3>{float(r),float(g),float(b)};
        result[r+edge*b+LutAtlasWidth*g]={c[0],c[1],c[2],1};
    }
    return result;
}
inline std::string ExportCube(const Cube& cube) {
    std::ostringstream out;out.imbue(std::locale::classic());out.precision(9);
    out<<"TITLE \"Magpie OKLab LUT\"\nLUT_3D_SIZE "<<(cube.size?cube.size:33)<<'\n';
    out<<"DOMAIN_MIN "<<cube.minimum[0]<<' '<<cube.minimum[1]<<' '<<cube.minimum[2]<<'\n';
    out<<"DOMAIN_MAX "<<cube.maximum[0]<<' '<<cube.maximum[1]<<' '<<cube.maximum[2]<<'\n';
    if(cube.size)for(const auto& c:cube.values)out<<c[0]<<' '<<c[1]<<' '<<c[2]<<'\n';
    else for(unsigned b=0;b<33;++b)for(unsigned g=0;g<33;++g)for(unsigned r=0;r<33;++r)out<<r/32.f<<' '<<g/32.f<<' '<<b/32.f<<'\n';
    return out.str();
}
}
