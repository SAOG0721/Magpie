"""Exercise production StepTimer with a controlled clock and OS wait boundary."""
from pathlib import Path
import re
import sys

repo = Path(__file__).resolve().parents[1]
output = Path(sys.argv[1])
header = (repo / 'src/Magpie.Core/StepTimer.h').read_text(encoding='utf-8-sig')
body = (repo / 'src/Magpie.Core/StepTimer.cpp').read_text(encoding='utf-8-sig')
body = re.sub(r'^#include[^\n]*\n', '', body, flags=re.M)
header = header.replace('#pragma once', '').replace('std::chrono::steady_clock', 'TestClock')
body = body.replace('steady_clock', 'TestClock')
prefix = r'''
#define NOMINMAX
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <limits>
#include <optional>
#include <utility>
using namespace std::chrono_literals;
struct TestClock {
    using duration = std::chrono::nanoseconds;
    using rep = duration::rep;
    using period = duration::period;
    using time_point = std::chrono::time_point<TestClock>;
    static constexpr bool is_steady = true;
    static inline time_point current{};
    static time_point now() { return current; }
};
static unsigned waits=0, yields=0, creates=0, sets=0;
static DWORD waitCount=0, waitTimeout=0, waitMask=0, waitFlags=0;
static DWORD waitResult=WAIT_OBJECT_0, createFlags=0;
static HANDLE waitHandles[2]{};
static LONGLONG dueTime=0;
static bool failHighResolution=false, failCreate=false, failSet=false;
void ResetWaits() {
    waits=yields=creates=sets=0;
    waitCount=waitTimeout=waitMask=waitFlags=createFlags=0;
    waitResult=WAIT_OBJECT_0;
    waitHandles[0]=waitHandles[1]=nullptr;
    dueTime=0;
    failHighResolution=failCreate=failSet=false;
}
namespace wil {
class unique_handle {
    HANDLE value=nullptr;
public:
    void reset(HANDLE handle) { value=handle; }
    HANDLE get() const { return value; }
    explicit operator bool() const { return value!=nullptr; }
};
using unique_event_nothrow=unique_handle;
}
HANDLE MockCreate(LPSECURITY_ATTRIBUTES, LPCWSTR, DWORD flags, DWORD access) {
    ++creates; createFlags=flags;
    assert(access==(TIMER_MODIFY_STATE|SYNCHRONIZE));
    return failCreate || (failHighResolution && (flags&CREATE_WAITABLE_TIMER_HIGH_RESOLUTION))
        ? nullptr : reinterpret_cast<HANDLE>(1);
}
template<class... T> BOOL MockSet(HANDLE timer, const LARGE_INTEGER* due, T...) {
    ++sets; assert(timer==reinterpret_cast<HANDLE>(1)); dueTime=due->QuadPart;
    return !failSet;
}
DWORD MockWait(DWORD count, const HANDLE* handles, DWORD timeout, DWORD mask, DWORD flags) {
    ++waits; waitCount=count; waitTimeout=timeout; waitMask=mask; waitFlags=flags;
    assert(count<=2);
    waitHandles[0]=count ? handles[0] : nullptr;
    waitHandles[1]=count>1 ? handles[1] : nullptr;
    return waitResult;
}
template<class... T> DWORD MockSingleWait(T...) { return WAIT_OBJECT_0; }
template<class... T> void MockSleep(T...) { ++yields; }
#undef CreateWaitableTimerEx
#define CreateWaitableTimerEx MockCreate
#define CreateWaitableTimerExW MockCreate
#define SetWaitableTimerEx MockSet
#define MsgWaitForMultipleObjectsEx MockWait
#define WaitForSingleObject MockSingleWait
#define Sleep MockSleep
#include "FrameTrace.h"
#include "FramePacingWait.h"
namespace Magpie::FrameTrace {
unsigned wakeMarks=0;
int64_t wakeResult=0;
void MockMark(Event event, int64_t a=0, int64_t=0) {
    if (event==Event::CaptureWake) { ++wakeMarks; wakeResult=a; }
}
}
#define Mark MockMark
'''
suffix = r'''
int main() {
    using namespace Magpie;
    for (bool strict : {false,true}) {
        StepTimer timer;
        timer.Initialize(0,80.0f,strict);
        bool fps=false;
        TestClock::current=TestClock::time_point(100ms);
        timer.WaitForNextFrame(false,fps);
        timer.CaptureStarting();
        timer.PrepareForRender();
        TestClock::current=TestClock::time_point(119ms);
        assert(timer.WaitForNextFrame(false,fps)==StepTimerStatus::WaitForNewFrame);
        timer.CaptureStarting();
        timer.PrepareForRender();
        TestClock::current=TestClock::time_point(125ms);
        assert(timer.WaitForNextFrame(false,fps)==(strict ? StepTimerStatus::WaitForFPSLimiter : StepTimerStatus::WaitForNewFrame));
        TestClock::current=TestClock::time_point(131ms);
        timer.WaitForNextFrame(false,fps);
        if (strict) assert(yields==0);
        TestClock::current=TestClock::time_point(131500us);
        assert(timer.WaitForNextFrame(false,fps)==StepTimerStatus::WaitForNewFrame);
        timer.CaptureStarting();
        timer.PrepareForRender();
        // A stall must not trigger a burst. Refresh after late message work too.
        TestClock::current=TestClock::time_point(800ms);
        timer.WaitForNextFrame(false,fps);
        TestClock::current=TestClock::time_point(810ms);
        timer.CaptureStarting();
        timer.PrepareForRender();
        TestClock::current=TestClock::time_point(820ms);
        if (strict) assert(timer.WaitForNextFrame(false,fps)==StepTimerStatus::WaitForFPSLimiter);
        TestClock::current=TestClock::time_point(822500us);
        assert(timer.WaitForNextFrame(false,fps)==StepTimerStatus::WaitForNewFrame);
    }
    assert(waits>0);
	// A same-capture refresh is an internal rendering turn, not a new content
	// limiter epoch. A resumed capture is eligible immediately, even if the
	// quality update occurred between two content-limit deadlines.
	for (bool strict : {false,true}) for (float rate : {60.f,90.f,120.f}) {
		StepTimer timer;
		timer.Initialize(0,rate,strict);
		const auto period=std::chrono::duration_cast<TestClock::duration>(std::chrono::duration<float>(1/rate));
		const auto start=TestClock::time_point(2s);
		bool fps=false;
		TestClock::current=start;
		timer.WaitForNextFrame(false,fps); timer.CaptureStarting(); timer.PrepareForRender();
		const auto quality=start+period*9/4;
		TestClock::current=quality;
		assert(timer.WaitForNextFrame(false,fps)==StepTimerStatus::WaitForNewFrame);
		timer.CaptureStarting(); timer.PrepareForRender(false);
		TestClock::current=start+period*5/2;
		assert(timer.WaitForNextFrame(false,fps)==StepTimerStatus::WaitForNewFrame);
		timer.CaptureStarting(); timer.PrepareForRender();
		assert(timer.FrameCount()==3);
		TestClock::current+=period/4;
		assert(timer.WaitForNextFrame(false,fps)==StepTimerStatus::WaitForFPSLimiter);
	}
	// The minimum-FPS redraw clock still advances. Separating the maximum-FPS
	// content anchor must not cause an idle source to force draws continuously.
	{
		StepTimer timer; timer.Initialize(10,120.f,true);
		bool fps=false; TestClock::current=TestClock::time_point(10s);
		timer.WaitForNextFrame(false,fps); timer.CaptureStarting(); timer.PrepareForRender();
		TestClock::current+=100ms;
		assert(timer.WaitForNextFrame(false,fps)==StepTimerStatus::ForceNewFrame);
		timer.CaptureStarting(); timer.PrepareForRender(false);
		TestClock::current+=1ms;
		assert(timer.WaitForNextFrame(false,fps)==StepTimerStatus::WaitForNewFrame);
		TestClock::current+=100ms;
		assert(timer.WaitForNextFrame(false,fps)==StepTimerStatus::ForceNewFrame);
	}
    // The final 500 us is a single interruptible timer wait in either mode.
    // An early message returns control but cannot bypass the content limiter.
    for (bool strict : {false,true}) {
        ResetWaits();
        StepTimer timer; timer.Initialize(0,80.f,strict);
        bool fps=false; TestClock::current=TestClock::time_point(100ms);
        timer.WaitForNextFrame(false,fps); timer.CaptureStarting(); timer.PrepareForRender();
        const HANDLE source=reinterpret_cast<HANDLE>(2);
        TestClock::current=TestClock::time_point(112ms);
        waitResult=WAIT_OBJECT_0+1; // Message index for a one-handle wait.
        assert(timer.WaitForNextFrame(true,fps,source)==StepTimerStatus::WaitForFPSLimiter);
        assert(waits==1 && yields==0 && creates==1 && sets==1 && dueTime==-5000);
        assert(waitCount==1 && waitHandles[0]==reinterpret_cast<HANDLE>(1));
        assert(waitTimeout==INFINITE && waitMask==QS_ALLINPUT && waitFlags==MWMO_INPUTAVAILABLE);
        TestClock::current=TestClock::time_point(112250us);
        assert(timer.WaitForNextFrame(false,fps,source)==StepTimerStatus::WaitForFPSLimiter);
        assert(waits==2 && creates==1 && sets==2 && dueTime==-2500 && yields==0);
        TestClock::current=TestClock::time_point(112500us);
        assert(timer.WaitForNextFrame(false,fps,source)==StepTimerStatus::WaitForNewFrame);
        assert(waits==2);
    }
    // Source waits keep the current capture event and return the raw OS result
    // to tracing. The borrowed event is never retained across capture rebuilds.
    for (bool strict : {false,true}) {
        ResetWaits(); FrameTrace::wakeMarks=0;
        StepTimer timer; timer.Initialize(0,{},strict);
        const HANDLE source=reinterpret_cast<HANDLE>(2);
        waitResult=WAIT_OBJECT_0+1;
        timer._WaitForMsgAndTimer(500us,source);
        assert(waits==1 && yields==0 && sets==1 && dueTime==-5000);
        assert(waitCount==2 && waitHandles[1]==source && waitTimeout==INFINITE);
        assert(FrameTrace::wakeMarks==1 && FrameTrace::wakeResult==waitResult);
        const HANDLE rebuiltSource=reinterpret_cast<HANDLE>(3);
        waitResult=WAIT_FAILED;
        timer._WaitForMsgAndTimer(2001ns,rebuiltSource);
        assert(creates==1 && waitHandles[1]==rebuiltSource && dueTime==-21);
        assert(FrameTrace::wakeMarks==2 && FrameTrace::wakeResult==WAIT_FAILED);
        timer._WaitForMsgAndTimer(500us);
        assert(waitCount==1 && waitHandles[1]==nullptr && yields==0);
    }
    // Nonpositive deadlines do not wait, allocate a timer, or invent wake marks.
    {
        ResetWaits(); FrameTrace::wakeMarks=0;
        StepTimer timer;
        timer._WaitForMsgAndTimer(0ns);
        timer._WaitForMsgAndTimer(-1ns,reinterpret_cast<HANDLE>(2));
        assert(!waits && !creates && !sets && !yields && !FrameTrace::wakeMarks);
        assert(WaitForFramePacing(0ns,timer._hTimer)==WAIT_TIMEOUT);
        assert(!waits && !creates);
    }
    // Old systems can fall back to a normal timer. Timer creation/arming
    // failures still wait on messages and the source event with bounded time.
    {
        ResetWaits(); failHighResolution=true;
        StepTimer timer;
        timer._WaitForMsgAndTimer(2ms,reinterpret_cast<HANDLE>(2));
        assert(creates==2 && createFlags==0 && sets==1 && waitCount==2);
        assert(waitTimeout==INFINITE && dueTime==-20000);
    }
    {
        ResetWaits(); failCreate=true; waitResult=WAIT_OBJECT_0;
        StepTimer timer; FrameTrace::wakeMarks=0;
        timer._WaitForMsgAndTimer(120ms,reinterpret_cast<HANDLE>(2));
        assert(creates==2 && !sets && waits==1 && waitCount==1);
        assert(waitHandles[0]==reinterpret_cast<HANDLE>(2) && waitTimeout==50);
        assert(waitMask==QS_ALLINPUT && waitFlags==MWMO_INPUTAVAILABLE);
        assert(FrameTrace::wakeMarks==1 && FrameTrace::wakeResult==waitResult);
    }
    {
        ResetWaits(); failSet=true; waitResult=WAIT_OBJECT_0;
        StepTimer timer;
        timer._WaitForMsgAndTimer(500us,reinterpret_cast<HANDLE>(2));
        assert(creates==1 && sets==1 && waits==1 && waitCount==1 && waitTimeout==1);
        assert(waitHandles[0]==reinterpret_cast<HANDLE>(2));
        waitResult=WAIT_TIMEOUT;
        assert(WaitForFramePacing(500us,timer._hTimer)==WAIT_TIMEOUT);
        assert(waitCount==0 && waitTimeout==1 && !waitHandles[0] && !yields);
        assert(waitMask==QS_ALLINPUT && waitFlags==MWMO_INPUTAVAILABLE);
    }
    // Quiet NoWait sources pass a finite poll delay without a capture event.
    // Redraw/quality deadlines must still wake earlier than that poll bound.
    for (bool strict : {false,true}) {
        ResetWaits();
        StepTimer timer; timer.Initialize(100,{},strict);
        bool fps=false; TestClock::current=TestClock::time_point(50s);
        assert(timer.WaitForNextFrame(false,fps,nullptr,4ms)==StepTimerStatus::WaitForNewFrame);
        assert(waits==0 && timer.FrameCount()==0);
        timer.CaptureStarting(); timer.PrepareForRender();
        TestClock::current+=8ms;
        assert(timer.WaitForNextFrame(true,fps,nullptr,4ms)==StepTimerStatus::WaitForNewFrame);
        assert(waits==1 && waitCount==1 && waitTimeout==INFINITE && dueTime==-20000);
        assert(timer.WaitForNextFrame(true,fps,nullptr,1ms)==StepTimerStatus::WaitForNewFrame);
        assert(waits==2 && dueTime==-10000);
        TestClock::current+=2ms;
        assert(timer.WaitForNextFrame(true,fps,nullptr,4ms)==StepTimerStatus::ForceNewFrame);
        assert(waits==2);
        timer.CaptureStarting(); timer.PrepareForRender(false);
        TestClock::current+=1ms;
        assert(timer.WaitForNextFrame(true,fps,nullptr,4ms)==StepTimerStatus::WaitForNewFrame);
        assert(waits==3 && dueTime==-40000 && waitMask==QS_ALLINPUT && waitFlags==MWMO_INPUTAVAILABLE);
    }
    std::cout << "PASS: finite NoWait polling preserves first-capture admission, earlier redraw/quality deadlines and message wakeups\n";
    std::cout << "PASS: production StepTimer strict 80 FPS starts, legacy phase behavior, late capture, long stalls and passive waits\n";
	std::cout << "PASS: production StepTimer 60/90/120 FPS same-capture refresh preserves next content admission; minimum-FPS redraws remain bounded\n";
    std::cout << "PASS: sub-ms timer/message/source waits, early limiter wake, rebuilt event, raw wake trace and timer-failure fallbacks\n";
}
'''
(output / 'frame_sync_timer.cpp').write_text(
    prefix + '#define private public\n' + header + '\n#undef private\n' + body + suffix,
    encoding='utf-8')
