// Standalone GPU smoke: production adapters, no ScalingWindow or user settings.
#include "pch.h"
#include "ScalingOptions.h"
#include <parallel_hashmap/phmap.h>
#define private public
#include "DeviceResources.h"
#undef private
#include "DLSSSRUpscaler.h"
#include "DLSSFrameGenerator.h"
#include "DLSSNRMultiPass.h"
#include <DirectXPackedVector.h>
#include "NgxD3D12Core.h"
#include "DirectXHelper.h"
#include "Logger.h"
#include <shlobj.h>
#include <iostream>
#include <d3d11sdklayers.h>
#include <d3d12sdklayers.h>

// Test-only cache-root injection. All loader verification/loading code below is
// production source; Windows KnownFolder resolution alone is redirected so the
// probe never writes the installed Magpie cache or config.
static HRESULT SmokeKnownFolderPath(REFKNOWNFOLDERID, DWORD, HANDLE, PWSTR* result) {
    const auto path = std::filesystem::current_path().parent_path().parent_path();
    std::filesystem::create_directories(path);
    std::wstring text(32768, L'\0');
    const DWORD length = GetShortPathNameW(path.c_str(), text.data(), static_cast<DWORD>(text.size()));
    if (!length || length >= text.size()) return HRESULT_FROM_WIN32(GetLastError());
    text.resize(length);
    std::wcout << L"Isolated cache root=" << text << std::endl;
    *result = static_cast<PWSTR>(CoTaskMemAlloc((text.size() + 1) * sizeof(wchar_t)));
    if (!*result) return E_OUTOFMEMORY;
    std::memcpy(*result, text.c_str(), (text.size() + 1) * sizeof(wchar_t));
    return S_OK;
}
#define SHGetKnownFolderPath SmokeKnownFolderPath
#include "DlssgSm86Runtime.cpp"
#undef SHGetKnownFolderPath

