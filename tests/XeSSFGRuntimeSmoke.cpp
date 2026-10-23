#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <xell/xell_d3d12.h>
#include <xess_fg/xefg_swapchain_d3d12.h>
#include <cassert>
#include <iostream>
#include <filesystem>
#include <chrono>
#include "../src/Magpie.Core/XeSSFGTiming.h"
#include "../src/Magpie.Core/XeSSFGCompatibility.h"
#include "../src/Magpie.Core/PresentationFrameRate.h"
#pragma comment(lib,"d3d12.lib")
#pragma comment(lib,"dxgi.lib")
#pragma comment(lib,"user32.lib")
using Microsoft::WRL::ComPtr;
using namespace Magpie::XeSSFGCompatibility;
#define API(module, name) auto name = reinterpret_cast<decltype(&::name)>(GetProcAddress(module, #name)); assert(name)
int wmain(int argc,wchar_t** argv) {
    assert(argc==2);
    const std::filesystem::path runtime=argv[1];
    HMODULE fg=LoadLibraryW((runtime/L"libxess_fg.dll").c_str());
    HMODULE ll=LoadLibraryW((runtime/L"libxell.dll").c_str());
    assert(fg && ll);
    API(fg,xefgSwapChainD3D12CreateContext);
    API(fg,xefgSwapChainSetLatencyReduction);
    API(fg,xefgSwapChainGetProperties);
    API(fg,xefgSwapChainD3D12InitFromSwapChainDesc);
    API(fg,xefgSwapChainD3D12GetSwapChainPtr);
    API(fg,xefgSwapChainSetNumInterpolatedFrames);
    API(fg,xefgSwapChainSetEnabled);
    API(fg,xefgSwapChainDestroy);
    API(fg,xefgSwapChainD3D12TagFrameResource);
    API(fg,xefgSwapChainTagFrameConstants);
    API(fg,xefgSwapChainSetPresentId);
    API(fg,xefgSwapChainGetLastPresentStatus);
    API(ll,xellD3D12CreateContext);
    API(ll,xellSetSleepMode);
    API(ll,xellDestroyContext);
    API(ll,xellSleep);
    API(ll,xellAddMarkerData);
    ComPtr<IDXGIFactory6> factory;
    assert(SUCCEEDED(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory))));
    ComPtr<IDXGIAdapter1> adapter;
    DXGI_ADAPTER_DESC1 desc{};
    for(UINT i=0; factory->EnumAdapterByGpuPreference(i,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_PPV_ARGS(&adapter))==S_OK; ++i) {
        adapter->GetDesc1(&desc);
        if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && desc.VendorId==0x10DE) break;
        adapter.Reset();
    }
    assert(adapter);
    std::wcout<<L"GPU: "<<desc.Description<<std::endl;
    ComPtr<ID3D12Device> device;
    assert(SUCCEEDED(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device))));
    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC q{};
    assert(SUCCEEDED(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue))));
    WNDCLASSW wc{}; wc.lpfnWndProc=DefWindowProcW; wc.hInstance=GetModuleHandleW(nullptr); wc.lpszClassName=L"MagpieXeSSSmoke";
    assert(RegisterClassW(&wc));
    HWND window=CreateWindowExW(0,wc.lpszClassName,L"XeSSFG runtime initialization test",WS_OVERLAPPEDWINDOW,0,0,640,360,nullptr,nullptr,wc.hInstance,nullptr);
    assert(window);
    ShowWindow(window,SW_SHOWNOACTIVATE);
    for(const auto [multiplier, pacing] : {std::pair{2u,true}, {3u,false}, {4u,false},
        {3u,true}, {4u,true}, {2u,true}, {4u,true}, {2u,true}}) {
        Lease lease;
        assert(lease.Acquire(multiplier>2,multiplier,reinterpret_cast<const void*>(xefgSwapChainD3D12CreateContext)));
        assert(lease.ObservesOutput());
        assert(lease.Patched() == (multiplier > 2));
        Pacing::outputProfiler.SetActive(true);
        Magpie::PresentationFrameRate outputRate;
        outputRate.StartProfile();
        size_t observedIntervals = 0;
        auto activeSince=std::chrono::steady_clock::now();
        // Diagnostic control only, before any SDK context/callback exists.
        // Production always uses the complete compatibility + pacing path.
        if(multiplier>2) Pacing::g_enabled=pacing;
        std::cout<<"mode="<<(multiplier==2?"native":pacing?"compatibility+pacing":"unlock-only-control")<<std::endl;
        xell_context_handle_t xell=nullptr;
        assert(xellD3D12CreateContext(device.Get(),&xell)==XELL_RESULT_SUCCESS);
        xell_sleep_params_t sleep{}; sleep.bLowLatencyMode=1;
        assert(xellSetSleepMode(xell,&sleep)==XELL_RESULT_SUCCESS);
        xefg_swapchain_handle_t context=nullptr;
        auto result=xefgSwapChainD3D12CreateContext(device.Get(),&context);
        std::cout<<multiplier<<"x CreateContext="<<int(result)<<std::endl;
        assert(result>=0 && context);
        assert(xefgSwapChainSetLatencyReduction(context,xell)>=0);
        xefg_swapchain_properties_t props{};
        assert(xefgSwapChainGetProperties(context,&props)>=0);
        std::cout<<"maxInterpolations="<<props.maxSupportedInterpolations<<std::endl;
        assert(props.maxSupportedInterpolations>=multiplier-1);
        if(multiplier==2) assert(props.maxSupportedInterpolations==1); // patch restored on native path
        DXGI_SWAP_CHAIN_DESC1 chain{};
        chain.Width=640;chain.Height=360;chain.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        chain.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;chain.BufferCount=3;
        chain.SampleDesc.Count=1;chain.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
        chain.Flags=DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        xefg_swapchain_d3d12_init_params_t init{};
        init.maxInterpolatedFrames=multiplier-1; init.uiMode=XEFG_SWAPCHAIN_UI_MODE_NONE;
        result=xefgSwapChainD3D12InitFromSwapChainDesc(context,window,&chain,nullptr,queue.Get(),factory.Get(),&init);
        std::cout<<"InitSwapChain="<<int(result)<<std::endl;
        assert(result>=0);
        ComPtr<IDXGISwapChain4> swap;
        assert(xefgSwapChainD3D12GetSwapChainPtr(context,IID_PPV_ARGS(&swap))>=0);
        assert(xefgSwapChainSetNumInterpolatedFrames(context,multiplier-1)>=0);
        assert(xefgSwapChainSetEnabled(context,true)>=0);
        ComPtr<ID3D12Fence> fence;
        assert(SUCCEEDED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence))));
        HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr); assert(event);
        uint64_t fenceValue=0;
        const auto wait=[&] {
            assert(SUCCEEDED(queue->Signal(fence.Get(),++fenceValue)));
            assert(SUCCEEDED(fence->SetEventOnCompletion(fenceValue,event)));
            assert(WaitForSingleObject(event,10000)==WAIT_OBJECT_0);
        };
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> commands;
        assert(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator))));
        assert(SUCCEEDED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&commands))));
        D3D12_HEAP_PROPERTIES gpuHeap{};gpuHeap.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_HEAP_PROPERTIES uploadHeap{};uploadHeap.Type=D3D12_HEAP_TYPE_UPLOAD;
        ComPtr<ID3D12Resource> motion,depth;
        std::array<ComPtr<ID3D12Resource>,2> uploads;
        for(int i=0;i<2;++i) {
            D3D12_RESOURCE_DESC texture{};
            texture.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;texture.Width=640;texture.Height=360;
            texture.DepthOrArraySize=1;texture.MipLevels=1;texture.SampleDesc.Count=1;
            texture.Format=i ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R16G16_FLOAT;
            auto& resource=i ? depth : motion;
            assert(SUCCEEDED(device->CreateCommittedResource(&gpuHeap,D3D12_HEAP_FLAG_NONE,&texture,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&resource))));
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
            UINT64 total=0;device->GetCopyableFootprints(&texture,0,1,0,&footprint,nullptr,nullptr,&total);
            D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;buffer.Width=total;
            buffer.Height=1;buffer.DepthOrArraySize=1;buffer.MipLevels=1;buffer.SampleDesc.Count=1;buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            assert(SUCCEEDED(device->CreateCommittedResource(&uploadHeap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&uploads[i]))));
            void* data=nullptr;assert(SUCCEEDED(uploads[i]->Map(0,nullptr,&data)));
            auto* words=static_cast<uint32_t*>(data);
            std::fill(words,words+total/4,i?0x3F800000u:0u);uploads[i]->Unmap(0,nullptr);
            D3D12_TEXTURE_COPY_LOCATION to{};to.pResource=resource.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            D3D12_TEXTURE_COPY_LOCATION from{};from.pResource=uploads[i].Get();from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;from.PlacedFootprint=footprint;
            commands->CopyTextureRegion(&to,0,0,0,&from,nullptr);
            D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource=resource.Get();barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_DEST;
            barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            commands->ResourceBarrier(1,&barrier);
        }
        assert(SUCCEEDED(commands->Close()));
        ID3D12CommandList* lists[]{commands.Get()};queue->ExecuteCommandLists(1,lists);wait();
        ComPtr<ID3D12DescriptorHeap> rtvs;
        D3D12_DESCRIPTOR_HEAP_DESC heap{};heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;heap.NumDescriptors=3;
        assert(SUCCEEDED(device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&rtvs))));
        std::array<ComPtr<ID3D12Resource>,3> backBuffers;
        const UINT stride=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        for(UINT i=0;i<3;++i) {
            assert(SUCCEEDED(swap->GetBuffer(i,IID_PPV_ARGS(&backBuffers[i]))));
            auto rtv=rtvs->GetCPUDescriptorHandleForHeapStart();rtv.ptr+=stride*i;
            device->CreateRenderTargetView(backBuffers[i].Get(),nullptr,rtv);
        }
        uint64_t successfulFrames=0,fullBursts=0,measuredSubmissions=0,timingResets=0;
        Magpie::XeSSFGTiming timing;
        double previousExtraWaitMs=0;
        for(uint32_t frame=1;frame<=60;++frame) {
            if(frame==21)Pacing::outputProfiler.SetActive(false);
            if(frame==26){activeSince=std::chrono::steady_clock::now();Pacing::outputProfiler.SetActive(true);}
            MSG msg{};while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) {TranslateMessage(&msg);DispatchMessageW(&msg);}
            xellSleep(xell,frame);
            xellAddMarkerData(xell,frame,XELL_INPUT_SAMPLE);
            xellAddMarkerData(xell,frame,XELL_SIMULATION_START);
            xellAddMarkerData(xell,frame,XELL_SIMULATION_END);
            xellAddMarkerData(xell,frame,XELL_RENDERSUBMIT_START);
            assert(SUCCEEDED(allocator->Reset()));
            assert(SUCCEEDED(commands->Reset(allocator.Get(),nullptr)));
            const auto index=swap->GetCurrentBackBufferIndex();
            D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource=backBuffers[index].Get();
            barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_PRESENT;barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_RENDER_TARGET;
            commands->ResourceBarrier(1,&barrier);
            auto rtv=rtvs->GetCPUDescriptorHandleForHeapStart();rtv.ptr+=stride*index;
            const float background[4]{0.02f,0.04f,0.08f,1};
            const float foreground[4]{0.2f,0.6f,0.9f,1};
            commands->ClearRenderTargetView(rtv,background,0,nullptr);
            D3D12_RECT rect{LONG(frame*10),100,LONG(frame*10+80),240};
            commands->ClearRenderTargetView(rtv,foreground,1,&rect);
            std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);
            commands->ResourceBarrier(1,&barrier);
            assert(SUCCEEDED(commands->Close()));queue->ExecuteCommandLists(1,lists);wait();
            xefg_swapchain_d3d12_resource_data_t tag{};
            tag.type=XEFG_SWAPCHAIN_RES_MOTION_VECTOR;tag.validity=XEFG_SWAPCHAIN_RV_UNTIL_NEXT_PRESENT;
            tag.resourceSize={640,360};tag.pResource=motion.Get();tag.incomingState=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            assert(xefgSwapChainD3D12TagFrameResource(context,nullptr,frame,&tag)>=0);
            tag.type=XEFG_SWAPCHAIN_RES_DEPTH;tag.pResource=depth.Get();
            assert(xefgSwapChainD3D12TagFrameResource(context,nullptr,frame,&tag)>=0);
            xefg_swapchain_frame_constant_data_t constants{};
            for(int i=0;i<16;i+=5) {constants.viewMatrix[i]=1;constants.projectionMatrix[i]=1;}
            constants.motionVectorScaleX=constants.motionVectorScaleY=1;
            constants.frameRenderTime=1000.0f/60;constants.resetHistory=frame==1;
            if(multiplier>2) {
                // Exercise the production timing path with the real metadata
                // contract: session stays constant, frame ID/timestamp advance.
                // Also simulate a capture restart and a resource generation change.
                const Magpie::XeSSFGSourceSample source{frame,frame>20?2u:1u,frame>40?2u:1u,
                    1000000 + static_cast<int64_t>(frame)*166667};
                const auto estimate=timing.Submit(source,
                    std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(),
                    previousExtraWaitMs);
                timingResets+=estimate.reset;
                constants.frameRenderTime=static_cast<float>(estimate.fedMs);
                constants.resetHistory=estimate.reset;
                if(estimate.reset) lease.Reset();
                Pacing::sourcePeriodNs.store(static_cast<int64_t>(estimate.fedMs*1000000));
            }
            assert(xefgSwapChainTagFrameConstants(context,frame,&constants)>=0);
            assert(xefgSwapChainSetPresentId(context,frame)>=0);
            xellAddMarkerData(xell,frame,XELL_RENDERSUBMIT_END);
            xellAddMarkerData(xell,frame,XELL_PRESENT_START);
            const auto extraBefore=Pacing::extraWaitNs.load();
            assert(SUCCEEDED(swap->Present(0,0)));
            previousExtraWaitMs=static_cast<double>(Pacing::extraWaitNs.load()-extraBefore)/1000000;
            xellAddMarkerData(xell,frame,XELL_PRESENT_END);
            wait();
            xefg_swapchain_present_status_t status{};
            result=xefgSwapChainGetLastPresentStatus(context,&status);
            if(result<0 || status.frameGenResult<0) std::cout<<"frame="<<frame<<" status="<<int(result)<<" FG="<<int(status.frameGenResult)<<std::endl;
            assert(result>=0 && status.frameGenResult>=0);
            outputRate.Record(status.framesPresented,1);
            auto intervals=Pacing::outputProfiler.Take();
            if(frame>=21&&frame<=25)assert(intervals.count==0);
            for(size_t i=0;i<intervals.count;++i)assert(intervals.samples[i].begin>=activeSince);
            observedIntervals+=intervals.count;
            outputRate.RecordIntervals(intervals);
            if((frame-1)%20>=5) {
                ++measuredSubmissions;successfulFrames+=status.framesPresented;fullBursts+=status.framesPresented==multiplier;
            }
        }
        // SDK output runs asynchronously. Observe actual callback timestamps,
        // including the final queued burst, rather than deriving an interval.
        const auto observationEnd = std::chrono::steady_clock::now()+std::chrono::milliseconds(100);
        while (std::chrono::steady_clock::now()<observationEnd) {
            auto intervals=Pacing::outputProfiler.Take();
            observedIntervals+=intervals.count;
            outputRate.RecordIntervals(intervals);
            Sleep(1);
        }
        const auto outputPlot=outputRate.Profile();
        size_t plottedIntervals=0;
        for(const auto& point:outputPlot.plot.points)plottedIntervals+=point.interval.count;
        std::cout<<"native output observation: calls="<<Pacing::outputCalls.load()<<" queuedIntervals="<<observedIntervals
            <<" plottedIntervals="<<plottedIntervals<<" lastResult="<<Pacing::outputRecords[(Pacing::outputPosition+511)%512].result<<std::endl;
        assert(observedIntervals>=successfulFrames && plottedIntervals>0 && outputPlot.individualTimestamps);
        Pacing::outputProfiler.SetActive(false);
        std::cout<<"post-warmup SDK frames="<<successfulFrames<<"/"<<measuredSubmissions
            <<" fullBursts="<<fullBursts<<" timingResets="<<timingResets<<std::endl;
        assert(fullBursts==measuredSubmissions && successfulFrames==measuredSubmissions*multiplier);
        if(multiplier>2) assert(timingResets==3);
        if(multiplier>2) {
            const auto stats=Pacing::ReadOutputStats();
            std::cout<<"providerCalls="<<Pacing::outputCalls.load()<<" schedulerCalls="<<Pacing::schedulerCalls.load()
                <<" gapMs="<<stats.p50<<","<<stats.p95<<","<<stats.p99<<std::endl;
            assert(Pacing::outputCalls>0);
        }
        backBuffers={};
        CloseHandle(event);
        assert(xefgSwapChainSetEnabled(context,false)>=0);
        HANDLE latency=swap->GetFrameLatencyWaitableObject();
        swap.Reset();
        if(latency) CloseHandle(latency);
        result=xefgSwapChainDestroy(context);
        std::cout<<"Destroy="<<int(result)<<std::endl;
        assert(result>=0);
        assert(xellDestroyContext(xell)==XELL_RESULT_SUCCESS);
        assert(lease.Release(true));
    }
    DestroyWindow(window);
    std::cout<<"Native 2x / compatibility 3x / 4x full bursts, production timing, history recovery and same-process teardown passed; no display or image-quality validation."<<std::endl;
}
