// Filled from production sources by prepare_adaptive_presenter_recovery_test.py.
#include "PresenterFailure.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <vector>
using namespace Magpie;
using RealClock = std::chrono::steady_clock;
struct TestClock {
    using time_point = RealClock::time_point;
    static inline time_point time{};
    static time_point now() { return time; }
    static void Advance() { time += std::chrono::milliseconds(250); }
};
using DWORD = uint32_t; using UINT = uint32_t; using HRESULT = int32_t;
using HANDLE = void*; using HWND = void*;
constexpr DWORD WAIT_OBJECT_0 = 0, WAIT_TIMEOUT = 258, WAIT_FAILED = 0xffffffff;
constexpr DWORD QS_ALLINPUT = 1, MWMO_INPUTAVAILABLE = 1;
constexpr HRESULT S_OK = 0, S_FALSE = 1, E_FAIL = -1, E_UNEXPECTED = -2;
constexpr HRESULT DXGI_STATUS_OCCLUDED = 0x087a0001;
constexpr HRESULT DXGI_ERROR_DEVICE_REMOVED = -3, DXGI_ERROR_DEVICE_RESET = -4;
constexpr HRESULT DXGI_ERROR_DEVICE_HUNG = -5, DXGI_ERROR_DRIVER_INTERNAL_ERROR = -6;
constexpr UINT DXGI_FORMAT_UNKNOWN = 0, D3D11_QUERY_EVENT = 0, D3D11_ASYNC_GETDATA_DONOTFLUSH = 1;
constexpr bool FALSE = false;
static bool FAILED(HRESULT hr) { return hr < 0; }
static bool SUCCEEDED(HRESULT hr) { return hr >= 0; }
static HRESULT HRESULT_FROM_WIN32(DWORD code) { return -static_cast<HRESULT>(code); }
static DWORD GetLastError() { return 6; }
static void Require(bool value, const char* message) {
    if (!value) { std::cerr << message << '\n'; std::exit(42); }
}
struct POINT { long x = 0, y = 0; };
struct RECT { long left = 0, top = 0, right = 1280, bottom = 720; };
struct SIZE { long cx = 1280, cy = 720; };
struct D3D11_QUERY_DESC { UINT Query, MiscFlags; };
static DWORD fakeCapacity = WAIT_OBJECT_0;
static int capacityQueries = 0, resizeCalls = 0, presents = 0, clearCalls = 0;
static int queryEnds = 0, gpuWaits = 0, dwmWaits = 0, detachCalls = 0, commits = 0;
static UINT lastWidth = 0, lastHeight = 0, lastFlags = 0;
static bool contextBound = false, dcompAttached = true, windowResizing = false;
static bool dcompDrawing = false;
static HRESULT resizeResult = S_OK, bufferResult = S_OK, rtvResult = S_OK;
static HRESULT fakePresentResult = S_OK, removedResult = S_OK, queryCreateResult = S_OK;
static HRESULT queryResult = S_OK, detachResult = S_OK, fakeCommitResult = S_OK, fakeRestoreResult = S_OK;
static std::vector<DWORD> waitTimeouts;
static DWORD WaitForSingleObject(HANDLE, DWORD timeout) {
    ++capacityQueries; waitTimeouts.push_back(timeout); return fakeCapacity;
}
static DWORD MsgWaitForMultipleObjectsEx(DWORD, HANDLE*, DWORD timeout, DWORD, DWORD) {
    waitTimeouts.push_back(timeout); return fakeCapacity;
}
static bool IsWindowVisible(HWND) { return true; }
struct Object { int refs = 0; };
struct ID3D11Texture2D : Object {};
struct ID3D11RenderTargetView : Object {};
struct ID3D11Query : Object {};
static ID3D11Texture2D texture;
static ID3D11RenderTargetView target;
static ID3D11Query query;
namespace winrt {
template<class T> struct com_ptr {
    T* p = nullptr;
    com_ptr() = default;
    explicit com_ptr(T* value) : p(value) { if (p) ++p->refs; }
    com_ptr(const com_ptr& other) : com_ptr(other.p) {}
    ~com_ptr() { if (p) --p->refs; }
    com_ptr& operator=(const com_ptr& other) {
        if (other.p) ++other.p->refs;
        if (p) --p->refs;
        p = other.p; return *this;
    }
    com_ptr& operator=(std::nullptr_t) { if (p) --p->refs; p = nullptr; return *this; }
    T* get() const { return p; } T* operator->() const { return p; }
    T** put() { *this = nullptr; return &p; }
    explicit operator bool() const { return p != nullptr; }
};
}
template<class T> static void Assign(T** out, T* object) { ++object->refs; *out = object; }
namespace wil {
struct unique_event_nothrow {
    HANDLE get() const { return reinterpret_cast<HANDLE>(1); }
    explicit operator bool() const { return true; }
};
}
namespace fmt { template<class... T> std::string format(const char* message, T...) { return message; } }
struct Logger {
    static inline int warnings = 0;
    static Logger& Get() { static Logger value; return value; }
    template<class... T> void Info(T...) {} template<class... T> void Error(T...) {}
    template<class... T> void ComError(T...) {}
    template<class... T> void ComWarn(T...) { ++warnings; }
};
namespace FrameTrace {
enum class Event { CapacityBusy, PresentGpuWait, DcompCommit, RenderDecision, OverlayDeferred, OverlayContentionWait };
struct Scope {
    template<class... T> Scope(T...) {} template<class... T> void Data(T...) {} void End() {}
};
template<class... T> void Mark(T...) {} template<class... T> void Presentation(T...) {}
static int Tick() { return 0; }
}
struct ScalingWindow {
    RECT rect;
    static ScalingWindow& Get() { static ScalingWindow value; return value; }
    bool IsResizingOrMoving() const { return windowResizing; }
    bool IsFirstFramePending() const { return false; }
    HWND Handle() const { return nullptr; }
    const RECT& RendererRect() const { return rect; }
};
struct Win32Helper {
    static SIZE GetSizeOfRect(const RECT& rect) { return { rect.right - rect.left, rect.bottom - rect.top }; }
    static void WaitForDwmComposition() { ++dwmWaits; }
};
struct IDXGISwapChain4 : Object {
    HRESULT ResizeBuffers(UINT count, UINT w, UINT h, UINT format, UINT flags) {
        ++resizeCalls; lastWidth = w; lastHeight = h; lastFlags = flags;
        Require(!contextBound && texture.refs == 0 && target.refs == 0, "ResizeBuffers must release direct and indirect buffer references");
        Require(count == 0 && format == DXGI_FORMAT_UNKNOWN && flags == 0x42, "resize must preserve buffer count, HDR/SDR format and immutable VRR flags");
        return resizeResult;
    }
    HRESULT GetBuffer(int, int, ID3D11Texture2D** out) {
        if (SUCCEEDED(bufferResult)) Assign(out, &texture);
        return bufferResult;
    }
};
struct IDCompositionVirtualSurface : Object {
    HRESULT BeginDraw(void*, int, winrt::com_ptr<ID3D11Texture2D>* out, POINT*) {
        Require(!dcompDrawing, "prior DComp BeginDraw must be closed even when view creation fails");
        dcompDrawing = true; Assign(out->put(), &texture); return S_OK;
    }
    HRESULT EndDraw() { dcompDrawing = false; return S_OK; }
};
struct IDCompositionDesktopDevice : Object { HRESULT Commit() { ++commits; return fakeCommitResult; } };
struct IDCompositionTarget : Object {};
struct IDCompositionVisual2 : Object {
    HRESULT SetContent(IDCompositionVirtualSurface* surface) {
        if (!surface) ++detachCalls;
        const HRESULT hr = surface ? fakeRestoreResult : detachResult;
        if (SUCCEEDED(hr)) dcompAttached = surface != nullptr;
        return hr;
    }
};
struct Device {
    HRESULT GetDeviceRemovedReason() { return removedResult; }
    HRESULT CreateRenderTargetView(ID3D11Texture2D*, void*, ID3D11RenderTargetView** out) {
        if (SUCCEEDED(rtvResult)) Assign(out, &target);
        return rtvResult;
    }
    HRESULT CreateQuery(const D3D11_QUERY_DESC* desc, ID3D11Query** out) {
        Require(desc->Query == D3D11_QUERY_EVENT && !desc->MiscFlags, "completion must use an event query");
        if (SUCCEEDED(queryCreateResult)) Assign(out, &query);
        return queryCreateResult;
    }
};
struct Context {
    void ClearState() { contextBound = false; ++clearCalls; }
    void Flush() {}
    void DiscardView(ID3D11RenderTargetView* view) { Require(view != nullptr, "Present needs a rebuilt RTV"); }
    void End(ID3D11Query*) { ++queryEnds; }
    HRESULT GetData(ID3D11Query*, void* data, UINT size, UINT flags) {
        Require(!data && size == 0 && flags == D3D11_ASYNC_GETDATA_DONOTFLUSH, "completion polling must not flush or block");
        return queryResult;
    }
};
struct DeviceResources {
    mutable Device device; mutable Context context;
    Device* GetD3DDevice() const { return &device; }
    Context* GetD3DDC() const { return &context; }
};
struct ReflexController {
    bool available = false; int renderStarts = 0, renderEnds = 0;
    void SetPresentationAvailable(bool value) { available = value; }
    void FrontendRender(uint64_t, uint64_t, bool start) { start ? ++renderStarts : ++renderEnds; }
    void Present(uint64_t, uint64_t, bool, bool) {}
};
enum class ContentFrameCountSource { Reported };
struct ContentFrameCount {
    std::optional<uint32_t> count; ContentFrameCountSource source;
    std::optional<TestClock::time_point> time;
};
struct PresenterBase {
    const DeviceResources* _deviceResources = nullptr;
    std::optional<uint32_t> _lastPresentedFrameCount = 0;
    TestClock::time_point _lastSubmissionTime{};
    bool vrrPath = true;
    struct Policy { UINT ResizeFlags() const { return 0x42; } } _vrr;
    void _RefreshVRREnvironment(bool) {}
    void _VRRPathAvailable(bool value) { vrrPath = value; }
    void _WaitForGpu() { ++gpuWaits; }
    bool HasIndependentOverlay() const { return false; }
    HRESULT _PresentWithVRR(IDXGISwapChain4*, bool retry) {
        Require(retry, "normal VRR retry policy must remain enabled");
        ++presents; if (fakePresentResult == S_OK) contextBound = false;
        return fakePresentResult;
    }
};
/* GATE */
#define IID_PPV_ARGS(p) 0, p
/* CLASS */
bool AdaptivePresenter::_ResizeDCompVisual(HWND) noexcept { dcompAttached = true; return true; }
/* METHODS */

