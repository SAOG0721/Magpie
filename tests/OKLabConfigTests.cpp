#include "OKLabConfig.h"
#include "OKLabParameters.h"
#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
#include <iostream>
#include <limits>
#include <cstdlib>
void Require(bool v,const char* message){if(!v){std::cerr<<message<<'\n';std::exit(1);}}
int main(){
    using namespace Magpie::OKLab;
    std::string saved=ExportCube({}),loaded;
    rapidjson::StringBuffer buffer;rapidjson::PrettyWriter writer(buffer);writer.StartObject();WriteAsset(writer,saved);writer.EndObject();
    rapidjson::Document document;document.Parse(buffer.GetString());
    Require(!document.HasParseError()&&ReadAsset(document.GetObj(),loaded)&&saved==loaded,"Embedded LUT/version round trip changed bytes");
    for(const auto text:{"{}","{\"oklabVersion\":1}"}){document.Parse(text);Require(ReadAsset(document.GetObj(),loaded)&&loaded.empty(),"Missing asset must restore the neutral default");}
    for(const auto text:{"{\"oklabVersion\":2}","{\"oklabVersion\":1.5}","{\"oklabLut\":5}","{\"oklabLut\":\"LUT_3D_SIZE 2\"}"}){document.Parse(text);Require(!ReadAsset(document.GetObj(),loaded),"Invalid future/asset data must reach recovery or import rejection");}
    for(const auto& p:Parameters){Require(Normalize(p,std::numeric_limits<float>::quiet_NaN())==p.value,"Missing/nonfinite control default");Require(Normalize(p,-1e20f)==p.minimum&&Normalize(p,1e20f)==p.maximum,"Out-of-range control normalization");}
    std::cout<<"PASS production asset JSON round trip, missing keys, future schema rejection and 285 numeric domains\n";
}
