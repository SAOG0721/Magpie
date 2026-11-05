#pragma once
#include "DLSSNRColorShader.h"
#include "DLSSNRSparseShader.h"
#include <d3dcompiler.h>
#include <array>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

// Probe-only: the real Sparse dispatch is finished and unbound before Capture.
// GT is bound at b1 only for the extra audit dispatch, then removed. No ideal
// motion, mask, or reset reaches the product Map/Compose/Fill/NGX execution.
namespace FlowRejectionAudit {
inline constexpr unsigned CounterCount=32, SumCount=8, Stride=44;
struct Context {
    const char* stage="";
    const char* recovery="";
    uint64_t physical=0,sequence=0,target=0,past=0,future=0;
    uint64_t fullHistory=0,liteHistory=0;
    bool reverse=false;
};

inline std::string ShaderSource() {
    return std::string(Magpie::DLSSNR_COLOR_HLSL)+std::string(Magpie::DLSSNR_SPARSE_HLSL)+R"hlsl(
cbuffer AuditParams:register(b1) {
    uint GroupWidth,HasReverse,UseGT,AuditPad;
    float PastGTX,FutureGTX,AuditPad2,AuditPad3;
};
RWByteAddressBuffer AuditData:register(u5);
groupshared uint AuditCounts[32];
groupshared float4 AuditSumA[64],AuditSumB[64];
groupshared uint PhaseLo,PhaseHi;
void AuditBegin(uint lane) {
    if(lane<32)AuditCounts[lane]=0;
    if(lane==0){PhaseLo=0;PhaseHi=0;}
    GroupMemoryBarrierWithGroupSync();
}
void Count(uint index,bool yes){if(yes)InterlockedAdd(AuditCounts[index],1);}
void AuditEnd(uint3 group,uint lane,float4 a,float4 b) {
    AuditSumA[lane]=a;AuditSumB[lane]=b;
    GroupMemoryBarrierWithGroupSync();
    [unroll]for(uint step=32;step>0;step>>=1){
        if(lane<step){AuditSumA[lane]+=AuditSumA[lane+step];AuditSumB[lane]+=AuditSumB[lane+step];}
        GroupMemoryBarrierWithGroupSync();
    }
    uint offset=(group.x+group.y*GroupWidth)*44*4;
    if(lane<32)AuditData.Store(offset+lane*4,AuditCounts[lane]);
    if(lane==0){
        AuditData.Store4(offset+32*4,asuint(AuditSumA[0]));
        AuditData.Store4(offset+36*4,asuint(AuditSumB[0]));
        AuditData.Store(offset+40*4,PhaseLo);AuditData.Store(offset+41*4,PhaseHi);
        AuditData.Store(offset+42*4,0);AuditData.Store(offset+43*4,0);
    }
}
float MaxRgb(float3 c){return max(c.r,max(c.g,c.b));}
[numthreads(8,8,1)]void AuditPair(uint3 id:SV_DispatchThreadID,uint3 group:SV_GroupID,uint lane:SV_GroupIndex){
    AuditBegin(lane);float4 a=0,b=0;
    if(all(id.xy<Size)){
        float2 uv=(id.xy+.5)/float2(Size);
        float c=PastConfidence.SampleLevel(LinearClamp,uv,0);
        float2 mv=PastMotion.SampleLevel(LinearClamp,uv,0);
        float2 previous=uv+mv/float2(Size);
        float3 now=Target.Load(int3(id.xy,0)).rgb;
        float3 diff=abs(now-PastReference.SampleLevel(LinearClamp,previous,0).rgb);
        bool finiteMv=all(isfinite(mv)),finiteDiff=all(isfinite(diff));
        bool inside=Inside(previous),finiteC=isfinite(c);
        float error=finiteDiff?MaxRgb(diff):0;
        // Exact product MeasureInputChange predicate, evaluated densely.
        bool rejected=c<.5||!inside||!finiteDiff||error>.06;
        Count(0,true);Count(1,c<=0);Count(2,c<.5);Count(3,c<.999);
        Count(4,!finiteMv);Count(5,!inside);Count(6,finiteDiff&&error>.035);
        Count(7,finiteDiff&&error>.06);Count(8,finiteDiff&&error>=.12);Count(9,rejected);
        Count(10,c>=.5&&inside&&finiteDiff&&error>.06);
        Count(11,c<.5&&inside&&finiteDiff&&error<=.06);
        float fb=0;
        if(HasReverse){
            Count(12,true);Count(13,!inside);
            float2 reverse=FutureMotion.SampleLevel(LinearClamp,previous,0);
            fb=inside&&finiteMv&&all(isfinite(reverse))?length(mv+reverse):0;
            Count(14,inside&&fb>=.75+.05*length(mv));Count(23,inside&&fb>.25);
        }
        float epe=0,gtError=0;bool gtInside=false;
        if(UseGT){
            float2 gt=uv+float2(PastGTX,0)/float2(Size);gtInside=Inside(gt);
            if(gtInside){
                epe=finiteMv?length(mv-float2(PastGTX,0)):0;
                gtError=MaxRgb(abs(now-PastReference.SampleLevel(LinearClamp,gt,0).rgb));
                Count(15,true);Count(16,epe>.25);Count(17,epe>.5);Count(18,epe>1);
                Count(19,gtError>.035);Count(20,gtError>.06);
                Count(21,finiteMv&&epe<=.25&&rejected);Count(22,epe>1&&!rejected);
                Count(24,finiteMv&&epe<=.25&&c<.5);Count(25,epe>1&&c>=.5);
                Count(26,finiteMv&&epe<=.25);
            }
        }
        Count(27,finiteDiff);Count(28,finiteC);Count(29,finiteMv);
        a=float4(finiteC?c:0,finiteDiff?error:0,fb,gtInside?epe:0);
        b=float4(finiteMv?mv.x:0,finiteMv?mv.y:0,gtInside?gtError:0,0);
        if(rejected){if(lane<32)InterlockedOr(PhaseLo,1u<<lane);else InterlockedOr(PhaseHi,1u<<(lane-32));}
    }
    AuditEnd(group,lane,a,b);
}
void Endpoint(float2 uv,float3 now,Texture2D<float4> reference,Texture2D<float4> residual,
    Texture2D<float2> motion,Texture2D<float> confidence,out float c,out float photo,out float error,out float w){
    c=confidence.SampleLevel(LinearClamp,uv,0);photo=0;error=0;float3 unused;
    w=SourceWeight(uv,now,reference,residual,motion,confidence,unused);
    float2 previous=uv+motion.SampleLevel(LinearClamp,uv,0)/float2(Size);
    if(!Inside(previous)||!isfinite(c)){c=0;return;}
    float3 old=reference.SampleLevel(LinearClamp,previous,0).rgb;
    float3 correction=residual.SampleLevel(LinearClamp,previous,0).rgb;
    if(!all(isfinite(old))||!all(isfinite(correction))){c=0;return;}
    error=MaxRgb(abs(now-old));photo=1-smoothstep(.035,.12,error);
}
[numthreads(8,8,1)]void AuditCompose(uint3 id:SV_DispatchThreadID,uint3 group:SV_GroupID,uint lane:SV_GroupIndex){
    AuditBegin(lane);float4 a=0,b=0;
    if(all(id.xy<Size)){
        float2 uv=(id.xy+.5)/float2(Size);float3 now=Target.Load(int3(id.xy,0)).rgb;
        float ca=0,pa=0,ea=0,wa=0,cb=0,pb=0,eb=0,wb=0;
        if(HasPast)Endpoint(uv,now,PastReference,PastResidual,PastMotion,PastConfidence,ca,pa,ea,wa);
        if(HasFuture)Endpoint(uv,now,FutureReference,FutureResidual,FutureMotion,FutureConfidence,cb,pb,eb,wb);
        Count(0,true);Count(1,HasPast!=0);
        Count(2,HasPast&&ca<=0);Count(3,HasPast&&ca>0&&ca<.999);Count(4,HasPast&&ca>=.999);
        Count(5,HasPast&&pa<=0);Count(6,HasPast&&pa>0&&pa<.999);Count(7,HasPast&&pa>=.999);
        Count(8,HasPast&&wa<=0);Count(9,HasPast&&wa>0&&wa<.999);Count(10,HasPast&&wa>=.999);
        Count(11,HasFuture&&cb<=0);Count(12,HasFuture&&cb>0&&cb<.999);Count(13,HasFuture&&cb>=.999);
        Count(14,HasFuture&&pb<=0);Count(15,HasFuture&&pb>0&&pb<.999);Count(16,HasFuture&&pb>=.999);
        Count(17,HasFuture&&wb<=0);Count(18,HasFuture&&wb>0&&wb<.999);Count(19,HasFuture&&wb>=.999);
        float trust=max(wa,wb),stored=RawValidity.Load(int3(id.xy,0));
        Count(20,trust<=0);Count(21,trust>0&&trust<.999);Count(22,trust>=.999);
        Count(23,stored<=0);Count(24,stored>0&&stored<.999);Count(25,stored>=.999);
        Count(26,trust<.999&&stored>=.999);Count(27,trust>=.999&&stored<.999);
        Count(28,max(pa,pb)<.999);Count(29,max(ca,cb)<.999);
        if(Approximate&&stored<.999){
            float mass=0;
            [unroll]for(int r=2;r<=8;r*=2){
                [unroll]for(int y=-1;y<=1;++y)[unroll]for(int x=-1;x<=1;++x){
                    if(x==0&&y==0)continue;
                    int2 n=clamp(int2(id.xy)+int2(x,y)*r,int2(0,0),int2(Size)-1);
                    float v=RawValidity.Load(int3(n,0));
                    float difference=MaxRgb(abs(Target.Load(int3(n,0)).rgb-now));
                    mass+=(v>=.999?1-smoothstep(.035,.08,difference):0)/r;
                }
            }
            Count(30,mass>1e-5);Count(31,mass<=1e-5);
        }
        a=float4(wa,wb,ca,cb);b=float4(pa,pb,ea,eb);
    }
    AuditEnd(group,lane,a,b);
}
)hlsl";
}

