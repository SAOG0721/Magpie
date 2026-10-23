#include "../src/Magpie.Core/PresentationFrameRate.h"
#include <cassert>
#include <cstdio>

using namespace Magpie;
using namespace std::chrono;
static const ProfilerHistory::Time base(100s);
static bool Near(double value,double expected,double tolerance=0.01){return std::abs(value-expected)<tolerance;}
static uint32_t IntervalCount(const ProfilerHistory::Plot& plot){uint32_t count=0;for(const auto& p:plot.points)count+=p.interval.count;return count;}

// Compile the real Renderer admission branch against a harmless presenter.
struct Gate {
    struct Presenter {ContentFrameCount sample{1};ContentFrameCount LastContentFrameCount(){return sample;}} presenter;
    Presenter* _presenter=&presenter;
    PresentationFrameRate _presentationRate;
    uint64_t _frontendCaptureFrameId=1,_lastCountedRealFrameId=0;
    void Submit(bool submitted,bool contentFrame,bool generatedFrame){
#include "ProfilerPresentationGate.inc"
    }
};

int main(){
    ProfilerHistory capture;capture.Reset(base);
    PresentationFrameRate output;output.StartProfile(base);
    for(int i=0;i<360;++i){
        const auto at=base+nanoseconds(int64_t(i)*1000000000/120);
        if(i%2==0)capture.Frame(at);
        output.Record(1,i%2==0?1:0,at,ContentFrameCountSource::Reported,at);
    }
    capture.Observe(base+3s);
    auto cp=capture.Read(base+3s);
    auto op=output.Profile(base+3s);
    assert(Near(cp.points.back().fps,60) && Near(op.plot.points.back().fps,120));
    assert(Near(cp.points[98].interval.Mean(),1000.0/60));
    assert(Near(op.plot.points[98].interval.Mean(),1000.0/120));
    assert(op.individualTimestamps && !op.plot.points.back().warming);
    const auto intervals=IntervalCount(op.plot);
    for(int i=0;i<40;++i)assert(IntervalCount(output.Profile(base+3s).plot)==intervals);
    for(int i=0;i<100;++i)output.RecordIdleSubmission(base+3s+milliseconds(i));
    assert(IntervalCount(output.Profile(base+3100ms).plot)==intervals);
    assert(output.Profile(base+5s).plot.points.back().fps==0);

    // The FPS is identical for batches, but there is no per-output interval.
    output.StopProfile();output.StartProfile(base);
    for(int i=0;i<180;++i)output.Record(2,1,base+nanoseconds(int64_t(i)*1000000000/60),ContentFrameCountSource::Submitted);
    op=output.Profile(base+3s);
    assert(Near(op.plot.points.back().fps,120) && op.source==ContentFrameCountSource::Submitted);
    assert(!op.individualTimestamps && IntervalCount(op.plot)==0);
    output.Record(std::nullopt,1,base+3100ms,ContentFrameCountSource::Submitted);
    assert(!output.Profile(base+3500ms).plot.points.back().fpsValid);
    assert(output.Profile(base+5s).plot.points.back().fpsValid);

    // Real event intervals retain an isolated stall instead of deriving 1000/FPS.
    output.StartProfile(base);
    output.Record(1,1,base,ContentFrameCountSource::Reported,base);
    output.Record(1,1,base+10ms,ContentFrameCountSource::Reported,base+10ms);
    output.Record(1,1,base+40ms,ContentFrameCountSource::Reported,base+40ms);
    op=output.Profile(base+50ms);
    assert(IntervalCount(op.plot)==2 && op.plot.points.back().interval.maximum==30);
    output.Record(std::nullopt,1,base+60ms);
    output.Record(1,1,base+80ms,ContentFrameCountSource::Reported,base+80ms);
    output.Record(1,1,base+90ms,ContentFrameCountSource::Reported,base+90ms);
    assert(IntervalCount(output.Profile(base+95ms).plot)==3); // Never bridge the unknown batch.

    // Pause and generation resets isolate the old chain; source switches do too.
    output.SynchronizeProfile(1,true,base+100ms);
    output.Record(1,1,base+200ms,ContentFrameCountSource::Reported,base+200ms);
    assert(output.Profile(base+500ms).plot.suspended);
    output.SynchronizeProfile(2,false,base+1s);
    output.Record(1,1,base+1s,ContentFrameCountSource::Reported,base+1s);
    output.Record(1,1,base+1016ms,ContentFrameCountSource::Reported,base+1016ms);
    op=output.Profile(base+1100ms);
    assert(IntervalCount(op.plot)==1);
    for(const auto& point:op.plot.points)assert(point.interval.maximum<=16);
    output.Record(2,1,base+1200ms,ContentFrameCountSource::Submitted);
    op=output.Profile(base+1300ms);
    assert(op.source==ContentFrameCountSource::Submitted && IntervalCount(op.plot)==0 && !op.individualTimestamps);
    output.StopProfile();output.Record(1,1,base+2s,ContentFrameCountSource::Reported,base+2s);
    assert(!output.Profile(base+3s).active);
    output.StartProfile(base+3s);
    assert(IntervalCount(output.Profile(base+3100ms).plot)==0);

    // Failed submits, auxiliary redraws, invalid IDs and repeated old real frames
    // must not enter output content counts; generated outputs still count.
    Gate gate;
    gate.Submit(false,true,false);gate.Submit(true,false,false);
    gate._frontendCaptureFrameId=0;gate.Submit(true,true,false);
    gate._frontendCaptureFrameId=1;gate.Submit(true,true,false);
    for(int i=0;i<20;++i)gate.Submit(true,true,false);
    gate.Submit(true,true,true);
    gate._frontendCaptureFrameId=2;gate.Submit(true,true,false);
    const auto rate=gate._presentationRate.Get(PresentationFrameRate::Clock::now()+1s);
    assert(rate.total==3 && rate.real==2 && rate.generated==1);
    std::puts("Profiler input/output: 60/120 FPS, real intervals, SDK missing intervals, unknown counts, idle/duplicate exclusion, pause/source/generation reset and production output gate passed.");
}
