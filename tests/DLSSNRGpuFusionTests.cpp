// Compares the production fused kernels with their staged predecessors using
// CPU WARP by default; --hardware explicitly selects NVIDIA for native paired
// comparisons. No NGX runtime or optical-flow SDK is used.
#define NOMINMAX
#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <DirectXPackedVector.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <iomanip>
#include <limits>
#include <map>
#include <random>
#include <string>
#include <vector>
#include "DLSSNRResidualUnderTest.h"
#include "DLSSNRSparseShader.h"

using Microsoft::WRL::ComPtr;
using Pixel = std::array<float,4>;
using HalfPixel = std::array<uint16_t,4>;
using Inputs = std::array<ID3D11ShaderResourceView*,23>;
using Outputs = std::array<ID3D11UnorderedAccessView*,5>;
constexpr auto HalfFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
struct SparseConstants {
    uint32_t width, height, mapCount=0, confidenceBits=0;
    uint32_t inferenceMap=0, hasPast=1, finalize=0, reserved=0;
};
struct Texture {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
    UINT width=0, height=0, stride=0;
    DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN;
};
void Check(HRESULT hr) {
    if (FAILED(hr)) { std::cerr << "D3D failure: " << std::hex << hr << '\n'; std::abort(); }
}
UINT Stride(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R32G32B32A32_FLOAT: return 16;
    case HalfFormat: return 8;
    case DXGI_FORMAT_R8_UNORM: return 1;
    default: return 4;
    }
}
struct Harness {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> dc;
    ComPtr<ID3D11InfoQueue> debug;
    ComPtr<ID3D11Buffer> constants, counts, countReadback;
    ComPtr<ID3D11UnorderedAccessView> countUav;
    ComPtr<ID3D11SamplerState> sampler;
    std::map<std::string,ComPtr<ID3D11ComputeShader>> shaders;
    uint64_t cases=0, comparedPixels=0;
    explicit Harness(bool hardware) {
        ComPtr<IDXGIAdapter1> adapter;
        if (hardware) {
            ComPtr<IDXGIFactory1> factory; Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
            for (UINT i=0;;++i) {
                ComPtr<IDXGIAdapter1> candidate;
                const auto result=factory->EnumAdapters1(i,&candidate);
                if (result==DXGI_ERROR_NOT_FOUND) break;
                Check(result); DXGI_ADAPTER_DESC1 description{}; Check(candidate->GetDesc1(&description));
                if (description.VendorId==0x10de) { adapter=candidate; break; }
            }
            if (!adapter) { std::cerr << "NVIDIA adapter is unavailable\n"; std::abort(); }
        }
        const auto driver=hardware?D3D_DRIVER_TYPE_UNKNOWN:D3D_DRIVER_TYPE_WARP;
        auto hr=D3D11CreateDevice(adapter.Get(),driver,nullptr,D3D11_CREATE_DEVICE_DEBUG,
            nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&dc);
        if (hr==DXGI_ERROR_SDK_COMPONENT_MISSING) hr=D3D11CreateDevice(adapter.Get(),driver,
            nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&dc);
        Check(hr); device.As(&debug);
        D3D11_BUFFER_DESC cb{}; cb.ByteWidth=sizeof(ResampleConstants); cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        Check(device->CreateBuffer(&cb,nullptr,&constants));
        cb.ByteWidth=16; cb.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
        cb.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        Check(device->CreateBuffer(&cb,nullptr,&counts));
        D3D11_UNORDERED_ACCESS_VIEW_DESC view{}; view.Format=DXGI_FORMAT_R32_TYPELESS;
        view.ViewDimension=D3D11_UAV_DIMENSION_BUFFER; view.Buffer.NumElements=4; view.Buffer.Flags=D3D11_BUFFER_UAV_FLAG_RAW;
        Check(device->CreateUnorderedAccessView(counts.Get(),&view,&countUav));
        cb.BindFlags=cb.MiscFlags=0; cb.Usage=D3D11_USAGE_STAGING; cb.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        Check(device->CreateBuffer(&cb,nullptr,&countReadback));
        D3D11_SAMPLER_DESC sample{}; sample.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sample.AddressU=sample.AddressV=sample.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP; sample.MaxLOD=D3D11_FLOAT32_MAX;
        Check(device->CreateSamplerState(&sample,&sampler));
        const std::string sparse=std::string(Magpie::DLSSNR_COLOR_HLSL)+std::string(Magpie::DLSSNR_SPARSE_HLSL);
        for (const auto* entry:{"ComposeResidual","FillResidual","OutputColor","ExtractResidual","FillOutput"}) Compile(sparse,entry);
        const std::string probes=sparse+R"(
[numthreads(8,8,1)] void ProbeTrust(uint3 id:SV_DispatchThreadID) {
    if (any(id.xy>=Size)) return;
    float3 correction=0,now=Target.Load(int3(id.xy,0)).rgb;
    float trust=QuantizedTrust(HasPast?SourceWeight((id.xy+.5)/float2(Size),now,correction):0);
    float storedTrust=RawValidity.Load(int3(id.xy,0));
    ColorOut[id.xy]=float4(trust,storedTrust,trust-storedTrust,0);
}
[numthreads(8,8,1)] void ProbeCorrection(uint3 id:SV_DispatchThreadID) {
    if (any(id.xy>=Size)) return;
    float3 correction=0,now=Target.Load(int3(id.xy,0)).rgb;
    float trust=QuantizedTrust(HasPast?SourceWeight((id.xy+.5)/float2(Size),now,correction):0);
    float3 storedCorrection=f16tof32(f32tof16(correction));
    ColorOut[id.xy]=float4(storedCorrection-RawResidual.Load(int3(id.xy,0)).rgb,trust);
}
)";
        Compile(probes,"ProbeTrust"); Compile(probes,"ProbeCorrection");
        for (const auto* entry:{"CompositeResidualVertical","ExtractControlledResidualVertical"}) Compile(verticalShader,entry);
        Compile(prepareShader,"PrepareResidual");
        Compile(RESIDUAL_HORIZONTAL_HLSL,"UpsampleResidualHorizontal");
        Compile(COLOR_DOWNSAMPLE_HLSL,"DownsampleColorVertical");
        Compile(COLOR_DOWNSAMPLE_HLSL,"DownsampleColorHorizontal");
        Compile(COLOR_CONVERT_HLSL,"ConvertToRgba");
    }
    void Compile(const std::string& source,const char* entry) {
        ComPtr<ID3DBlob> code,error;
        const auto hr=D3DCompile(source.data(),source.size(),entry,nullptr,nullptr,entry,"cs_5_0",
            D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_WARNINGS_ARE_ERRORS|D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&error);
        if (error) std::cerr.write(static_cast<const char*>(error->GetBufferPointer()),error->GetBufferSize());
        Check(hr); Check(device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&shaders[entry]));
    }
    Texture Make(UINT w,UINT h,DXGI_FORMAT format,const void* data=nullptr,bool writable=true) {
        Texture t; t.width=w; t.height=h; t.format=format; t.stride=Stride(format);
        D3D11_TEXTURE2D_DESC desc{}; desc.Width=w; desc.Height=h; desc.Format=format;
        desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
        desc.BindFlags=D3D11_BIND_SHADER_RESOURCE|(writable?D3D11_BIND_UNORDERED_ACCESS:0);
        D3D11_SUBRESOURCE_DATA initial{data,w*t.stride,0};
        Check(device->CreateTexture2D(&desc,data?&initial:nullptr,&t.texture));
        Check(device->CreateShaderResourceView(t.texture.Get(),nullptr,&t.srv));
        if (writable) Check(device->CreateUnorderedAccessView(t.texture.Get(),nullptr,&t.uav));
        return t;
    }
    Texture Color(UINT w,UINT h,DXGI_FORMAT format,const std::vector<Pixel>& pixels) {
        if (format==DXGI_FORMAT_R32G32B32A32_FLOAT) return Make(w,h,format,pixels.data());
        if (format==HalfFormat) {
            std::vector<HalfPixel> half(pixels.size());
            for (size_t i=0;i<pixels.size();++i) for (size_t c=0;c<4;++c)
                half[i][c]=DirectX::PackedVector::XMConvertFloatToHalf(pixels[i][c]);
            return Make(w,h,format,half.data());
        }
        std::vector<std::array<uint8_t,4>> bytes(pixels.size());
        for (size_t i=0;i<pixels.size();++i) {
            for (size_t c=0;c<4;++c) bytes[i][c]=uint8_t(std::lround(std::clamp(pixels[i][c],0.f,1.f)*255));
            if (format==DXGI_FORMAT_B8G8R8A8_UNORM) std::swap(bytes[i][0],bytes[i][2]);
        }
        return Make(w,h,format,bytes.data(),false);
    }
    template<class T> void Run(const char* entry,const T& settings,const Inputs& inputs,const Outputs& outputs,UINT w,UINT h) {
        std::array<std::byte,sizeof(ResampleConstants)> bytes{};
        static_assert(sizeof(T)<=sizeof(bytes)); std::memcpy(bytes.data(),&settings,sizeof(T));
        dc->UpdateSubresource(constants.Get(),0,nullptr,bytes.data(),0,0);
        auto* cb=constants.Get(); auto* sample=sampler.Get();
        dc->CSSetShader(shaders.at(entry).Get(),nullptr,0); dc->CSSetConstantBuffers(0,1,&cb); dc->CSSetSamplers(0,1,&sample);
        dc->CSSetShaderResources(0,UINT(inputs.size()),inputs.data()); dc->CSSetUnorderedAccessViews(0,UINT(outputs.size()),outputs.data(),nullptr);
        dc->Dispatch((w+7)/8,(h+7)/8,1);
        Inputs empty{}; Outputs unbound{};
        dc->CSSetShaderResources(0,UINT(empty.size()),empty.data()); dc->CSSetUnorderedAccessViews(0,UINT(unbound.size()),unbound.data(),nullptr);
    }
    void ClearCounts() { const UINT zero[4]{}; dc->ClearUnorderedAccessViewUint(countUav.Get(),zero); }
    uint32_t Count() {
        dc->CopyResource(countReadback.Get(),counts.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{}; Check(dc->Map(countReadback.Get(),0,D3D11_MAP_READ,0,&mapped));
        const auto value=*static_cast<const uint32_t*>(mapped.pData); dc->Unmap(countReadback.Get(),0); return value;
    }
    std::vector<uint8_t> Read(const Texture& t) {
        D3D11_TEXTURE2D_DESC desc{}; t.texture->GetDesc(&desc);
        desc.BindFlags=desc.MiscFlags=0; desc.Usage=D3D11_USAGE_STAGING; desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging; Check(device->CreateTexture2D(&desc,nullptr,&staging));
        dc->CopyResource(staging.Get(),t.texture.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{}; Check(dc->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped));
        std::vector<uint8_t> data(size_t(t.width)*t.height*t.stride);
        for (UINT y=0;y<t.height;++y) std::memcpy(data.data()+size_t(y)*t.width*t.stride,
            static_cast<const uint8_t*>(mapped.pData)+size_t(y)*mapped.RowPitch,size_t(t.width)*t.stride);
        dc->Unmap(staging.Get(),0); return data;
    }
    void Equal(const Texture& old,const Texture& fused,const std::string& label) {
        const auto a=Read(old),b=Read(fused);
        assert(a.size()==b.size());
        for (size_t i=0;i<a.size();++i) if (a[i]!=b[i]) {
            // NaN payloads are unspecified; preserve their per-channel class.
            if (old.format==HalfFormat) {
                const size_t offset=i&~size_t(1); uint16_t ah,bh;
                std::memcpy(&ah,a.data()+offset,2); std::memcpy(&bh,b.data()+offset,2);
                if ((ah&0x7c00)==0x7c00 && (bh&0x7c00)==0x7c00 && (ah&0x3ff) && (bh&0x3ff)) continue;
            }
            std::cerr << label << " byte " << i << ": old=" << unsigned(a[i]) << " fused=" << unsigned(b[i]);
            if (old.format==HalfFormat) {
                const size_t offset=i&~size_t(1); uint16_t ah,bh;
                std::memcpy(&ah,a.data()+offset,2); std::memcpy(&bh,b.data()+offset,2);
                std::cerr << " halfOld=" << ah << " halfFused=" << bh << " valueOld="
                    << DirectX::PackedVector::XMConvertHalfToFloat(ah) << " valueFused="
                    << DirectX::PackedVector::XMConvertHalfToFloat(bh);
            }
            std::cerr << '\n';
            std::abort();
        }
        comparedPixels+=uint64_t(old.width)*old.height;
    }
    void DebugClean() {
        if (!debug) return;
        for (UINT64 i=0;i<debug->GetNumStoredMessages();++i) {
            SIZE_T size=0; Check(debug->GetMessage(i,nullptr,&size)); std::vector<char> bytes(size);
            auto* message=reinterpret_cast<D3D11_MESSAGE*>(bytes.data()); Check(debug->GetMessage(i,message,&size));
            if (message->Severity<=D3D11_MESSAGE_SEVERITY_WARNING) { std::cerr << message->pDescription << '\n'; std::abort(); }
        }
        debug->ClearStoredMessages();
    }
};

