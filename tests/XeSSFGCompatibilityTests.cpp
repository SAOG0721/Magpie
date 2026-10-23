#include <windows.h>
#include <cassert>
#include <iostream>
#include <map>
#include <string>
#include <vector>
#include "../src/Magpie.Core/include/XeSSFGParameters.h"
#include "../src/Magpie.Core/include/OpticalFlowDefaults.h"
#include "../src/Magpie.Core/XeSSFGTiming.h"
#include "../src/Magpie.Core/XeSSFGCompatibility.h"

using namespace Magpie;
using namespace Magpie::XeSSFGCompatibility;
struct Effect { std::wstring name; std::map<std::wstring, float> parameters; int scale = 7; };
struct Mode { std::wstring name; std::vector<Effect> effects; };

static void Migration() {
    for (const auto* id : {L"XeSSFG\\XeSS_FrameGeneration_x2_ZeroMV", L"XeSSFG\\XeSS_MultiFrameGeneration_ZeroMV"}) {
        for (int method = 0; method < 3; ++method) for (int amd = 0; amd < 2; ++amd) for (int nv = 1; nv <= 5; ++nv) {
            std::vector<Mode> modes{{L"Custom", {{L"First"}, {id, {{L"multiplier",4.0f},
                {L"opticalFlowMethod",float(method)}, {L"amdOpticalFlowMode",float(amd)}, {L"nvidiaOpticalFlowQuality",float(nv)}}},
                {L"Last"}, {id}}}};
            assert(MigrateXeSSFGEffects(modes));
            uint32_t oldFlowVersion = 0;
            ApplyOpticalFlowDefaultsMigration(modes, oldFlowVersion);
            auto& e = modes[0].effects[1];
            assert(modes.size()==1 && modes[0].name==L"Custom" && modes[0].effects.size()==3);
            assert(e.name==XESS_FG_EFFECT && e.scale==7 && e.parameters[L"multiplier"]==2);
            assert(e.parameters[L"opticalFlowMethod"]==method && e.parameters[L"amdOpticalFlowMode"]==amd && e.parameters[L"nvidiaOpticalFlowQuality"]==nv);
            e.parameters[L"multiplier"] = 4;
            assert(!MigrateXeSSFGEffects(modes));
            assert(e.parameters[L"multiplier"]==4);
        }
        std::vector<Mode> missing{{L"Missing", {{id}}}};
        assert(MigrateXeSSFGEffects(missing));
        assert(missing[0].effects[0].parameters[L"opticalFlowMethod"]==0);
    }
    std::vector<Mode> fresh{{L"New", {{std::wstring(XESS_FG_EFFECT)}}}};
    assert(MigrateXeSSFGEffects(fresh));
    auto& p=fresh[0].effects[0].parameters;
    assert(p[L"multiplier"]==2 && p[L"opticalFlowMethod"]==1 && p[L"amdOpticalFlowMode"]==1);
    p[L"opticalFlowMethod"] = NAN;
    p[L"multiplier"] = 3.5f;
    assert(MigrateXeSSFGEffects(fresh));
    assert(p[L"opticalFlowMethod"]==1 && p[L"multiplier"]==2);
}
struct FaultBackend {
    std::array<uint8_t, 8> memory{};
    int reject = -1, fail = -1, rollbackFail = -1, writes = 0;
    bool Preflight(Patch& p) noexcept { return int(p.rva)!=reject && memory[p.rva]==p.original[0]; }
    bool Write(Patch& p, bool restore) noexcept {
        ++writes;
        memory[p.rva] = restore ? p.original[0] : p.replacement[0];
        return int(p.rva)!=(restore?rollbackFail:fail);
    }
};
static void Transactions() {
    std::array<Patch,8> patches{};
    for(uint32_t i=0;i<8;++i) { patches[i].rva=i; patches[i].size=1; patches[i].replacement[0]=9; }
    for(int i=0;i<8;++i) {
        FaultBackend rejected; rejected.reject=i;
        assert(InstallPatches(patches,rejected)==PatchResult::Rejected && rejected.writes==0);
        FaultBackend failed; failed.fail=i;
        assert(InstallPatches(patches,failed)==PatchResult::RolledBack);
        for(auto v:failed.memory) assert(v==0);
        failed.rollbackFail=i;
        assert(InstallPatches(patches,failed)==PatchResult::Poisoned);
    }
    FaultBackend ok;
    assert(InstallPatches(patches,ok)==PatchResult::Success);
    assert(RestorePatches(patches,ok));
    for(auto v:ok.memory) assert(v==0);
}
static void Timing() {
    XeSSFGTiming clock;
    assert(clock.Submit({1,1,1,100000},100,0).reset);
    auto x=clock.Submit({2,1,1,300000},160,40);
    assert(x.captured && x.sourceMs==20 && x.fedMs==20); // independent capture ignores Present wait
    x=clock.Submit({4,1,1,700000},220,40);
    assert(!x.reset && !x.captured && x.sourceMs==40 && x.fedMs==20); // skipped frame, same session
    assert(clock.Submit({5,2,1,900000},240,0).reset); // capture restarted
    x=clock.Submit({6,2,1,1100000},260,0);
    assert(!x.reset && x.captured && x.fedMs==20); // recover on the next frame
    assert(clock.Submit({7,2,2,1300000},280,0).reset); // resource recreated
    assert(clock.Submit({8,2,2,1200000},300,0).reset); // time goes backwards
    assert(clock.Submit({9,2,2,1400000},1000,0).reset); // long pause
    clock.Reset();
    assert(clock.Submit({1,1,1,0},100,0).reset);
    assert(clock.Submit({2,1,1,0},150,30).fedMs==20);
    assert(clock.Submit({2,1,1,0},170,0).reset); // repeated source identity
    clock.Reset();
    unsigned resets = 0;
    for (uint64_t frame = 1; frame <= 120; ++frame) {
        x = clock.Submit({frame,1,1,1000000 + static_cast<int64_t>(frame)*166667},
            1000 + static_cast<double>(frame)*16.6667, 0);
        resets += x.reset;
        if (frame > 1) assert(x.captured && std::abs(x.fedMs-16.6667)<0.0001);
    }
    assert(resets==1); // production capture sequence remains constant for all 120 frames
    // Startup outlier, real low FPS, sustained rate change and pause recovery.
    for (const int period : {16, 25, 33, 100, 250}) {
        clock.Reset();
        int64_t timestamp = 1000000;
        double now = 1000;
        clock.Submit({1,1,1,timestamp},now,0);
        timestamp += 4000000; now += 400;
        assert(clock.Submit({2,1,1,timestamp},now,0).fedMs == 400);
        for (uint64_t f = 3; f <= 14; ++f) {
            timestamp += period * 10000; now += period;
            assert(clock.Submit({f,1,1,timestamp},now,0).fedMs == period);
        }
        for (uint64_t f = 15; f <= 25; ++f) {
            timestamp += 2000000; now += 200;
            x = clock.Submit({f,1,1,timestamp},now,0);
        }
        assert(x.fedMs == 200); // no permanent clamp to the earlier fast rate
        timestamp += 10000000; now += 1000;
        assert(clock.Submit({26,1,1,timestamp},now,0).reset);
        timestamp += period * 10000; now += period;
        assert(clock.Submit({27,1,1,timestamp},now,0).fedMs == period);
    }
}
static void* NativeTimestamp(void*, int64_t* out, void*, void*, uint32_t, uint32_t) { *out=100000000; return out; }
static void Deadlines() {
    Pacing::g_enabled = true;
    Pacing::g_tsNative=&NativeTimestamp;
    QueryPerformanceFrequency(&Pacing::g_freq);
    std::array<uint8_t, 0x400> ctx{};
    float measured=4;
    memcpy(ctx.data()+Pacing::RingOffset+Pacing::RingMeasuredOffset,&measured,4);
    Pacing::EnterContext(ctx.data());
    int64_t timing[2]{9,20000000}, out=0;
    Pacing::TsDetour(nullptr,&out,nullptr,timing,1,4);
    assert(out==104000000); // restores 5 ms step versus native 1 ms clamp
    Pacing::TsDetour(nullptr,&out,nullptr,timing,2,4);
    assert(out==109000000);
    Pacing::sourcePeriodNs.store(16000000);
    timing[1]=200000000; // inflated SDK ring must not stretch the source cadence
    Pacing::TsDetour(nullptr,&out,nullptr,timing,1,4);
    assert(out==103000000);
    Pacing::TsDetour(nullptr,&out,nullptr,timing,2,4);
    assert(out==107000000);
    // Delay beyond the whole burst: deadlines stay in the SDK time domain.
    // The old QPC reanchor appended a fresh full interval after this delay.
    Sleep(30);
    Pacing::TsDetour(nullptr,&out,nullptr,timing,3,4);
    assert(out==111000000);
    Pacing::TsDetour(nullptr,&out,nullptr,timing,1,4);
    assert(out==103000000); // next burst has no inherited lateness
    Pacing::TsDetour(nullptr,&out,nullptr,timing,3,4);
    assert(out==111000000); // skipped index preserves the original slot
    timing[1] = 24000000;
    Pacing::sourcePeriodNs.store(24000000);
    Pacing::TsDetour(nullptr,&out,nullptr,timing,1,3);
    assert(out==106666667);
    Sleep(30);
    Pacing::TsDetour(nullptr,&out,nullptr,timing,2,3);
    assert(out==114666667);
    // Fallback must also leave expired slots expired, not add a full interval.
    Pacing::g_intervalQpc = Pacing::QpcFromNs(10000000);
    Pacing::g_periodNs = 30000000;
    LARGE_INTEGER now{}; QueryPerformanceCounter(&now);
    const auto expired = now.QuadPart - Pacing::QpcFromNs(100000000);
    Pacing::g_targetQpc = expired;
    Pacing::PaceFrame(2,3);
    assert(Pacing::g_targetQpc == expired + Pacing::g_intervalQpc);
    Pacing::sourcePeriodNs.store(0);
    Pacing::resetEpoch.fetch_add(1);
    Pacing::EnterContext(ctx.data());
    assert(Pacing::g_nextDeadlineNs==0 && Pacing::g_lastTsIndex==0);
    timing[1]=20000000;
    Pacing::TsDetour(nullptr,&out,nullptr,timing,2,4);
    assert(out==108000000); // first observed callback can be a later slot
    Pacing::TsDetour(nullptr,&out,nullptr,timing,3,3);
    assert(out==100000000); // invalid slot must retain native output
    Pacing::g_enabled = false;
}
static void Runtime(const wchar_t* path) {
    HMODULE module=LoadLibraryW(path);
    assert(module && KnownFile(module));
    const auto* entry=reinterpret_cast<const void*>(GetProcAddress(module,"xefgSwapChainD3D12CreateContext"));
    ImageBackend backend{reinterpret_cast<uint8_t*>(module),0x015ED000};
    for(uint32_t multiplier:{3u,4u,3u,4u}) {
        Lease lease, duplicate;
        assert(lease.Acquire(true,multiplier,entry));
        assert(!duplicate.Acquire(false,2,entry));
        assert(lease.Release(true));
        auto original=MakePatches(multiplier-1);
        for(auto& p:original) assert(backend.Preflight(p));
        assert(duplicate.Acquire(false,2,entry));
        assert(duplicate.ObservesOutput() && !duplicate.Patched() && !Pacing::g_enabled);
        for(size_t i=0;i<original.size();++i)if(i!=5)assert(backend.Preflight(original[i]));
        assert(duplicate.Release(true));
        for(auto& p:original)assert(backend.Preflight(p));
    }
    Lease unknown;
    assert(!unknown.Acquire(true,4,reinterpret_cast<const void*>(&Runtime)));
    assert(!unknown.Acquire(true,5,entry));
    assert(unknown.Acquire(false,2,reinterpret_cast<const void*>(&Runtime)));
    assert(!unknown.ObservesOutput() && !unknown.Patched());
    assert(unknown.Release(true));
    Lease uncertain;
    assert(uncertain.Acquire(false,2,entry));
    assert(!uncertain.Release(false));
    assert(!unknown.Acquire(false,2,entry));
    FreeLibrary(module);
}
int wmain(int argc,wchar_t** argv) {
    Migration(); Transactions(); Timing(); Deadlines();
    if(argc>1) Runtime(argv[1]);
    std::cout<<"XeSSFG migration, rollback faults, timing, deadlines and requested runtime checks passed\n";
}