// Compile the real idle-routing decisions as well as the presenter. All quiet
// source/UI predicates default false, so recovery cannot piggyback on input.
struct Renderer {
    AdaptivePresenter* _presenter = nullptr;
    bool softwareFG = false, _hasFrameGeneration = false, proxyFG = false;
	std::atomic<bool> _backendContentInFlight{false};
	std::optional<TestClock::time_point> _overlayContentionUntil;
    std::atomic<bool> _synchronousFramePresentationEnabled{true};
    std::atomic<uint32_t> _pendingDLSSFGFrontendFrames{0}, _latestSharedTextureSlot{0};
    uint32_t _sharedTextureSlotCount = 1;
    std::array<uint64_t, 1> _lastAccessMutexKeys{1};
    std::array<std::atomic<uint64_t>, 1> _sharedTextureMutexKeys{1};
    std::atomic<uint64_t> _frameSyncAcknowledgedKey{1};
    bool _frameSyncEnabled = false, _frameSyncUsesSharedSlot = false;
    bool _frontendBaseNeedsPresent = false, _isPassThroughActive = false;
    bool _frontendPresentedBaseValid = true, _pendingFrontendFrame = false;
    std::optional<int> _frontendPacingDeadline;
    struct Metadata { uint64_t frameId = 1, captureSequence = 1, resourceGeneration = 1, outputHistoryRevision = 1; };
    Metadata _frontendFrameMetadata, _frontendPresentedFrameMetadata;
    struct Cursor {
        bool NeedRedraw() const { return false; } bool IsMinimumRefreshDue() const { return false; }
        bool HasVisibilityTransition() const { return false; } bool HasOriginalRefreshPending() const { return false; }
    } _cursorDrawer;
    struct Overlay { bool NeedRedraw(double) const { return false; } bool HasCriticalInput() const { return false; } } _overlayDrawer;
    struct Timer { double FPS() const { return 60; } } _stepTimer;
    struct Clock {
		bool IsDue(TestClock::time_point, bool urgent) const { return urgent; }
		std::chrono::nanoseconds PollInterval() const { return std::chrono::milliseconds(8); }
	} _overlayPresentationClock;
    bool _HasPendingOverlayAction() const { return false; }
	bool HasPendingContent() const { return false; }
    bool _HasSoftwareFrameGenerator() const { return softwareFG; }
    bool _HasProxyFrameGenerator() const { return proxyFG; }
    bool _TryIdleRedraw() const { return false; }
    bool _FrontendOverlayRender() const { return false; }
    bool _SubmitFrontendFrame() const { return true; }
    int failureReports = 0;
    void _ReportPresenterFailure() { ++failureReports; }
    bool _FrontendRender(bool, uint32_t, void*, bool stable) {
        Require(stable, "static recovery must replay the stable base without consuming a content ID");
        if (!_CanRenderOverlay() || !_presenter->PrepareFrame()) return false;
        winrt::com_ptr<ID3D11Texture2D> tex;
        winrt::com_ptr<ID3D11RenderTargetView> rtv; POINT point;
        if (!_presenter->BeginFrame(tex, rtv, point)) return false;
        contextBound = true;
        return _presenter->EndFrame();
    }
    bool Render(bool force = false, bool waitForGpu = false) noexcept;
    bool RenderOverlay() noexcept;
    bool _CanRenderOverlay() noexcept;
};
/* FRONTEND */

