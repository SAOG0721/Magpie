// Isolated benchmark: expose production resource ownership only in this TU.
#include "pch.h"
#include "ScalingOptions.h"
#include <parallel_hashmap/phmap.h>
#define private public
#include "DeviceResources.h"
#include "DLSSNRFilter.h"
#undef private
#include "DLSSNRCostBenchmarkGpu.h"
#include "DLSSNRBenchmarkPacing.h"
#include "DLSSNRCostBenchmarkFilter.generated.cpp"
#include "FrameGuidanceD3D12Interop.cpp"
#include <iostream>
#include <thread>
#include <filesystem>
#include <set>
#include <d3dcompiler.h>

using namespace Magpie;
using namespace CostBenchmark;
void Require(bool value, const char* stage = "require") {
    if (!value) { std::cerr << "Benchmark failed: " << stage << '\n'; Logger::Get().Flush(); throw std::runtime_error(stage); }
}
void Check(HRESULT hr) { winrt::check_hresult(hr); }

struct Options {
    uint32_t width = 2560, height = 1440, percent = 100;
    int model = 1, passes = 1, mode = 0, samples = 120, warmup = 60, dumpTail = 0;
    double fps = 0;
    bool coherent = false;
    std::string scene = "static", prefix = "DLSSNRCost", preset = "default";
};

