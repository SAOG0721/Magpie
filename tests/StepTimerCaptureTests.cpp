// Execute the production implementation with exposed clock anchors and wait
// boundary. Accounting is deterministic; no renderer or window is run.
#include "pch.h"
#include <optional>
#include <atomic>
#include <limits>
#include <cassert>
#include <iostream>
#define private public
#include "StepTimer.h"
#undef private
#undef MP_ENABLE_FRAME_TRACE
#include "../src/Magpie.Core/StepTimer.cpp"

void TestInterruptibleWaits() {
    using namespace std::chrono;
    MSG message{};
    // Create this thread's queue without opening a window.
    PeekMessageW(&message,nullptr,0,0,PM_NOREMOVE);
    for (bool strict : {false,true}) {
        Magpie::StepTimer timer;
        timer.Initialize(0,{},strict);
        wil::unique_handle source(CreateEventW(nullptr,TRUE,TRUE,nullptr));
        assert(source);
        auto start=steady_clock::now();
        timer._WaitForMsgAndTimer(2s,source.get());
        assert(steady_clock::now()-start<1s);
        // Source events remain owned by capture. Replacing one does not leave
        // the timer waiting on the event from the previous capture session.
        assert(ResetEvent(source.get()));
        wil::unique_handle rebuiltSource(CreateEventW(nullptr,TRUE,TRUE,nullptr));
        assert(rebuiltSource);
        start=steady_clock::now();
        timer._WaitForMsgAndTimer(2s,rebuiltSource.get());
        assert(steady_clock::now()-start<1s);

        constexpr UINT wakeMessage=WM_APP+37;
        assert(PostThreadMessageW(GetCurrentThreadId(),wakeMessage,0,0));
        start=steady_clock::now();
        timer._WaitForMsgAndTimer(2s);
        assert(steady_clock::now()-start<1s);
        assert(PeekMessageW(&message,nullptr,wakeMessage,wakeMessage,PM_REMOVE));
        assert(message.message==wakeMessage);
        // Stopping stays with the outer loop: the wait returns without
        // dispatching or consuming its queued quit message.
        PostQuitMessage(37);
        start=steady_clock::now();
        timer._WaitForMsgAndTimer(2s);
        assert(steady_clock::now()-start<1s);
        assert(PeekMessageW(&message,nullptr,0,0,PM_REMOVE));
        assert(message.message==WM_QUIT && message.wParam==37);
    }
    std::cout << "PASS production StepTimer: source/rebuilt-source events, messages and stop wake both wait modes without dispatch.\n";
}

int main() {
    using namespace std::chrono;
    Magpie::StepTimer timer;
    timer.Initialize(0, {});
    // PrepareForRender samples steady_clock internally. A future anchor keeps
    // that wall clock from closing our injected one-second test window early.
    const auto start = steady_clock::now() + hours(24);
    timer._lastSecondTime = start;
    timer._thisFrameStartTime = start;
	timer._lastRenderStartTime = start;
    for (unsigned frame = 0; frame < 90; ++frame) {
        timer._nextFrameStartTime = start + milliseconds(frame*10 + 1);
        timer.PrepareForRender(true);
        assert(timer._thisFrameStartTime == timer._nextFrameStartTime);
        timer._nextFrameStartTime += milliseconds(5);
        timer.PrepareForRender(false);
        assert(timer._thisFrameStartTime == timer._nextFrameStartTime - milliseconds(5));
		assert(timer._lastRenderStartTime == timer._nextFrameStartTime);
        assert(timer.FrameCount() == (frame+1)*2);
    }
    assert(timer._UpdateFPS(start + seconds(1)));
    assert(timer.FPS() == 90);
    assert(timer.RenderUpdatesPerSecond() == 180);
    assert(timer.FrameCount() == 180);

    // A static image can receive a quality refresh without inventing content.
    timer._nextFrameStartTime = start + seconds(1) + milliseconds(1);
    timer.PrepareForRender(false);
    assert(timer._thisFrameStartTime == start + milliseconds(891));
	assert(timer._lastRenderStartTime == timer._nextFrameStartTime);
    assert(timer._UpdateFPS(start + seconds(2)));
    assert(timer.FPS() == 0 && timer.RenderUpdatesPerSecond() == 1);
    assert(timer.FrameCount() == 181);

    // The compatibility default remains a new content frame. Idle reporting
    // clears both rates while retaining the monotonically increasing clock.
    timer._nextFrameStartTime = start + seconds(2) + milliseconds(1);
    timer.PrepareForRender();
    assert(timer._UpdateFPS(start + seconds(3)));
    assert(timer.FPS() == 1 && timer.RenderUpdatesPerSecond() == 1);
    assert(timer.FrameCount() == 182);
    assert(timer._UpdateFPS(start + seconds(4)));
    assert(timer.FPS() == 0 && timer.RenderUpdatesPerSecond() == 0);
    assert(timer.FrameCount() == 182);
    std::cout << "PASS production StepTimer: 90 captures + 90 same-capture updates = "
        "90 content FPS, 180 updates, 180 clock ticks; static/idle/default accounting preserved.\n";
    TestInterruptibleWaits();
}
