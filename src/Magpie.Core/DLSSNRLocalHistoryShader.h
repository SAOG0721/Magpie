#pragma once
#include <string_view>

namespace Magpie {
// The caller supplies colors from both frames in the same space. All lanes
// in an 8x8 tile call together before any pixel can exit the shader.
inline constexpr std::string_view DLSSNR_LOCAL_HISTORY_HLSL = R"hlsl(
static const float LocalHistoryExactError=2.0/255.0;
static const float LocalHistorySoftLimit=8.0/255.0;
static const int2 LocalHistoryTaps[4]={int2(2,2),int2(5,2),int2(2,5),int2(5,5)};
groupshared uint LocalHistoryRequests;
groupshared uint LocalHistoryBestOffset;
groupshared uint LocalHistoryBestRefinement;
groupshared float3 LocalHistoryCurrent[4];

struct LocalHistoryPatch {
    int2 tile;
    uint2 extent;
    uint4 region;
    float2 scale;
    int radius;
    bool guideHasValidity;
    bool normalizedGuide;
};
bool IsLocalHistoryAnchor(uint lane) {
    uint2 local=uint2(lane%8,lane/8);
    return all((local==2)|(local==5));
}
float LocalHistoryColorError(float3 a, float3 b) {
    float3 difference=abs(a-b);
    return max(difference.r,max(difference.g,difference.b));
}
float LocalHistoryTrust(float error) {
    return 1-smoothstep(LocalHistoryExactError,LocalHistorySoftLimit,error);
}
float LocalHistoryPatchError(Texture2D<float4> history, SamplerState linearSampler, LocalHistoryPatch patch, float2 offset) {
    float2 first=patch.tile+LocalHistoryTaps[0]+offset;
    float2 last=patch.tile+LocalHistoryTaps[3]+offset;
    if (any(first<float2(patch.region.xy)) || any(last>float2(patch.region.zw)-1)) return 1;
    float error=0;
    [unroll] for (uint tap=0;tap<4;++tap) {
        float2 pos=patch.tile+LocalHistoryTaps[tap]+offset;
        float4 previous=!patch.normalizedGuide && all(patch.scale==1) && all(frac(offset)==0) ?
            history.Load(int3(pos,0)) : history.SampleLevel(linearSampler,(pos+.5)/float2(patch.extent),0);
        if (patch.guideHasValidity && (!all(isfinite(previous)) || previous.a<.999)) return 1;
        error=max(error,LocalHistoryColorError(LocalHistoryCurrent[tap],previous.rgb));
    }
    return error;
}
uint LocalHistoryScore(float error, uint distance, uint index) {
    return (uint(round(saturate(error)*4095))<<16)|(distance<<8)|index;
}
float LocalHistoryScoreError(uint score) {
    return float(score>>16)/4095.0;
}
bool FindLocalHistoryOffset(Texture2D<float4> history, SamplerState linearSampler, LocalHistoryPatch patch,
                           uint lane, float3 current, bool needsRecovery, out float2 offset, out float patchError) {
    offset=0;
    patchError=1;
    if (any(patch.tile+LocalHistoryTaps[0]<int2(patch.region.xy)) ||
        any(patch.tile+LocalHistoryTaps[3]>=int2(patch.region.zw))) return false;
    if (lane==0) {
        LocalHistoryRequests=0;
        LocalHistoryBestOffset=0xffffffff;
        LocalHistoryBestRefinement=0xffffffff;
    }
    uint2 local=uint2(lane%8,lane/8);
    bool anchor=IsLocalHistoryAnchor(lane);
    if (anchor) LocalHistoryCurrent[uint(local.x==5)+2*uint(local.y==5)]=current;
    GroupMemoryBarrierWithGroupSync();
    if (anchor && needsRecovery) InterlockedAdd(LocalHistoryRequests,1);
    GroupMemoryBarrierWithGroupSync();
    if (LocalHistoryRequests<2) return false;
    float spread=max(LocalHistoryColorError(LocalHistoryCurrent[0],LocalHistoryCurrent[1]),
                 max(LocalHistoryColorError(LocalHistoryCurrent[0],LocalHistoryCurrent[2]),
                     LocalHistoryColorError(LocalHistoryCurrent[0],LocalHistoryCurrent[3])));
    if (!isfinite(spread) || spread<3*LocalHistoryExactError) return false;

    // Search in source-pixel units. An integer displacement in the source
    // can be fractional in a downsampled reconstruction.
    const uint side=uint(2*patch.radius+1);
    [loop] for (uint candidate=lane;candidate<side*side;candidate+=64) {
        int2 delta=int2(candidate%side,candidate/side)-patch.radius;
        float error=LocalHistoryPatchError(history,linearSampler,patch,delta*patch.scale);
        InterlockedMin(LocalHistoryBestOffset,LocalHistoryScore(error,uint(dot(float2(delta),float2(delta))),candidate));
    }
    GroupMemoryBarrierWithGroupSync();
    uint chosen=LocalHistoryBestOffset&255;
    int2 coarse=int2(chosen%side,chosen/side)-patch.radius;
    offset=coarse*patch.scale;
    patchError=LocalHistoryScoreError(LocalHistoryBestOffset);
    if (patchError<=LocalHistoryExactError) return true;

    // Keep the best coarse match even when it misses the final threshold.
    // Half-pixel refinement can turn that nearest match into a valid one.
    if (lane<9) {
        int2 subpixel=int2(lane%3,lane/3)-1;
        float2 delta=coarse+.5*subpixel;
        if (all(abs(delta)<=patch.radius)) {
            float error=LocalHistoryPatchError(history,linearSampler,patch,delta*patch.scale);
            uint distance=uint(4*dot(delta,delta));
            InterlockedMin(LocalHistoryBestRefinement,LocalHistoryScore(error,min(distance,255u),lane));
        }
    }
    GroupMemoryBarrierWithGroupSync();
    float refinedError=LocalHistoryScoreError(LocalHistoryBestRefinement);
    if (refinedError<patchError) {
        uint refined=LocalHistoryBestRefinement&255;
        offset=(coarse+.5*(int2(refined%3,refined/3)-1))*patch.scale;
        patchError=refinedError;
    }
    return patchError<min(LocalHistorySoftLimit,.5*spread);
}
)hlsl";
}