struct Fixture {
    DeviceResources& resources;
    uint32_t width, height;
    winrt::com_ptr<ID3D11Texture2D> input, output;
    std::array<winrt::com_ptr<ID3D11Texture2D>,3> guideTextures;
    FrameGuidanceView zero, guidance;
    std::vector<uint32_t> pixels;
    winrt::com_ptr<ID3D11Texture2D> knownMotion, knownConfidence;
    winrt::com_ptr<ID3D11UnorderedAccessView> knownMotionUav, inputUav;
    winrt::com_ptr<ID3D11ComputeShader> patternShader;
    winrt::com_ptr<ID3D11Buffer> patternConstants;
    Fixture(DeviceResources& owner, uint32_t w, uint32_t h, bool coherent) : resources(owner), width(w), height(h) {
        input = DirectXHelper::CreateTexture2D(owner.GetD3DDevice(), DXGI_FORMAT_R8G8B8A8_UNORM,w,h,D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS);
        output = DirectXHelper::CreateTexture2D(owner.GetD3DDevice(), DXGI_FORMAT_R8G8B8A8_UNORM,w,h,D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS);
        const DXGI_FORMAT formats[]{DXGI_FORMAT_R16G16_FLOAT,DXGI_FORMAT_R32_FLOAT,DXGI_FORMAT_R8_UNORM};
        FrameGuidanceResource* channels[]{&zero.motion,&zero.depth,&zero.confidence};
        for (size_t i = 0; i < 3; ++i) {
            guideTextures[i] = DirectXHelper::CreateSharedTexture2D(owner.GetD3DDevice(),formats[i],w,h,D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS,"cost fixture");
            winrt::com_ptr<ID3D11UnorderedAccessView> uav;
            Check(owner.GetD3DDevice()->CreateUnorderedAccessView(guideTextures[i].get(),nullptr,uav.put()));
            const float value[4]{};
            owner.GetD3DDC()->ClearUnorderedAccessViewFloat(uav.get(),value);
            *channels[i] = {guideTextures[i].get(),formats[i],{.resourceGeneration=1,.sourceExtent={w,h},.validRegion={0,0,w,h},.valid=true,.isZero=true}};
        }
        guidance = zero;
        if (coherent) {
            knownMotion=DirectXHelper::CreateSharedTexture2D(owner.GetD3DDevice(),DXGI_FORMAT_R16G16_FLOAT,w,h,D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS,"known motion");
            knownConfidence=DirectXHelper::CreateSharedTexture2D(owner.GetD3DDevice(),DXGI_FORMAT_R8_UNORM,w,h,D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS,"known confidence");
            Check(owner.GetD3DDevice()->CreateUnorderedAccessView(knownMotion.get(),nullptr,knownMotionUav.put()));
            winrt::com_ptr<ID3D11UnorderedAccessView> confidenceUav;
            Check(owner.GetD3DDevice()->CreateUnorderedAccessView(knownConfidence.get(),nullptr,confidenceUav.put()));
            const float zeroValue[4]{}, oneValue[4]{1,1,1,1};
            owner.GetD3DDC()->ClearUnorderedAccessViewFloat(knownMotionUav.get(),zeroValue);
            owner.GetD3DDC()->ClearUnorderedAccessViewFloat(confidenceUav.get(),oneValue);
            guidance.motion.texture=knownMotion.get(); guidance.confidence.texture=knownConfidence.get();
            guidance.motion.metadata.isZero = false; guidance.confidence.metadata.isZero = false;
        }
        Check(owner.GetD3DDevice()->CreateUnorderedAccessView(input.get(),nullptr,inputUav.put()));
        const char* patternSource=R"(
cbuffer Params : register(b0) { uint width,height,offset,pad; };
RWTexture2D<float4> output : register(u0);
[numthreads(8,8,1)] void main(uint3 tid:SV_DispatchThreadID) {
    if (tid.x>=width || tid.y>=height) return;
    uint x=(tid.x+offset)%width,y=tid.y;
    uint checker=((x/9+y/9)&1)*45;
    uint r=35+((x*155/width+checker)%185);
    uint g=25+((y*150/height+checker)%190);
    uint b=20+(((x/3+y/5)%105)+checker);
    output[tid.xy]=float4(r,g,b,255)/255.;
})";
        winrt::com_ptr<ID3DBlob> blob,errors;
        Check(D3DCompile(patternSource,std::strlen(patternSource),"cost fixture",nullptr,nullptr,"main","cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,blob.put(),errors.put()));
        Check(owner.GetD3DDevice()->CreateComputeShader(blob->GetBufferPointer(),blob->GetBufferSize(),nullptr,patternShader.put()));
        D3D11_BUFFER_DESC cb{}; cb.ByteWidth=16; cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        Check(owner.GetD3DDevice()->CreateBuffer(&cb,nullptr,patternConstants.put()));
        pixels.resize(size_t(w)*h);
    }
    void UpdatePixels(uint64_t frame, std::string_view scene) {
        // Deterministic multi-scale texture. Static uses fresh capture IDs but
        // identical pixels; translation uses a known horizontal 2px/frame shift.
        const uint32_t offset = scene == "translate" ? uint32_t(frame*2) : 0;
        auto* dc=resources.GetD3DDC();
        const uint32_t values[4]{width,height,offset,0};
        dc->UpdateSubresource(patternConstants.get(),0,nullptr,values,0,0);
        auto* uav=inputUav.get(); auto* cb=patternConstants.get();
        dc->CSSetShader(patternShader.get(),nullptr,0); dc->CSSetConstantBuffers(0,1,&cb);
        dc->CSSetUnorderedAccessViews(0,1,&uav,nullptr); dc->Dispatch((width+7)/8,(height+7)/8,1);
        ID3D11UnorderedAccessView* emptyUav=nullptr;
        dc->CSSetUnorderedAccessViews(0,1,&emptyUav,nullptr); dc->CSSetShader(nullptr,nullptr,0);
        if (knownMotionUav) {
            const float motion[4]{scene=="translate"?2.f:0.f,0,0,0};
            dc->ClearUnorderedAccessViewFloat(knownMotionUav.get(),motion);
        }
    }
    NativeEffectDrawContext Capture(uint64_t frame, double fps) {
        for (auto* view : {&zero,&guidance}) for (auto* channel : {&view->motion,&view->depth,&view->confidence}) {
            channel->metadata.frameId=frame;
            channel->metadata.timestamp100ns=int64_t(double(frame)*10000000./(fps ? fps : 60.));
        }
        return {.input=input.get(),.output=output.get(),.frameId=frame,.previousCaptureFrameId=frame-1,
            .inputRevision=frame,.targetContentFrameRate=fps,.frameGuidance=guidance,.zeroFrameGuidance=zero};
    }
};

struct OutputFence {
    DeviceResources& resources;
    winrt::com_ptr<ID3D11Fence> fence;
    wil::unique_event_nothrow event;
    uint64_t value = 0;
    OutputFence(DeviceResources& owner) : resources(owner) {
        Check(owner.GetD3DDevice()->CreateFence(0,D3D11_FENCE_FLAG_NONE,IID_PPV_ARGS(fence.put()))); Check(event.create());
    }
    uint64_t Signal() { Check(resources.GetD3DDC()->Signal(fence.get(),++value)); resources.GetD3DDC()->Flush(); return value; }
    void Wait(uint64_t target) {
        Check(fence->SetEventOnCompletion(target,event.get())); Require(WaitForSingleObject(event.get(),30000)==WAIT_OBJECT_0,"output fence timeout");
    }
};

void ReadOutput(DeviceResources& resources, ID3D11Texture2D* texture, const std::string& path) {
    D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
    desc.Usage=D3D11_USAGE_STAGING; desc.BindFlags=0; desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ; desc.MiscFlags=0;
    winrt::com_ptr<ID3D11Texture2D> stage; Check(resources.GetD3DDevice()->CreateTexture2D(&desc,nullptr,stage.put()));
    resources.GetD3DDC()->CopyResource(stage.get(),texture);
    D3D11_MAPPED_SUBRESOURCE map{}; Check(resources.GetD3DDC()->Map(stage.get(),0,D3D11_MAP_READ,0,&map));
    std::ofstream file(path,std::ios::binary);
    for (uint32_t y=0;y<desc.Height;++y) file.write(static_cast<const char*>(map.pData)+size_t(y)*map.RowPitch,desc.Width*4);
    resources.GetD3DDC()->Unmap(stage.get(),0);
}

struct NgxCollector {
    std::map<std::pair<const DLSSNRFilter::Impl*,uint32_t>,uint64_t> observed;
    GpuTrace& trace;
    void CollectImpl(DLSSNRFilter::Impl& impl, const char* branch) {
        if (!impl.timestampFrequency || !impl.timestampReadback) return;
        for (auto& slot : impl.commandSlots) {
            if (!slot.completionValue || slot.completionValue == observed[{&impl,slot.timestampQuery}] ||
                impl.fence12->GetCompletedValue() < slot.completionValue) continue;
            const auto offset = size_t(slot.timestampQuery)*sizeof(uint64_t);
            const D3D12_RANGE range{offset,offset+sizeof(uint64_t)*DLSSNRFilter::Impl::TIMESTAMP_STRIDE};
            void* data=nullptr; Check(impl.timestampReadback->Map(0,&range,&data));
            const auto* stamp = reinterpret_cast<const uint64_t*>(static_cast<const uint8_t*>(data)+offset);
            auto add = [&](std::string name,size_t pair) {
                if (stamp[pair+1]>=stamp[pair]) trace.samples.push_back({slot.timestampFrameId,std::string(branch)+name,"D3D12_compute",
                    impl.width,impl.height,double(stamp[pair+1]-stamp[pair])*1000./double(impl.timestampFrequency),stamp[pair],stamp[pair+1],impl.timestampFrequency});
            };
            add("_ngx_chain",0);
            for (uint32_t pass=slot.timestampFirstPass;pass<slot.timestampPassCount;++pass) add("_ngx_pass_"+std::to_string(pass+1),2+pass*2);
            const D3D12_RANGE written{0,0}; impl.timestampReadback->Unmap(0,&written);
            observed[{&impl,slot.timestampQuery}]=slot.completionValue;
        }
    }
    void Collect(DLSSNRFilter& filter) {
        // A CPU worker can reuse a command slot before its new submission is
        // acquired by the owner. Never interpret that slot's previous ticket
        // as authorization to read the in-progress query storage.
        if (filter.InferenceSubmissionPending()) return;
        CollectImpl(*filter._impl,"full");
    }
};

void WriteResults(const Options& options, GpuTrace& trace, DLSSNRFilter& filter, double initializeMs, double elapsedMs) {
    std::ofstream csv(options.prefix+".samples.csv");
    csv << "frame,stage,domain,width,height,milliseconds,startTick,endTick,frequency\n" << std::setprecision(12);
    std::map<std::string,std::vector<double>> groups;
    for (const auto& sample : trace.samples) if (sample.frame>uint64_t(options.warmup) && sample.frame<=uint64_t(options.warmup+options.samples)) {
        csv << sample.frame << ',' << sample.stage << ',' << sample.domain << ',' << sample.width << ',' << sample.height << ',' << sample.milliseconds
            << ',' << sample.startTick << ',' << sample.endTick << ',' << sample.frequency << '\n';
        groups[sample.stage+"|"+sample.domain+"|"+std::to_string(sample.width)+"x"+std::to_string(sample.height)].push_back(sample.milliseconds);
    }
    std::ofstream summary(options.prefix+".summary.json");
    summary << std::setprecision(12) << "{\n  \"model\":" << options.model << ",\"modelRequested\":\"" << (options.model==1?"HT":"CC Small FP8")
        << "\",\"modelResolved\":\"unknown_private_runtime\",\"preset\":\"" << options.preset << "\",\"percent\":" << options.percent
        << ",\"passes\":" << options.passes << ",\"mode\":" << options.mode
        << ",\"width\":" << options.width << ",\"height\":" << options.height << ",\"samples\":" << options.samples << ",\"warmup\":" << options.warmup
        << ",\"fps\":" << options.fps << ",\"scene\":\"" << options.scene << "\",\"coherentGuidance\":" << (options.coherent?"true":"false")
        << ",\"dumpTail\":" << options.dumpTail << ",\"measurementClass\":\"" << (options.dumpTail?"quality_readback_perturbed":"performance") << "\""
        << ",\"debugLayer\":false,\"initializeMs\":" << initializeMs << ",\"measuredWallMs\":" << elapsedMs
        << ",\"timestampDroppedFrames\":" << trace.droppedFrames << ",\"timestampDroppedEvents\":" << trace.droppedEvents
        << ",\"timestampDisjointFrames\":" << trace.disjointFrames << ",\n  \"totalsIncludingWarmup\":{\"fullEvaluations\":" << filter._impl->evaluateCount;
    if (filter._performance) {
        const auto& p=*filter._performance;
        summary << ",\"fullSubmissions\":" << p.fullSubmissions
            << ",\"fullCompleted\":" << p.fullCompleted << ",\"adoptedResults\":" << p.adoptedResults
            << ",\"lateDiscards\":" << p.lateDiscards << ",\"unusedExpired\":" << p.unusedExpired << ",\"pipelineFallbacks\":" << p.pipelineFallbacks;
    }
    summary << "},\n  \"stages\":{\n";
    bool first=true;
    for (auto& [name,values] : groups) {
        std::sort(values.begin(),values.end());
        auto percentile=[&](double q) { return values[std::min(values.size()-1,size_t(std::ceil(values.size()*q)-1))]; };
        if (!first) summary << ",\n"; first=false;
        summary << "    \"" << name << "\":{\"n\":" << values.size() << ",\"mean\":" << std::accumulate(values.begin(),values.end(),0.)/values.size()
            << ",\"p50\":" << percentile(.5) << ",\"p95\":" << percentile(.95) << ",\"p99\":" << percentile(.99) << ",\"max\":" << values.back() << '}';
    }
    summary << "\n  }\n}\n";
}

int main(int argc,char** argv) {
    try {
        Options options;
        for (int i=1;i<argc;++i) {
            const std::string arg=argv[i];
            auto value=[&]() { Require(i+1<argc,"missing CLI value"); return std::string(argv[++i]); };
            if (arg=="--model") options.model=std::stoi(value());
            else if (arg=="--percent") options.percent=std::stoul(value());
            else if (arg=="--passes") options.passes=std::stoi(value());
            else if (arg=="--mode") options.mode=std::stoi(value());
            else if (arg=="--samples") options.samples=std::stoi(value());
            else if (arg=="--warmup") options.warmup=std::stoi(value());
            else if (arg=="--dump-tail") options.dumpTail=std::stoi(value());
            else if (arg=="--fps") options.fps=std::stod(value());
            else if (arg=="--scene") options.scene=value();
            else if (arg=="--prefix") options.prefix=value();
            else if (arg=="--preset") options.preset=value();
            else if (arg=="--width") options.width=std::stoul(value());
            else if (arg=="--height") options.height=std::stoul(value());
            else if (arg=="--coherent") options.coherent=true;
            else throw std::runtime_error("Unknown argument: "+arg);
        }
        Require(options.samples>0 && options.warmup>=0 && options.passes>=1 && options.passes<=3 && options.percent>=25 && options.percent<=100 &&
            options.model>=1 && options.model<=2 && options.mode>=0 && options.mode<=2 && options.fps>=0 &&
            options.dumpTail>=0 && options.dumpTail<=options.samples,"option range");
        Require(options.scene=="static" || options.scene=="translate","scene must be static or translate");
        Require(options.preset=="default" || options.preset=="user","preset must be default or user");
        winrt::init_apartment(); Logger::Get().Initialize(spdlog::level::warn,L"DLSSNRCostBenchmark.log",10000000,1);
        DeviceResources resources; Check(CreateDXGIFactory2(0,IID_PPV_ARGS(resources._dxgiFactory.put())));
        for (UINT i=0;;++i) {
            winrt::com_ptr<IDXGIAdapter1> adapter; Check(resources._dxgiFactory->EnumAdapters1(i,adapter.put()));
            DXGI_ADAPTER_DESC1 desc{}; Check(adapter->GetDesc1(&desc)); if (desc.VendorId!=0x10de) continue;
            resources._graphicsAdapter=adapter.as<IDXGIAdapter4>();
            winrt::com_ptr<ID3D11Device> device; winrt::com_ptr<ID3D11DeviceContext> dc;
            Check(D3D11CreateDevice(adapter.get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,0,nullptr,0,D3D11_SDK_VERSION,device.put(),nullptr,dc.put()));
            resources._d3dDevice=device.as<ID3D11Device5>(); resources._d3dDC=dc.as<ID3D11DeviceContext4>(); break;
        }
        Fixture fixture(resources,options.width,options.height,options.coherent);
        fixture.UpdatePixels(0,options.scene);
        NgxD3D12Core core; DLSSNRSettings settings;
        settings.modelPreset=options.model; settings.inputResolutionPercent=options.percent;
        settings.processingMode=static_cast<DLSSNRProcessingMode>(options.mode);
        if (options.preset=="user") {
            settings.style=2; settings.useAutoMask=true; settings.uiCorrection=true;
            settings.residualHueProtection=.5f; settings.residualChromaProtection=.5f;
            settings.residualDarkProtection=.5f; settings.residualHighlightProtection=.5f;
            settings.residualLocalCompression=.1f; settings.residualMidtoneStrength=1.1f;
        }
        if (options.coherent) { settings.residualTransfer=DLSSNRResidualTransfer::PerPixel; settings.motionRequest=MotionVectorRequest::Amd(AmdOpticalFlowMode::Quality); }
        std::vector<DLSSNRSettings> passes(options.passes,settings); DLSSNRFilter filter;
        const auto initialized=std::chrono::steady_clock::now();
        Require(filter.InitializeChain(resources,core,fixture.input.get(),fixture.output.get(),passes),"InitializeChain");
        Require(filter._impl->modelPreset==options.model,"model hint was not propagated");
        const double initializeMs=Milliseconds(initialized);
        GpuTrace profiler; profiler.Initialize(resources.GetD3DDevice(),resources.GetD3DDC()); trace=&profiler;
        NgxCollector collector{{},profiler}; OutputFence outputFence(resources);
        std::ofstream frameCsv(options.prefix+".frames.csv");
        frameCsv << "frame,submitCpuMs,outputCompletionObservedMs,drawStartIntervalMs,fullEvaluations,fullCompleted,adoptedResults,coverage,fullReadyMs,fullAdoptionMs\n" << std::setprecision(12);
        std::ofstream tailCsv;
        if (options.dumpTail) {
            tailCsv.open(options.prefix+".tail.csv");
            tailCsv << "frame,inputFile,outputFile,readySequence,fullEvaluations,adoptedResults,coverage\n";
        }
        DLSSNRBenchmarkPacing pacer;
        auto schedule=std::chrono::steady_clock::now(), previousStart=schedule, measuredStart=schedule;
        for (uint64_t frame=1;frame<=uint64_t(options.warmup+options.samples);++frame) {
            if (options.fps>0) {
                pacer.WaitUntil(schedule, [&] {
                    collector.Collect(filter); filter.ServiceAsync();
                    if (filter.HasPendingPostOutputWork()) filter.ServiceAfterOutput();
                });
                schedule += std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(1./options.fps));
            }
            if (frame==uint64_t(options.warmup+1)) measuredStart=std::chrono::steady_clock::now();
            if (options.scene=="translate") fixture.UpdatePixels(frame,options.scene);
            auto context=fixture.Capture(frame,options.fps);
            collector.Collect(filter); filter.ServiceAsync();
            profiler.Begin(frame);
            const auto started=std::chrono::steady_clock::now();
            const double interval=std::chrono::duration<double,std::milli>(started-previousStart).count(); previousStart=started;
            { Region span("draw_gpu_span_including_wait",resources.GetD3DDC(),options.width,options.height); Require(filter.Draw(context),"Draw"); }
            const double submitCpuMs=Milliseconds(started);
            // Publication boundary is a GPU fence, not a desktop Present.
            const auto published=outputFence.Signal();
            filter.ServiceAfterOutput();
            profiler.End(); resources.GetD3DDC()->Flush();
            // Isolation mode retires each frame. Fixed-FPS mode uses the same
            // fence only as an observation and never waits in the frame loop.
            double outputObserved=-1;
            if (options.fps==0) { outputFence.Wait(published); outputObserved=Milliseconds(started); }
            profiler.Poll(); collector.Collect(filter);
            const auto snapshot=filter.GetPerformanceSnapshot();
            if (frame>uint64_t(options.warmup)) {
                profiler.samples.push_back({frame,"draw_submit_cpu","CPU",options.width,options.height,submitCpuMs});
                if (outputObserved>=0) profiler.samples.push_back({frame,"output_observation_including_cpu_wait","CPU_GPU_wall",options.width,options.height,outputObserved});
                frameCsv << frame << ',' << submitCpuMs << ',' << outputObserved << ',' << interval << ',' << filter._impl->evaluateCount << ','
                    << snapshot.fullCompleted << ',' << snapshot.fullAdopted
                    << ',' << snapshot.enhancementCoverage << ',' << snapshot.fullReadyMs << ',' << snapshot.fullAdoptionMs << '\n';
            }
            Require(filter.IsHealthy(),"healthy");
            if (options.dumpTail && frame>uint64_t(options.warmup+options.samples-options.dumpTail)) {
                // Deliberately blocking quality capture. Its run is labelled
                // perturbed and cannot be used as performance evidence.
                const std::string name=options.prefix+".tail."+std::to_string(frame);
                ReadOutput(resources,fixture.input.get(),name+".input.rgba");
                ReadOutput(resources,fixture.output.get(),name+".output.rgba");
                tailCsv << frame << ',' << name << ".input.rgba," << name << ".output.rgba,"
                    << (filter._performance?filter._performance->readySequence:frame) << ','
                    << filter._impl->evaluateCount
                    << ',' << snapshot.fullAdopted << ',' << snapshot.enhancementCoverage << '\n';
            }
        }
        const double elapsedMs=Milliseconds(measuredStart);
        Require(filter.Drain(),"Drain");
        // Some drivers do not retire a final disjoint timestamp batch until
        // subsequent timestamp work is submitted. A zero-work sentinel plus
        // final GetData flushing happens strictly outside measurement.
        profiler.Begin(uint64_t(options.warmup+options.samples+1)); profiler.End();
        outputFence.Wait(outputFence.Signal());
        const auto queryDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        while (profiler.Pending() && std::chrono::steady_clock::now()<queryDeadline) {
            profiler.Poll(true); collector.Collect(filter); std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        Require(profiler.Pending()==0,"unretired GPU timestamp queries");
        trace=nullptr;
        WriteResults(options,profiler,filter,initializeMs,elapsedMs);
        ReadOutput(resources,fixture.input.get(),options.prefix+".input.rgba");
        ReadOutput(resources,fixture.output.get(),options.prefix+".output.rgba");
        std::cout << "Complete: " << options.prefix << " model=" << options.model << " percent=" << options.percent << " passes=" << options.passes << " mode=" << options.mode << '\n';
        Logger::Get().Flush(); return 0;
    } catch (const winrt::hresult_error& e) { std::cerr << "HRESULT " << std::hex << e.code().value << '\n'; return 2; }
      catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 2; }
}
