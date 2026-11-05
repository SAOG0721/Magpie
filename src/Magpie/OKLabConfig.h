#pragma once
#include "OKLabLut.h"

namespace Magpie::OKLab {
// Version and embedded asset are one portable configuration contract. Numeric
// controls continue through the application's existing parameter serializer.
template<class Writer> void WriteAsset(Writer& writer,std::string_view asset) {
    writer.Key("oklabVersion");writer.Uint(1);
    if(!asset.empty()){writer.Key("oklabLut");writer.String(asset.data(),static_cast<unsigned>(asset.size()));}
}
template<class Object> bool ReadAsset(const Object& object,std::string& asset) {
    const auto version=object.FindMember("oklabVersion");
    if(version!=object.MemberEnd()&&(!version->value.IsUint()||version->value.GetUint()!=1))return false;
    const auto lut=object.FindMember("oklabLut");
    if(lut==object.MemberEnd()){asset.clear();return true;}
    if(!lut->value.IsString())return false;
    const std::string_view text{lut->value.GetString(),lut->value.GetStringLength()};
    Cube cube;std::string error;if(!ParseCube(text,cube,error))return false;
    asset.assign(text);return true;
}
}
