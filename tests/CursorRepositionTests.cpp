// Real production methods + real dispatcher timers/coroutines. All input/window
// APIs are mocked: this executable never clips or moves the desktop cursor.
#include <Windows.h>
#include <DispatcherQueue.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.System.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>

namespace winrt { using namespace Windows::System; }
using winrt::operator co_await;
using namespace std::chrono_literals;
inline bool operator==(const POINT& a, const POINT& b) { return a.x == b.x && a.y == b.y; }
inline bool operator==(const RECT& a, const RECT& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

#define private public
#include "../src/Magpie.Core/CursorManager.h"
#undef private

namespace {
int checks = 0;
DWORD frontend = 0;
const RECT desktop{ -1920, -200, 2560, 1440 };
RECT clip = desktop;
POINT cursor{ 200, 300 };
bool queryFails = false, setFails = false, positionFails = false, windowsValid = true, childRelated = true;
bool monitorPresent = true;
int clipCalls = 0, queryCalls = 0, moveCalls = 0, postCalls = 0, sleepCalls = 0, cleanupErrors = 0;
int queryFailuresRemaining = 0, setFailuresRemaining = 0, timerFailureStage = 0;
bool timerFailureBreaksQuery = false, timerFailureBreaksSet = false, foreignOnFailedQuery = false;
bool failShutdownRestoreQuery = false, failShutdownRestoreSet = false;
const RECT foreignClip{ -500, -300, 100, 200 };
HWND postedChild{};
WPARAM postedSource{};
LPARAM postedArea{};
std::mutex hitMutex;
std::condition_variable hitCv;
bool holdHitTest = false;
std::atomic<int> hitActive = 0, hitMaximum = 0, hitCalls = 0;
struct TraceEntry { Magpie::FrameTrace::Event event; int64_t a, b, start, end; };
std::vector<TraceEntry> traces;

void Check(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
void RequireFrontend() {
    if (GetCurrentThreadId() != frontend) std::terminate();
}
BOOL MockGetClipCursor(LPRECT value) {
    RequireFrontend();
    ++queryCalls;
    if (queryFails || queryFailuresRemaining > 0) {
        if (queryFailuresRemaining > 0) --queryFailuresRemaining;
        if (foreignOnFailedQuery) { clip = foreignClip; foreignOnFailedQuery = false; }
        return FALSE;
    }
    *value = clip;
    return TRUE;
}
BOOL MockClipCursor(const RECT* value) {
    RequireFrontend();
    ++clipCalls;
    if (setFails || setFailuresRemaining > 0) {
        if (setFailuresRemaining > 0) --setFailuresRemaining;
        return FALSE;
    }
    clip = value ? *value : desktop;
    cursor.x = std::clamp(cursor.x, clip.left, clip.right - 1);
    cursor.y = std::clamp(cursor.y, clip.top, clip.bottom - 1);
    return TRUE;
}
BOOL MockSetCursorPos(int x, int y) {
    RequireFrontend();
    ++moveCalls;
    cursor = { x, y };
    return TRUE;
}
BOOL MockGetCursorPos(LPPOINT value) {
    RequireFrontend();
    if (positionFails) return FALSE;
    *value = cursor;
    return TRUE;
}
BOOL MockIsWindow(HWND value) { RequireFrontend(); return windowsValid && value; }
BOOL MockIsChild(HWND, HWND) { RequireFrontend(); return childRelated; }
HMONITOR MockMonitorFromPoint(POINT, DWORD) {
    RequireFrontend();
    return monitorPresent ? reinterpret_cast<HMONITOR>(uintptr_t(1)) : nullptr;
}
BOOL MockPostMessage(HWND value, UINT message, WPARAM wparam, LPARAM lparam) {
    RequireFrontend();
    Check(message == WM_SETCURSOR, "Wrong shape update message");
    ++postCalls;
    postedChild = value; postedSource = wparam; postedArea = lparam;
    return TRUE;
}
void MockSleep(DWORD milliseconds) {
    RequireFrontend();
    Check(milliseconds == 8, "Shutdown hold duration changed");
    ++sleepCalls;
    if (failShutdownRestoreQuery) queryFails = true;
    if (failShutdownRestoreSet) setFails = true;
}
bool HasTrace(Magpie::FrameTrace::Event event, Magpie::FrameTrace::CursorRepositionReason reason) {
    return std::any_of(traces.begin(), traces.end(), [&](const auto& entry) {
        return entry.event == event && entry.b == int64_t(reason);
    });
}
void PumpOnce() {
    MSG message{};
    while (PeekMessage(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessage(&message);
    }
}
template<class Fn>
void PumpUntil(Fn&& done, const char* message) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!done() && std::chrono::steady_clock::now() < deadline) {
        PumpOnce();
        MsgWaitForMultipleObjectsEx(0, nullptr, 1, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
    PumpOnce();
    Check(done(), message);
}
void ReleaseHitTest() {
    { std::lock_guard lock(hitMutex); holdHitTest = false; }
    hitCv.notify_all();
}
}

namespace Magpie::FrameTrace {
bool Enabled() noexcept { return true; }
int64_t Tick() noexcept {
    RequireFrontend();
    return std::chrono::steady_clock::now().time_since_epoch().count();
}
uint64_t Frame() noexcept { RequireFrontend(); return 91; }
void Record(Event event, int64_t start, int64_t end, uint64_t, int64_t a, int64_t b) noexcept {
    RequireFrontend();
    traces.push_back({ event, a, b, start, end });
}
void Mark(Event event, int64_t a, int64_t b) noexcept { Record(event, Tick(), Tick(), Frame(), a, b); }
}

namespace Magpie {
#include "RuntimeQuitTypes.inc"
// Only creation/start failure injection wraps the real dispatcher/timer; normal
// timer ticks and all background/foreground coroutine transitions remain native.
struct TestTimer {
    winrt::DispatcherQueueTimer timer;
    void Interval(std::chrono::milliseconds value) { timer.Interval(value); }
    void IsRepeating(bool value) { timer.IsRepeating(value); }
    template<class Fn> void Tick(Fn&& value) { timer.Tick(std::forward<Fn>(value)); }
    void Start() {
        if (timerFailureStage == 2) {
            queryFails = timerFailureBreaksQuery; setFails = timerFailureBreaksSet;
            throw winrt::hresult_error(E_FAIL);
        }
        timer.Start();
    }
    void Stop() { timer.Stop(); }
    operator winrt::DispatcherQueueTimer() const { return timer; }
};
struct TestDispatcher {
    winrt::DispatcherQueue dispatcher;
    TestTimer CreateTimer() const {
        if (timerFailureStage == 1) {
            queryFails = timerFailureBreaksQuery; setFails = timerFailureBreaksSet;
            throw winrt::hresult_error(E_FAIL);
        }
        return { dispatcher.CreateTimer() };
    }
    auto operator co_await() const { return winrt::operator co_await(dispatcher); }
};
struct ScalingOptions {
    bool debug = false;
    bool IsDebugMode() const { return debug; }
};
struct SourceTracker {
    HWND hwnd = reinterpret_cast<HWND>(uintptr_t(71));
    HWND Handle() const { return hwnd; }
};
struct Renderer {
    RECT source{ 100, 200, 1100, 700 }, dest{ 0, 0, 2000, 1000 };
    bool parameters = false;
    const RECT& SrcRect() const { return source; }
    const RECT& DestRect() const { return dest; }
    bool IsEffectParametersVisible() const { return parameters; }
};
struct ScalingWindow {
    static inline uint32_t runId = 1;
    static inline winrt::DispatcherQueue dispatcher{ nullptr };
    static inline Magpie::CursorManager* manager = nullptr;
    Magpie::CursorManager*& _cursorManager = manager;
    struct Retirement {
        bool pending = false;
        bool Ready(unsigned depth) const { return pending && !depth; }
        void Complete() { pending = false; }
    } _retirement;
    bool _finalizingRetirement = false, _isDestroying = false;
    unsigned _windowCallbackDepth = 0;
    int finished = 0;
    ScalingOptions options;
    SourceTracker source;
    Magpie::Renderer renderer;
    RECT rendererRect{ 0, 0, 2000, 1000 };
    bool foreground = true;
    bool live = true;
    static ScalingWindow& Get() { static ScalingWindow window; return window; }
    static uint32_t RunId() { return runId; }
    static TestDispatcher Dispatcher() { return { dispatcher }; }
    explicit operator bool() const { return live; }
    Magpie::CursorManager* TryGetCursorManager() { return manager; }
    const ScalingOptions& Options() const { return options; }
    const SourceTracker& SrcTracker() const { return source; }
    const Magpie::Renderer& Renderer() const { return renderer; }
    const RECT& RendererRect() const { return rendererRect; }
    bool IsSourceForeground() const { return foreground; }
    void _FinishDestroy() { ++finished; }
    bool CompletePendingDestroy() noexcept;
    bool IsRetiring() const { return _retirement.pending; }
    void Stop() { if (manager) manager->BeginShutdown(); }
    void Destroy() { live = false; _retirement.pending = true; CompletePendingDestroy(); }
};
struct Win32Helper {
    static SIZE GetSizeOfRect(const RECT& value) { return { value.right - value.left, value.bottom - value.top }; }
    static int16_t AdvancedWindowHitTest(HWND, POINT, UINT timeout, HWND* child) {
        if (GetCurrentThreadId() == frontend || timeout != 10) std::terminate();
        const int active = ++hitActive;
        int previous = hitMaximum.load();
        while (previous < active && !hitMaximum.compare_exchange_weak(previous, active)) {}
        ++hitCalls;
        { std::unique_lock lock(hitMutex); hitCv.wait(lock, [] { return !holdHitTest; }); }
        *child = reinterpret_cast<HWND>(uintptr_t(72));
        --hitActive;
        return HTCLIENT;
    }
};
struct Logger {
    static Logger& Get() { static Logger log; return log; }
    void Win32Error(const char*) const { ++cleanupErrors; }
    void Error(const char*) const { ++cleanupErrors; }
};
void CursorManager::_ShowSystemCursor(bool, bool) {}
void CursorManager::_AdjustCursorSpeed() noexcept {}
void CursorManager::_RestoreCursorSpeed() noexcept {}

#define GetClipCursor MockGetClipCursor
#define ClipCursor MockClipCursor
#define GetCursorPos MockGetCursorPos
#define SetCursorPos MockSetCursorPos
#define IsWindow MockIsWindow
#define IsChild MockIsChild
#define MonitorFromPoint MockMonitorFromPoint
#undef PostMessage
#define PostMessage MockPostMessage
#define Sleep MockSleep
#include "CursorRepositionProduction.inc"
#include "CursorRetirementProduction.inc"
#undef GetClipCursor
#undef ClipCursor
#undef GetCursorPos
#undef SetCursorPos
#undef IsWindow
#undef IsChild
#undef MonitorFromPoint
#undef PostMessage
#undef Sleep
}

using namespace Magpie;
using Reason = FrameTrace::CursorRepositionReason;
using Event = FrameTrace::Event;

static bool QuitMayReturn(ScalingWindow& scalingWindow, bool quitRequested) {
#include "CursorQuitGateProduction.inc"
}

struct Fixture {
    std::unique_ptr<CursorManager> manager;
    Fixture() {
        clip = desktop; cursor = { 200, 300 };
        queryFails = setFails = positionFails = false; windowsValid = childRelated = true;
        monitorPresent = true;
        clipCalls = queryCalls = moveCalls = postCalls = sleepCalls = cleanupErrors = 0;
        queryFailuresRemaining = setFailuresRemaining = timerFailureStage = 0;
        timerFailureBreaksQuery = timerFailureBreaksSet = foreignOnFailedQuery = false;
        failShutdownRestoreQuery = failShutdownRestoreSet = false;
        traces.clear();
        ScalingWindow::Get().live = true;
        ScalingWindow::Get().options.debug = false;
        ScalingWindow::Get().source.hwnd = reinterpret_cast<HWND>(uintptr_t(71));
        ScalingWindow::Get().renderer = {};
        ScalingWindow::Get().rendererRect = { 0, 0, 2000, 1000 };
        ScalingWindow::Get().foreground = true;
        ScalingWindow::Get()._retirement.pending = false;
        ScalingWindow::Get()._finalizingRetirement = ScalingWindow::Get()._isDestroying = false;
        ScalingWindow::Get()._windowCallbackDepth = 0;
        ScalingWindow::Get().finished = 0;
        ++ScalingWindow::runId;
        manager = std::make_unique<CursorManager>();
        ScalingWindow::manager = manager.get();
    }
    ~Fixture() {
        ReleaseHitTest();
        if (manager && manager->_isCursorShapeInFlight)
            PumpUntil([&] { return !manager->_isCursorShapeInFlight; }, "Shape query not drained");
        manager.reset();
        ScalingWindow::manager = nullptr;
    }
    CursorManager* operator->() { return manager.get(); }
    void Finish() {
        manager->_cursorReposition->deadline = std::chrono::steady_clock::time_point{};
        manager->_CompleteCursorReposition(manager->_cursorReposition->id);
    }
};

int main() try {
    frontend = GetCurrentThreadId();
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    winrt::DispatcherQueueController controller{ nullptr };
    const DispatcherQueueOptions options{ sizeof(DispatcherQueueOptions), DQTYPE_THREAD_CURRENT, DQTAT_COM_NONE };
    winrt::check_hresult(CreateDispatcherQueueController(options,
        reinterpret_cast<ABI::Windows::System::IDispatcherQueueController**>(winrt::put_abi(controller))));
    ScalingWindow::dispatcher = controller.DispatcherQueue();
    {
        Fixture f;
        POINT pos{ 0, 0 };
        f->_StartCapture(pos);
        Check(pos == POINT{ 100, 200 } && f->_isUnderCapture, "Capture did not map destination origin to source origin");
        Check(f->_StopCapture(pos) && pos == POINT{ 0, 0 } && !f->_isUnderCapture,
            "Stop-capture did not restore destination coordinates");
        ScalingWindow::Get().renderer.dest = { 100, 100, 2100, 1100 };
        ScalingWindow::Get().rendererRect = { 0, 0, 2200, 1200 };
        pos = { 1100, 700 }; f->_isUnderCapture = true;
        monitorPresent = false;
        Check(!f->_StopCapture(pos) && f->_isUnderCapture && pos == POINT{ 1099, 699 },
            "Missing target monitor did not retain/clamp capture");
        pos = { 1100, 700 };
        Check(f->_StopCapture(pos, true) && pos == POINT{ 2200, 1200 }, "Shutdown letterbox mapping changed");
    }
    {
        Fixture f;
        const auto started = std::chrono::steady_clock::now();
        f->_ReliableSetCursorPos({ 400, 500 });
        Check(f->_cursorReposition.has_value() && cursor == POINT{ 400, 500 }, "Point hold not installed immediately");
        f->_CompleteCursorReposition(f->_cursorReposition->id);
        Check(f->_cursorReposition.has_value(), "Early callback shortened the 8 ms input drain");
        PumpUntil([&] { return !f->_cursorReposition; }, "Native timer did not complete hold");
        Check(std::chrono::steady_clock::now() - started >= 8ms, "Native timer shortened hold");
        Check(clip == desktop && cursor == POINT{ 400, 500 }, "Timer failed to restore clip/retain new position");
        Check(sleepCalls == 0 && hitCalls == 0, "Presentation relocation blocked");
        Check(HasTrace(Event::CursorRepositionComplete, Reason::Completed), "Completion trace absent");
    }
    {
        Fixture f;
        const RECT confinement{ 20, 30, 900, 1000 }, replacement{ 10, 15, 800, 850 };
        f->_SetClipCursor(confinement);
        f->_ReliableSetCursorPos({ 400, 500 });
        const RECT point = clip;
        f->_SetClipCursor(replacement, true);
        Check(clip == point && f->_cursorReposition->restoreClip == replacement, "3D clip shortened hold or was lost");
        f.Finish();
        Check(clip == replacement && f->_lastRealClip == replacement && f->_lastClip == replacement,
            "Updated confinement was not committed");
        const int calls = clipCalls;
        f->_SetClipCursor(replacement);
        Check(clipCalls == calls, "Unchanged confinement emitted another WM_MOUSEMOVE");
        f->_SetClipCursor(replacement, true);
        Check(clipCalls == calls + 1, "3D mode stopped reinforcing source confinement");
        f->_RestoreClipCursor();
        Check(clip == desktop, "Committed confinement could not be released");
    }
    {
        Fixture f;
        f->_SetClipCursor({ 20, 30, 900, 1000 });
        f->_ReliableSetCursorPos({ 400, 500 });
        const RECT point = clip;
        f->_RestoreClipCursor();
        Check(clip == point, "Release shortened required point hold");
        f.Finish();
        Check(clip == desktop && f->_lastClip.left == std::numeric_limits<LONG>::max(), "Deferred release lost ownership");
        f->_ReliableSetCursorPos({ 400, 500 });
        f->_RestoreClipCursor();
        f->_SetClipCursor({ 5, 6, 1000, 1100 });
        f.Finish();
        Check(clip == RECT{ 5, 6, 1000, 1100 }, "Latest clip after a deferred release did not win");
    }
    {
        Fixture f;
        const RECT foreign{ -100, 10, 900, 900 };
        clip = foreign;
        f->_ReliableSetCursorPos({ 400, 500 });
        f->_RestoreClipCursor();
        f.Finish();
        Check(clip == foreign, "Normal restore removed a pre-existing external clip");
        f->_ReliableSetCursorPos({ 400, 500 });
        clip = { -500, -300, 100, 200 };
        const RECT changed = clip;
        f.Finish();
        Check(clip == changed && !f->_cursorReposition, "Late completion overwrote external clip");
        Check(HasTrace(Event::CursorRepositionCancel, Reason::ExternalClip), "External clip cancellation trace absent");
        f->_ReliableSetCursorPos({ 400, 500 });
        const uint64_t old = f->_cursorReposition->id;
        clip = changed;
        f->_SetClipCursor({ 10, 20, 1000, 1100 });
        f->_CompleteCursorReposition(old);
        Check(clip == RECT{ 10, 20, 1000, 1100 } && !f->_cursorReposition,
            "External clip plus a new explicit confinement retained stale restore work");
        f->_ReliableSetCursorPos({ 400, 500 });
        clip = changed;
        const int calls = clipCalls;
        f->_RestoreClipCursor();
        Check(clip == changed && clipCalls == calls && !f->_cursorReposition,
            "Release overwrote external clipping during a hold");
    }
    {
        Fixture f;
        f->_ReliableSetCursorPos({ 400, 500 });
        const uint64_t old = f->_cursorReposition->id;
        f->_SetClipCursor({ 5, 6, 1000, 1100 });
        f->_ReliableSetCursorPos({ 600, 700 });
        f->_CompleteCursorReposition(old);
        Check(f->_cursorReposition->id != old && cursor == POINT{ 600, 700 }, "Stale timer changed successor request");
        f.Finish();
        Check(clip == RECT{ 5, 6, 1000, 1100 }, "Supersession saved its own point clip");
        Check(HasTrace(Event::CursorRepositionCancel, Reason::Superseded), "Supersession trace absent");
    }
    {
        Fixture f;
        f->_ReliableSetCursorPos({ 400, 500 });
        const uint64_t id = f->_cursorReposition->id;
        f->BeginShutdown();
        const int calls = clipCalls;
        f->_ReliableSetCursorPos({ 600, 700 });
        f->_CompleteCursorReposition(id);
        Check(clipCalls == calls, "Atomic shutdown began new USER32 work before owner cut");
        f->ReleasePendingCursorReposition();
        Check(clip == desktop && !f->_cursorReposition, "Owner-cut cleanup retained point clip");
        Check(HasTrace(Event::CursorRepositionCancel, Reason::Shutdown), "Shutdown cancellation trace absent");
    }
    for (int stage = 1; stage <= 2; ++stage) {
        for (int failure = 0; failure < 2; ++failure) {
            Fixture f;
            timerFailureStage = stage;
            timerFailureBreaksQuery = failure == 0;
            timerFailureBreaksSet = failure == 1;
            f->_ReliableSetCursorPos({ 400, 500 });
            Check(!f->_cursorReposition && clip == desktop && clipCalls == 0 && queryCalls == 1 && moveCalls == 1,
                "Timer failure acquired a point clip that would need a failing restore");
            Check(HasTrace(Event::CursorRepositionCancel, Reason::TimerUnavailable), "Timer failure was not traced");
            queryFails = setFails = false;
        }
    }
    for (int failure = 0; failure < 2; ++failure) {
        Fixture f;
        f->_ReliableSetCursorPos({ 400, 500 });
        f->BeginShutdown();
        if (failure == 0) queryFailuresRemaining = 1;
        else setFailuresRemaining = 1;
        const int queries = queryCalls;
        Check(f->ReleasePendingCursorReposition(), "One-shot shutdown restore failure lost its progress source");
        Check(!f->_cursorReposition && clip == desktop && queryCalls >= queries + 2 && cleanupErrors == 0,
            "Bounded shutdown recovery required a future Update/destructor");
    }
    {
        Fixture f;
        f->_ReliableSetCursorPos({ 400, 500 });
        f->BeginShutdown();
        queryFailuresRemaining = 1;
        foreignOnFailedQuery = true;
        const int calls = clipCalls;
        Check(f->ReleasePendingCursorReposition() && clip == foreignClip && clipCalls == calls,
            "External clip takeover between bounded retries was overwritten");
    }
    for (int failure = 0; failure < 3; ++failure) {
        Fixture f;
        auto& window = ScalingWindow::Get();
        f->_ReliableSetCursorPos({ 400, 500 });
        f->BeginShutdown();
        window._retirement.pending = true;
        queryFails = failure != 1;
        setFails = failure == 1;
        const int queries = queryCalls;
        Check(!window.CompletePendingDestroy() && f->_cursorReposition && window.finished == 0,
            "Failed cleanup allowed retirement to drop its cursor owner");
        Check(queryCalls == queries + 4 && cleanupErrors == 1, "Cleanup did not use its bounded attempt budget");
        const int queriesAfterFailure = queryCalls, clipsAfterFailure = clipCalls;
        Check(!window.CompletePendingDestroy() && queryCalls == queriesAfterFailure && clipCalls == clipsAfterFailure,
            "Retirement retry ignored its backoff");
        queryFails = setFails = false;
        if (failure == 2) clip = foreignClip;
        f->_nextCursorCleanupAttempt = {};
        Check(window.CompletePendingDestroy() && !window.IsRetiring() && !f->_cursorReposition && window.finished == 1,
            "Retirement could not recover without another cursor Update/destructor");
        Check(clip == (failure == 2 ? foreignClip : desktop), "Retirement recovery restored the wrong clip");
        if (failure == 2) Check(clipCalls == clipsAfterFailure, "Late retirement overwrote foreign takeover");
    }
    for (int failure = 0; failure < 3; ++failure) {
        Fixture f;
        auto& window = ScalingWindow::Get();
        f->_isUnderCapture = true;
        f->_ReliableSetCursorPos({ 100, 200 });
        f->BeginShutdown();
        failShutdownRestoreQuery = failure != 1;
        failShutdownRestoreSet = failure == 1;
        window._retirement.pending = true;
        Check(!window.CompletePendingDestroy() && f->_cursorReposition && !f->_isUnderCapture && window.finished == 0,
            "Final synchronous positioning lost a failed point-clip restore");
        Check(cursor == POINT{ 0, 0 } && sleepCalls == 1, "Final mapped position/hold was not installed before retaining cleanup");
        queryFails = setFails = failShutdownRestoreQuery = failShutdownRestoreSet = false;
        if (failure == 2) clip = foreignClip;
        const int calls = clipCalls;
        f->_nextCursorCleanupAttempt = {};
        Check(window.CompletePendingDestroy() && !f->_cursorReposition && sleepCalls == 1 && window.finished == 1,
            "Final coordinate cleanup could not complete without remapping/recreating the hold");
        Check(clip == (failure == 2 ? foreignClip : desktop), "Final coordinate cleanup overwrote its restore target");
        if (failure == 2) Check(clipCalls == calls, "Final coordinate retry overwrote a foreign clip");
    }
    {
        Fixture f;
        auto& scalingWindow = ScalingWindow::Get();
        bool quitRequested = false;
        std::atomic<bool> _quitting = false;
        std::atomic<uint64_t> _commandGeneration = 0;
        const auto _State = [](ScalingState) {};
#include "CursorQuitProduction.inc"
        f->_ReliableSetCursorPos({ 400, 500 });
        queryFails = true;
        requestQuit();
        Check(quitRequested && scalingWindow.IsRetiring() && f->_cursorReposition,
            "Quit did not retain the failed cleanup owner");
        Check(!QuitMayReturn(scalingWindow, quitRequested) && scalingWindow.finished == 0,
            "Quit returned before retained input cleanup completed");
        queryFails = false;
        f->_nextCursorCleanupAttempt = {};
        Check(QuitMayReturn(scalingWindow, quitRequested) && !f->_cursorReposition && scalingWindow.finished == 1 && clip == desktop,
            "Quit failed to retry retirement independently of cursor Update");
    }
    {
        Fixture f;
        queryFails = true;
        f->_ReliableSetCursorPos({ 400, 500 });
        Check(!f->_cursorReposition && moveCalls == 1, "Query failure did not use bounded best-effort positioning");
        queryFails = false; setFails = true;
        f->_ReliableSetCursorPos({ 600, 700 });
        Check(!f->_cursorReposition && moveCalls == 2, "Clip failure left an active request");
        setFails = false;
        f->_ReliableSetCursorPos({ 400, 500 });
        queryFails = true;
        f.Finish();
        Check(f->_cursorReposition.has_value(), "Unknown clip ownership was restored unsafely");
        queryFails = false; setFails = true;
        f.Finish();
        Check(f->_cursorReposition.has_value(), "Failed restore discarded retry state");
        setFails = false;
        f.Finish();
        Check(!f->_cursorReposition && clip == desktop, "Transient failures could not recover");
        f->_ReliableSetCursorPos({ 400, 500 });
        queryFails = true;
        const auto failedUntil = std::chrono::steady_clock::now() + 20ms;
        PumpUntil([&] { return std::chrono::steady_clock::now() >= failedUntil; }, "Retry interval not reached");
        Check(f->_cursorReposition.has_value(), "Native retry discarded clip ownership");
        queryFails = false;
        PumpUntil([&] { return !f->_cursorReposition; }, "Native timer did not recover a transient restore query failure");
        f->_ReliableSetCursorPos({ 400, 500 });
        queryFails = true;
        f->_ReliableSetCursorPos({ 600, 700 });
        f->_ReliableSetCursorPos({ 650, 750 });
        Check(f->_nextCursorReposition == POINT{ 650, 750 }, "Failed restore dropped/reordered the latest relocation");
        queryFails = false;
        f.Finish();
        Check(f->_cursorReposition && cursor == POINT{ 650, 750 } && !f->_nextCursorReposition,
            "Recovering restore did not apply the deferred latest position");
        f.Finish();
    }
    {
        Fixture f;
        f->_isUnderCapture = true;
        f->_ReliableSetCursorPos({ 100, 200 });
        f.Finish();
        Check(sleepCalls == 0, "Capture relocation slept on presentation thread");
        PumpUntil([&] { return !f->_isCursorShapeInFlight; }, "Background cursor-shape query did not return");
        Check(postCalls == 1 && postedChild == reinterpret_cast<HWND>(uintptr_t(72)) &&
            postedSource == WPARAM(ScalingWindow::Get().source.hwnd) && LOWORD(postedArea) == HTCLIENT &&
            HIWORD(postedArea) == WM_MOUSEMOVE, "Async cursor shape changed WM_SETCURSOR semantics");
    }
    for (int scenario = 0; scenario < 7; ++scenario) {
        Fixture f;
        f->_isUnderCapture = true;
        { std::lock_guard lock(hitMutex); holdHitTest = true; }
        f->_ReliableSetCursorPos({ 400, 500 });
        f.Finish();
        PumpUntil([] { return hitActive.load() == 1; }, "Blocked hit-test fixture never started");
        if (scenario == 0) ++f->_cursorRepositionId;
        if (scenario == 1) f->_isUnderCapture = false;
        if (scenario == 2) cursor.x += 1;
        if (scenario == 3) ScalingWindow::Get().source.hwnd = reinterpret_cast<HWND>(uintptr_t(99));
        if (scenario == 4) windowsValid = false;
        if (scenario == 5) positionFails = true;
        if (scenario == 6) childRelated = false;
        ReleaseHitTest();
        PumpUntil([&] { return !f->_isCursorShapeInFlight; }, "Cancelled query did not finish");
        Check(postCalls == 0, "Late/stale shape query posted to source");
    }
    {
        Fixture f;
        f->_isUnderCapture = true;
        { std::lock_guard lock(hitMutex); holdHitTest = true; }
        f->_ReliableSetCursorPos({ 400, 500 }); f.Finish();
        PumpUntil([] { return hitActive.load() == 1; }, "Coalescing fixture did not start");
        const int initial = hitCalls;
        for (int i = 0; i < 20; ++i) {
            f->_ReliableSetCursorPos({ 500 + i, 600 + i }); f.Finish();
        }
        Check(hitCalls == initial, "Shape refresh created concurrent hit tests");
        ReleaseHitTest();
        PumpUntil([&] { return !f->_isCursorShapeInFlight; }, "Coalesced query did not drain");
        Check(hitCalls == initial + 1 && postCalls == 1 && hitMaximum == 1,
            "Shape requests were not bounded/coalesced to latest position");
    }
    {
        Fixture f;
        f->_isUnderCapture = true;
        f->_ReliableSetCursorPos({ 100, 200 });
        f->BeginShutdown();
        f->ReleasePendingCursorReposition();
        f.manager.reset();
        Check(cursor == POINT{ 0, 0 } && clip == desktop && sleepCalls == 1,
            "Destruction did not finish coordinate restoration synchronously");
        ScalingWindow::manager = nullptr;
        ++ScalingWindow::runId;
        PumpOnce();
        Check(clip == desktop, "Late timer changed a successor session");
    }
    {
        Fixture f;
        alignas(CursorManager) std::byte storage[sizeof(CursorManager)];
        auto* original = std::construct_at(reinterpret_cast<CursorManager*>(storage));
        ScalingWindow::manager = original;
        original->_isUnderCapture = true;
        { std::lock_guard lock(hitMutex); holdHitTest = true; }
        original->_ReliableSetCursorPos({ 400, 500 });
        original->_cursorReposition->deadline = std::chrono::steady_clock::time_point{};
        original->_CompleteCursorReposition(original->_cursorReposition->id);
        PumpUntil([] { return hitActive.load() == 1; }, "Retirement query never started");
        const std::weak_ptr lifetime = original->_lifetime;
        original->BeginShutdown(); original->ReleasePendingCursorReposition();
        std::destroy_at(original);
        ++ScalingWindow::runId;
        auto* successor = std::construct_at(reinterpret_cast<CursorManager*>(storage));
        ScalingWindow::manager = successor;
        successor->_ReliableSetCursorPos({ 700, 800 });
        successor->_cursorReposition->deadline = std::chrono::steady_clock::now() + 10s;
        const RECT successorClip = clip;
        const int calls = clipCalls;
        ReleaseHitTest();
        PumpUntil([&] { return lifetime.expired(); }, "Retired query retained its lifetime");
        Check(postCalls == 0 && clipCalls == calls && clip == successorClip && successor->_cursorReposition,
            "Old query touched reused manager storage or successor input");
        successor->BeginShutdown(); successor->ReleasePendingCursorReposition();
        std::destroy_at(successor);
        ScalingWindow::manager = f.manager.get();
    }
    ScalingWindow::dispatcher = nullptr;
    const auto shutdown = controller.ShutdownQueueAsync();
    PumpUntil([&] { return shutdown.Status() != winrt::Windows::Foundation::AsyncStatus::Started; }, "Dispatcher shutdown timed out");
    std::cout << "Production cursor relocation: " << checks << " checks passed; native timers/async, mocked input, no GPU/UI.\n";
    return 0;
} catch (const std::exception& error) {
    ReleaseHitTest();
    std::cerr << error.what() << '\n';
    return 1;
}