void DisplayEquivalence(Harness& h) {
    std::mt19937 random(0x061203); std::uniform_real_distribution<float> unit(0,1);
    for (const auto [w,height]:{std::pair{1u,1u},std::pair{13u,9u},std::pair{31u,17u}})
    for (const auto inputFormat:{DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_B8G8R8A8_UNORM,DXGI_FORMAT_R32G32B32A32_FLOAT})
    for (unsigned seed=0;seed<24;++seed) {
        const size_t n=size_t(w)*height; std::vector<Pixel> target(n),reference(n),residual(n);
        std::vector<std::array<uint16_t,2>> motion(n); std::vector<uint8_t> confidence(n);
        for (size_t i=0;i<n;++i) {
            for (size_t c=0;c<4;++c) {
                target[i][c]=unit(random); reference[i][c]=target[i][c]+(unit(random)-.5f)*.14f;
                residual[i][c]=(unit(random)-.5f)*1.4f;
            }
            confidence[i]=uint8_t((i*37+seed*23)%256);
            motion[i]={DirectX::PackedVector::XMConvertFloatToHalf(seed%2 ? unit(random)-.5f : 0),0};
        }
        if (inputFormat==DXGI_FORMAT_R32G32B32A32_FLOAT && seed==4) {
            for (size_t i=0;i<n;i+=17) target[i][i%3]=std::numeric_limits<float>::quiet_NaN();
            for (size_t i=0;i<n;i+=19) residual[i][i%3]=std::numeric_limits<float>::infinity();
        }
        auto now=h.Color(w,height,inputFormat,target),old=h.Color(w,height,inputFormat,reference);
        auto delta=h.Color(w,height,HalfFormat,residual),mv=h.Make(w,height,DXGI_FORMAT_R16G16_FLOAT,motion.data());
        auto conf=h.Make(w,height,DXGI_FORMAT_R8_UNORM,confidence.data());
        Inputs inputs{}; inputs[0]=now.srv.Get(); inputs[1]=old.srv.Get(); inputs[2]=delta.srv.Get(); inputs[3]=mv.srv.Get(); inputs[4]=conf.srv.Get();
        for (UINT hasPast=0;hasPast<2;++hasPast) for (const auto format:{HalfFormat,DXGI_FORMAT_R8G8B8A8_UNORM}) {
            SparseConstants values{w,height}; values.hasPast=hasPast; values.finalize=format!=HalfFormat;
            auto raw=h.Make(w,height,HalfFormat),rawTrust=h.Make(w,height,DXGI_FORMAT_R8_UNORM);
            auto filled=h.Make(w,height,HalfFormat),filledTrust=h.Make(w,height,DXGI_FORMAT_R8_UNORM);
            auto output=h.Make(w,height,format),trust=h.Make(w,height,DXGI_FORMAT_R8_UNORM);
            auto fused=h.Make(w,height,format),fusedTrust=h.Make(w,height,DXGI_FORMAT_R8_UNORM);
            h.ClearCounts(); h.Run("ComposeResidual",values,inputs,{raw.uav.Get(),rawTrust.uav.Get(),h.countUav.Get()},w,height);
            const auto count=h.Count(); Inputs fillInputs=inputs; fillInputs[21]=raw.srv.Get(); fillInputs[22]=rawTrust.srv.Get();
            h.Run("FillResidual",values,fillInputs,{filled.uav.Get(),filledTrust.uav.Get()},w,height);
            Inputs outputInputs{}; outputInputs[21]=filled.srv.Get(); outputInputs[22]=filledTrust.srv.Get();
            h.Run("OutputColor",values,outputInputs,{output.uav.Get(),trust.uav.Get()},w,height);
            auto fusedRaw=h.Make(w,height,HalfFormat),fusedRawTrust=h.Make(w,height,DXGI_FORMAT_R8_UNORM);
            h.ClearCounts(); h.Run("ComposeResidual",values,inputs,{fusedRaw.uav.Get(),fusedRawTrust.uav.Get(),h.countUav.Get()},w,height);
            Inputs fusedInputs=inputs; fusedInputs[21]=fusedRaw.srv.Get(); fusedInputs[22]=fusedRawTrust.srv.Get();
            h.Run("FillOutput",values,fusedInputs,{fused.uav.Get(),fusedTrust.uav.Get()},w,height);
            if (h.Count()!=count) { std::cerr << "Contribution count changed\n"; std::abort(); }
            const std::string label="compose seed="+std::to_string(seed)+" input="+std::to_string(inputFormat)+" output="+std::to_string(format)+" past="+std::to_string(hasPast);
            const auto stagedBytes=h.Read(output),fusedBytes=h.Read(fused);
            if (stagedBytes!=fusedBytes) {
                const auto different=std::mismatch(stagedBytes.begin(),stagedBytes.end(),fusedBytes.begin()).first-stagedBytes.begin();
                const size_t pixel=size_t(different)/output.stride;
                auto probe=h.Make(w,height,DXGI_FORMAT_R32G32B32A32_FLOAT);
                for (const auto* entry:{"ProbeTrust","ProbeCorrection"}) {
                    h.Run(entry,values,fillInputs,{probe.uav.Get()},w,height);
                    const auto bytes=h.Read(probe); Pixel p{}; std::memcpy(&p,bytes.data()+pixel*sizeof(Pixel),sizeof(p));
                    std::cerr << label << " " << w << 'x' << height << " pixel=" << pixel << ' ' << entry << std::setprecision(12);
                    for (float v:p) std::cerr << ' ' << v << "(0x" << std::hex << std::bit_cast<uint32_t>(v) << std::dec << ')';
                    std::cerr << '\n';
                }
            }
            h.Equal(output,fused,label); h.Equal(trust,fusedTrust,label+" validity");
            h.DebugClean(); ++h.cases;
        }
    }
}