using namespace Magpie;
static void Require(bool good, const char* stage) {
    if (!good) { Logger::Get().Flush(); std::cerr << "FAIL stage=" << stage << std::endl; throw std::runtime_error(stage); }
}
static void Check(HRESULT hr, const char* stage) {
    if (FAILED(hr)) std::cerr << "HRESULT=" << std::hex << uint32_t(hr) << std::dec << std::endl;
    Require(SUCCEEDED(hr), stage);
}
static uint64_t ReadOutput(DeviceResources& resources, ID3D11Texture2D* source) {
    auto* dc = resources.GetD3DDC();
    D3D11_TEXTURE2D_DESC desc{}; source->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = desc.MiscFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    winrt::com_ptr<ID3D11Texture2D> staging;
    Check(resources.GetD3DDevice()->CreateTexture2D(&desc, nullptr, staging.put()), "staging-create");
    dc->CopyResource(staging.get(), source);
    winrt::com_ptr<ID3D11Fence> fence;
    Check(resources.GetD3DDevice()->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.put())), "fence-create");
    wil::unique_event_nothrow event; Require(event.try_create(wil::EventOptions::None, nullptr), "event-create");
    Check(dc->Signal(fence.get(), 1), "fence-signal");
    Check(fence->SetEventOnCompletion(1, event.get()), "fence-event"); dc->Flush();
    Require(event.wait(15000), "GPU-completion-timeout");
    D3D11_MAPPED_SUBRESOURCE mapped{}; Check(dc->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped), "readback-map");
    uint64_t hash = 1469598103934665603ull;
    const bool hdr=desc.Format==DXGI_FORMAT_R16G16B16A16_FLOAT;
    const uint32_t bytes=hdr?8:4;
    float low=std::numeric_limits<float>::max(),high=std::numeric_limits<float>::lowest();
    for (uint32_t y=0; y<desc.Height; ++y) {
        const auto* row=static_cast<const uint8_t*>(mapped.pData)+size_t(y)*mapped.RowPitch;
        for (uint32_t x=0; x<desc.Width*bytes; ++x) hash=(hash^row[x])*1099511628211ull;
        for (uint32_t x=0; x<desc.Width; ++x) for (uint32_t c=0;c<3;++c) {
            const float value=hdr?DirectX::PackedVector::XMConvertHalfToFloat(reinterpret_cast<const uint16_t*>(row)[x*4+c]):float(row[x*4+c]);
            Require(std::isfinite(value),"finite-output-pixels");low=std::min(low,value);high=std::max(high,value);
        }
    }
    dc->Unmap(staging.get(),0);
    Require(high > low && high > 0, "output-has-spatial-color-content");
    Check(resources.GetD3DDevice()->GetDeviceRemovedReason(), "D3D11-device-health");
    std::cout << "gpuCompleted=true outputHash=" << std::hex << hash << std::dec << " rgbRange=" << low << ',' << high << std::endl;
    return hash;
}
static void RunNr(DeviceResources& resources, bool hdr) {
    constexpr uint32_t w=512,h=288;
    const auto format=hdr?DXGI_FORMAT_R16G16B16A16_FLOAT:DXGI_FORMAT_R8G8B8A8_UNORM;
    auto input=DirectXHelper::CreateTexture2D(resources.GetD3DDevice(),format,w,h,D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS);
    auto output=DirectXHelper::CreateTexture2D(resources.GetD3DDevice(),format,w,h,D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS);
    Require(input && output,"NR-textures");
    FrameGuidanceView zero;
    std::array<winrt::com_ptr<ID3D11Texture2D>,3> textures;
    FrameGuidanceResource* channels[]{&zero.motion,&zero.depth,&zero.confidence};
    const DXGI_FORMAT formats[]{DXGI_FORMAT_R16G16_FLOAT,DXGI_FORMAT_R32_FLOAT,DXGI_FORMAT_R8_UNORM};
    for (size_t i=0;i<3;++i) {
        textures[i]=DirectXHelper::CreateSharedTexture2D(resources.GetD3DDevice(),formats[i],w,h,
            D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS,"preset smoke zero guidance");
        Require(bool(textures[i]),"NR-guidance-textures");
        winrt::com_ptr<ID3D11UnorderedAccessView> uav;
        Check(resources.GetD3DDevice()->CreateUnorderedAccessView(textures[i].get(),nullptr,uav.put()),"NR-guidance-UAV");
        const float zeros[4]{};resources.GetD3DDC()->ClearUnorderedAccessViewFloat(uav.get(),zeros);
        *channels[i]={textures[i].get(),formats[i],{.resourceGeneration=1,.sourceExtent={w,h},.validRegion={0,0,w,h},.valid=true,.isZero=true}};
    }
    EffectOption option;option.name="DLSSNR\\DLSSNR_AI_Filter";
    option.parameters["multiPass"]=1;option.parameters["antiFlicker"]=1;option.parameters["antiFlickerTimeMs"]=5;
    NgxD3D12Core core;DLSSNRMultiPass chain;
    Require(chain.Initialize(resources,core,input.get(),output.get(),option,hdr),"NR-Initialize");
    std::cout << "featureCreated=true NR passes=1 antiFlicker=1 hdr=" << hdr << std::endl;
    uint64_t frame=0,priorHash=0;bool changed=false;
    for (float time : {5.f,80.f,200.f}) {
        option.parameters["antiFlickerTimeMs"]=time;
        const std::array<std::string,1> names{"antiFlickerTimeMs"};
        Require(chain.ApplyLiveParameters(option,names),"NR-time-live-apply");
        if (frame) {
            Require(chain.Draw({.input=input.get(),.output=output.get(),.frameId=frame,.previousCaptureFrameId=frame-1,
                .inputRevision=frame,.isNewCaptureFrame=false,.frameGuidance=zero,.zeroFrameGuidance=zero}),"NR-duplicate-Draw");
            Require(ReadOutput(resources,output.get())==priorHash,"NR-time-only-duplicate-keeps-output");
            std::cout << "duplicateOutputUnchanged=true timeMs=" << time << " nativeEvaluateCounter=unobserved" << std::endl;
        }
        for (unsigned n=0;n<2;++n) {
            ++frame;
            for(auto* channel:channels){channel->metadata.frameId=frame;channel->metadata.timestamp100ns=int64_t(frame)*166667;}
            std::vector<uint8_t> pixels(size_t(w)*h*(hdr?8:4));
            for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x){
                const float values[]{.15f+float(x)/w*.5f+float(frame)*.015f,.1f+float(y)/h*.6f,.35f,1.f};
                for(uint32_t c=0;c<4;++c){
                    if(hdr)reinterpret_cast<uint16_t*>(pixels.data())[(size_t(y)*w+x)*4+c]=DirectX::PackedVector::XMConvertFloatToHalf(values[c]);
                    else pixels[(size_t(y)*w+x)*4+c]=uint8_t(std::lround(values[c]*255));
                }
            }
            resources.GetD3DDC()->UpdateSubresource(input.get(),0,nullptr,pixels.data(),w*(hdr?8:4),0);
            Require(chain.Draw({.input=input.get(),.output=output.get(),.frameId=frame,.previousCaptureFrameId=frame-1,
                .inputRevision=frame,.frameGuidance=zero,.zeroFrameGuidance=zero}) && chain.IsDlssNrProcessing(),"NR-new-capture-Draw");
            const auto hash=ReadOutput(resources,output.get());changed|=priorHash&&priorHash!=hash;priorHash=hash;
            std::cout << "evaluateReturned=true NR frame=" << frame << " timeMs=" << time << std::endl;
        }
    }
    Require(chain.Drain(),"NR-drain");Check(core.Device()->GetDeviceRemovedReason(),"NR-D3D12-device-health");Require(changed,"NR-changing-output");
    std::cout << "PASS kind=nr hdr=" << hdr << " captures=6 timeMs=5,80,200 duplicateOutputUnchanged=true nativeEvaluateCounter=unobserved" << std::endl;
}
int main(int argc, char** argv) {
    if (argc != 4) { std::cerr << "usage: DLSSPresetSmoke sr|fg preset multiplier\n"; return 2; }
    try {
        winrt::init_apartment();
        Logger::Get().Initialize(spdlog::level::info,L"DLSSPresetSmoke.log",10000000,1);
        const bool fg=std::string_view(argv[1])=="fg";
        const int preset=std::stoi(argv[2]); const unsigned multiplier=std::stoul(argv[3]);
        std::cout << "kind=" << argv[1] << " requestedPreset=" << preset << " requestedMultiplier=" << multiplier << " effectiveModel=unknown" << std::endl;
        DeviceResources resources;
        Check(CreateDXGIFactory2(0,IID_PPV_ARGS(resources._dxgiFactory.put())),"DXGI-create");
        bool found=false;
        for (UINT i=0;;++i) {
            winrt::com_ptr<IDXGIAdapter1> adapter;
            if (resources._dxgiFactory->EnumAdapters1(i,adapter.put()) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_ADAPTER_DESC1 desc{}; Check(adapter->GetDesc1(&desc),"adapter-desc");
            if (desc.VendorId != 0x10de) continue;
            resources._graphicsAdapter=adapter.as<IDXGIAdapter4>();
            winrt::com_ptr<ID3D11Device> device; winrt::com_ptr<ID3D11DeviceContext> context;
            Check(D3D11CreateDevice(adapter.get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,0,nullptr,0,D3D11_SDK_VERSION,
                device.put(),nullptr,context.put()),"D3D11-create");
            resources._d3dDevice=device.as<ID3D11Device5>(); resources._d3dDC=context.as<ID3D11DeviceContext4>();
            std::wcout << L"GPU=" << desc.Description << std::endl; found=true; break;
        }
        Require(found,"NVIDIA-adapter");
        if (std::string_view(argv[1])=="nr") {RunNr(resources,preset!=0);Logger::Get().Flush();return 0;}
        const uint32_t width=fg?1280:640, height=fg?720:360;
        auto input=DirectXHelper::CreateTexture2D(resources.GetD3DDevice(),DXGI_FORMAT_R8G8B8A8_UNORM,width,height,
            D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS);
        auto output=DirectXHelper::CreateTexture2D(resources.GetD3DDevice(),DXGI_FORMAT_R8G8B8A8_UNORM,1280,720,
            D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS);
        Require(input && output,"input-output-textures");
        NgxD3D12Core core; DLSSFrameGenerator generator; DLSSSRUpscaler upscaler;
        if (fg) Require(generator.Initialize(resources,core,input.get(),{width,height},
            {.multiplier=multiplier,.renderPreset=ParseDLSSFGRenderPreset(float(preset))}),"FG-Initialize");
        else Require(upscaler.Initialize(resources,input.get(),output.get(),{.renderPreset=preset}),"SR-Initialize");
        std::cout << "featureCreated=true" << std::endl;
        const FrameGuidanceView zero{}; uint32_t published=0; uint64_t priorHash=0; bool changed=false;
        for (uint64_t frame=1;frame<=6;++frame) {
            std::vector<uint32_t> pixels(size_t(width)*height);
            for (uint32_t y=0;y<height;++y) for (uint32_t x=0;x<width;++x) {
                const bool box=x>100+frame*8 && x<300+frame*8 && y>100 && y<250;
                const uint8_t r=box?230:uint8_t(30+(x*110/width));
                const uint8_t g=uint8_t(20+y*160/height), b=box?40:120;
                pixels[size_t(y)*width+x]=r|(uint32_t(g)<<8)|(uint32_t(b)<<16)|0xff000000u;
            }
            resources.GetD3DDC()->UpdateSubresource(input.get(),0,nullptr,pixels.data(),width*4,0);
            if (fg) {
                Require(generator.Draw(input.get(),frame,{},{},[&](ID3D11Texture2D* generated,uint64_t) {
                    const auto hash=ReadOutput(resources,generated); changed|=priorHash && priorHash!=hash; priorHash=hash; ++published; return true;
                }),"FG-Draw");
            } else {
                Require(upscaler.Draw({.input=input.get(),.output=output.get(),.frameId=frame,.inputRevision=frame,.frameGuidance=zero,.zeroFrameGuidance=zero}),"SR-Draw");
                const auto hash=ReadOutput(resources,output.get()); changed|=priorHash && priorHash!=hash; priorHash=hash;
            }
            std::cout << "evaluateReturned=true frame=" << frame << std::endl;
        }
        if (fg) { Require(generator.Drain(),"FG-drain"); Check(core.Device()->GetDeviceRemovedReason(),"D3D12-device-health"); Require(published>0,"FG-generated-output-published"); }
        Require(changed,"output-tracks-changing-input");
        Logger::Get().Flush();
        std::cout << "PASS kind=" << argv[1] << " requestedPreset=" << preset << " generatedReadbacks=" << published << " effectiveModel=unknown" << std::endl;
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << std::endl; return 1; }
    catch (const winrt::hresult_error& error) { std::cerr << "WinRT HRESULT=" << std::hex << uint32_t(error.code()) << std::endl; return 1; }
}