inline winrt::com_ptr<ID3DBlob> Compile(const char* entry) {
    const auto source=ShaderSource();winrt::com_ptr<ID3DBlob> blob,error;
    const auto hr=D3DCompile(source.data(),source.size(),"flow rejection audit",nullptr,nullptr,entry,"cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,blob.put(),error.put());
    if(FAILED(hr)&&error)std::cerr.write(static_cast<const char*>(error->GetBufferPointer()),error->GetBufferSize());
    winrt::check_hresult(hr);return blob;
}
inline void ValidateShaders(){Compile("AuditPair");Compile("AuditCompose");}

struct Observer {
    struct Record {Context context;std::string stage,recovery;winrt::com_ptr<ID3D11Buffer> data;uint32_t width=0,height=0;};
    ID3D11Device* device=nullptr;ID3D11DeviceContext* dc=nullptr;
    winrt::com_ptr<ID3D11ComputeShader> pairShader,composeShader;
    winrt::com_ptr<ID3D11Buffer> settings,params;
    winrt::com_ptr<ID3D11SamplerState> sampler;
    std::vector<Record> records;
    uint64_t first=12,every=30;
    bool useGT=false,translation=false;
    Observer(ID3D11Device* dev,ID3D11DeviceContext* context,uint64_t firstFrames,uint64_t everyFrames,bool moving)
        :device(dev),dc(context),first(firstFrames),every(everyFrames),useGT(true),translation(moving){
        auto pair=Compile("AuditPair"),compose=Compile("AuditCompose");
        winrt::check_hresult(device->CreateComputeShader(pair->GetBufferPointer(),pair->GetBufferSize(),nullptr,pairShader.put()));
        winrt::check_hresult(device->CreateComputeShader(compose->GetBufferPointer(),compose->GetBufferSize(),nullptr,composeShader.put()));
        D3D11_BUFFER_DESC d{};d.ByteWidth=48;d.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        winrt::check_hresult(device->CreateBuffer(&d,nullptr,settings.put()));d.ByteWidth=32;
        winrt::check_hresult(device->CreateBuffer(&d,nullptr,params.put()));
        D3D11_SAMPLER_DESC s{};s.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;s.AddressU=s.AddressV=s.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;s.MaxLOD=D3D11_FLOAT32_MAX;
        winrt::check_hresult(device->CreateSamplerState(&s,sampler.put()));
    }
    void Capture(const Context& info,uint32_t width,uint32_t height,const void* values,
        ID3D11ShaderResourceView* const* source,ID3D11ShaderResourceView* rawValidity,
        ID3D11ShaderResourceView* reverseMotion,ID3D11ShaderResourceView* reverseConfidence){
        if(info.physical>first&&(!every||info.physical%every))return;
        const bool pair=std::string_view(info.stage)=="physical_precheck";
        const uint32_t groupsX=(width+7)/8,groupsY=(height+7)/8;
        Record record;record.context=info;record.stage=info.stage;record.recovery=info.recovery;record.width=width;record.height=height;
        D3D11_BUFFER_DESC d{};d.ByteWidth=groupsX*groupsY*Stride*4;d.BindFlags=D3D11_BIND_UNORDERED_ACCESS;d.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        winrt::check_hresult(device->CreateBuffer(&d,nullptr,record.data.put()));
        D3D11_UNORDERED_ACCESS_VIEW_DESC v{};v.Format=DXGI_FORMAT_R32_TYPELESS;v.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;v.Buffer.NumElements=d.ByteWidth/4;v.Buffer.Flags=D3D11_BUFFER_UAV_FLAG_RAW;
        winrt::com_ptr<ID3D11UnorderedAccessView> result;winrt::check_hresult(device->CreateUnorderedAccessView(record.data.get(),&v,result.put()));
        struct Params{uint32_t groups,reverse,gt,pad;float pastDx,futureDx,pad2,pad3;};
        auto delta=[&](uint64_t sourceFrame){return translation&&sourceFrame?float(int64_t(info.target)-int64_t(sourceFrame))*2.f:0.f;};
        const Params p{groupsX,uint32_t(info.reverse),uint32_t(useGT),0,delta(info.past),delta(info.future),0,0};
        dc->UpdateSubresource(settings.get(),0,nullptr,values,0,0);dc->UpdateSubresource(params.get(),0,nullptr,&p,0,0);
        ID3D11Buffer* cbs[]{settings.get(),params.get()};auto* sample=sampler.get();
        ID3D11ShaderResourceView* views[28]{};std::copy_n(source,28,views);
        if(pair){views[8]=info.reverse?reverseMotion:nullptr;views[9]=info.reverse?reverseConfidence:nullptr;}
        else views[27]=rawValidity;
        ID3D11UnorderedAccessView* outputs[8]{};outputs[5]=result.get();
        dc->CSSetShader(pair?pairShader.get():composeShader.get(),nullptr,0);dc->CSSetConstantBuffers(0,2,cbs);dc->CSSetSamplers(0,1,&sample);
        dc->CSSetShaderResources(0,28,views);dc->CSSetUnorderedAccessViews(0,8,outputs,nullptr);dc->Dispatch(groupsX,groupsY,1);
        std::fill_n(views,28,nullptr);std::fill_n(outputs,8,nullptr);std::fill_n(cbs,2,nullptr);
        dc->CSSetShaderResources(0,28,views);dc->CSSetUnorderedAccessViews(0,8,outputs,nullptr);dc->CSSetConstantBuffers(0,2,cbs);dc->CSSetShader(nullptr,nullptr,0);
        records.push_back(std::move(record));
    }
    void Write(const std::string& prefix){
        constexpr std::array pairNames{"pixels","confidence_zero","confidence_lt50","confidence_lt999","motion_nonfinite","out_of_bounds","warp_error_gt035","warp_error_gt060","warp_error_ge120","precheck_rejected","confidence_pass_photo_fail","confidence_fail_photo_pass","reverse_available","fb_outside","fb_ge_provider_threshold","gt_interior","epe_gt025","epe_gt050","epe_gt100","gt_warp_error_gt035","gt_warp_error_gt060","epe_le025_precheck_rejected","epe_gt100_precheck_accepted","fb_gt025","epe_le025_confidence_lt50","epe_gt100_confidence_ge50","epe_le025","photo_finite","confidence_finite","motion_finite","reserved30","reserved31"};
        constexpr std::array composeNames{"pixels","has_past","past_map_zero","past_map_partial","past_map_full","past_photo_zero","past_photo_partial","past_photo_full","past_weight_zero","past_weight_partial","past_weight_full","future_map_zero","future_map_partial","future_map_full","future_photo_zero","future_photo_partial","future_photo_full","future_weight_zero","future_weight_partial","future_weight_full","float_trust_zero","float_trust_partial","float_trust_full","r8_trust_zero","r8_trust_partial","r8_trust_full","float_invalid_r8_full","float_full_r8_invalid","without_confidence_invalid","without_photometric_invalid","r8_invalid_local_support","r8_invalid_no_support"};
        constexpr std::array pairSums{"confidence_sum","warp_error_sum","fb_error_sum","epe_sum","motion_x_sum","motion_y_sum","gt_warp_error_sum","reserved_sum"};
        constexpr std::array composeSums{"past_weight_sum","future_weight_sum","past_map_confidence_sum","future_map_confidence_sum","past_photo_weight_sum","future_photo_weight_sum","past_warp_error_sum","future_warp_error_sum"};
        std::ofstream pairs(prefix+".pair-audit.csv"),composes(prefix+".compose-audit.csv"),phases(prefix+".precheck-phases.csv");
        auto header=[&](std::ofstream& file,const auto& names,const auto& sums){
            file<<"auditRecord,physicalFrame,captureSequence,targetFrame,pastSourceFrame,futureSourceFrame,targetToPastFrames,targetToFutureFrames,fullHistoryFrame,liteHistoryFrame,targetToFullHistoryFrames,targetToLiteHistoryFrames,recoveryBeforeDecision,width,height";
            for(auto name:names)file<<','<<name<<"_count";for(auto name:sums)file<<','<<name;file<<'\n'<<std::setprecision(12);
        };
        header(pairs,pairNames,pairSums);header(composes,composeNames,composeSums);
        phases<<"auditRecord,physicalFrame,recoveryBeforeDecision,offsetX,offsetY,sampledPixels,rejectedPixels,rejectedFraction,productOffset\n"<<std::setprecision(12);
        for(size_t index=0;index<records.size();++index){
            auto& r=records[index];D3D11_BUFFER_DESC d{};r.data->GetDesc(&d);d.Usage=D3D11_USAGE_STAGING;d.BindFlags=d.MiscFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            winrt::com_ptr<ID3D11Buffer> staging;winrt::check_hresult(device->CreateBuffer(&d,nullptr,staging.put()));dc->CopyResource(staging.get(),r.data.get());
            D3D11_MAPPED_SUBRESOURCE map{};winrt::check_hresult(dc->Map(staging.get(),0,D3D11_MAP_READ,0,&map));const auto* data=static_cast<const uint32_t*>(map.pData);
            std::array<uint64_t,CounterCount> counts{};std::array<double,SumCount> sums{};std::array<uint64_t,64> phaseCounts{},phaseTotal{};
            const uint32_t gx=(r.width+7)/8,gy=(r.height+7)/8;
            for(uint32_t y=0;y<gy;++y)for(uint32_t x=0;x<gx;++x){
                const auto* group=data+(y*gx+x)*Stride;
                for(unsigned i=0;i<CounterCount;++i)counts[i]+=group[i];
                for(unsigned i=0;i<SumCount;++i){float f;std::memcpy(&f,group+CounterCount+i,4);sums[i]+=f;}
                for(unsigned lane=0;lane<64;++lane)if(x*8+lane%8<r.width&&y*8+lane/8<r.height){
                    ++phaseTotal[lane];phaseCounts[lane]+=(group[40+lane/32]>>(lane%32))&1;
                }
            }
            dc->Unmap(staging.get(),0);r.data=nullptr;
            const auto& c=r.context;auto& file=r.stage=="physical_precheck"?pairs:composes;
            auto distance=[&](uint64_t id){return id?int64_t(c.target)-int64_t(id):INT64_MIN;};
            file<<index+1<<','<<c.physical<<','<<c.sequence<<','<<c.target<<','<<c.past<<','<<c.future<<','<<distance(c.past)<<','<<distance(c.future)<<','<<c.fullHistory<<','<<c.liteHistory<<','<<distance(c.fullHistory)<<','<<distance(c.liteHistory)<<','<<r.recovery<<','<<r.width<<','<<r.height;
            for(auto n:counts)file<<','<<n;for(auto sum:sums)file<<','<<sum;file<<'\n';
            if(r.stage=="physical_precheck")for(unsigned lane=0;lane<64;++lane)phases<<index+1<<','<<c.physical<<','<<r.recovery<<','<<lane%8<<','<<lane/8<<','<<phaseTotal[lane]<<','<<phaseCounts[lane]<<','<<(phaseTotal[lane]?double(phaseCounts[lane])/double(phaseTotal[lane]):0)<<','<<(lane==0)<<'\n';
        }
        std::ofstream note(prefix+".flow-audit.json");
        note<<"{\n  \"measurementClass\":\"probe_gpu_audit_postrun_readback\",\n  \"auditFirstFrames\":"<<first<<",\n  \"auditEveryFrames\":"<<every<<",\n  \"auditRecords\":"<<records.size()<<",\n  \"gtForAuditOnly\":true,\n  \"gtTranslationPixelsPerCapture\":"<<(translation?2:0)<<",\n  \"gtReachesProduct\":false,\n  \"readbacksDuringProductLoop\":false,\n  \"missingDistanceSentinel\":-9223372036854775808,\n  \"notes\":[\"Extra audit dispatches alter GPU scheduling; these runs are diagnostic, not performance baselines.\",\"Pair confidence is the provider-published dense confidence. NVIDIA may already combine cost and FB consistency; AMD uses the documented fixed .65.\",\"FB is recomputed from the exported dense forward/reverse fields, not the raw NVOF grid/cost.\",\"Pair counters are dense. precheck-phases.csv separates all 64 phases; product phase is offset (0,0).\",\"Target compose reuses product SourceWeight and reads the actual R8 raw validity before Fill; float-vs-R8 differences are explicit.\",\"No-confidence/no-photometric counters are audit-only factor ablations, not engine changes.\",\"GT EPE and GT warp error use only interior pixels; counts include their GT denominator. Other count fractions use pixels_count.\",\"Full/lite history frame columns name their last submitted inference input, not a guaranteed complete current NR endpoint.\"]\n}\n";
    }
};
inline Observer* observer=nullptr;
}
