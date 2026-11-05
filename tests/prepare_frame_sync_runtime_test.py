"""Test the production renderer limiter configuration without any graphics API."""
from pathlib import Path
import sys

repo = Path(__file__).resolve().parents[1]
output = Path(sys.argv[1])
source = (repo/'src/Magpie.Core/Renderer.cpp').read_text(encoding='utf-8-sig')
header = (repo/'src/Magpie.Core/Renderer.h').read_text(encoding='utf-8-sig')

def function(signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

prefix = r'''
#include "FramePacingOptions.h"
#include "FrameRefreshSettings.h"
#include "FramePresentationTiming.h"
#include "ReflexController.h"
#include <atomic>
#include <cassert>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <vector>
#include <cstdlib>
#include <functional>
#undef assert
#define assert(condition) do { if (!(condition)) { std::cerr << "FAILED: " << #condition << '\n'; std::exit(42); } } while(false)
namespace fmt { template<class... T> std::string format(T&&...) { return {}; } }
namespace Magpie {
struct EffectOption { std::string name; std::map<std::string,float> parameters; };
bool IsFrameGenerationEffect(std::string_view name) { return name == "DLSSFG" || name == "XeSSFG"; }
struct ScalingOptions {
    FrameRefreshSettings frameRefresh;
    std::function<void(uintptr_t, bool, double, bool)> contentTargetChanged;
    bool benchmark=false;
    bool isFrontEdgeSyncEnabled=true;
    float frontEdgeSyncFrameRate=80, minFrameRate=0;
    std::optional<float> maxFrameRate;
    bool IsBenchmarkMode() const { return benchmark; }
};
struct SourceTracker { uintptr_t Handle() const { return 0x123; } };
struct ScalingWindow {
    ScalingOptions options;
    static ScalingWindow& Get() { static ScalingWindow window; return window; }
    auto& Options() { return options; }
    SourceTracker SrcTracker() const { return {}; }
};
struct Logger { static Logger& Get() { static Logger logger; return logger; } void Info(std::string) {} void Warn(std::string) {} };
struct Timer {
    std::optional<float> limit;
    bool strict=false;
    float minimum=0;
    void Initialize(float minimumValue, std::optional<float> value, bool strictValue=false) { minimum=minimumValue; limit=value; strict=strictValue; }
};
struct Driver : ReflexDriver {
    ReflexSleepStatus QueryStatus() noexcept override { return {0,true}; }
    bool failed=false, actualOn=true, failClear=false;
    uint32_t interval=0;
    ReflexConfigurationResult Configure(ReflexSettings value) noexcept override {
        if (failClear && !value.minimumIntervalUs) return {-3,0,false,false};
        interval=value.minimumIntervalUs;
        return {failed && value.lowLatency ? -1 : 0,0,true,actualOn && value.lowLatency};
    }
    int Sleep() noexcept override { return failed ? -1 : 0; }
    int Marker(ReflexMarker,uint64_t) noexcept override { return 0; }
    int RegisterGenerationQueue(ID3D12CommandQueue*) noexcept override { return 0; }
    int Generation(ID3D12CommandQueue*,uint64_t,uint64_t,bool) noexcept override { return 0; }
    int FrontendRender(uint64_t,uint64_t,bool) noexcept override { return 0; }
    int Present(uint64_t,uint64_t,bool,bool) noexcept override { return 0; }
    void ReportFailure(const char*,int) noexcept override {}
};
struct Reflex : ReflexController {
    Driver* driver;
    Reflex() { auto fake=std::make_unique<Driver>(); driver=fake.get(); Initialize(std::move(fake)); }
};
struct FG { unsigned Multiplier() const { return 2; } };
struct Renderer {
    std::optional<float> _captureMaxFrameRate;
    std::vector<EffectOption> _runtimeEffectOptions;
    float _frameRateFilterTarget=0;
    std::atomic<double> _presentationRefreshRate=240, _existingBaseFrameRateLimit=0;
	std::atomic<double> _idleRedrawRate=0;
    unsigned _configuredFrameGenerationMultiplier=1;
    bool _frameSyncEnabled=true, _frameSyncUsesSharedSlot=true;
    std::unique_ptr<FG> _dlssFrameGenerator;
    std::unique_ptr<FG> _nvidiaVfxFrameGenerator;
    bool _HasSoftwareFrameGenerator() const noexcept { return _dlssFrameGenerator || _nvidiaVfxFrameGenerator; }
    uint32_t _SoftwareFrameMultiplier() const noexcept;
    std::vector<int> _effectDrawers{1};
    FrameSyncBackend _frameSyncBackend=FrameSyncBackend::FrontEdge;
    FrameSyncBackend _appliedFrameSyncBackend=FrameSyncBackend::None;
    Reflex _reflex;
    bool _reflexFallbackLogged=false;
    Timer _stepTimer;
    double _baseFrameRateLimit=0;
    std::optional<double> _reportedContentTarget;
    bool _reportedFixedContent=false, _reportedContentConstraint=false;
    CaptureFrameCadence _captureCadence;
    std::chrono::nanoseconds _synchronousPresentInterval{};
    // ACTIVE_RESOLVER_FROM_PRODUCTION
    double _FrameSyncFrameRate() const noexcept;
    void _UpdateFrameRateLimits() noexcept;
};
'''
suffix = r'''
}
int main() {
    using namespace Magpie;
    auto& options=ScalingWindow::Get().Options();
    Renderer renderer;
    renderer._UpdateFrameRateLimits();
    assert(!renderer._stepTimer.limit && renderer._reflex.interval==0);
    renderer._frameSyncBackend=FrameSyncBackend::Async;
    renderer._UpdateFrameRateLimits();
    assert(renderer._stepTimer.limit==80 && renderer._stepTimer.strict && renderer._reflex.interval==0);
    renderer._frameSyncBackend=FrameSyncBackend::Reflex;
    renderer._UpdateFrameRateLimits();
    assert(!renderer._stepTimer.limit && renderer._reflex.interval==12500);
    options.maxFrameRate=60.0f;
    renderer._UpdateFrameRateLimits();
    assert(!renderer._stepTimer.limit && renderer._reflex.interval==16667 && renderer._baseFrameRateLimit==60);
    renderer._reflex.driver->failed=true;
    renderer._reflex.BeginCapture();
    renderer._UpdateFrameRateLimits();
    assert(renderer._appliedFrameSyncBackend==FrameSyncBackend::Async && renderer._stepTimer.limit==60);
    assert(renderer._reflexFallbackLogged);
    // Test direct-NVAPI base units through production resolver + controller +
    // renderer configuration, including Off-query and monitor changes.
    for (unsigned multiplier=2; multiplier<=4; ++multiplier) {
        Renderer pacing;
        pacing._configuredFrameGenerationMultiplier=multiplier;
        pacing._runtimeEffectOptions={{"DLSSFG",{}}};
        pacing._dlssFrameGenerator=std::make_unique<FG>();
        pacing._frameSyncBackend=ResolveFrameSyncBackend({true,80,FrameSyncMode::Reflex},true,false,true,false);
        pacing._reflex.driver->actualOn=false;
        options.maxFrameRate.reset();
        for (float requested : {80.0f, 0.0f}) {
            options.frontEdgeSyncFrameRate=requested;
            for (double refresh : {240.0, 144.0}) {
                pacing._presentationRefreshRate=refresh;
                const double expected=requested ? requested : refresh/multiplier;
                pacing._UpdateFrameRateLimits();
                assert(pacing._appliedFrameSyncBackend==FrameSyncBackend::Reflex);
                assert(pacing._reflex.State()==ReflexState::DriverOff);
                assert(!pacing._stepTimer.limit && pacing._reflex.driver->interval==FrameSyncIntervalUs(expected));
                pacing._captureMaxFrameRate=30.0f;
                pacing._UpdateFrameRateLimits();
                assert(!pacing._stepTimer.limit && pacing._reflex.driver->interval==33334);
                pacing._captureMaxFrameRate.reset();
            }
        }
        pacing._reflex.SetPresentationAvailable(false);
        pacing._UpdateFrameRateLimits();
        assert(pacing._appliedFrameSyncBackend==FrameSyncBackend::Async && pacing._reflex.driver->interval==0);
        pacing._reflex.SetPresentationAvailable(true);
        pacing._UpdateFrameRateLimits();
        assert(!pacing._stepTimer.limit && pacing._appliedFrameSyncBackend==FrameSyncBackend::Reflex);
        pacing._reflex.driver->failClear=true;
        pacing._reflex.driver->failed=true;
        pacing._reflex.BeginCapture();
        pacing._UpdateFrameRateLimits();
        assert(pacing._reflex.CaptureBlocked() && !pacing._stepTimer.limit);
    }
    options.maxFrameRate=60.0f; options.frontEdgeSyncFrameRate=80;
    // DLSS compatibility path retains Front Edge. Its failed FG fallback must still cap.
    Renderer dlss;
    dlss._frameSyncUsesSharedSlot=false;
    dlss._runtimeEffectOptions={{"DLSSFG",{}}};
    dlss._configuredFrameGenerationMultiplier=2;
    dlss._dlssFrameGenerator=std::make_unique<FG>();
    dlss._UpdateFrameRateLimits();
    assert(!dlss._stepTimer.limit && dlss._reflex.interval==0);
    for (unsigned multiplier=2;multiplier<=4;++multiplier) {
        dlss._configuredFrameGenerationMultiplier=multiplier;
        dlss._frameSyncBackend=ResolveFrameSyncBackend({true,80,FrameSyncMode::Reflex},true,false,true,false);
        dlss._UpdateFrameRateLimits();
        assert(!dlss._stepTimer.limit && dlss._reflex.interval==16667);
    }
    dlss._reflex.SetFrameRateLimit(0);
    dlss._configuredFrameGenerationMultiplier=2;
    dlss._frameSyncBackend=FrameSyncBackend::FrontEdge;
    dlss._dlssFrameGenerator.reset();
    dlss._UpdateFrameRateLimits();
    assert(dlss._stepTimer.limit==60 && dlss._reflex.interval==0);
    options.maxFrameRate.reset(); options.frontEdgeSyncFrameRate=0;
    dlss._frameSyncBackend=FrameSyncBackend::Async;
    dlss._UpdateFrameRateLimits();
    assert(dlss._stepTimer.limit==120 && dlss._reflex.interval==0);
    dlss._frameSyncBackend=FrameSyncBackend::XeLL;
    dlss._UpdateFrameRateLimits();
    assert(!dlss._stepTimer.limit && dlss._reflex.interval==0);
    // Saved idle target stays 30 while the runtime projects 20. FG disables
    // backend idle work only; the separately configured cursor isn't involved.
    Renderer idle;
    options.minFrameRate=30; options.frontEdgeSyncFrameRate=20;
    idle._frameSyncBackend=FrameSyncBackend::Async;
    idle._UpdateFrameRateLimits();
    assert(idle._stepTimer.minimum==20 && options.minFrameRate==30);
    idle._runtimeEffectOptions={{"DLSSFG",{}}};
    idle._UpdateFrameRateLimits();
    assert(idle._stepTimer.minimum==0 && options.minFrameRate==30);
    idle._runtimeEffectOptions.clear();
    idle._frameSyncBackend=FrameSyncBackend::None; idle._frameSyncEnabled=false;
    idle._UpdateFrameRateLimits();
    assert(idle._stepTimer.limit==20 && idle._stepTimer.minimum==20);
    options.frontEdgeSyncFrameRate=0; idle._presentationRefreshRate=144;
    idle._configuredFrameGenerationMultiplier=3;
    idle._UpdateFrameRateLimits();
    assert(idle._stepTimer.limit==48);
    idle._presentationRefreshRate=240; idle._UpdateFrameRateLimits();
    assert(idle._stepTimer.limit==80);
    options.isFrontEdgeSyncEnabled=false; idle._UpdateFrameRateLimits();
    assert(!idle._stepTimer.limit && idle._stepTimer.minimum==30);
    // The RTSS notification reports the actual base target without adding a
    // limiter. Repeated targets are deduplicated; mode/constraint changes notify.
    options.isFrontEdgeSyncEnabled=true; options.frontEdgeSyncFrameRate=0;
    options.frameRefresh.contentMode=ContentFrameRateMode::Auto;
    options.maxFrameRate.reset();
    unsigned notifications=0;
    bool reportedFixed=false, reportedConstrained=false;
    double reportedRate=0;
    options.contentTargetChanged=[&](uintptr_t sourceHandle, bool fixed, double baseRate, bool constrained) {
        assert(sourceHandle==0x123);
        ++notifications; reportedFixed=fixed; reportedRate=baseRate; reportedConstrained=constrained;
    };
    Renderer reported;
    reported._frameSyncBackend=FrameSyncBackend::Async;
    reported._presentationRefreshRate=144;
    for (unsigned multiplier=2; multiplier<=4; ++multiplier) {
        reported._configuredFrameGenerationMultiplier=multiplier;
        reported._UpdateFrameRateLimits();
        assert(reportedFixed && !reportedConstrained && reportedRate==144.0/multiplier);
        const auto before=notifications;
        reported._UpdateFrameRateLimits();
        assert(notifications==before);
    }
    reported._captureMaxFrameRate=30.0f;
    reported._UpdateFrameRateLimits();
    assert(reportedFixed && reportedConstrained && reportedRate==30);
    options.frameRefresh.contentMode=ContentFrameRateMode::Source;
    auto before=notifications;
    reported._UpdateFrameRateLimits();
    assert(notifications==before+1 && !reportedFixed && !reportedConstrained);
    options.frameRefresh.contentMode=ContentFrameRateMode::Auto; options.benchmark=true;
    before=notifications; reported._UpdateFrameRateLimits();
    assert(notifications==before && !reportedFixed);
    options.benchmark=false; reported._UpdateFrameRateLimits();
    assert(notifications==before+1 && reportedFixed && reportedConstrained);
    options.contentTargetChanged=[](uintptr_t, bool, double, bool) { throw 1; };
    reported._captureMaxFrameRate=20.0f; reported._UpdateFrameRateLimits();
    assert(reported._baseFrameRateLimit==20 && reported._stepTimer.limit==20);
    options.contentTargetChanged={};
	// NR processing and frame synchronization are independent dimensions. The
	// common overlay/trace changes must never install another base limiter.
	options.minFrameRate=0; options.maxFrameRate.reset();
	for (auto backend : {FrameSyncBackend::None, FrameSyncBackend::FrontEdge, FrameSyncBackend::Async, FrameSyncBackend::Reflex})
	for (unsigned nrMode=0; nrMode<4; ++nrMode) for (float rate : {60.f,90.f,120.f}) {
		Renderer cell;
		cell._frameSyncBackend=backend;
		cell._frameSyncEnabled=backend!=FrameSyncBackend::None;
		cell._runtimeEffectOptions={{"DLSSNR\\DLSSNR_AI_Filter",{{"processingMode",float(nrMode)}}}};
		options.isFrontEdgeSyncEnabled=cell._frameSyncEnabled;
		options.frontEdgeSyncFrameRate=rate;
		cell._UpdateFrameRateLimits();
		assert(cell._appliedFrameSyncBackend==backend);
		assert(cell._stepTimer.limit==(backend==FrameSyncBackend::Async ? std::optional(rate) : std::nullopt));
		assert(cell._stepTimer.strict==(backend==FrameSyncBackend::Async || backend==FrameSyncBackend::Reflex));
		assert(cell._reflex.driver->interval==(backend==FrameSyncBackend::Reflex ? FrameSyncIntervalUs(rate) : 0));
	}
	std::cout << "PASS: all 12 synchronized NR combinations at 60/90/120 plus source-following keep one base limiter owner\n";
    std::cout << "PASS: production content-target notifications use base FPS, track FG/capture/mode changes, deduplicate and isolate callback failures\n";
    std::cout << "PASS: production renderer single limiter ownership, lower profile cap, Reflex failure fallback, DLSS recovery, XeLL handoff, idle clamp/FG disable and unsupported-strategy targets\n";
}
'''
start = header.index('FrameSyncBackend ActiveFrameSyncBackend()')
end = header.index('\n\t}', start) + len('\n\t}')
prefix = prefix.replace('// ACTIVE_RESOLVER_FROM_PRODUCTION', header[start:end])
suffix = suffix.replace('._reflex.interval', '._reflex.driver->interval')
runtime = prefix + function('uint32_t Renderer::_SoftwareFrameMultiplier()') + '\n' + function('double Renderer::_FrameSyncFrameRate()') + '\n' + function('void Renderer::_UpdateFrameRateLimits()') + suffix
(output/'frame_sync_runtime.cpp').write_text(runtime, encoding='utf-8')
# Controlled regressions of production code must compile and fail at runtime.
policy_dir = output/'negative_policy'
policy_dir.mkdir(exist_ok=True)
# Resolve the transitive pacing include to the same mutated policy. Otherwise
# pragma-once permits two different paths and both enum declarations are loaded.
(policy_dir/'FrameRefreshSettings.h').write_text(
    (repo/'src/Magpie.Core/include/FrameRefreshSettings.h').read_text(encoding='utf-8-sig'), encoding='utf-8')
policy = (repo/'src/Magpie.Core/include/FramePacingOptions.h').read_text(encoding='utf-8-sig')
needle = 'if (settings.mode == FrameSyncMode::Reflex)\n\t\treturn FrameSyncBackend::Reflex;'
assert needle in policy
(policy_dir/'FramePacingOptions.h').write_text(policy.replace(needle, 'if (settings.mode == FrameSyncMode::Reflex)\n\t\treturn dlssFG ? FrameSyncBackend::Async : FrameSyncBackend::Reflex;'), encoding='utf-8')
(policy_dir/'runtime.cpp').write_text(runtime, encoding='utf-8')
needle = '_stepTimer.Initialize(useFrameGeneration ? 0.0f : minFrameRate, consumerPacing ? std::nullopt : fallbackLimit,'
assert needle in runtime
(output/'negative_double.cpp').write_text(runtime.replace(needle, '_stepTimer.Initialize(useFrameGeneration ? 0.0f : minFrameRate, consumerPacing && _appliedFrameSyncBackend != FrameSyncBackend::Reflex ? std::nullopt : fallbackLimit,'), encoding='utf-8')
