"""Exercise production overlay admission and WGC callback timestamps without a GPU."""
from pathlib import Path
import sys

repo = Path(__file__).resolve().parents[1]
out = Path(sys.argv[1])
out.mkdir(parents=True, exist_ok=True)


def block(source, marker):
    start = source.index(marker)
    brace = source.index("{", start)
    end, depth = brace + 1, 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


renderer = (repo / "src/Magpie.Core/Renderer.cpp").read_text(encoding="utf-8-sig")
clock_source = (repo / "src/Magpie.Core/FramePresentationTiming.h").read_text(encoding="utf-8-sig")
clock = (block(clock_source, "class OverlayPresentationClock {") + ";").replace("std::chrono::steady_clock", "TestClock")
admission = block(renderer, "bool Renderer::_CanRenderOverlay()").replace("std::chrono::steady_clock", "TestClock")
prefix = r'''
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>
#define Check(condition) do { if (!(condition)) { std::cerr << "FAILED " << __LINE__ << ": " << #condition << '\n'; std::exit(42); } } while(false)
using namespace std::chrono_literals;
struct TestClock {
    using duration=std::chrono::nanoseconds;
    using time_point=std::chrono::time_point<TestClock>;
    static inline time_point current{};
    static time_point now() { return current; }
};
namespace Magpie {
namespace FrameTrace {
enum class Event { OverlayDeferred, OverlayContentionWait, WgcNotification, WgcNotificationOverflow };
struct Sample { Event event; int64_t tick; uint64_t frame; int64_t a,b; };
static std::vector<Sample> records;
static std::thread::id owner=std::this_thread::get_id();
inline void Record(Event e,int64_t start,int64_t end,uint64_t frame,int64_t a=0,int64_t b=0) {
    Check(std::this_thread::get_id()==owner && start==end);
    records.push_back({e,start,frame,a,b});
}
inline void Mark(Event e,int64_t a=0,int64_t b=0) { Record(e,0,0,0,a,b); }
}
/* CLOCK */
struct ScalingWindow {
    bool resizing=false;
    static ScalingWindow& Get() { static ScalingWindow window; return window; }
    bool IsResizingOrMoving() const { return resizing; }
};
struct Renderer {
    struct Presenter {
        bool recovery=false, independent=false;
        bool NeedsRedraw() const { return recovery; }
        bool HasIndependentOverlay() const { return independent; }
    } presenter;
    Presenter* _presenter=&presenter;
    bool _hasFrameGeneration=false, action=false, contentPending=false;
    std::atomic<bool> _backendContentInFlight=false;
    std::optional<TestClock::time_point> _overlayContentionUntil;
    OverlayPresentationClock _overlayPresentationClock;
    struct Cursor {
        bool minimum=false,visibility=false,original=false;
        bool IsMinimumRefreshDue() const { return minimum; }
        bool HasVisibilityTransition() const { return visibility; }
        bool HasOriginalRefreshPending() const { return original; }
    } _cursorDrawer;
    struct Overlay { bool critical=false; bool HasCriticalInput() const { return critical; } } _overlayDrawer;
    bool _HasPendingOverlayAction() const { return action; }
    bool HasPendingContent() const { return contentPending; }
    bool _CanRenderOverlay() noexcept;
};
/* ADMISSION */
namespace wil { struct unique_event_nothrow {}; }
/* SIGNAL */
}
'''
signal = block((repo / "src/Magpie.Core/GraphicsCaptureFrameSource.cpp").read_text(encoding="utf-8-sig"),
               "struct GraphicsCaptureFrameSource::FrameReadySignal {") + ";"
