// Calls the current production wrapper, flow service, Filter and Sparse engine.
// Scene generation never supplies validity masks, motion, or forced NR resets.
#define main SparseProductionUnusedCostMain
#include "DLSSNRCostBenchmark.cpp"
#undef main
#include "FrameGuidanceService.h"
#define private public
#include "DLSSNRMultiPass.h"
#undef private
#include "DLSSNRRecoveryScene.h"
#include "DLSSNRSparseProbeCapture.h"
#include <sstream>

struct ProductionProbeOptions {
    Options base;
    std::string flow="amd1";
    int interval=2,anti=2,period=24,dumpTail=0,resetAt=-1;
    int idleAfter=-1,idleMs=200,tailMs=250;
    bool validateOnly=false,diagnosticValidity=false,diversePasses=false;
    ProductionProbeOptions() { base.model=2;base.percent=50;base.mode=3;base.fps=60;base.preset="user";base.samples=180;base.prefix="FourModesProduction"; }
};

// Structural proof complements the per-pass runtime Evaluate counters below:
// no retained engine can own a separate lightweight NR chain.
template<class T> concept HasLightweightBranch = requires(T& owner) { owner.lite; };
static_assert(!HasLightweightBranch<DLSSNRFilter::Performance>);
static_assert(!HasLightweightBranch<DLSSNRFilter::Sparse>);

struct ProductionProbeScene {
    winrt::com_ptr<ID3D11ComputeShader> shader;
    winrt::com_ptr<ID3D11Buffer> constants;
    static std::string Source() {
        return std::string(Magpie::RecoveryScene::Hlsl)+R"hlsl(
cbuffer SceneParams:register(b0) { uint width,height,frame,kind; uint warmup,period,translate,pad; };
RWTexture2D<float4> output:register(u0);
[numthreads(8,8,1)] void Generate(uint3 id:SV_DispatchThreadID) {
    if(id.x>=width||id.y>=height)return;
    uint2 p=id.xy;
    if(translate!=0)p.x=(p.x+frame*2)%width;
    output[id.xy]=float4(SceneColorBytes(kind,frame,width,height,warmup,period,p)/255.,1);
})hlsl";
    }
    static winrt::com_ptr<ID3DBlob> Compile() {
        const auto source=Source();winrt::com_ptr<ID3DBlob> blob,error;
        const auto hr=D3DCompile(source.data(),source.size(),"Production control scene",nullptr,nullptr,"Generate","cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,blob.put(),error.put());
        if(FAILED(hr)&&error)std::cerr.write(static_cast<const char*>(error->GetBufferPointer()),error->GetBufferSize());Check(hr);return blob;
    }
    ProductionProbeScene(DeviceResources& r) {
        auto blob=Compile();Check(r.GetD3DDevice()->CreateComputeShader(blob->GetBufferPointer(),blob->GetBufferSize(),nullptr,shader.put()));
        D3D11_BUFFER_DESC desc{};desc.ByteWidth=32;desc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        Check(r.GetD3DDevice()->CreateBuffer(&desc,nullptr,constants.put()));
    }
    void Draw(DeviceResources& r,Fixture& f,uint64_t frame,const ProductionProbeOptions& o) {
        const auto kind=o.base.scene=="translate"?Magpie::RecoveryScene::Kind::Static:Magpie::RecoveryScene::ParseKind(o.base.scene);
        const uint32_t data[8]{f.width,f.height,uint32_t(frame),uint32_t(kind),uint32_t(o.base.warmup),uint32_t(o.period),uint32_t(o.base.scene=="translate"),0};
        auto* dc=r.GetD3DDC();auto* cb=constants.get();auto* output=f.inputUav.get();
        dc->UpdateSubresource(cb,0,nullptr,data,0,0);dc->CSSetConstantBuffers(0,1,&cb);dc->CSSetShader(shader.get(),nullptr,0);
        dc->CSSetUnorderedAccessViews(0,1,&output,nullptr);dc->Dispatch((f.width+7)/8,(f.height+7)/8,1);
        output=nullptr;dc->CSSetUnorderedAccessViews(0,1,&output,nullptr);dc->CSSetShader(nullptr,nullptr,0);
    }
};

