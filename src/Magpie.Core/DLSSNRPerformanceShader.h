#pragma once
#include "DLSSNRLocalHistoryShader.h"
#include <string>

namespace Magpie {
// All history samples stay in their original source coordinates. Motion is
// composed by sampling each older vector at the already displaced position.
inline const std::string DLSSNR_PERFORMANCE_HLSL = std::string(DLSSNR_LOCAL_HISTORY_HLSL) + R"hlsl(
cbuffer Settings : register(b0) {
    uint2 Extent;
    uint2 SourceExtent;
    uint Age;
    uint Reproject;
    uint HasHistory;
    uint ProcessingMode;
    uint ConfidenceBits;
    uint RecoverNvidiaResidual;
    uint UseSourceGuide;
    uint Reserved;
};
Texture2D<float4> CurrentColor : register(t0);
Texture2D<float4> HistoryDelta : register(t1);
Texture2D<float4> HistoryColor : register(t2);
Texture2D<float2> Motion0 : register(t3);
Texture2D<float2> Motion1 : register(t4);
Texture2D<float2> Motion2 : register(t5);
Texture2D<float2> Motion3 : register(t6);
Texture2D<float> Confidence0 : register(t7);
Texture2D<float> Confidence1 : register(t8);
Texture2D<float> Confidence2 : register(t9);
Texture2D<float> Confidence3 : register(t10);
Texture2D<float4> FullOutput : register(t11);
Texture2D<float4> CurrentSourceColor : register(t12);
Texture2D<float4> HistorySourceColor : register(t13);
SamplerState LinearClamp : register(s0);
RWTexture2D<float4> Output : register(u0);
RWTexture2D<float> Validity : register(u1);
RWByteAddressBuffer Invalid : register(u2);
RWTexture2D<float2> MotionOut : register(u3);

float2 Displace(float2 uv, out float motionConfidence, out float reuseAcceptance, out bool supported) {
    motionConfidence = reuseAcceptance = 1;
    supported = true;
    if (!Reproject) return uv;
    [unroll] for (uint i=0;i<4;++i) {
        if (i>=Age) break;
        float2 mv = i==0 ? Motion0.SampleLevel(LinearClamp,uv,0) :
            i==1 ? Motion1.SampleLevel(LinearClamp,uv,0) :
            i==2 ? Motion2.SampleLevel(LinearClamp,uv,0) : Motion3.SampleLevel(LinearClamp,uv,0);
        float confidence = (ConfidenceBits & (1u<<i)) == 0 ? 1 :
            i==0 ? Confidence0.SampleLevel(LinearClamp,uv,0) :
            i==1 ? Confidence1.SampleLevel(LinearClamp,uv,0) :
            i==2 ? Confidence2.SampleLevel(LinearClamp,uv,0) : Confidence3.SampleLevel(LinearClamp,uv,0);
        if (!all(isfinite(mv)) || !isfinite(confidence)) {
            motionConfidence=reuseAcceptance=0; supported=false; return uv;
        }
        motionConfidence *= saturate(confidence);
        // Provider confidence is not NR strength: AMD's valid-motion baseline
        // is .65 even on a static image. Accept coherent motion at full strength
        // while retaining a soft rejection band for weak evidence. Keep raw
        // confidence separate for the motion field submitted to NGX.
        reuseAcceptance *= smoothstep(.1,.5,confidence);
        uv += mv/float2(SourceExtent);
        if (any(uv < .5/float2(SourceExtent)) || any(uv > 1-.5/float2(SourceExtent))) {
            motionConfidence=reuseAcceptance=0; supported=false; return uv;
        }
    }
    return uv;
}
float2 Displace(float2 uv, out float motionConfidence, out float reuseAcceptance) {
    bool supported;
    return Displace(uv,motionConfidence,reuseAcceptance,supported);
}
float3 CurrentGuideColor(uint2 p, bool sourceGuide) {
    return sourceGuide ? CurrentSourceColor.SampleLevel(LinearClamp,(p+.5)/float2(Extent),0).rgb :
                         CurrentColor.Load(int3(p,0)).rgb;
}
float3 HistoryGuideColor(float2 uv, bool sourceGuide) {
    return sourceGuide ? HistorySourceColor.SampleLevel(LinearClamp,uv,0).rgb :
                         HistoryColor.SampleLevel(LinearClamp,uv,0).rgb;
}
float HistoryWeight(uint2 p, out float2 historyUV, out bool supported) {
    historyUV=(p+.5)/float2(Extent);
    supported=false;
    if (!HasHistory) return 0;
    if (Age==0) {
        supported=all(isfinite(HistoryDelta.Load(int3(p,0)).rgb));
        return supported ? 1 : 0;
    }
    float motionConfidence, trust;
    historyUV=Displace(historyUV,motionConfidence,trust,supported);
    if (!supported) return 0;
    supported=false;
    if (!all(isfinite(HistoryDelta.SampleLevel(LinearClamp,historyUV,0).rgb))) return 0;
    float3 current=CurrentColor.Load(int3(p,0)).rgb;
    float3 previous=HistoryColor.SampleLevel(LinearClamp,historyUV,0).rgb;
    if (!all(isfinite(current)) || !all(isfinite(previous))) return 0;
    supported=true;
    float error=LocalHistoryColorError(current,previous);
    // Image-only confidence is deliberately conservative: no surface/depth claim.
    return trust*(1-smoothstep(.035,.12,error));
}
float HistoryWeight(uint2 p, out float2 historyUV) {
    bool supported;
    return HistoryWeight(p,historyUV,supported);
}

bool StationaryGuideMatch(uint2 pixel, float3 current, bool sourceGuide) {
    if (any(pixel==0) || any(pixel+1>=Extent)) return false;
    const float2 uv=(pixel+.5)/float2(Extent);
    if (LocalHistoryColorError(current,HistoryGuideColor(uv,sourceGuide))>LocalHistoryExactError ||
        !all(isfinite(HistoryDelta.Load(int3(pixel,0)).rgb))) return false;
    // Match the complete neighborhood so a same-colored center on a moving
    // edge cannot recover the old surface's enhancement.
    [unroll] for (int y=-1;y<=1;++y) {
        [unroll] for (int x=-1;x<=1;++x) {
            if (x==0 && y==0) continue;
            uint2 p=int2(pixel)+int2(x,y);
            float2 previous=(p+.5)/float2(Extent);
            if (LocalHistoryColorError(CurrentGuideColor(p,sourceGuide),
                                       HistoryGuideColor(previous,sourceGuide))>LocalHistoryExactError) return false;
        }
    }
    return all(isfinite(current));
}

float3 ContinuousResidual(uint2 p, float2 historyUV, float3 history, float weight, bool supported) {
    if (!supported) return 0;
    if (Age==0 || weight>=1) return history;
    // This is an actual binomial low-pass of the immutable source residual.
    // Weak evidence retains only bounded slow correction; full-frequency detail
    // returns with pixel trust. Neither this output nor its low-pass is a new NR
    // observation, so the host's original source-age/epoch limit remains active.
    float3 current=CurrentColor.Load(int3(p,0)).rgb;
    float3 previous=HistoryColor.SampleLevel(LinearClamp,historyUV,0).rgb;
    float3 difference=abs(current-previous);
    float support=1-smoothstep(.12,.25,max(difference.r,max(difference.g,difference.b)));
    float3 low=0;
    float total=0;
    [unroll] for (int y=-1;y<=1;++y) {
        [unroll] for (int x=-1;x<=1;++x) {
            float2 uv=historyUV+float2(x,y)/float2(Extent);
            if (any(uv<.5/float2(Extent)) || any(uv>1-.5/float2(Extent))) continue;
            float3 value=HistoryDelta.SampleLevel(LinearClamp,uv,0).rgb;
            float3 reference=HistoryColor.SampleLevel(LinearClamp,uv,0).rgb;
            if (!all(isfinite(value)) || !all(isfinite(reference))) continue;
            float3 localDifference=abs(current-reference);
            float localSupport=1-smoothstep(.12,.25,max(localDifference.r,max(localDifference.g,localDifference.b)));
            float tap=(x==0 ? 2 : 1)*(y==0 ? 2 : 1)*localSupport;
            low+=value*tap;
            total+=tap;
        }
    }
    low=total>0 ? clamp(low/total,-.08,.08)*(.5*support) : 0;
    return lerp(low,history,weight);
}
[numthreads(8,8,1)]
void PublishEnhancement(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy>=Extent)) return;
    float3 residual=FullOutput.Load(int3(tid.xy,0)).rgb-CurrentColor.Load(int3(tid.xy,0)).rgb;
    Output[tid.xy]=float4(residual,0);
}
groupshared uint GroupInvalid;
[numthreads(8,8,1)]
void CheckReuse(uint3 tid : SV_DispatchThreadID, uint lane : SV_GroupIndex) {
    if (lane==0) GroupInvalid=0;
    GroupMemoryBarrierWithGroupSync();
    if (all(tid.xy<Extent)) {
        float2 uv;
        float weight=HistoryWeight(tid.xy,uv);
        if (weight<1) InterlockedOr(GroupInvalid,1);
    }
    GroupMemoryBarrierWithGroupSync();
    if (lane==0 && GroupInvalid) { uint unused; Invalid.InterlockedOr(0,1,unused); }
}
void ComposePixel(uint2 pixel, uint2 group, uint lane, bool allowRecovery) {
    bool active=all(pixel<Extent);
    bool sourceGuide=allowRecovery && UseSourceGuide;
    bool supported=false;
    float2 historyUV=0;
    float weight=active ? HistoryWeight(pixel,historyUV,supported) : 0;
    float3 current=active ? CurrentColor.Load(int3(pixel,0)).rgb : 0;
    if (allowRecovery && HasHistory && Reproject && Age>0) {
        float2 offset;
        float patchError;
        float3 guide=active ? (sourceGuide ? CurrentGuideColor(pixel,true) : current) : 0;
        const bool needsPatch=weight<.75;
        if (active && needsPatch && StationaryGuideMatch(pixel,guide,sourceGuide)) {
            historyUV=(pixel+.5)/float2(Extent);
            weight=1;
            supported=true;
        }
        // Every lane participates in the tile search. Each output pixel still
        // needs its own color match before accepting the recovered alignment.
        bool rejected=active && weight<1;
        const LocalHistoryPatch patch={int2(group*8),Extent,uint4(0,0,Extent),float2(Extent)/float2(SourceExtent),
                                       int(min(6u,3*Age)),false,sourceGuide};
        bool found;
        if (sourceGuide)
            found=FindLocalHistoryOffset(HistorySourceColor,LinearClamp,patch,lane,guide,needsPatch,offset,patchError);
        else
            found=FindLocalHistoryOffset(HistoryColor,LinearClamp,patch,lane,guide,needsPatch,offset,patchError);
        if (found && rejected) {
            float2 previous=pixel+offset;
            if (all(previous>=0) && all(previous<=float2(Extent)-1)) {
                float2 uv=(previous+.5)/float2(Extent);
                float pixelError=LocalHistoryColorError(guide,HistoryGuideColor(uv,sourceGuide));
                float recovered=LocalHistoryTrust(max(patchError,pixelError));
                if (recovered>weight && all(isfinite(HistoryDelta.SampleLevel(LinearClamp,uv,0).rgb))) {
                    historyUV=uv;
                    weight=recovered;
                    supported=true;
                }
            }
        }
    }
    if (!active) return;
    bool continuous=ProcessingMode==1 || ProcessingMode==2;
    float3 history=weight>0 || (continuous && supported) ? HistoryDelta.SampleLevel(LinearClamp,historyUV,0).rgb : 0;
    if (!all(isfinite(history))) { history=0; weight=0; }
    float3 residual=continuous ? ContinuousResidual(pixel,historyUV,history,weight,supported) : history*weight;
    Output[pixel]=float4(current+residual,1);
    Validity[pixel]=weight;
}
[numthreads(8,8,1)]
void ComposeCurrent(uint3 tid : SV_DispatchThreadID) {
    ComposePixel(tid.xy,0,0,false);
}
[numthreads(8,8,1)]
void ComposeNvidiaAlternating(uint3 tid : SV_DispatchThreadID, uint3 group : SV_GroupID, uint lane : SV_GroupIndex) {
    ComposePixel(tid.xy,group.xy,lane,true);
}
[numthreads(8,8,1)]
void MeasureCoverage(uint3 tid : SV_DispatchThreadID) {
    uint2 p=tid.xy*8;
    if (any(p>=Extent)) return;
    float2 uv;
    float weight=RecoverNvidiaResidual ? FullOutput.Load(int3(p,0)).r : HistoryWeight(p,uv);
    uint unused;
    Invalid.InterlockedAdd(0,1,unused);
    if (weight<=0) Invalid.InterlockedAdd(4,1,unused);
    Invalid.InterlockedAdd(8,(uint)round(saturate(weight)*1024),unused);
}
[numthreads(8,8,1)]
void ComposeMotion(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy>=Extent)) return;
    float2 uv=(tid.xy+.5)/float2(Extent);
    float trust, reuseAcceptance;
    float2 previous=Displace(uv,trust,reuseAcceptance);
    MotionOut[tid.xy]=(previous-uv)*float2(SourceExtent);
    Validity[tid.xy]=trust;
}
)hlsl";
}
