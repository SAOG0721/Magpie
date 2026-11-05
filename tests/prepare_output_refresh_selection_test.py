"""Execute Renderer same-capture admission/priority code with deterministic state."""
from pathlib import Path
import sys

repo = Path(__file__).resolve().parents[1]
source = (repo / 'src/Magpie.Core/Renderer.cpp').read_text(encoding='utf-8-sig')
begin = source.index('\t\t\tif (stepTimerStatus != StepTimerStatus::ForceNewFrame && !_forceNextRender &&')
end = source.index('\t\t\t[[fallthrough]];', begin)
waiting = source[begin:end].replace('break;', 'return {};')
reason_begin = source.index('\t\t\tconst FrameUpdateReason updateReason =', end)
reason_end = source.index('\n\t\t\tFrameTrace::Mark', reason_begin)
reason = source[reason_begin:reason_end]
prefix = r'''
#include "FrameUpdateReason.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <optional>
using namespace Magpie;
enum class StepTimerStatus { WaitForNewFrame, WaitForFPSLimiter, ForceNewFrame };
enum class FrameSourceState { Waiting, NewFrame };
struct ScalingWindow { static ScalingWindow Get() { return {}; } int Handle() const { return 0; } };
struct CommonSharedConstants { static constexpr int WM_FRONTEND_RENDER=1; };
void PostMessage(int,int,int,int) {}
struct Renderer {
    bool _forceNextRender=false, software=false;
    uint64_t _capturedFrameId=1;
    std::atomic<uint32_t> _pendingDLSSFGFrontendFrames=0;
    struct Idle { bool ready=false; std::chrono::nanoseconds Remaining(std::chrono::steady_clock::time_point) const { return std::chrono::nanoseconds(ready ? 0 : 1); } } _nativeOutputIdleClock;
    bool _HasSoftwareFrameGenerator() const { return software; }
    std::optional<FrameUpdateReason> Select(FrameSourceState frameSourceState, bool nativeOutputReady, StepTimerStatus stepTimerStatus=StepTimerStatus::WaitForNewFrame) {
        const bool fpsUpdated=false;
        if (frameSourceState==FrameSourceState::Waiting) {
'''
suffix = r'''
int main() {
    Renderer r;
    const auto waiting=FrameSourceState::Waiting, captured=FrameSourceState::NewFrame;
    assert(!r.Select(waiting,true)); // a source period has not established idle
    r._nativeOutputIdleClock.ready=true;
    assert(r.Select(waiting,true)==FrameUpdateReason::QualityRefresh);
    r._forceNextRender=true;
    assert(r.Select(waiting,true)==FrameUpdateReason::ParameterUpdate);
    r.software=true; r._pendingDLSSFGFrontendFrames=2;
    assert(!r.Select(waiting,true)); // old generated/real FIFO batch drains first
    assert(r.Select(captured,true)==FrameUpdateReason::NewCapture);
    r._pendingDLSSFGFrontendFrames=0;
    assert(r.Select(waiting,true)==FrameUpdateReason::ParameterUpdate);
    r._forceNextRender=false;
    assert(r.Select(waiting,true)==FrameUpdateReason::QualityRefresh);
    assert(!r.Select(waiting,false));
    assert(r.Select(waiting,false,StepTimerStatus::ForceNewFrame)==FrameUpdateReason::RenderRefresh);
    r._capturedFrameId=0;
    assert(!r.Select(waiting,true));
    std::cout << "PASS production Renderer source-idle selection, new-capture/parameter priority, FG FIFO drain before quality refresh, and first-capture guard\n";
}
'''
output = prefix + waiting + '\n}\n' + reason + '\nreturn updateReason;\n}\n};\n' + suffix
(Path(sys.argv[1]) / 'output_refresh_selection.cpp').write_text(output, encoding='utf-8')

# The owner must leave a previously slept Reflex capture candidate intact when
# only its displayed quality/parameters change. Extract the actual guards from
# both renderer functions, then execute them against the production controller.
backend = source.index('void Renderer::_BackendRender(')
start = source.index('\tif (isNewCaptureFrame) _reflex.BeginCaptureRender();', backend)
capture_start = source[start:source.index('\n', start)]
complete = source.index('void Renderer::_CompleteBackendFrame(')
start = source.index('\tif (isNewCaptureFrame) _reflex.EndCaptureRender();', complete)
capture_finish_end = source.index('\n\t});', start) + len('\n\t});')
capture_finish = source[start:capture_finish_end]
reflex_prefix = r'''
#include "ReflexController.h"
#include <cassert>
#include <iostream>
using namespace Magpie;
namespace wil {
template<class F> struct ScopeExit { F callback; ~ScopeExit() { callback(); } };
template<class F> auto scope_exit(F callback) { return ScopeExit<F>{callback}; }
}
struct Driver : ReflexDriver {
    unsigned sleeps=0, starts=0, ends=0;
    ReflexConfigurationResult Configure(ReflexSettings) noexcept override { return {0,0,true,true}; }
    ReflexSleepStatus QueryStatus() noexcept override { return {0,true}; }
    int Sleep() noexcept override { ++sleeps; return 0; }
    int Marker(ReflexMarker marker,uint64_t) noexcept override {
        if (marker==ReflexMarker::RenderStart) ++starts;
        if (marker==ReflexMarker::RenderEnd) ++ends;
        return 0;
    }
    int RegisterGenerationQueue(ID3D12CommandQueue*) noexcept override { return 0; }
    int Generation(ID3D12CommandQueue*,uint64_t,uint64_t,bool) noexcept override { return 0; }
    int FrontendRender(uint64_t,uint64_t,bool) noexcept override { return 0; }
    int Present(uint64_t,uint64_t,bool,bool) noexcept override { return 0; }
    void ReportFailure(const char*,int) noexcept override {}
};
struct Owner {
    ReflexController _reflex;
    void Draw(bool isNewCaptureFrame) {
'''
reflex_suffix = r'''
int main() {
    for (unsigned rate : {60u,90u,120u}) {
        Owner owner;
        auto driver=std::make_unique<Driver>(); auto* observed=driver.get();
        owner._reflex.Initialize(std::move(driver), {.minimumIntervalUs=1'000'000/rate});
        assert(owner._reflex.BeginCapture(10)==10 && observed->sleeps==1);
        for (unsigned update=0; update<2; ++update) {
            owner.Draw(false); owner.Publish(false);
            owner._reflex.DiscardCaptureRender();
            assert(owner._reflex.CaptureFrameId()==10);
            assert(owner._reflex.BeginCapture(10)==10 && observed->sleeps==1);
            assert(observed->starts==0 && observed->ends==0);
        }
        owner.Draw(true); owner.Publish(true);
        assert(owner._reflex.CaptureFrameId()==0 && observed->starts==1 && observed->ends==1);
        assert(owner._reflex.BeginCapture(11)==11 && observed->sleeps==2);
    }
    std::cout << "PASS production Renderer/Reflex 60/90/120 Hz same-capture updates retain the slept candidate; only the next captured frame advances its lifecycle\n";
}
'''
reflex_output = reflex_prefix + capture_start + '\n}\nvoid Publish(bool isNewCaptureFrame) {\n' + capture_finish + '\n}\n};\n' + reflex_suffix
(Path(sys.argv[1]) / 'output_refresh_reflex.cpp').write_text(reflex_output, encoding='utf-8')