struct ProbeTexture {
    winrt::com_ptr<ID3D11Texture2D> texture;
    void Create(DeviceResources& r,uint32_t w,uint32_t h,DXGI_FORMAT format) {
        texture=DirectXHelper::CreateTexture2D(r.GetD3DDevice(),format,w,h,D3D11_BIND_SHADER_RESOURCE);
        Require(bool(texture),"probe snapshot allocation");
    }
};

// Optional diagnostic work, deliberately outside normal product execution.
// Its CPU/GPU spans are separate and its runs cannot be performance baselines.
struct ProbeValidityAudit {
    winrt::com_ptr<ID3D11ComputeShader> shader;
    winrt::com_ptr<ID3D11Buffer> counts,readback;
    winrt::com_ptr<ID3D11UnorderedAccessView> uav;
    static winrt::com_ptr<ID3DBlob> Compile() {
        constexpr std::string_view source=R"hlsl(
Texture2D<float> Validity:register(t0);
RWByteAddressBuffer Counts:register(u0);
groupshared uint ZeroCount,PartialCount;
[numthreads(8,8,1)] void Audit(uint3 id:SV_DispatchThreadID,uint lane:SV_GroupIndex) {
    if(lane==0){ZeroCount=0;PartialCount=0;}
    GroupMemoryBarrierWithGroupSync();
    uint w,h;Validity.GetDimensions(w,h);
    if(id.x<w&&id.y<h){float v=Validity.Load(int3(id.xy,0));if(v<=0)InterlockedAdd(ZeroCount,1);if(v<.999)InterlockedAdd(PartialCount,1);}
    GroupMemoryBarrierWithGroupSync();
    if(lane==0){uint unused;Counts.InterlockedAdd(0,ZeroCount,unused);Counts.InterlockedAdd(4,PartialCount,unused);}
})hlsl";
        winrt::com_ptr<ID3DBlob> blob,error;const auto hr=D3DCompile(source.data(),source.size(),"Probe validity audit",nullptr,nullptr,"Audit","cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,blob.put(),error.put());
        if(FAILED(hr)&&error)std::cerr.write(static_cast<const char*>(error->GetBufferPointer()),error->GetBufferSize());Check(hr);return blob;
    }
    explicit ProbeValidityAudit(DeviceResources& r) {
        const auto blob=Compile();Check(r.GetD3DDevice()->CreateComputeShader(blob->GetBufferPointer(),blob->GetBufferSize(),nullptr,shader.put()));
        D3D11_BUFFER_DESC d{};d.ByteWidth=16;d.BindFlags=D3D11_BIND_UNORDERED_ACCESS;d.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        Check(r.GetD3DDevice()->CreateBuffer(&d,nullptr,counts.put()));D3D11_UNORDERED_ACCESS_VIEW_DESC view{};view.Format=DXGI_FORMAT_R32_TYPELESS;view.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;view.Buffer.NumElements=4;view.Buffer.Flags=D3D11_BUFFER_UAV_FLAG_RAW;
        Check(r.GetD3DDevice()->CreateUnorderedAccessView(counts.get(),&view,uav.put()));d.BindFlags=d.MiscFlags=0;d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        Check(r.GetD3DDevice()->CreateBuffer(&d,nullptr,readback.put()));
    }
    std::pair<double,double> Run(DeviceResources& r,ID3D11ShaderResourceView* validity,uint32_t width,uint32_t height) {
        if(!validity)return {-1,-1};auto* dc=r.GetD3DDC();const UINT zero[4]{};dc->ClearUnorderedAccessViewUint(uav.get(),zero);auto* target=uav.get();
        dc->CSSetShader(shader.get(),nullptr,0);dc->CSSetShaderResources(0,1,&validity);dc->CSSetUnorderedAccessViews(0,1,&target,nullptr);dc->Dispatch((width+7)/8,(height+7)/8,1);
        validity=nullptr;target=nullptr;dc->CSSetShaderResources(0,1,&validity);dc->CSSetUnorderedAccessViews(0,1,&target,nullptr);dc->CSSetShader(nullptr,nullptr,0);
        dc->CopyResource(readback.get(),counts.get());D3D11_MAPPED_SUBRESOURCE mapped{};Check(dc->Map(readback.get(),0,D3D11_MAP_READ,0,&mapped));
        const auto* values=static_cast<const uint32_t*>(mapped.pData);const auto result=std::pair{double(values[0])/(double(width)*height),double(values[1])/(double(width)*height)};dc->Unmap(readback.get(),0);return result;
    }
};