static void Reset() {
    Require(texture.refs == 0 && target.refs == 0 && query.refs == 0, "prior session must release all resources");
    TestClock::time = {};
    fakeCapacity = WAIT_OBJECT_0;
    capacityQueries = resizeCalls = presents = clearCalls = queryEnds = gpuWaits = dwmWaits = detachCalls = commits = 0;
    lastWidth = lastHeight = lastFlags = 0;
    contextBound = windowResizing = dcompDrawing = false; dcompAttached = true;
    resizeResult = bufferResult = rtvResult = fakePresentResult = removedResult = queryCreateResult = S_OK;
    queryResult = detachResult = fakeCommitResult = fakeRestoreResult = S_OK;
    waitTimeouts.clear(); Logger::warnings = 0; ScalingWindow::Get().rect = {};
}
struct Session {
    DeviceResources resources;
    IDXGISwapChain4 chain;
    IDCompositionVirtualSurface surface;
    IDCompositionDesktopDevice composition;
    IDCompositionVisual2 visual;
    ReflexController reflex;
    AdaptivePresenter p;
    Session() {
        p._deviceResources = &resources;
        p._dxgiSwapChain = winrt::com_ptr<IDXGISwapChain4>(&chain);
        p._dcompSurface = winrt::com_ptr<IDCompositionVirtualSurface>(&surface);
        p._dcompDevice = winrt::com_ptr<IDCompositionDesktopDevice>(&composition);
        p._dcompVisual = winrt::com_ptr<IDCompositionVisual2>(&visual);
        p._backBuffer = winrt::com_ptr<ID3D11Texture2D>(&texture);
        p._backBufferRtv = winrt::com_ptr<ID3D11RenderTargetView>(&target);
        p._isDCompPresenting = true;
        p.SetReflexController(&reflex);
    }
    bool EndResize() { bool redraw = false; p.OnEndResize(redraw); return redraw; }
    bool Frame() {
        if (!p.PrepareFrame()) return false;
        winrt::com_ptr<ID3D11Texture2D> tex;
        winrt::com_ptr<ID3D11RenderTargetView> rtv; POINT point;
        if (!p.BeginFrame(tex, rtv, point)) return false;
        contextBound = true;
        return p.EndFrame();
    }
};
int main() {
    Reset(); {
        Session s; contextBound = true;
        Require(s.EndResize(), "normal resize exit must request the replacement frame");
        Require(s.Frame() && !dcompAttached && !s.p._isSwitchingToSwapChain, "successful Present, GPU completion and Commit must finish recovery");
        Require(capacityQueries == 1 && resizeCalls == 1 && gpuWaits == 0, "recovery must retain the fakeCapacity token and avoid blocking GPU waits");
        Require(!s.p.NeedsRedraw() && s.p.SupportsDeferredPresent(), "normal deferred presentation must resume after recovery");
        const auto queries = queryEnds;
        Require(s.Frame() && queryEnds == queries, "normal frames must not create recovery queries");
    }
    for (HRESULT result : { E_FAIL, DXGI_STATUS_OCCLUDED, S_FALSE }) {
        Reset(); Session s; Require(s.EndResize(), "buffers should be ready"); fakePresentResult = result;
        s.Frame();
        Require(dcompAttached && detachCalls == 0 && s.p._isDCompPresenting, "failed or ineffective Present must preserve the DComp fallback");
        Require(s.p._lastPresentedFrameCount == 0u, "ineffective Present must not count as content");
        Require(!s.p.HasFatalError(), "occlusion/transient errors must not terminate the session");
        fakePresentResult = S_OK; TestClock::Advance();
        Require(s.Frame() && !dcompAttached, "transient Present must recover on a later attempt");
    }
    for (int stage = 0; stage < 4; ++stage) {
        Reset(); Session s; contextBound = true;
        if (stage == 0) fakeCapacity = WAIT_TIMEOUT;
        if (stage == 1) resizeResult = E_FAIL;
        if (stage == 2) bufferResult = E_FAIL;
        if (stage == 3) rtvResult = E_FAIL;
        Require(!s.EndResize() && s.p._isDCompPresenting, "resize/fakeCapacity/buffer/view failures must keep DComp rendering");
        const int queries = capacityQueries, resizes = resizeCalls;
        Require(!s.p.NeedsRedraw(), "retry must be delayed");
        for (int frame = 0; frame < 10; ++frame) Require(s.p.PrepareFrame(), "DComp remains usable while retry is delayed");
        Require(resizeCalls == resizes && capacityQueries == queries, "drawing DComp must not busy-retry recovery");
        fakeCapacity = WAIT_OBJECT_0; resizeResult = bufferResult = rtvResult = S_OK;
        TestClock::Advance(); Require(s.p.NeedsRedraw() && s.Frame() && !dcompAttached, "each transient stage must recover");
        Require(capacityQueries == (stage == 0 ? 2 : 1), "an acquired token must survive failed resize/buffer/view attempts");
        Require(std::all_of(waitTimeouts.begin(), waitTimeouts.end(), [](DWORD t) { return t == 0; }), "recovery fakeCapacity checks must be nonblocking");
    }
    Reset(); {
        Session s; s.EndResize(); queryResult = S_FALSE;
        Require(s.Frame() && dcompAttached && detachCalls == 0, "GPU work still pending must keep the fallback");
        Require(s.Frame() && queryEnds == 1 && gpuWaits == 0, "pending GPU completion must be polled, not restarted or synchronously waited");
        queryResult = S_OK;
        Require(s.Frame() && !dcompAttached && dwmWaits == 1, "completed replacement must detach after compositor synchronization");
    }
    for (int stage = 0; stage < 4; ++stage) {
        Reset(); Session s; s.EndResize();
        if (stage == 0) queryCreateResult = E_FAIL;
        if (stage == 1) queryResult = E_FAIL;
        if (stage == 2) detachResult = E_FAIL;
        if (stage == 3) fakeCommitResult = E_FAIL;
        s.Frame();
        Require(dcompAttached && s.p._isDCompPresenting, "failed completion/detach/Commit must retain or restore fallback content");
        queryCreateResult = queryResult = detachResult = fakeCommitResult = S_OK;
        TestClock::Advance(); Require(s.Frame() && !dcompAttached, "cleanup failure must recover later");
    }
    Reset(); {
        Session s; s.EndResize(); fakeCommitResult = E_FAIL; fakeRestoreResult = E_FAIL;
        s.Frame(); Require(s.p.HasFatalError() && !s.p.NeedsRedraw(), "unrestorable fallback must report a terminal failure");
    }
    for (bool pendingGpu : { false, true }) {
        Reset(); Session s;
        if (pendingGpu) queryResult = S_FALSE; else fakeCapacity = WAIT_TIMEOUT;
        s.EndResize(); s.Frame();
        for (int attempt = 0; attempt < 12; ++attempt) { TestClock::Advance(); s.Frame(); }
        Require(s.p._recoveryAttempts == 8 && !s.p._nextRecoveryAttempt && s.p._isDCompPresenting && dcompAttached,
            "persistent fakeCapacity/GPU stalls must exhaust a bounded budget and stay on DComp");
        const int calls = resizeCalls; TestClock::Advance(); s.Frame();
        Require(resizeCalls == calls && !s.p.NeedsRedraw(), "exhausted recovery must not restart by itself");
        fakeCapacity = WAIT_OBJECT_0; queryResult = S_OK;
        Require(s.EndResize() && s.Frame() && !dcompAttached, "explicit resize exit can start a fresh recovery cycle");
    }
    Reset(); {
        Session s; s.p._dxgiSwapChain = nullptr;
        Require(!s.EndResize() && !s.p.NeedsRedraw() && s.Frame(), "intentional DComp must never attempt DXGI recovery");
        Require(resizeCalls == 0 && presents == 0 && detachCalls == 0, "DirectFlip-disabled output must stay on DComp");
    }
    for (bool pendingGpu : { false, true }) {
        Reset(); Session s;
        if (pendingGpu) queryResult = S_FALSE; else fakeCapacity = WAIT_TIMEOUT;
        s.EndResize(); s.Frame(); windowResizing = true; TestClock::Advance();
        Require(!s.p.NeedsRedraw() && s.p.OnResize(), "new resize must suspend and cancel old recovery");
        Require(!s.p._nextRecoveryAttempt && !s.p._switchCompletionQuery, "new resize must retire old completion state");
        windowResizing = false; fakeCapacity = WAIT_OBJECT_0; queryResult = S_OK;
        ScalingWindow::Get().rect.right = 1920;
        Require(s.EndResize() && s.Frame() && lastWidth == 1920, "new recovery must use the current renderer dimensions");
    }
    for (int stage = 0; stage < 4; ++stage) {
        Reset(); Session s;
        if (stage == 0) removedResult = DXGI_ERROR_DEVICE_REMOVED;
        if (stage == 1) resizeResult = DXGI_ERROR_DEVICE_RESET;
        s.EndResize();
        if (stage == 2) fakePresentResult = DXGI_ERROR_DEVICE_REMOVED;
        if (stage == 3) { fakeCapacity = WAIT_FAILED; s.p._frameLatencyGate.Reset(); }
        s.Frame();
        Require(s.p.HasFatalError() && !s.p._nextRecoveryAttempt && !s.p.PrepareFrame(), "device/event failure must terminate retries");
        auto code = s.p.GetFailure()->code; s.p._Fail("later cleanup", E_FAIL);
        Require(s.p.GetFailure()->code == code, "first terminal cause must be preserved");
    }
    Reset(); {
        Session s; s.p._isDCompPresenting = false; fakeCapacity = WAIT_TIMEOUT;
        Require(!s.p.PrepareFrame() && s.p.WasFrameCapacityBusy() && !s.p.HasFatalError(), "normal backpressure must remain retryable");
        fakeCapacity = WAIT_FAILED; Require(!s.p.WaitForFrameCapacity(10) && s.p.HasFatalError(), "message-pump wait failure must propagate");
    }
    for (int output = 0; output < 3; ++output) {
        Reset(); Session s; Renderer renderer; renderer._presenter = &s.p;
        // Ordinary, DLSSFG and VFG share this presenter and software-FG route.
        renderer.softwareFG = renderer._hasFrameGeneration = output != 0;
        fakeCapacity = WAIT_TIMEOUT; s.EndResize();
        Require(!renderer.Render(), "quiet output must not add a frame before recovery is due");
        fakeCapacity = WAIT_OBJECT_0; TestClock::Advance();
        if (output != 0) {
            renderer._pendingDLSSFGFrontendFrames = 1;
            Require(!renderer.Render(), "queued FG content must retain priority over recovery replay");
            renderer._pendingDLSSFGFrontendFrames = 0;
        }
        Require(renderer.Render() && !dcompAttached && presents == 1, "quiet ordinary/software-FG output must service a due recovery");
        Require(!renderer.Render(), "successful recovery must not enable continuous static redraws");
    }
    Reset(); {
        Session s; s.EndResize(); queryResult = S_FALSE; s.Frame();
        Require(s.p._switchCompletionQuery && s.p._nextRecoveryAttempt, "session must have outstanding recovery for destruction coverage");
    }
    Reset(); {
        Session s; TestClock::Advance();
        Require(!s.p.NeedsRedraw() && !s.p._switchCompletionQuery, "a new session cannot inherit the old session's recovery");
        rtvResult = E_FAIL;
        Require(!s.Frame() && !dcompDrawing, "failed DComp RTV must balance BeginDraw before retry");
        rtvResult = S_OK;
        Require(s.Frame() && !dcompDrawing, "DComp must remain drawable after an RTV failure");
    }
    Reset();
    std::cout << "PASS: production adaptive recovery failures, bounded retries, tokens, resize lifecycle and static frontend routing\n";
}