void ExtractionEquivalence(Harness& h) {
    std::mt19937 random(0x16f); std::uniform_real_distribution<float> unit(0,1);
    for (const auto [w,height]:{std::pair{13u,9u},std::pair{31u,17u}})
    for (const auto inputFormat:{DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_B8G8R8A8_UNORM,DXGI_FORMAT_R32G32B32A32_FLOAT})
    for (UINT reduced:{0u,1u}) {
        const UINT rh=reduced?(height+1)/2:height; const size_t n=size_t(w)*height;
        std::vector<Pixel> original(n),correction(size_t(w)*rh);
        for (auto& p:original) for (auto& c:p) c=unit(random);
        for (auto& p:correction) for (auto& c:p) c=(unit(random)-.5f)*3;
        if (inputFormat==DXGI_FORMAT_R32G32B32A32_FLOAT) {
            original[0][0]=std::numeric_limits<float>::quiet_NaN();
            correction[1][1]=std::numeric_limits<float>::infinity();
            correction[2][2]=-std::numeric_limits<float>::infinity();
        }
        auto now=h.Color(w,height,inputFormat,original),delta=h.Color(w,rh,HalfFormat,correction);
        Inputs inputs{}; inputs[0]=now.srv.Get(); inputs[1]=delta.srv.Get();
        for (UINT debug=0;debug<8;++debug) for (UINT finalize=0;finalize<2;++finalize) {
            ResampleConstants values{}; values.sourceWidth=w; values.sourceHeight=height;
            values.targetWidth=reduced?(w+1)/2:w; values.targetHeight=rh; values.debugView=debug; values.finalizeOutput=finalize;
            auto color=h.Make(w,height,HalfFormat),old=h.Make(w,height,HalfFormat),fused=h.Make(w,height,HalfFormat);
            h.Run("CompositeResidualVertical",values,inputs,{color.uav.Get()},w,height);
            Inputs extraction{}; extraction[0]=now.srv.Get(); extraction[1]=color.srv.Get();
            h.Run("ExtractResidual",SparseConstants{w,height},extraction,{old.uav.Get()},w,height);
            h.Run("ExtractControlledResidualVertical",values,inputs,{fused.uav.Get()},w,height);
            h.Equal(old,fused,"extract debug="+std::to_string(debug)+" finalize="+std::to_string(finalize));
            h.DebugClean(); ++h.cases;
        }
    }
}

