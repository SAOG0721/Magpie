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

static void AsyncIntervals(){
    PresentationIntervalQueue queue;
    assert(!queue.Active() && queue.Begin(base).serial==0);
    queue.SetActive(true);
    const auto emit=[&](auto time,bool success=true){auto s=queue.Begin(time);queue.Complete(s,success);};
    emit(base);emit(base+8ms);emit(base+20ms);
    auto batch=queue.Take();
    assert(batch.count==2 && batch.samples[0].end-batch.samples[0].begin==8ms &&
        batch.samples[1].end-batch.samples[1].begin==12ms);
    assert(queue.Take().count==0);
    // Failed output and missing callback are gaps, never long fake intervals.
    emit(base+30ms,false);emit(base+40ms);emit(base+45ms);
    [[maybe_unused]] auto missing=queue.Begin(base+50ms);
    emit(base+60ms);emit(base+67ms);
    batch=queue.Take();
    assert(batch.count==2 && batch.samples[0].end-batch.samples[0].begin==5ms &&
        batch.samples[1].end-batch.samples[1].begin==7ms);
    const auto stale=queue.Begin(base+70ms);
    queue.SetActive(false);queue.SetActive(true);
    queue.Complete(stale,true);emit(base+1s);emit(base+1009ms);
    batch=queue.Take();assert(batch.count==1 && batch.samples[0].end-batch.samples[0].begin==9ms);
    // Completing two producers out of order must not connect across them.
    auto first=queue.Begin(base+1010ms),second=queue.Begin(base+1020ms);
    queue.Complete(second,true);queue.Complete(first,true);emit(base+1030ms);
    assert(queue.Take().count==0);
    queue.Reset(true);
    for(int i=0;i<=1000;++i)emit(base+2s+milliseconds(i*2));
    batch=queue.Take();assert(batch.count==64);
    for(size_t i=0;i<batch.count;++i)assert(batch.samples[i].end-batch.samples[i].begin==2ms);

    PresentationFrameRate rate;rate.StartProfile(base);
    rate.Record(2,1,base+20ms);
    PresentationIntervals actual;actual.count=2;
    actual.samples[0]={base,base+8ms};actual.samples[1]={base+8ms,base+20ms};
    rate.RecordIntervals(actual,base+20ms);
    rate.RecordIntervals(actual,base+20ms); // Re-reading does not duplicate samples or FPS.
    auto plot=rate.Profile(base+20ms);
    assert(plot.individualTimestamps && IntervalCount(plot.plot)==2 && Near(plot.plot.points.back().fps,100));
    rate.Record(std::nullopt,1,base+30ms);
    assert(!rate.Profile(base+30ms).plot.points.back().fpsValid && IntervalCount(rate.Profile(base+30ms).plot)==2);
    rate.SynchronizeProfile(1,true,base+40ms);
    rate.RecordIntervals(actual,base+40ms);
    rate.SynchronizeProfile(1,false,base+1s);
    rate.RecordIntervals(actual,base+1s);
    assert(IntervalCount(rate.Profile(base+1s).plot)==0);
    actual.count=1;actual.samples[0]={base+1s,base+1010ms};
    rate.RecordIntervals(actual,base+1010ms);
    assert(IntervalCount(rate.Profile(base+1010ms).plot)==1);
    rate.SynchronizeProfile(2,false,base+2s);rate.RecordIntervals(actual,base+2s);
    assert(IntervalCount(rate.Profile(base+2s).plot)==0);
    rate.StopProfile();rate.RecordIntervals(actual,base+3s);
    assert(!rate.Profile(base+3s).active);
}

int main(){
    AsyncIntervals();
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