EffectOption ProductionOption(const ProductionProbeOptions& o) {
    const auto& b=o.base;EffectOption effect;effect.name="DLSSNR\\DLSSNR_AI_Filter";auto& p=effect.parameters;
    p["resolutionControl"]=1;p["inputResolutionPercent"]=float(b.percent);p["modelPreset"]=float(b.model);
    p["processingMode"]=float(b.mode);p["sparseUpdateInterval"]=float(o.interval);p["__dlssnrModeVersion"]=1;
    p["multiPass"]=float(b.passes);p["antiFlicker"]=float(o.anti);p["antiFlickerTimeMs"]=150;
    p["residualTransfer"]=1;
    if(o.flow.starts_with("amd")){p["opticalFlowMethod"]=1;p["amdOpticalFlowMode"]=float(o.flow.back()-'0');}
    else if(o.flow.starts_with("nv")){p["opticalFlowMethod"]=2;p["nvidiaOpticalFlowQuality"]=float(o.flow.back()-'0');}
    else p["opticalFlowMethod"]=0;
    if(b.preset=="user") {
        p["style"]=2;p["useAutoMask"]=1;p["uiCorrection"]=1;p["residualHueProtection"]=.5f;p["residualChromaProtection"]=.5f;
        p["residualDarkProtection"]=.5f;p["residualHighlightProtection"]=.5f;p["residualLocalCompression"]=.1f;p["residualMidtoneStrength"]=1.1f;
    }
    if(o.diversePasses) {
        p["pass2_intensity"]=.65f;p["pass2_localToneStrength"]=.8f;p["pass2_style"]=1;
        p["pass3_intensity"]=.4f;p["pass3_localStructureStrength"]=.6f;p["pass3_style"]=0;
    }
    return effect;
}

struct ProductionRow {
    uint64_t record=0,physical=0,target=0,targetClock=0,previousTarget=0,history=0,version=0,filterVersion=0,fence=0;
    int64_t physicalAdmission=0,targetAdmission=0;
    bool physicalNew=false,targetNew=false,reverseAvailable=false;
    std::string phase;
    DLSSNRSparseOutputInfo info;
    DLSSNRPerformanceSnapshot snapshot;
    double flowCpu=0,wrapperCpu=0,submitCpu=0,readyObserved=-1;
    double zeroFallback=-1,notFullyTrusted=-1,validityProbeCpu=0;
    bool pending=false,waiting=false;
    uint64_t actualFullEvaluations=0;
};