void PreparedSnapshotEquivalence(Harness& h) {
    constexpr UINT w=31,height=17;
    std::mt19937 random(0x12); std::uniform_real_distribution<float> unit(0,1);
    for (const auto inputFormat:{DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_B8G8R8A8_UNORM}) for (UINT percent:{25u,50u,100u}) {
        std::vector<Pixel> pixels(w*height); for (auto& p:pixels) for (auto& c:p) c=unit(random);
        auto input=h.Color(w,height,inputFormat,pixels);
        const UINT rw=(w*percent+50)/100,rh=(height*percent+50)/100;
        auto current=h.Make(rw,rh,DXGI_FORMAT_R8G8B8A8_UNORM),old=h.Make(rw,rh,DXGI_FORMAT_R8G8B8A8_UNORM);
        auto mailbox=h.Make(rw,rh,DXGI_FORMAT_R8G8B8A8_UNORM),inFlight=h.Make(rw,rh,DXGI_FORMAT_R8G8B8A8_UNORM);
        auto prepare=[&](Texture& output) {
            if (percent==100) {
                if (inputFormat==DXGI_FORMAT_R8G8B8A8_UNORM) h.dc->CopyResource(output.texture.Get(),input.texture.Get());
                else { Inputs inputs{}; inputs[0]=input.srv.Get(); h.Run("ConvertToRgba",ResampleConstants{},inputs,{output.uav.Get()},rw,rh); }
            } else {
                ResampleConstants values{}; values.sourceWidth=w; values.sourceHeight=height; values.targetWidth=rw; values.targetHeight=rh;
                auto temp=h.Make(w,rh,HalfFormat); Inputs inputs{}; inputs[0]=input.srv.Get();
                h.Run("DownsampleColorVertical",values,inputs,{temp.uav.Get()},w,rh);
                inputs[0]=temp.srv.Get(); h.Run("DownsampleColorHorizontal",values,inputs,{output.uav.Get()},rw,rh);
            }
        };
        prepare(current); prepare(old);
        h.dc->CopyResource(mailbox.texture.Get(),current.texture.Get());
        h.dc->CopyResource(inFlight.texture.Get(),mailbox.texture.Get());
        const float clear[4]{.9f,.7f,.5f,.3f};
        h.dc->ClearUnorderedAccessViewFloat(current.uav.Get(),clear);
        h.dc->CopyResource(mailbox.texture.Get(),current.texture.Get());
        h.Equal(old,inFlight,"prepared snapshot survives mailbox replacement");
        h.DebugClean(); ++h.cases;
    }
}

