#pragma once
#include <string_view>

namespace Magpie {
inline constexpr std::string_view DLSSNR_SPARSE_HLSL = R"hlsl(
cbuffer SparseSettings : register(b0) {
    uint2 Size; uint MapCount; uint ConfidenceBits;
    uint InferenceMap; uint HasPast; uint Finalize; uint Reserved;
};
Texture2D<float4> Target : register(t0);
Texture2D<float4> PastReference : register(t1);
Texture2D<float4> PastResidual : register(t2);
Texture2D<float2> PastMotion : register(t3);
Texture2D<float> PastConfidence : register(t4);
Texture2D<float2> Steps[8] : register(t5);
Texture2D<float> StepConfidence[8] : register(t13);
Texture2D<float4> RawResidual : register(t21);
Texture2D<float> RawValidity : register(t22);
SamplerState LinearClamp : register(s0);
RWTexture2D<float4> ColorOut : register(u0);
RWTexture2D<float> ValidityOut : register(u1);
RWByteAddressBuffer Counts : register(u2);
RWTexture2D<float2> MotionOut : register(u3);
RWTexture2D<float> ConfidenceOut : register(u4);

bool Inside(float2 uv) { return all(uv>=.5/float2(Size)) && all(uv<=1-.5/float2(Size)); }
// All decisions use the value stored in the R8_UNORM validity texture.
float QuantizedTrust(float value) { return isfinite(value)?round(saturate(value)*255)/255:0; }
[numthreads(8,8,1)] void MapMotion(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy>=Size)) return;
    float2 start=(id.xy+.5)/float2(Size),uv=start;
    float trust=1;
    [unroll] for (uint i=0;i<8;++i) {
        if (i>=MapCount) break;
        float2 mv=Steps[i].SampleLevel(LinearClamp,uv,0);
        float c=(ConfidenceBits&(1u<<i))?StepConfidence[i].SampleLevel(LinearClamp,uv,0):1;
        if (!all(isfinite(mv)) || !isfinite(c)) { trust=0; break; }
        trust*=InferenceMap?saturate(c):smoothstep(.1,.5,c);
        uv+=mv/float2(Size);
        if (!Inside(uv)) { trust=0; break; }
    }
    MotionOut[id.xy]=(uv-start)*float2(Size);
    ConfidenceOut[id.xy]=trust;
}
float SourceWeight(float2 uv,float3 current,out float3 correction) {
    correction=0;
    float c=PastConfidence.SampleLevel(LinearClamp,uv,0);
    float2 previous=uv+PastMotion.SampleLevel(LinearClamp,uv,0)/float2(Size);
    if (!isfinite(c) || c<=0 || !Inside(previous) || !all(isfinite(current))) return 0;
    float3 old=PastReference.SampleLevel(LinearClamp,previous,0).rgb;
    correction=PastResidual.SampleLevel(LinearClamp,previous,0).rgb;
    if (!all(isfinite(old)) || !all(isfinite(correction))) { correction=0; return 0; }
    float3 diff=abs(current-old);
    return c*(1-smoothstep(.035,.12,max(diff.r,max(diff.g,diff.b))));
}

groupshared uint ContributingCount;
[numthreads(8,8,1)] void ComposeResidual(uint3 id : SV_DispatchThreadID,uint lane:SV_GroupIndex) {
    if (lane==0) ContributingCount=0;
    GroupMemoryBarrierWithGroupSync();
    if (all(id.xy<Size)) {
        float2 uv=(id.xy+.5)/float2(Size);
        float3 now=Target.Load(int3(id.xy,0)).rgb,correction=0;
        float trust=QuantizedTrust(HasPast?SourceWeight(uv,now,correction):0);
        ColorOut[id.xy]=float4(correction,0); ValidityOut[id.xy]=trust;
        if (trust>0) InterlockedAdd(ContributingCount,1);
    }
    GroupMemoryBarrierWithGroupSync();
    if (lane==0 && ContributingCount) { uint unused; Counts.InterlockedAdd(0,ContributingCount,unused); }
}
// Keep the real R16/R8 compose boundary. In particular, the GPU's R8
// normalization can differ by one float ULP from shader division by 255.
[numthreads(8,8,1)] void FillOutput(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy>=Size)) return;
    float trust=RawValidity.Load(int3(id.xy,0));
    float3 correction=RawResidual.Load(int3(id.xy,0)).rgb*trust;
    // Preserve the old filled-colour FP16 store before final gamut mapping.
    // Keep the legacy expression's contraction rules; adding precise changes
    // the rounded FP16 result on NVIDIA at multiplication/addition boundaries.
    float3 filled=Target.Load(int3(id.xy,0)).rgb+correction;
    float3 storedColor=f16tof32(f32tof16(filled));
    ColorOut[id.xy]=float4(Finalize?FinalizeSDRColor(storedColor):storedColor,1);
    ValidityOut[id.xy]=trust;
}
// Staged entrypoints are retained for production-kernel equivalence tests.
[numthreads(8,8,1)] void FillResidual(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy>=Size)) return;
    float trust=RawValidity.Load(int3(id.xy,0));
    float3 correction=RawResidual.Load(int3(id.xy,0)).rgb*trust;
    // Unsupported pixels keep the current target's raw colour. A missing
    // mapping never borrows a spatial correction or another endpoint.
    ColorOut[id.xy]=float4(Target.Load(int3(id.xy,0)).rgb+correction,1);
    ValidityOut[id.xy]=trust;
}
[numthreads(8,8,1)] void ExtractResidual(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy>=Size)) return;
    ColorOut[id.xy]=float4(PastReference.Load(int3(id.xy,0)).rgb-Target.Load(int3(id.xy,0)).rgb,0);
}
[numthreads(8,8,1)] void OutputColor(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy>=Size)) return;
    float4 color=RawResidual.Load(int3(id.xy,0));
    ColorOut[id.xy]=float4(Finalize?FinalizeSDRColor(color.rgb):color.rgb,color.a);
    ValidityOut[id.xy]=RawValidity.Load(int3(id.xy,0));
}
)hlsl";
}