int main(int argc,char** argv) {
    try {
        ProductionProbeOptions o;
        for(int i=1;i<argc;++i) {
            const std::string arg=argv[i];auto value=[&](){Require(i+1<argc,"missing production argument");return std::string(argv[++i]);};
            if(arg=="--mode")o.base.mode=std::stoi(value());
            else if(arg=="--reset-at")o.resetAt=std::stoi(value());
            else if(arg=="--diverse-passes")o.diversePasses=true;
            else if(arg=="--flow")o.flow=value();
            else if(arg=="--interval")o.interval=std::stoi(value());
            else if(arg=="--model")o.base.model=std::stoi(value());
            else if(arg=="--percent")o.base.percent=std::stoul(value());
            else if(arg=="--passes")o.base.passes=std::stoi(value());
            else if(arg=="--anti")o.anti=std::stoi(value());
            else if(arg=="--preset")o.base.preset=value();
            else if(arg=="--scene")o.base.scene=value();
            else if(arg=="--fps")o.base.fps=std::stod(value());
            else if(arg=="--warmup")o.base.warmup=std::stoi(value());
            else if(arg=="--samples")o.base.samples=std::stoi(value());
            else if(arg=="--period")o.period=std::stoi(value());
            else if(arg=="--width")o.base.width=std::stoul(value());
            else if(arg=="--height")o.base.height=std::stoul(value());
            else if(arg=="--prefix")o.base.prefix=value();
            else if(arg=="--dump-tail")o.dumpTail=std::stoi(value());
            else if(arg=="--idle-after")o.idleAfter=std::stoi(value());
            else if(arg=="--idle-ms")o.idleMs=std::stoi(value());
            else if(arg=="--tail-ms")o.tailMs=std::stoi(value());
            else if(arg=="--validate-shaders-only")o.validateOnly=true;
            else if(arg=="--diagnostic-validity")o.diagnosticValidity=true;
            else throw std::runtime_error("Unknown production argument: "+arg);
        }
        const auto& b=o.base;
        Require(b.mode>=0&&b.mode<=3&&b.model>=0&&b.model<=2&&b.percent>=25&&b.percent<=100&&b.passes>=1&&b.passes<=3&&
            b.width>=32&&b.height>=32&&b.samples>0&&b.warmup>=0&&b.fps>0&&o.period>0&&o.interval>=2&&o.interval<=4&&
            o.dumpTail>=0&&o.dumpTail<=b.samples&&o.idleMs>=0&&o.tailMs>=0,"production option range");
        Require(o.flow=="amd0"||o.flow=="amd1"||o.flow=="nv1"||o.flow=="nv2"||o.flow=="nv3"||o.flow=="nv4"||o.flow=="nv5","real flow selection");
        Require(b.scene=="translate"||b.scene=="static"||b.scene=="occlusion"||b.scene=="cut"||b.scene=="lighting"||b.scene=="ui","production scene");
        Require(b.preset=="user"||b.preset=="default","production preset");
        if(o.validateOnly){ProductionProbeScene::Compile();ProbeValidityAudit::Compile();std::cout<<"Production scene and diagnostic audit compiled on CPU; no GPU created\n";return 0;}
        const auto effect=ProductionOption(o);Require(IsDLSSNRPerformanceOptionValid(effect),"production parameter compatibility");
        winrt::init_apartment();Logger::Get().Initialize(spdlog::level::info,L"DLSSNRSparseProductionProbe.log",20000000,1);
        DeviceResources resources;Check(CreateDXGIFactory2(0,IID_PPV_ARGS(resources._dxgiFactory.put())));
        for(UINT i=0;;++i){winrt::com_ptr<IDXGIAdapter1> adapter;Check(resources._dxgiFactory->EnumAdapters1(i,adapter.put()));DXGI_ADAPTER_DESC1 d{};Check(adapter->GetDesc1(&d));if(d.VendorId!=0x10de)continue;
            resources._graphicsAdapter=adapter.as<IDXGIAdapter4>();winrt::com_ptr<ID3D11Device> device;winrt::com_ptr<ID3D11DeviceContext> dc;
            Check(D3D11CreateDevice(adapter.get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,0,nullptr,0,D3D11_SDK_VERSION,device.put(),nullptr,dc.put()));
            resources._d3dDevice=device.as<ID3D11Device5>();resources._d3dDC=dc.as<ID3D11DeviceContext4>();break;}
        auto* dc=resources.GetD3DDC();Fixture fixture(resources,b.width,b.height,false);ProductionProbeScene scene(resources);scene.Draw(resources,fixture,1,o);
        CombinedBgraCapture capture(resources,fixture.input.get(),b.width,b.height);capture.Render();
        NgxD3D12Core core;DLSSNRMultiPass wrapper;const auto initialize=std::chrono::steady_clock::now();
        Require(wrapper.Initialize(resources,core,capture.texture.get(),fixture.output.get(),effect,false),"production wrapper initialization");
        const double initializeMs=Milliseconds(initialize);
        auto& filter=*wrapper._filter;Require(!wrapper._adaptiveEnabled,"production sparse adaptive disabled");
        Require(filter._passSettings.size()==size_t(b.passes),"all requested passes exist");
        for(size_t pass=0;pass<filter._passSettings.size();++pass) {
            const auto expected=ParseDLSSNRSettings(DLSSNRPassOption(effect,int(pass+1)));
            const auto& actual=filter._passSettings[pass];
            Require(actual.modelPreset==expected.modelPreset&&actual.inputResolutionPercent==expected.inputResolutionPercent&&
                actual.style==expected.style&&actual.intensity==expected.intensity&&actual.localToneStrength==expected.localToneStrength&&
                actual.localStructureStrength==expected.localStructureStrength,"complete pass specifications preserved");
        }
        std::unique_ptr<ProbeValidityAudit> validityAudit;if(o.diagnosticValidity)validityAudit=std::make_unique<ProbeValidityAudit>(resources);
        FrameGuidanceService guidance;const auto flowInitialize=std::chrono::steady_clock::now();
        const auto requirements=wrapper.GetFrameGuidanceRequirements();
        Require(guidance.Initialize(resources,capture.texture.get(),requirements),"production guidance initialization");
        const double flowInitializeMs=Milliseconds(flowInitialize);
        const auto request=ParseDLSSNRSettings(effect).motionRequest;
        GpuTrace profiler;profiler.Initialize(resources.GetD3DDevice(),dc);trace=&profiler;NgxCollector collector{{},profiler};OutputFence outputFence(resources);
        const uint64_t total=uint64_t(b.warmup)+b.samples;
        struct Dump{uint64_t physical=0,target=0;ProbeTexture physicalImage,targetImage,outputImage;};std::vector<Dump> dumps;
        for(uint64_t f=total-uint64_t(o.dumpTail)+1;o.dumpTail&&f<=total;++f){Dump d;d.physical=f;
            d.physicalImage.Create(resources,b.width,b.height,DXGI_FORMAT_R8G8B8A8_UNORM);d.targetImage.Create(resources,b.width,b.height,DXGI_FORMAT_B8G8R8A8_UNORM);
            d.outputImage.Create(resources,b.width,b.height,DXGI_FORMAT_R8G8B8A8_UNORM);dumps.push_back(std::move(d));}
        std::vector<ProductionRow> rows;size_t observed=0;uint64_t record=0,lastPhysical=0,previousTarget=0;
        FrameGuidanceConsumerViews views;int64_t admission=0;
        const auto runStart=std::chrono::steady_clock::now();
        auto collect=[&](){collector.Collect(filter);};
        auto observe=[&](){const auto done=outputFence.fence->GetCompletedValue();while(observed<rows.size()&&rows[observed].fence<=done){rows[observed].readyObserved=Milliseconds(runStart);++observed;}};
        auto service=[&](){collect();wrapper.ServiceAsync();if(wrapper.HasPendingPostOutputWork())wrapper.ServiceAfterOutput();profiler.Poll();observe();};
        auto draw=[&](bool physicalNew,const std::string& phase,double flowCpu){
            NativeEffectDrawContext context{.input=capture.texture.get(),.output=fixture.output.get(),.frameId=lastPhysical,.previousCaptureFrameId=lastPhysical?lastPhysical-1:0,
                .inputRevision=lastPhysical,.inputHistoryRevision=(o.resetAt>0&&lastPhysical>=uint64_t(o.resetAt))?2u:1u,
                .inputHistoryReset=physicalNew&&(lastPhysical==1||(o.resetAt>0&&lastPhysical==uint64_t(o.resetAt))),.isNewCaptureFrame=physicalNew,
                .captureFrameIndex=lastPhysical,.captureAdmissionTime100ns=admission,.captureReflexFrameId=lastPhysical,.targetContentFrameRate=b.fps,
                .guidanceGpuMs=guidance.LastGpuMilliseconds(),.guidanceGpuSample=guidance.LastGpuSample(),.frameGuidance=views.produced,.zeroFrameGuidance=views.zero};
            ProductionRow row;row.record=++record;row.phase=phase;row.physical=lastPhysical;row.physicalAdmission=admission;row.physicalNew=physicalNew;row.flowCpu=flowCpu;
            profiler.Begin(record);const auto started=std::chrono::steady_clock::now();
            {Region scope("production_wrapper_gpu_span",dc,b.width,b.height);Require(wrapper.Draw(context),"production wrapper Draw");}
            row.wrapperCpu=Milliseconds(started);const auto target=wrapper.OutputDrawContext(context);
            row.target=target.frameId;row.previousTarget=target.previousCaptureFrameId;row.targetClock=target.captureFrameIndex;row.targetAdmission=target.captureAdmissionTime100ns;
            row.targetNew=target.isNewCaptureFrame;row.history=target.inputHistoryRevision;row.version=wrapper.OutputContentVersion();row.filterVersion=filter.OutputContentVersion();
            row.info=filter.GetSparseOutputInfo();row.snapshot=filter.GetPerformanceSnapshot();
            row.actualFullEvaluations=filter._impl->evaluateCount;
            row.pending=filter._sparse?filter._sparse->pending:filter._performance?filter._performance->pending:false;
            row.waiting=filter._sparse?filter._sparse->waiting:filter._performance?filter._performance->latestWaiting:false;
            if(!filter._sparse&&!filter._performance)row.snapshot.fullSubmissions=row.actualFullEvaluations;
            if(validityAudit){const auto start=std::chrono::steady_clock::now();Region audit("probe_validity_audit_gpu_span",dc,b.width,b.height);
                const auto result=validityAudit->Run(resources,row.info.validity,b.width,b.height);row.zeroFallback=result.first;row.notFullyTrusted=result.second;row.validityProbeCpu=Milliseconds(start);}
            row.reverseAvailable=views.produced.HasReverseMotionForPair(lastPhysical,lastPhysical?lastPhysical-1:0,{b.width,b.height});
            Require(row.target>=previousTarget,"production target moved backward");
            Require(row.target==row.physical&&row.targetAdmission==row.physicalAdmission,"four-mode output must use the current capture, without whole-picture delay");
            if(!row.targetNew&&previousTarget)Require(row.target==previousTarget,"held output changed target identity");
            previousTarget=row.target;
            for(auto& dump:dumps)if(physicalNew&&dump.physical==lastPhysical){dump.target=row.target;Region copy("production_quality_snapshot",dc,b.width,b.height);
                dc->CopyResource(dump.physicalImage.texture.get(),fixture.input.get());dc->CopyResource(dump.targetImage.texture.get(),target.input);dc->CopyResource(dump.outputImage.texture.get(),fixture.output.get());}
            {Region marker("production_output_ready",dc,b.width,b.height);}
            row.fence=outputFence.Signal();wrapper.ServiceAfterOutput();profiler.End();dc->Flush();row.submitCpu=Milliseconds(started);rows.push_back(row);service();
            Require(wrapper.IsDlssNrProcessing(),"production NR remained healthy");
        };
        auto idle=[&](int milliseconds,const std::string& phase){const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(milliseconds);
            while(std::chrono::steady_clock::now()<deadline){service();if(wrapper.HasReadyOutput())draw(false,phase,0);std::this_thread::sleep_for(std::chrono::milliseconds(1));}
        };
        DLSSNRBenchmarkPacing pacer;auto schedule=std::chrono::steady_clock::now();
        for(uint64_t f=1;f<=total;++f){pacer.WaitUntil(schedule,service);schedule+=std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(1./b.fps));
            scene.Draw(resources,fixture,f,o);capture.Render();lastPhysical=f;admission=std::max<int64_t>(1,std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-runStart).count()/100);
            const auto started=std::chrono::steady_clock::now();
            guidance.BeginFrame(f,capture.texture.get(),requirements,1,1,admission);views=guidance.GetConsumerViews(f,{b.width,b.height},request);
            const double flowCpu=Milliseconds(started);Require(views.zero.IsValidFor(f,{b.width,b.height}),"production zero guidance identity");
            draw(true,(o.idleAfter>=0&&f==uint64_t(o.idleAfter+1))?"resume":"capture",flowCpu);
            if(o.idleAfter>=0&&f==uint64_t(o.idleAfter)){idle(o.idleMs,"idle");schedule=std::chrono::steady_clock::now();}
        }
        idle(o.tailMs,"tail");const double wallMs=Milliseconds(runStart);Require(wrapper.Drain(),"production wrapper drain");
        profiler.Begin(++record);profiler.End();outputFence.Wait(outputFence.Signal());
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        while(profiler.Pending()&&std::chrono::steady_clock::now()<deadline){profiler.Poll(true);collect();observe();std::this_thread::sleep_for(std::chrono::milliseconds(1));}
        Require(!profiler.Pending(),"production timestamp retirement");trace=nullptr;collect();observe();
        Require(capture.VerifyLogicalRgba(fixture.input.get()),"production BGRA logical input verified");
        std::map<uint64_t,std::pair<uint64_t,uint64_t>> readyTicks;
        std::ofstream samples(b.prefix+".samples.csv");samples<<"frame,stage,domain,width,height,milliseconds,startTick,endTick,frequency\n"<<std::setprecision(12);
        for(const auto& s:profiler.samples){samples<<s.frame<<','<<s.stage<<','<<s.domain<<','<<s.width<<','<<s.height<<','<<s.milliseconds<<','<<s.startTick<<','<<s.endTick<<','<<s.frequency<<'\n';
            if(s.stage=="production_output_ready")readyTicks[s.frame]={s.endTick,s.frequency};}
        std::ofstream frames(b.prefix+".production-frames.csv");
        frames<<"recordId,phase,physicalFrame,physicalNew,physicalAdmission100ns,targetFrame,targetClock,targetAdmission100ns,targetPreviousFrame,targetHistoryRevision,targetNew,outputVersion,filterOutputVersion,held,targetSequence,oldestNrSourceSequence,epoch,pastSourceFrame,observationRevision,sourceAge,flowCpuMs,wrapperCpuMs,submitCpuMs,outputReadyFence,outputReadyObservedMs,outputReadyGpuTick,outputFrequency,zeroCorrectionFallbackFraction,notFullyTrustedFraction,validityProbeCpuMs\n"<<std::setprecision(12);
        for(const auto& r:rows){const auto& i=r.info;const auto tick=readyTicks[r.record];
            frames<<r.record<<','<<r.phase<<','<<r.physical<<','<<r.physicalNew<<','<<r.physicalAdmission<<','<<r.target<<','<<r.targetClock<<','<<r.targetAdmission<<','<<r.previousTarget<<','<<r.history<<','<<r.targetNew<<','<<r.version<<','<<r.filterVersion<<','<<i.held<<','<<i.targetSequence<<','<<i.oldestSourceSequence<<','<<i.epoch<<','<<i.pastSourceFrameId<<','<<i.observationRevision<<','
                <<(i.oldestSourceSequence?i.targetSequence-i.oldestSourceSequence:0)<<','<<r.flowCpu<<','<<r.wrapperCpu<<','<<r.submitCpu<<','<<r.fence<<','<<r.readyObserved<<','<<tick.first<<','<<tick.second<<','<<r.zeroFallback<<','<<r.notFullyTrusted<<','<<r.validityProbeCpu<<'\n';}
        const auto last=filter.GetSparseOutputInfo();
        std::ofstream counterFrames(b.prefix+".counters.csv");
        counterFrames<<"recordId,physicalFrame,targetFrame,fullSubmissions,sameSpecRecoveries,continuousReuseOutputs,actualFullEvaluations,pending,waiting\n";
        for(const auto& r:rows)counterFrames<<r.record<<','<<r.physical<<','<<r.target<<','<<r.snapshot.fullSubmissions<<','
            <<r.snapshot.sameSpecRecoveries<<','<<r.snapshot.continuousReuseOutputs<<','<<r.actualFullEvaluations<<','<<r.pending<<','<<r.waiting<<'\n';
        std::ofstream counts(b.prefix+".counts.json");
        auto totals=filter.GetPerformanceSnapshot();
        if(!filter._performance&&!filter._sparse)totals.fullSubmissions=filter._impl->evaluateCount;
        counts<<"{\"lightweightBranchPresent\":false,\"lightweightProof\":\"compile_time_owner_absence_and_all_pass_evaluations\""
            <<",\"fullSubmissions\":"<<totals.fullSubmissions
            <<",\"sameSpecRecoveries\":"<<totals.sameSpecRecoveries<<",\"continuousReuseOutputs\":"<<totals.continuousReuseOutputs
            <<",\"explicitResetAt\":"<<o.resetAt<<",\"diversePasses\":"<<(o.diversePasses?"true":"false")<<",\"passes\":[";
        for(size_t pass=0;pass<filter._passSettings.size();++pass) {
            const auto& settings=filter._passSettings[pass];
            const auto& impl=pass?*filter._impl->laterPasses[pass-1]:*filter._impl;
            if(pass)counts<<',';
            counts<<"{\"pass\":"<<pass+1<<",\"modelPreset\":"<<settings.modelPreset<<",\"inputPercent\":"<<settings.inputResolutionPercent
                <<",\"width\":"<<impl.width<<",\"height\":"<<impl.height<<",\"style\":"<<settings.style<<",\"intensity\":"<<settings.intensity
                <<",\"localToneStrength\":"<<settings.localToneStrength<<",\"localStructureStrength\":"<<settings.localStructureStrength
                <<",\"evaluateCount\":"<<impl.evaluateCount<<",\"evaluateSuccessCount\":"<<impl.evaluateSuccessCount<<",\"evaluateFailureCount\":"<<impl.evaluateFailureCount<<'}';
            Require(impl.evaluateCount>0&&impl.evaluateFailureCount==0&&impl.evaluateSuccessCount==impl.evaluateCount,"all full passes evaluated successfully");
            Require(impl.evaluateCount==filter._impl->evaluateCount,"full inference omitted later pass");
        }
        counts<<"]}\n";
        std::ofstream summary(b.prefix+".production.json");
        summary<<std::setprecision(12)<<"{\n\"schemaVersion\":2,\"modeVersion\":1,\"diagnosticValidity\":"<<(o.diagnosticValidity?"true":"false")
            <<",\"probe\":\"actual_production_four_modes_real_flow\",\"mode\":"<<b.mode<<",\"model\":"<<b.model
            <<",\"fullPercent\":"<<b.percent<<",\"passes\":"<<b.passes<<",\"flow\":\""<<o.flow<<"\",\"scene\":\""<<b.scene
            <<"\",\"fps\":"<<b.fps<<",\"intervalRequested\":"<<o.interval
            <<",\"intervalEffective\":"<<((b.mode==1||b.mode==3)?o.interval:1)<<",\"bufferDelayEffective\":0"
            <<",\"width\":"<<b.width<<",\"height\":"<<b.height<<",\"warmup\":"<<b.warmup<<",\"samples\":"<<b.samples
            <<",\"period\":"<<o.period<<",\"antiFlicker\":"<<o.anti<<",\"initializeMs\":"<<initializeMs<<",\"flowInitializeMs\":"<<flowInitializeMs
            <<",\"wallMsIncludingWarmupIdleTail\":"<<wallMs<<",\"idleAfter\":"<<o.idleAfter<<",\"idleMs\":"<<o.idleMs<<",\"tailMs\":"<<o.tailMs
            <<",\"lastPhysicalFrame\":"<<lastPhysical<<",\"lastOutputTarget\":"<<previousTarget<<",\"lastOutputSequence\":"<<last.targetSequence
            <<",\"tailReachedLatest\":"<<(previousTarget==lastPhysical?"true":"false")
            <<",\"measurementClass\":\""<<(o.diagnosticValidity?"diagnostic_readback":o.dumpTail?"quality_gpu_snapshots":"performance")
            <<"\",\"resetEveryInference\":false,\"oracleValidityUsed\":false,\"idealMotionUsed\":false"
            <<",\"notes\":[\"Source rendered to BGRA8; actual flow and NR consume identical logical colors.\",\"Scene helper supplies no oracle mask, motion, or per-Evaluate reset.\",\"D3D11 frame is recordId; D3D12 frame is actual NR source frameId.\",\"Counters include warmup; timings are GPU-ready, not desktop Present.\"]}\n";
        std::ofstream images(b.prefix+".production-images.csv");images<<"physicalFrame,targetFrame,physicalRgba,targetBgra,outputRgba\n";
        for(auto& d:dumps){const std::string name=b.prefix+".frame-"+std::to_string(d.physical);ReadOutput(resources,d.physicalImage.texture.get(),name+".physical.rgba");ReadOutput(resources,d.targetImage.texture.get(),name+".target.bgra");ReadOutput(resources,d.outputImage.texture.get(),name+".output.rgba");images<<d.physical<<','<<d.target<<','<<name<<".physical.rgba,"<<name<<".target.bgra,"<<name<<".output.rgba\n";}
        Logger::Get().Flush();std::cout<<"Production probe complete: "<<b.prefix<<" lastTarget="<<previousTarget<<" latest="<<lastPhysical<<'\n';return 0;
    }catch(const winrt::hresult_error& e){std::cerr<<"HRESULT "<<std::hex<<e.code().value<<'\n';return 2;}
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}
}