void ControlledExtractionEquivalence(Harness& h) {
    constexpr UINT w=31,height=17;
    std::mt19937 random(0x0c1ab); std::uniform_real_distribution<float> unit(0,1);
    for (const auto inputFormat:{DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_B8G8R8A8_UNORM})
    for (UINT percent:{25u,67u,100u}) {
        const UINT rw=(w*percent+50)/100,rh=(height*percent+50)/100;
        std::vector<Pixel> original(w*height),low(size_t(rw)*rh),denoised(low.size());
        for (auto& p:original) for (auto& c:p) c=unit(random);
        for (size_t i=0;i<low.size();++i) for (size_t c=0;c<4;++c) {
            low[i][c]=unit(random); denoised[i][c]=low[i][c]+(unit(random)-.5f)*1.1f;
        }
        auto now=h.Color(w,height,inputFormat,original);
        auto reduced=h.Color(rw,rh,DXGI_FORMAT_R8G8B8A8_UNORM,low),nr=h.Color(rw,rh,HalfFormat,denoised);
        std::vector<uint8_t> neutral;
        for (UINT preset=0;preset<4;++preset) for (UINT debug=0;debug<8;++debug) {
            ResampleConstants values{}; values.sourceWidth=w; values.sourceHeight=height;
            values.targetWidth=rw; values.targetHeight=rh; values.finalizeOutput=0; values.debugView=debug;
            if (preset==1) {
                values.residualMultiplier=1.7f; values.residualSaturation=.35f; values.residualLightness=1.4f;
                values.shadowStructureMultiplier=.6f; values.reflectionGlowMultiplier=1.8f; values.midtoneStrength=1.3f;
                values.hueProtection=.65f; values.chromaProtection=.25f; values.darkProtection=.7f;
                values.highlightProtection=.8f; values.localCompression=.5f;
                values.lowFrequencyGain=.3f; values.detailGain=1.8f;
            } else if (preset==2) {
                values.residualMultiplier=.7f; values.residualSaturation=1.8f; values.residualLightness=.5f;
                values.shadowStructureMultiplier=1.5f; values.reflectionGlowMultiplier=.4f; values.midtoneStrength=.6f;
                values.hueProtection=1; values.chromaProtection=1; values.darkProtection=1;
                values.highlightProtection=1; values.localCompression=1;
                values.lowFrequencyGain=1.6f; values.detailGain=.2f;
            } else if (preset==3) {
                values.residualMultiplier=0; values.lowFrequencyGain=0; values.detailGain=0;
            }
            auto controlled=h.Make(rw,rh,HalfFormat),horizontal=h.Make(w,rh,HalfFormat);
            auto color=h.Make(w,height,HalfFormat),old=h.Make(w,height,HalfFormat),fused=h.Make(w,height,HalfFormat);
            Inputs inputs{}; inputs[0]=reduced.srv.Get(); inputs[1]=nr.srv.Get();
            h.Run("PrepareResidual",values,inputs,{controlled.uav.Get()},rw,rh);
            ID3D11ShaderResourceView* vertical=controlled.srv.Get();
            if (rw!=w) {
                inputs={}; inputs[0]=controlled.srv.Get();
                h.Run("UpsampleResidualHorizontal",values,inputs,{horizontal.uav.Get()},w,rh); vertical=horizontal.srv.Get();
            }
            inputs={}; inputs[0]=now.srv.Get(); inputs[1]=vertical;
            h.Run("CompositeResidualVertical",values,inputs,{color.uav.Get()},w,height);
            Inputs extract{}; extract[0]=now.srv.Get(); extract[1]=color.srv.Get();
            h.Run("ExtractResidual",SparseConstants{w,height},extract,{old.uav.Get()},w,height);
            h.Run("ExtractControlledResidualVertical",values,inputs,{fused.uav.Get()},w,height);
            h.Equal(old,fused,"controlled extraction preset="+std::to_string(preset)+" debug="+std::to_string(debug));
            if (debug==0) {
                const auto pixels=h.Read(fused);
                if (preset==0) neutral=pixels;
                else if (pixels==neutral) { std::cerr << "Non-neutral controls did not alter the fixture\n"; std::abort(); }
            }
            h.DebugClean(); ++h.cases;
        }
    }
}
int main(int argc,char** argv) {
    const bool hardware=argc==2 && std::string(argv[1])=="--hardware";
    assert(argc==1 || hardware);
    Harness h(hardware); DisplayEquivalence(h); ExtractionEquivalence(h); ControlledExtractionEquivalence(h); PreparedSnapshotEquivalence(h);
    std::cout << "GPU fusion " << (hardware?"NVIDIA":"WARP") << " equivalence: " << h.cases << " cases, " << h.comparedPixels
        << " pixel comparisons passed; R8 validity/counts and FP16/UNORM pixels match.\n";
}
