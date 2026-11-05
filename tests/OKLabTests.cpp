#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <DirectXPackedVector.h>
#include "OKLabColor.h"
#include "DLSSNRColorReference.h"
#include "OKLabRuntime.h"
using Microsoft::WRL::ComPtr;
using Pixel=std::array<float,4>;
unsigned W=64,H=64;
using Image=std::vector<Pixel>;
using Values=std::map<std::string,float>;
void Check(HRESULT hr){if(FAILED(hr)){std::cerr<<"HRESULT "<<std::hex<<hr<<'\n';std::abort();}}
void Require(bool condition,const char* message){if(!condition){std::cerr<<message<<'\n';std::abort();}}
float Difference(const Pixel&a,const Pixel&b){float v=0;for(unsigned c=0;c<3;++c)v=std::max(v,std::abs(a[c]-b[c]));return v;}
Pixel ReferenceHueChroma(const Pixel& p,double angle,double chroma){
    auto lab=ColorReference::ToLab({p[0],p[1],p[2]});
    const double a=lab[1],b=lab[2],r=angle*3.141592653589793/180;
    lab[1]=chroma*(a*std::cos(r)-b*std::sin(r));lab[2]=chroma*(a*std::sin(r)+b*std::cos(r));
    auto rgb=ColorReference::FromLab(lab);
    auto inside=[](const ColorReference::Color& c){return std::all_of(c.begin(),c.end(),[](double x){return x>=0&&x<=1;});};
    if(!inside(rgb)){double lo=0,hi=1;for(unsigned k=0;k<48;++k){double t=(lo+hi)/2;auto candidate=ColorReference::FromLab({lab[0],lab[1]*t,lab[2]*t});if(inside(candidate))lo=t;else hi=t;}rgb=ColorReference::FromLab({lab[0],lab[1]*lo,lab[2]*lo});}
    return {float(rgb[0]),float(rgb[1]),float(rgb[2]),p[3]};
}
struct Harness {
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> dc;
    ComPtr<ID3D11InfoQueue> debug;
    std::array<ComPtr<ID3D11Texture2D>,6> textures;
    std::array<ComPtr<ID3D11ShaderResourceView>,6> srv;
    std::array<ComPtr<ID3D11UnorderedAccessView>,6> uav;
    std::array<ComPtr<ID3D11ComputeShader>,3> shaders;
    ComPtr<ID3D11Texture2D> staging;
    ComPtr<ID3D11Buffer> constants;
    std::array<ComPtr<ID3D11SamplerState>,2> samplers;
    Magpie::OKLabRuntime runtime;
    unsigned runs=0;
    bool half=false;
    bool unorm8=false;
    bool hardware=false;
    double milliseconds=0;
    std::chrono::steady_clock::time_point captureTime=std::chrono::steady_clock::now();
    double frameSeconds=1.0/60;
    ComPtr<ID3D11Query> disjoint,start,finish;
    explicit Harness(const std::filesystem::path& folder,bool fp16=false,bool normalized8=false,bool gpu=false):half(fp16),unorm8(normalized8),hardware(gpu) {
        auto hr=D3D11CreateDevice(nullptr,gpu?D3D_DRIVER_TYPE_HARDWARE:D3D_DRIVER_TYPE_WARP,nullptr,gpu?0:D3D11_CREATE_DEVICE_DEBUG,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&dc);
        if(hr==DXGI_ERROR_SDK_COMPONENT_MISSING)hr=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&dc);
        Check(hr);device.As(&debug);
        if(hardware){D3D11_QUERY_DESC q{D3D11_QUERY_TIMESTAMP_DISJOINT,0};Check(device->CreateQuery(&q,&disjoint));q.Query=D3D11_QUERY_TIMESTAMP;Check(device->CreateQuery(&q,&start));Check(device->CreateQuery(&q,&finish));}
        for(unsigned i=0;i<3;++i) {
            ComPtr<ID3DBlob> code;
            const auto path=folder/((unorm8?L"r8":half?L"fp16":L"fp32")+std::wstring(L"-pass")+std::to_wstring(i)+L".cso");
            Check(D3DReadFileToBlob(path.c_str(),&code));
            Check(device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&shaders[i]));
            ComPtr<ID3D11ShaderReflection> reflection;Check(D3DReflect(code->GetBufferPointer(),code->GetBufferSize(),IID_PPV_ARGS(&reflection)));
            if(i==2)for(size_t p=0;p<std::size(Magpie::OKLab::Parameters);++p) {
                D3D11_SHADER_VARIABLE_DESC d{};
                Check(reflection->GetConstantBufferByName("__CB1")->GetVariableByName(std::string(Magpie::OKLab::Parameters[p].name).c_str())->GetDesc(&d));
                Require(d.StartOffset==(18+p)*4,"Parameter packing differs from EffectDrawer contract");
            }
            std::cout<<"Loaded EffectCompiler pass "<<i+1<<std::endl;
        }
        const unsigned widths[]{W,W,16,1,1,Magpie::OKLab::LutAtlasWidth},heights[]{H,H,9,1,1,Magpie::OKLab::LutAtlasEdge};
        for(unsigned i=0;i<6;++i) {
            D3D11_TEXTURE2D_DESC d{};d.Width=widths[i];d.Height=heights[i];d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
            d.Format=unorm8&&i<2?DXGI_FORMAT_R8G8B8A8_UNORM:half&&i<2?DXGI_FORMAT_R16G16B16A16_FLOAT:DXGI_FORMAT_R32G32B32A32_FLOAT;d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
            Check(device->CreateTexture2D(&d,nullptr,&textures[i]));
            Check(device->CreateShaderResourceView(textures[i].Get(),nullptr,&srv[i]));
            Check(device->CreateUnorderedAccessView(textures[i].Get(),nullptr,&uav[i]));
            float zero[4]{};dc->ClearUnorderedAccessViewFloat(uav[i].Get(),zero);
        }
        D3D11_TEXTURE2D_DESC sd{};textures[1]->GetDesc(&sd);sd.BindFlags=0;sd.Usage=D3D11_USAGE_STAGING;sd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        Check(device->CreateTexture2D(&sd,nullptr,&staging));
        D3D11_BUFFER_DESC bd{};bd.ByteWidth=unsigned((18+std::size(Magpie::OKLab::Parameters)+3)/4*16);bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        Check(device->CreateBuffer(&bd,nullptr,&constants));
        for(unsigned i=0;i<2;++i){D3D11_SAMPLER_DESC d{};d.Filter=i?D3D11_FILTER_MIN_MAG_MIP_POINT:D3D11_FILTER_MIN_MAG_MIP_LINEAR;d.AddressU=d.AddressV=d.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;d.MaxLOD=D3D11_FLOAT32_MAX;Check(device->CreateSamplerState(&d,&samplers[i]));}
        std::string error;Require(runtime.SetLut(dc.Get(),textures[5].Get(),{},error),"Identity LUT failed");
    }
    Image Run(const Image& input,const Values& values={},bool hdr=false,bool reset=true) {
        auto get=[&](std::string_view name,float fallback){auto it=values.find(std::string(name));return it==values.end()?fallback:it->second;};
        Require(runtime.Configure(device.Get(),get,hdr),"Runtime configuration failed");
        if(reset)runtime.Reset();
        std::vector<float> data((18+std::size(Magpie::OKLab::Parameters)+3)/4*4);
        auto integer=[&](size_t index,unsigned value){std::memcpy(&data[index],&value,4);};
        integer(0,W);integer(1,H);integer(2,W);integer(3,H);
        data[4]=data[6]=1.f/W;data[5]=data[7]=1.f/H;data[8]=data[9]=1;
        integer(10,16);integer(11,9);data[12]=1.f/16;data[13]=1.f/9;
        integer(14,1);integer(15,1);data[16]=data[17]=1;
        for(size_t i=0;i<std::size(Magpie::OKLab::Parameters);++i) {
            const auto&p=Magpie::OKLab::Parameters[i];float v=Magpie::OKLab::Normalize(p,get(p.name,p.value));
            if(p.integer)integer(18+i,static_cast<unsigned>(static_cast<int>(v)));else data[18+i]=v;
        }
        if(unorm8){std::vector<uint8_t> encoded(input.size()*4);for(size_t i=0;i<input.size();++i)for(size_t c=0;c<4;++c)encoded[i*4+c]=uint8_t(std::lround(std::clamp(input[i][c],0.f,1.f)*255));dc->UpdateSubresource(textures[0].Get(),0,nullptr,encoded.data(),W*4,0);}
        else if(half){std::vector<DirectX::PackedVector::HALF> encoded(input.size()*4);for(size_t i=0;i<input.size();++i)for(size_t c=0;c<4;++c)encoded[i*4+c]=DirectX::PackedVector::XMConvertFloatToHalf(input[i][c]);dc->UpdateSubresource(textures[0].Get(),0,nullptr,encoded.data(),W*8,0);}
        else dc->UpdateSubresource(textures[0].Get(),0,nullptr,input.data(),W*sizeof(Pixel),0);
        dc->UpdateSubresource(constants.Get(),0,nullptr,data.data(),0,0);
        ID3D11Buffer* cb=constants.Get();dc->CSSetConstantBuffers(0,1,&cb);
        ID3D11SamplerState* ss[]{samplers[0].Get(),samplers[1].Get()};dc->CSSetSamplers(0,2,ss);
        if(hardware){dc->Begin(disjoint.Get());dc->End(start.Get());}
        captureTime+=std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(frameSeconds));
        runtime.Begin(dc.Get(),hardware?std::chrono::steady_clock::now():captureTime);
        const std::array<std::vector<unsigned>,3> ins{{{0},{2,4},{0,3,5}}};const unsigned outs[]{2,3,1};
        for(unsigned pass=0;pass<3;++pass){
            if(pass<2&&!runtime.Automatic())continue;
            ID3D11ShaderResourceView* nulls[3]{};dc->CSSetShaderResources(0,3,nulls);
            std::vector<ID3D11ShaderResourceView*> inputs;for(auto i:ins[pass])inputs.push_back(srv[i].Get());
            dc->CSSetShaderResources(0,unsigned(inputs.size()),inputs.data());
            auto target=uav[outs[pass]].Get();dc->CSSetUnorderedAccessViews(0,1,&target,nullptr);
            dc->CSSetShader(shaders[pass].Get(),nullptr,0);dc->Dispatch(pass<2?1:(W+15)/16,pass<2?1:(H+15)/16,1);
            ID3D11UnorderedAccessView* empty=nullptr;dc->CSSetUnorderedAccessViews(0,1,&empty,nullptr);
        }
        runtime.End(dc.Get(),textures[3].Get(),textures[4].Get());
        if(hardware){
            dc->End(finish.Get());dc->End(disjoint.Get());dc->Flush();
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT timing{};UINT64 begin=0,end=0;HRESULT ready;
            do{ready=dc->GetData(disjoint.Get(),&timing,sizeof(timing),0);if(ready==S_FALSE)Sleep(1);}while(ready==S_FALSE);Check(ready);
            Check(dc->GetData(start.Get(),&begin,sizeof(begin),0));Check(dc->GetData(finish.Get(),&end,sizeof(end),0));
            Require(!timing.Disjoint&&timing.Frequency>0,"GPU timestamps disjoint");milliseconds=double(end-begin)*1000/timing.Frequency;++runs;return {};
        }
        dc->CopyResource(staging.Get(),textures[1].Get());D3D11_MAPPED_SUBRESOURCE mapped{};Check(dc->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped));
        Image result(W*H);for(unsigned y=0;y<H;++y){const auto* row=static_cast<const char*>(mapped.pData)+y*mapped.RowPitch;if(unorm8){const auto* bytes=reinterpret_cast<const uint8_t*>(row);for(unsigned x=0;x<W;++x)for(unsigned c=0;c<4;++c)result[y*W+x][c]=bytes[x*4+c]/255.f;}else if(half){const auto* data16=reinterpret_cast<const DirectX::PackedVector::HALF*>(row);for(unsigned x=0;x<W;++x)for(unsigned c=0;c<4;++c)result[y*W+x][c]=DirectX::PackedVector::XMConvertHalfToFloat(data16[x*4+c]);}else std::memcpy(result.data()+y*W,row,W*sizeof(Pixel));}dc->Unmap(staging.Get(),0);
        for(const auto&p:result)for(float v:p)Require(std::isfinite(v),"Nonfinite shader output");
        if(debug){for(UINT64 i=0;i<debug->GetNumStoredMessages();++i){SIZE_T size=0;Check(debug->GetMessage(i,nullptr,&size));std::vector<char> bytes(size);auto*m=reinterpret_cast<D3D11_MESSAGE*>(bytes.data());Check(debug->GetMessage(i,m,&size));if(m->Severity<=D3D11_MESSAGE_SEVERITY_WARNING){std::cerr<<m->pDescription<<'\n';std::abort();}}debug->ClearStoredMessages();}
        ++runs;return result;
    }
};
#ifndef OKLAB_NO_MAIN
int wmain(int argc,wchar_t**argv){
    Require(argc==2,"Expected output folder");
    using namespace Magpie::OKLab;
    const auto red=ToLab({1,0,0});Require(std::abs(red[0]-.62795536f)<1e-7&&std::abs(red[1]-.22486306f)<1e-7&&std::abs(red[2]-.12584630f)<1e-7,"Independent red reference");
    const auto white=ToLab({1,1,1});Require(std::abs(white[0]-1)<1e-7&&std::abs(white[1])<1e-7&&std::abs(white[2])<1e-7,"D65 white reference");
    Require(Visible("mix3Hue",3,3,0,0)&&!Visible("mix4Hue",3,3,0,0),"Mixer editor lost scope");
    Require(!Visible("curve2x1",8,0,0,1)&&Visible("curve2x1",8,0,0,2),"Curve editor lost scope");
    Cube cube;std::string error;Require(ParseCube(ExportCube({}),cube,error),"Identity cube round trip");
    auto sample=SampleCube(cube,{.13f,.51f,.72f});Require(std::abs(sample[0]-.13f)<1e-6&&std::abs(sample[2]-.72f)<1e-6,"Cube interpolation");
    Require(!ParseCube("LUT_3D_SIZE 999",cube,error),"LUT dimension validation");
    Require(!ParseCube("LUT_3D_SIZE 2\n0 0 nan\n",cube,error),"Nonfinite LUT validation");
    auto badDomain=ExportCube({});badDomain.replace(badDomain.find("DOMAIN_MAX 1 1 1"),16,"DOMAIN_MAX 1e30 1 1");
    Require(!ParseCube(badDomain,cube,error),"LUT domains must be representable by the shader");
    Harness h{std::filesystem::path(argv[1])};
    std::mt19937 random(19);std::uniform_real_distribution<float>d(0,1);
    Image image(W*H);for(auto&p:image)p={d(random),d(random),d(random),.37f};
    double roundtripError=0;for(const auto& p:image){auto rgb=ColorReference::FromLab(ColorReference::ToLab({p[0],p[1],p[2]}));for(unsigned c=0;c<3;++c)roundtripError=std::max(roundtripError,std::abs(rgb[c]-p[c]));}Require(roundtripError<2e-5,"Independent CPU encoded RGB roundtrip");
    auto out=h.Run(image);for(size_t i=0;i<image.size();++i)Require(out[i]==image[i],"Default must be exactly transparent");
    out=h.Run(image,{{"amount",0.f},{"exposure",4.f},{"bloom",1.f},{"dither",1.f}});for(size_t i=0;i<image.size();++i)Require(out[i]==image[i],"Zero amount bypass");
    Require(h.Run(image,{{"lutAmount",1.f}})==image,"No LUT asset must remain exactly neutral");
    Require(h.runtime.SetLut(h.dc.Get(),h.textures[5].Get(),ExportCube({}),error),"Explicit identity cube import");
    out=h.Run(image,{{"lutAmount",1.f}});float lutError=0;size_t worst=0;for(size_t i=0;i<image.size();++i){float delta=Difference(out[i],image[i]);if(delta>lutError){lutError=delta;worst=i;}}
    if(lutError>=2e-5)std::cerr<<"LUT max error "<<lutError<<" at "<<worst<<" input "<<image[worst][0]<<','<<image[worst][1]<<','<<image[worst][2]<<" output "<<out[worst][0]<<','<<out[worst][1]<<','<<out[worst][2]<<std::endl;
    Require(lutError<2e-5,"Identity LUT / conversion error");
    out=h.Run(image,{{"chroma",0.f}});
    for(size_t i=0;i<image.size();++i){const auto p=image[i];const auto lab=ToLab({Decode(p[0]),Decode(p[1]),Decode(p[2])});float v=lab[0]*lab[0]*lab[0];float encoded=v<=.0031308f?v*12.92f:1.055f*std::pow(v,1.f/2.4f)-.055f;Require(std::abs(out[i][0]-encoded)<2e-5,"Desaturation must preserve independently calculated L");}
    out=h.Run(image,{{"hue",37.f},{"chroma",1.4f}});float coordinateError=0;
    for(size_t i=0;i<image.size();++i)coordinateError=std::max(coordinateError,Difference(out[i],ReferenceHueChroma(image[i],37,1.4)));
    Require(coordinateError<1e-4,"CPU double hue/chroma/gamut reference mismatch");
    auto first=h.Run(image,{{"sel0Enabled",1.f},{"sel0Hue",0.f},{"sel0TargetHue",0.f},{"sel0Unify",1.f}});
    auto wrapped=h.Run(image,{{"sel0Enabled",1.f},{"sel0Hue",360.f},{"sel0TargetHue",360.f},{"sel0Unify",1.f}});
    for(size_t i=0;i<image.size();++i)Require(Difference(first[i],wrapped[i])<2e-5,"Selection hue wrap seam");
    auto seam=[&](double epsilon){Image scan(W*H);for(size_t i=0;i<scan.size();++i){auto rgb=ColorReference::FromLab({.6,.15,i<scan.size()/2?-epsilon:epsilon});scan[i]={float(rgb[0]),float(rgb[1]),float(rgb[2]),1};}auto result=h.Run(scan,{{"mix0Hue",45.f},{"mix7Hue",-45.f},{"mix0Light",.1f}});return Difference(result.front(),result.back());};
    Require(seam(1e-4)<.002f&&seam(1e-6)<5e-5,"Mixer hue seam must converge continuously");
    Cube swap;swap.size=2;for(unsigned b=0;b<2;++b)for(unsigned g=0;g<2;++g)for(unsigned r=0;r<2;++r)swap.values.push_back({float(b),float(g),float(r)});
    Require(h.runtime.SetLut(h.dc.Get(),h.textures[5].Get(),ExportCube(swap),error),"Color LUT import failed");
    out=h.Run(image,{{"lutAmount",1.f}});for(size_t i=0;i<image.size();++i)Require(Difference(out[i],{image[i][2],image[i][1],image[i][0],image[i][3]})<3e-5,"LUT axis ordering failed");
    Cube extended;extended.size=2;extended.maximum={2,2,2};
    for(unsigned b=0;b<2;++b)for(unsigned g=0;g<2;++g)for(unsigned r=0;r<2;++r)extended.values.push_back({float(r),float(g),float(b)});
    Require(h.runtime.SetLut(h.dc.Get(),h.textures[5].Get(),ExportCube(extended),error),"Extended LUT domain import");
    out=h.Run(Image(W*H,Pixel{3,3,3,1}),{{"lutAmount",1.f},{"lutDomain",1.f}},true);
    Require(std::abs(out[0][0]-1.5f)<1e-4,"LUT DOMAIN_MAX must apply above SDR white");
    Cube detail;detail.size=65;detail.values.resize(65*65*65);detail.values[1]={1,0,0};
    Require(h.runtime.SetLut(h.dc.Get(),h.textures[5].Get(),ExportCube(detail),error),"65-cube import");
    out=h.Run(Image(W*H,Pixel{1.f/64,0,0,1}),{{"lutAmount",1.f}});
    Require(out[0][0]>.9998f,"65-cube source node must not be lost to resampling");
    Require(h.runtime.SetLut(h.dc.Get(),h.textures[5].Get(),{},error),"Restore identity LUT failed");
    Image gray(W*H);for(size_t i=0;i<gray.size();++i){float v=float(i)/(gray.size()-1);gray[i]={v,v,v,.4f};}
    auto toneDelta=[&](float control){auto result=h.Run(gray,{{"shadows",control}});float delta=0;for(size_t i=0;i<gray.size();++i)delta=std::max(delta,Difference(result[i],gray[i]));return delta;};
    Require(toneDelta(.0001f)<toneDelta(.001f)*.2f&&toneDelta(.00001f)<2e-5,"Tone controls must approach the neutral limit continuously");
    out=h.Run(gray,{{"hue",170.f},{"chroma",2.5f}});float spread=0;for(auto&p:out)spread=std::max(spread,std::max(p[0],std::max(p[1],p[2]))-std::min(p[0],std::min(p[1],p[2])));Require(spread<2e-5,"Hue/chroma must preserve gray axis");
    for(float shadow:{-1.f,1.f})for(float high:{-1.f,1.f}) {
        out=h.Run(gray,{{"shadows",shadow},{"highlights",high},{"midtones",-shadow},{"toe",1.f},{"shoulder",1.f}});
        for(size_t i=1;i<out.size();++i)Require(out[i][0]+2e-5>=out[i-1][0],"Tone mapping is not monotonic");
    }
    out=h.Run(image,{{"styleMode",1.f},{"chroma",2.f},{"wheel0Chroma",.1f}});for(auto&p:out)Require(std::abs(p[0]-p[1])<2e-5&&std::abs(p[1]-p[2])<2e-5,"Black and white must remain achromatic");
    Cube redLut;redLut.size=2;redLut.values.assign(8,{.7f,.2f,.1f});Require(h.runtime.SetLut(h.dc.Get(),h.textures[5].Get(),ExportCube(redLut),error),"Tint LUT import");
    out=h.Run(image,{{"styleMode",1.f},{"lutAmount",1.f}});for(const auto&p:out)Require(std::abs(p[0]-p[1])<2e-5&&std::abs(p[1]-p[2])<2e-5,"Black and white must follow an intentionally colored LUT");
    Require(h.runtime.SetLut(h.dc.Get(),h.textures[5].Get(),{},error),"Restore no LUT");
    auto encoded=h.Run(image,{{"hue",25.f},{"chroma",.8f}});Image linear=image;for(auto&p:linear)for(unsigned c=0;c<3;++c)p[c]=Decode(p[c])*(500.f/80);
    out=h.Run(linear,{{"hue",25.f},{"chroma",.8f},{"referenceWhite",500.f},{"peakNits",500.f},{"outputGamut",0.f}},true);
    for(size_t i=0;i<out.size();++i)for(unsigned c=0;c<3;++c)Require(std::abs(out[i][c]*(80.f/500)-Decode(encoded[i][c]))<3e-5,"Encoded SDR and explicit linear route must decode once");
    // Independent xy checkpoints for the Planckian-locus fit (5000 / 6500 K).
    const double X=.1*.34499168/.35158667,Y=.1,Z=.1*(1-.34499168-.35158667)/.35158667;
    Image whitePatch(W*H,Pixel{float(3.2409699419*X-1.5373831776*Y-.4986107603*Z),float(-.9692436363*X+1.8759675015*Y+.0415550574*Z),float(.0556300797*X-.2039769589*Y+1.0569715142*Z),1});
    for(float tint:{0.f,.005f}){
        out=h.Run(whitePatch,{{"temperatureEnabled",1.f},{"sourceKelvin",5000.f},{"targetKelvin",6500.f},{"whiteTint",tint},{"referenceWhite",80.f}},true);
        const auto p=out[0];double x=.4123907993*p[0]+.3575843394*p[1]+.1804807884*p[2],y=.2126390059*p[0]+.7151686788*p[1]+.0721923154*p[2],z=.0193308187*p[0]+.1191947798*p[1]+.9505321522*p[2];
        Require(std::abs(x/(x+y+z)-.31349411)<2e-5&&std::abs(y/(x+y+z)-(.32366254+tint))<2e-5&&std::abs(y-.1)<2e-5,"Bradford source/target Kelvin and delta-y calibration");
    }
    Image card(W*H);const Pixel patches[]{{1,0,0,1},{1,.5f,0,1},{1,1,0,1},{0,1,0,1},{0,1,1,1},{0,0,1,1},{.5f,0,1,1},{1,0,1,1}};
    for(unsigned y=0;y<H;++y)for(unsigned x=0;x<W;++x){auto c=patches[std::min(7u,x*8/W)];float shade=float(y%(H/2))/(H/2-1);if(y>=H/2)c={.82f,.57f,.43f,1};for(unsigned k=0;k<3;++k)c[k]*=shade;card[y*W+x]=c;if(y>H*7/8)card[y*W+x]={float(x)/(W-1),float(x)/(W-1),float(x)/(W-1),1};}
    for(int preset=0;preset<6;++preset){Values values;Preset(preset,[&](std::string_view key,float v){values[std::string(key)]=v;});auto preview=h.Run(card,values);
        std::ofstream file(std::filesystem::path(argv[1])/("preset-"+std::to_string(preset)+".ppm"),std::ios::binary);file<<"P6\n"<<W<<' '<<H<<"\n255\n";
        for(const auto& pixel:preview)for(unsigned c=0;c<3;++c)file.put(char(std::lround(std::clamp(pixel[c],0.f,1.f)*255)));}
    out=h.Run(image,{{"autoExposure",1.f},{"autoWhite",1.f}});out=h.Run(image,{{"autoExposure",1.f},{"autoWhite",1.f},{"autoLock",1.f}},false,false);
    Image uniform(W*H,Pixel{.3f,.3f,.3f,1.f});
    out=h.Run(uniform,{{"autoExposure",1.f}});
    const float targetEncoded=1.055f*std::pow(.18f,1.f/2.4f)-.055f;
    Require(std::abs(out[0][0]-targetEncoded)<3e-5,"Auto exposure target is incorrect");
    uniform.assign(W*H,Pixel{.6f,.6f,.6f,1.f});
    out=h.Run(uniform,{{"autoExposure",1.f},{"autoLock",1.f}},false,false);
    Require(out[0][0]>.7f,"Automatic lock must preserve the previous estimate");
    h.runtime.Reset();
    out=h.Run(uniform,{{"autoExposure",1.f},{"autoLock",1.f}},false,false);
    Require(out[0][0]>.7f,"Automatic lock must survive a capture pause/reset");
    uniform.assign(W*H,Pixel{.3f,.4f,.5f,1.f});
    out=h.Run(uniform,{{"autoWhite",1.f}});
    Require(std::abs(out[0][0]-out[0][2])<3e-5,"Gray-world auto white balance is incorrect");
    // A one-frame flash must not replace the persistent estimate. A sustained
    // cut should converge faster than ordinary adaptation, and frame cadence
    // must not change its response for the same elapsed capture time.
    const Values autoValues{{"autoExposure",1.f},{"sceneThreshold",1.f}};
    Image dark(W*H,Pixel{.25f,.25f,.25f,1}),bright(W*H,Pixel{.7f,.7f,.7f,1});
    h.Run(dark,autoValues);out=h.Run(bright,autoValues,false,false);
    Require(out[0][0]>.65f,"One-frame flash snapped auto exposure to its target");
    out=h.Run(dark,autoValues,false,false);Require(std::abs(out[0][0]-targetEncoded)<.08f,"Flash caused excessive post-flash pumping");
    auto response=[&](double seconds,int frames){h.frameSeconds=seconds;h.Run(dark,autoValues);Image result;for(int i=0;i<frames;++i)result=h.Run(bright,autoValues,false,false);return result[0][0];};
    const float sixty=response(1.0/60,30),thirty=response(1.0/30,15);
    std::cout<<"Automatic 0.5 s response: "<<sixty<<" (60 Hz), "<<thirty<<" (30 Hz)\n";
    Require(std::abs(sixty-thirty)<.012f,"Automatic response depends on frame count rather than elapsed time");
    Require(sixty>.43f&&sixty<.62f,"Scene change adaptation did not converge within the bounded half-second response");
    h.frameSeconds=3;out=h.Run(dark,autoValues,false,false);Require(std::abs(out[0][0]-targetEncoded)<3e-5,"Long pause must start a fresh estimate");h.frameSeconds=1.0/60;
    Image pattern(W*H);for(unsigned y=0;y<H;++y)for(unsigned x=0;x<W;++x){float v=.45f+.05f*std::sin(x*6.2831853f/8);pattern[y*W+x]={v,v,v,1};}
    for(const auto control:{"clarity","textureStrength"}){out=h.Run(pattern,{{control,1.f},{"clarityRadius",2.f},{"textureRadius",2.f},{"noiseFloor",0.f}});Require(out[H/2*W+W/2+2][0]>pattern[H/2*W+W/2+2][0]+.015f,"Spatial detail control failed to enhance structure");}
    for(unsigned y=0;y<H;++y)for(unsigned x=0;x<W;++x){float noise=x%2?.04f:-.04f;pattern[y*W+x]={.5f+noise,.5f,.5f-noise,1};}
    out=h.Run(pattern,{{"chromaSmooth",1.f},{"chromaRadius",1.f}});float beforeNoise=0,afterNoise=0;
    for(size_t i=0;i<out.size();++i){auto a=ColorReference::ToLab({pattern[i][0],pattern[i][1],pattern[i][2]}),b=ColorReference::ToLab({out[i][0],out[i][1],out[i][2]});beforeNoise+=float(std::hypot(a[1],a[2]));afterNoise+=float(std::hypot(b[1],b[2]));}Require(afterNoise<beforeNoise*.7f,"Chroma smoothing must reduce alternating color noise");
    pattern.assign(W*H,Pixel{.05f,.05f,.05f,1});for(unsigned y=H/2-3;y<H/2+3;++y)for(unsigned x=W/2-3;x<W/2+3;++x)pattern[y*W+x]={1,1,1,1};
    out=h.Run(pattern,{{"bloom",1.f},{"bloomRadius",6.f},{"bloomThreshold",.5f}});Require(out[(H/2)*W+W/2+5][0]>.1f,"Bloom must spread linear-light energy beyond the bright source");
    for(unsigned y=0;y<H;++y)for(unsigned x=0;x<W;++x){float v=x%8<4?.6f:.8f;pattern[y*W+x]={v,v,v,1};}
    out=h.Run(pattern,{{"dehaze",.8f},{"dehazeRadius",4.f}});Require(out[H/2*W+W/2][0]<pattern[H/2*W+W/2][0]-.02f,"Dehaze must react to local veil estimation");
    for(unsigned y=0;y<H;++y)for(unsigned x=0;x<W;++x){float v=.3f+float(x/4)*.005f;pattern[y*W+x]={v,v,v,1};}
    out=h.Run(pattern,{{"deband",1.f},{"debandRadius",3.f},{"debandThreshold",.05f}});
    auto curvature=[](const Image& values){float sum=0;for(unsigned x=4;x<W-4;++x)sum+=std::abs(values[H/2*W+x-1][0]-2*values[H/2*W+x][0]+values[H/2*W+x+1][0]);return sum;};Require(curvature(out)<curvature(pattern)*.7f,"Deband must soften quantized flat gradients");
    pattern.assign(W*H,Pixel{.2f,.2f,.2f,.4f});out=h.Run(pattern,{{"exposure",1.f},{"maskMode",2.f},{"maskWidth",.5f},{"maskHeight",.5f}});Require(Difference(out[0],pattern[0])<2e-5&&out[W*H/2+W/2][0]>.25f,"Radial mask must limit the adjustment spatially");
    Image hdr=image;for(auto&p:hdr){p[0]=p[0]*8-1;p[1]*=4;p[2]*=6;}
    out=h.Run(hdr,{},true);Require(out==hdr,"HDR neutral must retain negative and super-white values");
    out=h.Run(hdr,{{"exposure",1.f},{"outputGamut",2.f}},true);
    out=h.Run(hdr,{{"exposure",1.f},{"outputGamut",1.f}},true);
    for(const auto&p:out){const float r=.8224619687f*p[0]+.1775380313f*p[1];const float g=.0331941989f*p[0]+.9668058011f*p[1];const float b=.0170826307f*p[0]+.0723974407f*p[1]+.9105199286f*p[2];Require(std::min({r,g,b})>=-2e-4&&std::max({r,g,b})<=12.5002f,"HDR P3 gamut boundary");}
    // Extreme single controls exercise dormant branches, curves and spatial code.
    for(const auto&p:Parameters){
        if(p.name=="amount"||p.name.starts_with("curve"))continue;
        for(float extreme:{p.minimum,p.maximum}) {
        Values values{{std::string(p.name),extreme},{"view",0.f}};
        if(p.group=="Selection")values["sel0Enabled"]=values["sel1Enabled"]=1;
        if(p.name.starts_with("bw"))values["styleMode"]=1;
        if(p.group=="Calibration")values["temperatureEnabled"]=1.f;
        h.Run(image,values);
        }
    }
    for(int c=0;c<6;++c)h.Run(image,{{"curve"+std::to_string(c)+"Enabled",1.f},{"curve"+std::to_string(c)+"y3",.8f}});
    Harness half{std::filesystem::path(argv[1]),true};Image exact(W*H,Pixel{-.5f,4.f,2.f,1.f});
    Require(half.Run(exact,{},true)==exact,"Native FP16 HDR identity");
    half.Run(exact,{{"exposure",1.f},{"outputGamut",2.f}},true);
    Harness bytes{std::filesystem::path(argv[1]),false,true};
    for(auto&p:image)for(float&v:p)v=std::round(v*255)/255;
    Require(bytes.Run(image)==image,"R8 UNORM identity must retain every channel byte");
    std::ofstream report(std::filesystem::path(argv[1])/"results.json");
    report<<"{\"runtime\":\"D3D11 WARP, actual EffectCompiler bytecode\",\"passes\":3,\"fp32Runs\":"<<h.runs<<",\"fp16Runs\":"<<half.runs<<",\"r8Runs\":"<<bytes.runs<<",\"identityLutError\":"<<lutError<<",\"coordinateError\":"<<coordinateError<<",\"graySpread\":"<<spread<<",\"debugLayer\":"<<(h.debug?"true":"false")<<"}\n";
    std::cout<<"Passed "<<h.runs<<" WARP image runs, LUT error "<<lutError<<", gray spread "<<spread<<std::endl;
}
#endif