signal = signal.replace("struct GraphicsCaptureFrameSource::FrameReadySignal", "struct FrameReadySignal")
suffix = r'''
int main() {
    using namespace Magpie;
    unsigned cells=0;
    // The common admission policy has no NR-mode or limiter side effects.
    // Exercise every requested synchronization/NR/rate cell with real code.
    for (unsigned sync=0;sync<4;++sync) for (unsigned nr=0;nr<4;++nr) for (double rate : {60.,90.,120.}) {
        (void)sync;(void)nr; ++cells;
        Renderer render;
        render._overlayPresentationClock.SetRefreshRate(rate);
        TestClock::current=TestClock::time_point(1s);
        render._backendContentInFlight=true;
        Check(!render._CanRenderOverlay());
        const auto until=render._overlayContentionUntil;
        Check(until && *until-TestClock::now()<=8ms);
        TestClock::current=*until-1ns;
        Check(!render._CanRenderOverlay() && render._overlayContentionUntil==until);
        TestClock::current=*until;
        Check(render._CanRenderOverlay()); // A permanently busy worker cannot starve UI.
        render._backendContentInFlight=false;
        Check(render._CanRenderOverlay() && !render._overlayContentionUntil);
        render.contentPending=true;
        Check(!render._CanRenderOverlay()); // FrontEdge queued content also owns priority.
        render.contentPending=false;
        Check(render._CanRenderOverlay() && !render._overlayContentionUntil);
        render._backendContentInFlight=true;
        Check(!render._CanRenderOverlay());
        for (unsigned critical=0;critical<7;++critical) {
            render.action=critical==0; render._overlayDrawer.critical=critical==1;
            render.presenter.recovery=critical==2; ScalingWindow::Get().resizing=critical==3;
            render._cursorDrawer.minimum=critical==4; render._cursorDrawer.visibility=critical==5;
            render._cursorDrawer.original=critical==6;
            Check(render._CanRenderOverlay() && !render._overlayContentionUntil);
        }
        render._cursorDrawer.original=false;
        render.presenter.independent=true;
        Check(render._CanRenderOverlay() && !render._overlayContentionUntil);
        render.presenter.independent=false; render._hasFrameGeneration=true;
        Check(render._CanRenderOverlay() && !render._overlayContentionUntil);
        render._hasFrameGeneration=false; render._backendContentInFlight=false;
        render._overlayPresentationClock.Presented(TestClock::now());
        Check(!render._CanRenderOverlay()); // Existing overlay cadence still applies.
    }
    {
        FrameReadySignal signal; signal.traceEnabled=true;
        for (int64_t i=1;i<=1100;++i) signal.TraceNotification(i);
        signal.DrainNotifications(7);
        Check(FrameTrace::records.back().event==FrameTrace::Event::WgcNotificationOverflow);
        Check(FrameTrace::records.back().a==76 && FrameTrace::records.back().b==7);
        int64_t serial=0;
        for (const auto& sample:FrameTrace::records) if (sample.event==FrameTrace::Event::WgcNotification) {
            Check(sample.frame==0 && sample.b==7 && sample.a==++serial && sample.tick==serial);
        }
        Check(serial==1024);
        FrameTrace::records.clear();
        signal.DrainNotifications(7,true);
        signal.TraceNotification(9999); signal.DrainNotifications(8);
        Check(FrameTrace::records.empty()); // A late callback cannot enter another capture session.
    }
    {
        FrameReadySignal signal; signal.traceEnabled=true;
        std::atomic<unsigned> finished=0;
        constexpr unsigned perThread=8000;
        auto notify=[&] {
            for (unsigned i=0;i<perThread;++i) signal.TraceNotification(i+1);
            ++finished;
        };
        std::thread first(notify),second(notify);
        while (finished.load()!=2) { signal.DrainNotifications(11); std::this_thread::yield(); }
        first.join();second.join();signal.DrainNotifications(11,true);
        uint64_t recorded=0,dropped=0; int64_t previous=0;
        for (const auto& sample:FrameTrace::records) {
            if (sample.event==FrameTrace::Event::WgcNotification) {
                Check(sample.frame==0 && sample.b==11 && sample.a>previous);
                previous=sample.a; ++recorded;
            } else if (sample.event==FrameTrace::Event::WgcNotificationOverflow) dropped+=sample.a;
        }
        Check(recorded+dropped==2*perThread);
    }
    std::cout << "PASS: " << cells << " production overlay cells; bounded capacity priority, critical input/idle/recovery/FG preserved; WGC per-notification order, overflow and concurrent close/drain\n";
}
'''
source = prefix.replace("/* CLOCK */", clock).replace("/* ADMISSION */", admission).replace("/* SIGNAL */", signal) + suffix
source = "#define MP_ENABLE_FRAME_TRACE\n" + source
(out / "overlay-contention.cpp").write_text(source, encoding="utf-8")
needle = "!_backendContentInFlight.load(std::memory_order_acquire)"
assert needle in source
(out / "overlay-contention-negative.cpp").write_text(source.replace(needle, "true"), encoding="utf-8")
