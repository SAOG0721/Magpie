#define NOMINMAX
#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <DirectXPackedVector.h>
#include <array>
#include <vector>
#include <string>
#include <iostream>
#include <fstream>
#include <random>
#include <cassert>
#include <cmath>
#include <map>
#include <cstddef>
#include <limits>
#include "DLSSNRDetailParameters.h"
#include "EffectParameterRules.h"
#include "DLSSNRResidualUnderTest.h"
#include "DLSSNRColorReference.h"
using Microsoft::WRL::ComPtr;
using Pixel = std::array<float,4>;
constexpr unsigned W=256;
using Image = std::array<Pixel,W>;
void Check(HRESULT hr) { if (FAILED(hr)) { std::cerr<<std::hex<<hr<<'\n'; std::abort(); } }
ComPtr<ID3DBlob> Compile(const std::string& s,const char* entry) {
    ComPtr<ID3DBlob> code,errors;
    const auto hr=D3DCompile(s.data(),s.size(),entry,nullptr,nullptr,entry,"cs_5_0",
        D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_WARNINGS_ARE_ERRORS|D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&errors);
    if (errors) std::cerr<<static_cast<const char*>(errors->GetBufferPointer());
    Check(hr); return code;
}
struct Harness {
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> dc;
    ComPtr<ID3D11InfoQueue> debug;
    ComPtr<ID3D11ComputeShader> shader, finalShader; ComPtr<ID3D11Buffer> cb;
    std::array<ComPtr<ID3D11Texture2D>,2> input;
    std::array<ComPtr<ID3D11ShaderResourceView>,2> srv;
    ComPtr<ID3D11Texture2D> output,finalOutput,staging;
    ComPtr<ID3D11UnorderedAccessView> uav,finalUav;
    ComPtr<ID3D11ShaderResourceView> residualSrv;
    bool half;
    Harness(bool fp16) : half(fp16) {
        auto hr=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,D3D11_CREATE_DEVICE_DEBUG,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&dc);
        if (hr==DXGI_ERROR_SDK_COMPONENT_MISSING) hr=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&dc);
        Check(hr); device.As(&debug);
        auto code=Compile(prepareShader,"PrepareResidual");
        // Reflection checks shader packing against the actual C++ constants.
        ComPtr<ID3D11ShaderReflection> reflection;
        Check(D3DReflect(code->GetBufferPointer(),code->GetBufferSize(),IID_PPV_ARGS(&reflection)));
        D3D11_SHADER_BUFFER_DESC reflected{};
        Check(reflection->GetConstantBufferByName("ResampleParams")->GetDesc(&reflected));
        assert(reflected.Size==sizeof(ResampleConstants));
        for (auto [name,offset] : std::array<std::pair<const char*,size_t>,9>{{
            {"HueProtection",offsetof(ResampleConstants,hueProtection)},
            {"DarkProtection",offsetof(ResampleConstants,darkProtection)}, {"HighlightProtection",offsetof(ResampleConstants,highlightProtection)},
            {"LocalCompression",offsetof(ResampleConstants,localCompression)}, {"LowFrequencyGain",offsetof(ResampleConstants,lowFrequencyGain)},
            {"DetailGain",offsetof(ResampleConstants,detailGain)}, {"DebugView",offsetof(ResampleConstants,debugView)},
            {"ChromaProtection",offsetof(ResampleConstants,chromaProtection)}, {"FinalizeOutput",offsetof(ResampleConstants,finalizeOutput)}}}) {
            D3D11_SHADER_VARIABLE_DESC variable{};
            Check(reflection->GetConstantBufferByName("ResampleParams")->GetVariableByName(name)->GetDesc(&variable));
            assert(variable.StartOffset==offset);
        }
        Check(device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&shader));
        code=Compile(verticalShader,"CompositeResidualVertical");
        Check(device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&finalShader));
        D3D11_TEXTURE2D_DESC desc{}; desc.Width=W; desc.Height=1;
        desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
        desc.Format=DXGI_FORMAT_R32G32B32A32_FLOAT; desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        for (unsigned i=0;i<2;++i) {
            Check(device->CreateTexture2D(&desc,nullptr,&input[i]));
            Check(device->CreateShaderResourceView(input[i].Get(),nullptr,&srv[i]));
        }
        if (half) desc.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.BindFlags=D3D11_BIND_UNORDERED_ACCESS|D3D11_BIND_SHADER_RESOURCE;
        Check(device->CreateTexture2D(&desc,nullptr,&output));
        Check(device->CreateUnorderedAccessView(output.Get(),nullptr,&uav));
        Check(device->CreateShaderResourceView(output.Get(),nullptr,&residualSrv));
        Check(device->CreateTexture2D(&desc,nullptr,&finalOutput));
        Check(device->CreateUnorderedAccessView(finalOutput.Get(),nullptr,&finalUav));
        desc.BindFlags=0; desc.Usage=D3D11_USAGE_STAGING; desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        Check(device->CreateTexture2D(&desc,nullptr,&staging));
        D3D11_BUFFER_DESC bd{}; bd.ByteWidth=sizeof(ResampleConstants); bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        Check(device->CreateBuffer(&bd,nullptr,&cb));
    }
    Image Run(const Image& o,const Image& n,ResampleConstants params, bool finalize=true) {
        params.sourceWidth=params.targetWidth=W; params.sourceHeight=params.targetHeight=1;
        dc->UpdateSubresource(input[0].Get(),0,nullptr,o.data(),sizeof(o),0);
        dc->UpdateSubresource(input[1].Get(),0,nullptr,n.data(),sizeof(n),0);
        dc->UpdateSubresource(cb.Get(),0,nullptr,&params,0,0);
        ID3D11ShaderResourceView* sources[]{srv[0].Get(),srv[1].Get()}; auto* target=uav.Get(); auto* buffer=cb.Get();
        dc->CSSetShader(shader.Get(),nullptr,0); dc->CSSetShaderResources(0,2,sources);
        dc->CSSetUnorderedAccessViews(0,1,&target,nullptr); dc->CSSetConstantBuffers(0,1,&buffer);
        dc->Dispatch((W+7)/8,1,1);
        ID3D11ShaderResourceView* ns[2]{}; ID3D11UnorderedAccessView* nt=nullptr;
        dc->CSSetShaderResources(0,2,ns); dc->CSSetUnorderedAccessViews(0,1,&nt,nullptr);
        if (finalize) {
            ID3D11ShaderResourceView* finalSources[]{srv[0].Get(),residualSrv.Get()};
            target=finalUav.Get(); dc->CSSetShader(finalShader.Get(),nullptr,0);
            dc->CSSetShaderResources(0,2,finalSources); dc->CSSetUnorderedAccessViews(0,1,&target,nullptr);
            dc->Dispatch((W+7)/8,1,1);
            dc->CSSetShaderResources(0,2,ns); dc->CSSetUnorderedAccessViews(0,1,&nt,nullptr);
        }
        dc->CopyResource(staging.Get(),finalize?finalOutput.Get():output.Get()); D3D11_MAPPED_SUBRESOURCE mapped{};
        Check(dc->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped));
        Image result{};
        for (unsigned i=0;i<W;++i) for (unsigned c=0;c<4;++c) {
            const auto delta=half ? DirectX::PackedVector::XMConvertHalfToFloat(static_cast<const DirectX::PackedVector::HALF*>(mapped.pData)[i*4+c]) : static_cast<const float*>(mapped.pData)[i*4+c];
            assert(std::isfinite(delta)); result[i][c]=finalize || c==3 ? delta : o[i][c]+delta;
        }
        dc->Unmap(staging.Get(),0);
        if (debug) {
            for (UINT64 i=0;i<debug->GetNumStoredMessages();++i) {
                SIZE_T size=0; Check(debug->GetMessage(i,nullptr,&size)); std::vector<char> bytes(size);
                auto* m=reinterpret_cast<D3D11_MESSAGE*>(bytes.data()); Check(debug->GetMessage(i,m,&size));
                if (m->Severity<=D3D11_MESSAGE_SEVERITY_WARNING) { std::cerr<<m->pDescription<<'\n'; std::abort(); }
            }
            debug->ClearStoredMessages();
        }
        return result;
    }
};
Image Filled(Pixel p) { Image result; result.fill(p); return result; }
float Difference(Pixel a,Pixel b) { return std::max({std::abs(a[0]-b[0]),std::abs(a[1]-b[1]),std::abs(a[2]-b[2])}); }
float MaxAdjacent(const Image& data) { float m=0; for (unsigned i=1;i<W;++i) m=std::max(m,Difference(data[i-1],data[i])); return m; }
int main(int argc,char** argv) {
    assert(argc==2);
    // Metadata/migration tests use the same helper as production UI/import.
    for (float mode : {0.f,1.f,.5f,-1.f,std::numeric_limits<float>::quiet_NaN()}) {
        std::map<std::wstring,float> old{{L"residualChromaTemporalStrength",mode},{L"residualColorMode",mode},{L"residualShowProtection",1.f},
            {L"residualShowAdvanced",0.f},{L"residualSaturation",.65f},{L"residualMultiplier",1.8f},
            {L"residualChromaProtection",.6f},{L"residualNeutralProtection",50.f},{L"residualSkinProtection",75.f},
            {L"residualShowColorZones",1.f},{L"residualShadowChroma",0.f},{L"residualMidtoneChroma",.5f},
            {L"residualHighlightChroma",2.f},{L"residualMaxAgeFrames",3.f},{L"residualHueProtection",.35f},{L"pass2_intensity",.7f},{L"multiPass",3.f}};
        assert(Magpie::NormalizeDLSSNRDetailParameters(old));
        assert(!old.contains(L"residualChromaTemporalStrength") && !old.contains(L"residualColorMode") && !old.contains(L"residualShowProtection"));
        for (const auto key : {L"residualNeutralProtection",L"residualSkinProtection",L"residualShowColorZones",
            L"residualShadowChroma",L"residualMidtoneChroma",L"residualHighlightChroma"}) assert(!old.contains(key));
        assert(!old.contains(L"residualMaxAgeFrames"));
        assert(old.at(L"residualChromaProtection")==.6f);
        assert(old.at(L"residualShowAdvanced")==1 && old.at(L"residualSaturation")==.65f);
        assert(old.at(L"residualMultiplier")==1.8f && old.at(L"residualHueProtection")==.35f);
        assert(old.at(L"pass2_intensity")==.7f && old.at(L"multiPass")==3);
        auto copy=old; assert(!Magpie::NormalizeDLSSNRDetailParameters(copy) && copy==old);
    }
    std::map<std::wstring,float> empty;
    assert(Magpie::NormalizeDLSSNRDetailParameters(empty));
    assert(empty.at(L"enableInputResolutionScaling")==1 && empty.at(L"inputResolutionPercent")==100);
    std::map<std::string,float> values{{"enableInputResolutionScaling",1.f}};
    auto get=[&](std::string_view name,float fallback) { auto i=values.find(std::string(name)); return i==values.end() ? fallback : i->second; };
    for (auto name : Magpie::DLSSNR_RESIDUAL_PARAMETERS) {
        assert(Magpie::IsEffectParameterVisible("DLSSNR\\DLSSNR_AI_Filter",name,get) == !Magpie::IsDLSSNRAdvancedParameter(name));
    }
    values["residualShowAdvanced"]=1;
    for (auto name : Magpie::DLSSNR_RESIDUAL_PARAMETERS)
        assert(Magpie::IsEffectParameterVisible("DLSSNR\\DLSSNR_AI_Filter",name,get));
    values["residualColorMode"]=0; // Obsolete selectors cannot hide advanced controls.
    assert(Magpie::IsEffectParameterVisible("DLSSNR\\DLSSNR_AI_Filter","residualHueProtection",get));
    values["residualShowAdvanced"]=0;
    assert(!Magpie::IsEffectParameterVisible("DLSSNR\\DLSSNR_AI_Filter","residualDebugView",get));
    assert(!Magpie::IsEffectParameterVisible("DLSSNR\\DLSSNR_AI_Filter","residualChromaTemporalStrength",get));
    values["antiFlicker"]=2;
    assert(!Magpie::IsEffectParameterVisible("DLSSNR\\DLSSNR_AI_Filter","residualChromaTemporalStrength",get));
    Compile(COLOR_CONVERT_HLSL,"ConvertToRgba");
    Compile(COLOR_DOWNSAMPLE_HLSL,"DownsampleColorVertical");
    Compile(COLOR_DOWNSAMPLE_HLSL,"DownsampleColorHorizontal");
    Compile(GUIDANCE_DOWNSAMPLE_HLSL,"DownsampleGuidance");
    Compile(RESIDUAL_HORIZONTAL_HLSL,"UpsampleResidualHorizontal");
    Compile(verticalShader,"CompositeResidualVertical");
    Harness h(false), half(true);
    auto o=Filled({.2f,.4f,.8f,1}), n=Filled({.5f,.5f,.5f,1});
    ResampleConstants p;
    auto neutral=h.Run(o,n,p);
    assert(Difference(neutral[0],n[0])<1e-7f);
    auto fromLab=[](ColorReference::Color lab) {
        const auto rgb=ColorReference::FromLab(lab);
        return Pixel{float(rgb[0]),float(rgb[1]),float(rgb[2]),1};
    };
    auto toLab=[](Pixel rgb) { return ColorReference::ToLab({rgb[0],rgb[1],rgb[2]}); };
    {
        // Global color protection must preserve L and suppress both a/b changes
        // equally for gray, skin-range and non-skin colors, including FP16.
        for (const auto lab : {ColorReference::Color{.55,0,0}, {.65,.064,.076},
                {.55,-.06,.03}, {.55,.04,-.08}, {.25,.015,.02}}) {
            const auto base=Filled(fromLab(lab));
            const auto nr=Filled(fromLab({lab[0]+.025,lab[1]+.012,lab[2]-.009}));
            const auto originalLab=toLab(base[0]), candidateLab=toLab(nr[0]);
            for (float amount : {0.f,.25f,.5f,1.f}) {
                p={}; p.chromaProtection=amount;
                for (auto* harness : {&h,&half}) {
                    const auto actual=toLab(harness->Run(base,nr,p,false)[0]);
                    const double tolerance=harness->half ? 2e-4 : 2e-5;
                    assert(std::abs(actual[0]-candidateLab[0])<tolerance);
                    for (int c=1;c<=2;++c) {
                        const double expected=originalLab[c]+(1-amount)*(candidateLab[c]-originalLab[c]);
                        assert(std::abs(actual[c]-expected)<tolerance);
                    }
                }
            }
            p={}; p.chromaProtection=1; p.hueProtection=1;
            const auto both=toLab(h.Run(base,nr,p,false)[0]);
            for (int c=1;c<=2;++c) assert(std::abs(both[c]-originalLab[c])<2e-5);
        }
        // A constant low band is equivalent to changing the candidate first;
        // compression must constrain that reconstructed candidate, not vice versa.
        const auto grayBase=Filled({.2f,.2f,.2f,1}), grayNr=Filled({.6f,.6f,.6f,1});
        p={}; p.lowFrequencyGain=2; p.localCompression=1;
        const auto frequencyThenCompression=h.Run(grayBase,grayNr,p);
        p.lowFrequencyGain=1;
        const auto combined=h.Run(grayBase,Filled({1,1,1,1}),p);
        assert(Difference(frequencyThenCompression[0],combined[0])<2e-5f);
        // Signed/over-range intermediate corrections survive until final mapping.
        const auto brightBase=Filled({.9f,.2f,.05f,1}), brightNr=Filled({.98f,.12f,.35f,1});
        p={}; p.residualMultiplier=2;
        const auto raw=h.Run(brightBase,brightNr,p,false), mapped=h.Run(brightBase,brightNr,p);
        assert(raw[0][0]>1 && mapped[0][0]<=1);
        const auto a=toLab(raw[0]), b=toLab(mapped[0]);
        assert(std::abs(a[1]*b[2]-a[2]*b[1])<2e-5);
        p={};
    }
    // Gray midtones have no chroma: gain zero removes only their NR lightness
    // correction; gain one is the exact neutral fast path.
    auto midOriginal=Filled({.4f,.4f,.4f,1}), midNr=Filled({.48f,.48f,.48f,1});
    p.midtoneStrength=0;
    auto midZero=h.Run(midOriginal,midNr,p);
    assert(Difference(midZero[0],midOriginal[0])<2e-5f);
    p.midtoneStrength=2;
    auto midDouble=h.Run(midOriginal,midNr,p);
    const auto originalLab=ColorReference::ToLab({.4,.4,.4});
    const auto nrLab=ColorReference::ToLab({.48,.48,.48});
    const auto doubledLab=ColorReference::ToLab({midDouble[0][0],midDouble[0][1],midDouble[0][2]});
    assert(std::abs((doubledLab[0]-originalLab[0])-2*(nrLab[0]-originalLab[0]))<2e-5);
    p={};
    p.residualMultiplier=0; p.hueProtection=p.darkProtection=p.highlightProtection=p.localCompression=1;
    p.lowFrequencyGain=2; p.detailGain=0;
    auto zero=h.Run(o,n,p); assert(Difference(zero[0],o[0])==0);
    p.residualMultiplier=2; zero=h.Run(o,o,p); assert(Difference(zero[0],o[0])<1e-6f);
    // Exact grayscale and its two sides stay continuous for every configuration.
    for (unsigned i=0;i<W;++i) { float t=(static_cast<int>(i)-128)*1e-5f; n[i]={.5f+t,.5f,.5f-t,1}; }
    p={}; p.residualSaturation=.5f;
    auto gray=h.Run(o,n,p); assert(MaxAdjacent(gray)<.001f);
    auto grayHalf=half.Run(o,n,p); assert(MaxAdjacent(grayHalf)<.001f);
    auto cpuOriginal=ColorReference::ToLab({.2,.4,.8}), cpuGray=ColorReference::ToLab({.5,.5,.5});
    for (int c=1;c<3;++c) cpuGray[c]=cpuOriginal[c]+.5*(cpuGray[c]-cpuOriginal[c]);
    auto cpuExpected=ColorReference::FromLab(cpuGray);
    for (int c=0;c<3;++c) assert(std::abs(cpuExpected[c]-gray[128][c])<2e-5);
    auto white=ColorReference::ToLab({1,1,1}); assert(std::abs(white[0]-1)<1e-7 && std::abs(white[1])<1e-7 && std::abs(white[2])<1e-7);
    // Same stored-RGB input, nonuniform directional gains: no whole-vector jump.
    o=Filled({.45f,.4f,.35f,1});
    double greenLo=.35,greenHi=.45;
    const double originalL=ColorReference::ToLab({.45,.4,.35})[0];
    for (int k=0;k<50;++k) { double mid=(greenLo+greenHi)*.5; if (ColorReference::ToLab({.47,mid,.335})[0]<originalL) greenLo=mid; else greenHi=mid; }
    const float greenCross=static_cast<float>((greenLo+greenHi)*.5);
    for (unsigned i=0;i<W;++i) { float t=(static_cast<int>(i)-128)*1e-6f; n[i]={.47f,greenCross+t,.335f,1}; }
    p={}; p.shadowStructureMultiplier=0; p.reflectionGlowMultiplier=2;
    auto signScan=h.Run(o,n,p); assert(MaxAdjacent(signScan)<.001f);
    // Near-neutral fast path matches the converted path to float error.
    p={}; auto exact=h.Run(o,n,p); p.residualSaturation=1.000001f;
    auto converted=h.Run(o,n,p); assert(Difference(exact[128],converted[128])<2e-5f);
    // Parameter scans and finite/bounded adversarial input incl. black/white.
    std::mt19937 random(69); std::uniform_real_distribution<float> unit(0,1);
    for (unsigned i=0;i<W;++i) { o[i]={unit(random),unit(random),unit(random),1}; n[i]={unit(random),unit(random),unit(random),1}; }
    o[0]={0,0,0,1}; n[0]={1,1,1,1}; o[1]=n[0]; n[1]=o[0];
    for (int k=0;k<=20;++k) {
        p={}; p.residualMultiplier=k*.1f;
        p.residualSaturation=2; p.residualLightness=2; p.shadowStructureMultiplier=0;
        p.hueProtection=p.darkProtection=p.highlightProtection=p.localCompression=k*.05f;
        auto result=half.Run(o,n,p);
        for (auto pixel:result) for (int c=0;c<3;++c) assert(pixel[c]>=-.0006f && pixel[c]<=1.0006f);
    }
    // Continuous slider probes around activation/default points. This includes
    // out-of-gamut candidates, where a default fast path could otherwise jump.
    float worstSliderJump=0;
    const std::array<float ResampleConstants::*,13> sliders{
        &ResampleConstants::residualMultiplier,&ResampleConstants::residualSaturation,
        &ResampleConstants::residualLightness,&ResampleConstants::shadowStructureMultiplier,
        &ResampleConstants::reflectionGlowMultiplier,&ResampleConstants::midtoneStrength,&ResampleConstants::hueProtection,
        &ResampleConstants::darkProtection,&ResampleConstants::highlightProtection,
        &ResampleConstants::localCompression,&ResampleConstants::lowFrequencyGain,&ResampleConstants::detailGain,
        &ResampleConstants::chromaProtection};
    o=Filled({.9f,.2f,.05f,1}); n=Filled({.98f,.12f,.35f,1});
    for (auto slider:sliders) for (float boundary:{0.f,.04f,.5f,1.f,1.5f,2.f}) {
        p={}; p.residualMultiplier=1.6f;
        p.*slider=std::max(boundary-1e-5f,0.f); auto before=h.Run(o,n,p);
        p.*slider=boundary; auto at=h.Run(o,n,p);
        p.*slider=boundary+1e-5f; auto after=h.Run(o,n,p);
        worstSliderJump=std::max({worstSliderJump,Difference(before[128],at[128]),Difference(at[128],after[128])});
    }
    assert(worstSliderJump<.002f);
    std::cout<<"Gray max adjacent: "<<MaxAdjacent(gray)<<"; FP16: "<<MaxAdjacent(grayHalf)<<"; worst slider boundary jump: "<<worstSliderJump<<'\n';
    // CPU-independent direction property: full hue protection keeps red on its
    // original ray even when the NR candidate crosses the chroma origin.
    o=Filled({.8f,.2f,.2f,1}); n=Filled({.2f,.8f,.8f,1});
    p={}; p.hueProtection=1;
    auto protectedColor=h.Run(o,n,p); assert(protectedColor[0][0]>=protectedColor[0][1]-1e-5f);
    // A constant residual lies entirely in the low band; high-only output is 0.
    o=Filled({.4f,.3f,.6f,1}); n=Filled({.5f,.4f,.7f,1});
    p={}; p.lowFrequencyGain=0;
    auto highOnly=h.Run(o,n,p); assert(Difference(highOnly[100],o[100])<2e-5f);
    p.lowFrequencyGain=1; p.detailGain=0;
    auto lowOnly=h.Run(o,n,p); assert(Difference(lowOnly[100],n[100])<2e-5f);
    // Alternating corrections inhabit the high band; this checks that the
    // advanced slider really controls detail even though the L/S axis sample
    // above mostly contains a constant, low-frequency residual.
    for (unsigned i=0;i<W;++i) { float delta=i%2 ? .03f : -.03f; n[i]={o[i][0]+delta,o[i][1]+delta,o[i][2]+delta,1}; }
    p={}; p.lowFrequencyGain=0;
    highOnly=h.Run(o,n,p); assert(Difference(highOnly[100],n[100])<2e-5f);
    p.lowFrequencyGain=1; p.detailGain=0;
    lowOnly=h.Run(o,n,p); assert(Difference(lowOnly[100],o[100])<2e-5f);
    // No broad correction leaking across a strongly different guide edge.
    for (unsigned i=0;i<W;++i) { o[i]=i<128 ? Pixel{.2f,.2f,.2f,1} : Pixel{.8f,.1f,.8f,1}; n[i]=o[i]; if (i>=128) n[i][1]+=.05f; }
    p={}; p.lowFrequencyGain=2; p.detailGain=0;
    auto guided=h.Run(o,n,p); assert(Difference(guided[127],o[127])<2e-5f);
    // Export actual shader responses on lightness/saturation axes (HSV here
    // is only a source color grid; processing remains Oklab).
    std::ofstream evidence(std::string(argv[1])+"/detail-results.json");
    evidence<<"{\"runtime\":\"D3D11 WARP, production HLSL, float/FP16 UAV\",\"grayMaxAdjacent\":"<<MaxAdjacent(gray)<<",\"grayFP16MaxAdjacent\":"<<MaxAdjacent(grayHalf)<<",\"signMaxAdjacent\":"<<MaxAdjacent(signScan)<<",\"sliderBoundaryMaxJump\":"<<worstSliderJump<<",\"oklabZeroLGreen\":"<<greenCross<<",\"grayOutput\":["<<gray[128][0]<<','<<gray[128][1]<<','<<gray[128][2]<<"],\"cpuGrayOutput\":["<<cpuExpected[0]<<','<<cpuExpected[1]<<','<<cpuExpected[2]<<"]}\n";
    std::ofstream csv(std::string(argv[1])+"/detail-response.csv");
    csv<<"control,lightness,saturation,r,g,b,delta\n";
    for (int control=0;control<11;++control) for (unsigned l=0;l<33;++l) {
        for (unsigned i=0;i<W;++i) {
            float light=l/32.f, saturation=(i%33)/32.f;
            // HSL Hue 220: source grid has actual L=light and S=saturation.
            float chroma=(1-std::abs(2*light-1))*saturation, base=light-chroma*.5f;
            o[i]={base,base+chroma/3,base+chroma,1};
            n[i]={std::clamp(o[i][0]+.03f,0.f,1.f),std::clamp(o[i][1]-.01f,0.f,1.f),std::clamp(o[i][2]-.02f,0.f,1.f),1};
        }
        p={};
        switch(control) {
        case 0:p.residualMultiplier=1.5f;break; case 1:p.residualSaturation=1.5f;break;
        case 2:p.residualLightness=1.5f;break; case 3:p.shadowStructureMultiplier=1.5f;break;
        case 4:p.reflectionGlowMultiplier=1.5f;break; case 5:p.hueProtection=1;break;
        case 6:p.darkProtection=1;break; case 7:p.highlightProtection=1;break;
        case 8:p.localCompression=1;break; case 9:p.lowFrequencyGain=1.5f;break; case 10:p.detailGain=1.5f;break;
        }
        auto result=h.Run(o,n,p);
        for (unsigned i=0;i<33;++i) csv<<control<<','<<l/32.f<<','<<i/32.f<<','<<result[i][0]<<','<<result[i][1]<<','<<result[i][2]<<','<<Difference(result[i],n[i])<<'\n';
    }
    std::cout<<"Production detail HLSL: grayscale/sign/neutral/zero, FP16, gamut, hue crossing, complementary bands, visibility/migration and response export passed; D3D11 debug clean.\n";
}
