#include "pch.h"
#include "EffectsProfiler.h"
#include <cassert>
#include <cstdio>
#include <thread>

using namespace Magpie;
using namespace std::chrono;

static void Run(D3D_DRIVER_TYPE type) {
    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    winrt::check_hresult(D3D11CreateDevice(nullptr,type,nullptr,0,nullptr,0,D3D11_SDK_VERSION,device.put(),nullptr,context.put()));
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width=1280; desc.Height=720; desc.MipLevels=1; desc.ArraySize=1;
    desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count=1;
    desc.BindFlags=D3D11_BIND_RENDER_TARGET;
    winrt::com_ptr<ID3D11Texture2D> texture;
    winrt::com_ptr<ID3D11RenderTargetView> view;
    winrt::check_hresult(device->CreateTexture2D(&desc,nullptr,texture.put()));
    winrt::check_hresult(device->CreateRenderTargetView(texture.get(),nullptr,view.put()));
    EffectsProfiler profiler;
    profiler.Start(device.get(),2);
    assert(profiler.IsProfiling() && profiler.GetSnapshot().gpuAvailable);
    bool measured=false;
    for(int i=0;i<20;++i){
        profiler.OnFrame(device.get(),1,1, i%2==0);
        profiler.OnBeginEffects(context.get());
        float color[]{float(i)/20.0f,0.2f,0.3f,1};
        for(int draw=0;draw<10;++draw) context->ClearRenderTargetView(view.get(),color);
        profiler.OnEndPass(context.get());
        profiler.OnEndPass(context.get(),false);
        profiler.OnEndEffects(context.get());
        context->Flush(); // Normal work submission, never done by the profiler.
        for(int retry=0;retry<100;++retry){
            profiler.Poll(context.get());
            auto snapshot=profiler.GetSnapshot();
            if(!snapshot.passTimings.empty()){
                assert(snapshot.passTimings.size()==2 && snapshot.passTimings[1]==0);
                assert(snapshot.passTimings[0]>=0);
                measured=true;
                break;
            }
            std::this_thread::sleep_for(1ms);
        }
    }
    assert(measured);
    std::this_thread::sleep_for(110ms); profiler.Poll(context.get());
    auto snapshot=profiler.GetSnapshot();
    auto plot=snapshot.history.Read(ProfilerHistory::Clock::now());
    uint32_t samples=0;
    for(const auto& point:plot.points) samples+=point.effects.count;
    assert(samples>1); // Last-value delivery must not lose completed history samples.
    uint32_t intervals=0;
    for(const auto& point:plot.points) intervals+=point.interval.count;
    assert(intervals==9); // Ten accepted captures; ten old-frame rerenders add no input events.
    assert(samples>10); // Rerenders still retain their actual effect timing samples.
    const auto generation=snapshot.generation;
    profiler.SetPassCount(device.get(),2,true);
    snapshot=profiler.GetSnapshot();
    assert(snapshot.generation>generation && snapshot.partial && snapshot.passTimings.empty());
    for(const auto& point:snapshot.history.Read(ProfilerHistory::Clock::now()).points) assert(!point.effects.count);
    profiler.OnFrame(device.get(),1,1, true);
    profiler.SetSuspended(device.get(),true);
    assert(profiler.GetSnapshot().history.Suspended());
    profiler.SetSuspended(device.get(),false);
    assert(!profiler.GetSnapshot().history.Suspended());
    profiler.OnFrame(device.get(),1,1, true);
    auto before=profiler.GetSnapshot().generation;
    profiler.OnFrame(device.get(),2,1, true);
    assert(profiler.GetSnapshot().generation>before);
    // A query failure disables only GPU metrics; processing history keeps working.
    profiler.OnBeginEffects(context.get());
    profiler.OnEndEffects(context.get()); // Deliberately missing both passes.
    assert(profiler.IsProfiling() && !profiler.GetSnapshot().gpuAvailable);
    profiler.OnFrame(device.get(),2,1, true);
    std::this_thread::sleep_for(110ms); profiler.Poll(context.get());
    snapshot=profiler.GetSnapshot();
    assert(snapshot.history.Active() && !snapshot.gpuAvailable);
    profiler.Stop();
    assert(!profiler.IsProfiling() && !profiler.GetSnapshot().history.Active());
    profiler.Start(device.get(),0);
    assert(profiler.IsProfiling() && !profiler.GetSnapshot().gpuAvailable);
    profiler.Stop();
    std::printf("EffectsProfiler %s: real queries, zero/reuse, history aggregation, same-size chain reset, pause, sequence and graceful query failure passed (%u samples).\n",type==D3D_DRIVER_TYPE_HARDWARE?"hardware":"WARP",samples);
}

int main() try {
    Run(D3D_DRIVER_TYPE_WARP);
    Run(D3D_DRIVER_TYPE_HARDWARE);
} catch(const winrt::hresult_error& error) {
    std::fprintf(stderr,"GPU profiler test HRESULT: %08x\n",unsigned(error.code().value));
    return 1;
}
