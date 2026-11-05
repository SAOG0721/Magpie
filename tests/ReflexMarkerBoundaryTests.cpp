// Template populated with production blocks by prepare_reflex_marker_boundary_test.py.
#include "ReflexController.h"
#include "FrameUpdateReason.h"
#include <array>
#include <chrono>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
using namespace Magpie;
using DWORD = unsigned long;
using HRESULT = int;
using HANDLE = int;
using UINT = unsigned;
constexpr DWORD WAIT_OBJECT_0 = 0, WAIT_TIMEOUT = 258, WAIT_FAILED = 0xffffffff;
constexpr int S_OK = 0, D3D11_USAGE_DEFAULT = 0, DXGI_PRESENT_ALLOW_TEARING = 1;
constexpr UINT D3D11_BIND_SHADER_RESOURCE = 8;
static void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
static std::vector<std::string> events;
static DWORD capacity = WAIT_OBJECT_0;
static int capacityQueries = 0, acquireResult = S_OK, fakeReleaseResult = S_OK, createResult = S_OK;
static bool renderOpen = false;
static bool coverageViolation = false;
static std::vector<std::pair<uint64_t, uint64_t>> renderIds;
DWORD WaitForSingleObject(HANDLE, DWORD) { ++capacityQueries; return capacity; }
DWORD MsgWaitForMultipleObjectsEx(DWORD, HANDLE*, DWORD, DWORD, DWORD) { return capacity; }
constexpr DWORD QS_ALLINPUT = 1, MWMO_INPUTAVAILABLE = 1;
static bool FAILED(HRESULT hr) { return hr < 0; }
static bool SUCCEEDED(HRESULT hr) { return hr >= 0; }
static int HRESULT_FROM_WIN32(int hr) { return -hr; }
static int GetLastError() { return 6; }
constexpr int E_UNEXPECTED = -2;
static void SetEvent(HANDLE) {}
struct Handle { HANDLE get() const { return 1; } };
struct POINT { long x = 0, y = 0; };
struct RECT { long left = 0, top = 0, right = 10, bottom = 10; };
struct D3D11_TEXTURE2D_DESC {
    UINT Width = 10, Height = 10, Format = 1, Usage = 0, BindFlags = 0, CPUAccessFlags = 0, MiscFlags = 0;
    UINT MipLevels = 1, ArraySize = 1;
    struct { UINT Count = 1, Quality = 0; } SampleDesc;
};
struct ID3D11Texture2D { void GetDesc(D3D11_TEXTURE2D_DESC* d) { *d = {}; } };
struct ID3D11RenderTargetView {};
struct IDXGIKeyedMutex {};
namespace winrt {
template<class T> struct com_ptr {
    T* p = nullptr;
    T* get() const { return p; }
	T* operator->() const { return p; }
    T** put() { p = nullptr; return &p; }
    explicit operator bool() const { return p != nullptr; }
    com_ptr& operator=(std::nullptr_t) { p = nullptr; return *this; }
};
}
namespace wil {
template<class F> struct Exit {
    F f; bool active = true;
    ~Exit() { if (active) f(); }
    void release() { active = false; }
};
template<class F> Exit<F> scope_exit(F f) { return {f}; }
}
namespace fmt { template<class... T> int format(T...) { return 0; } }
struct Logger {
    static Logger& Get() { static Logger l; return l; }
    void Info(int) {} void Warn(int) {} void ComError(const char*, int) {} void Win32Error(const char*) {}
};
namespace FrameTrace {
enum class Event { FrontendBase, FrontendAcquireBusy, FrontendAcquire, CapacityBusy, BeginFrame, FrameSyncAck };
struct Scope {
    template<class... T> Scope(T...) {} template<class... T> void Data(T...) {}
    void FrameId(uint64_t) {} void End() {}
};
template<class... T> void Mark(T...) {} void SetFrame(uint64_t) {} int Tick() { return 0; }
}
struct Driver final : ReflexDriver {
    ReflexConfigurationResult Configure(ReflexSettings s) noexcept override { return {0, 0, true, s.lowLatency}; }
    ReflexSleepStatus QueryStatus() noexcept override { return {0, true}; }
    int Sleep() noexcept override { return 0; }
    int Marker(ReflexMarker, uint64_t) noexcept override { return 0; }
    int RegisterGenerationQueue(ID3D12CommandQueue*) noexcept override { return 0; }
    int Generation(ID3D12CommandQueue*, uint64_t, uint64_t, bool) noexcept override { return 0; }
    int FrontendRender(uint64_t frame, uint64_t present, bool start) noexcept override {
        events.push_back(start ? "render-start" : "render-end"); renderOpen = start;
        if (start) renderIds.emplace_back(frame, present);
        return 0;
    }
    int Present(uint64_t, uint64_t, bool, bool start) noexcept override {
        events.push_back(start ? "present-start" : "present-end"); return 0;
    }
    void ReportFailure(const char*, int) noexcept override {}
};
static ID3D11Texture2D texture;
static ID3D11RenderTargetView target;
struct Device {
    HRESULT CreateTexture2D(const D3D11_TEXTURE2D_DESC*, void*, ID3D11Texture2D** p) {
        coverageViolation |= !renderOpen;
        if (SUCCEEDED(createResult)) *p = &texture;
        return createResult;
    }
    HRESULT CreateRenderTargetView(ID3D11Texture2D*, void*, ID3D11RenderTargetView** p) { *p = &target; return S_OK; }
};
struct Context {
    void CopyResource(ID3D11Texture2D*, ID3D11Texture2D*) {
        coverageViolation |= !renderOpen;
        events.push_back("copy");
    }
};
/* MOTION_TEXTURE */
struct Resources {
    Device device; Context context;
    Device* GetD3DDevice() { return &device; } Context* GetD3DDC() { return &context; }
    bool IsTearingSupported() const { return false; }
};
enum class ScalingError { PassThroughUnavailable };
struct Window {
    bool isVRREnabled = false;
    std::function<void(int, ScalingError, const char*, uint32_t)> reportErrorDetails;
    const Window& Options() const { return *this; } const Window& SrcTracker() const { return *this; }
    int Handle() const { return 0; } RECT RendererRect() const { return {}; }
};
struct ScalingWindow { static Window& Get() { static Window w; return w; } };
HRESULT AcquirePresentationTextures(const std::array<IDXGIKeyedMutex*, 3>&, uint64_t, int, size_t* failed = nullptr) {
    if (failed) *failed = 0; return acquireResult;
}
HRESULT ReleasePresentationTextures(const std::array<IDXGIKeyedMutex*, 3>&, uint64_t) { return fakeReleaseResult; }
struct PassThrough {
    Context* context = nullptr;
    IDXGIKeyedMutex* FrontendMutex(uint32_t) { return nullptr; } bool DisableSharing() { return false; }
    bool Consume(uint32_t) { context->CopyResource(&texture, &texture); return true; }
};
struct Metadata {
    uint64_t frameId = 40, captureSequence = 2, resourceGeneration = 1, outputHistoryRevision = 1;
    int64_t timestamp100ns = 10, captureAdmissionTime100ns = 9;
    bool generated = false, contentRedraw = false;
	FrameUpdateReason updateReason = FrameUpdateReason::NewCapture;
};
/* GATE */
#define IID_PPV_ARGS(p) 0, p
struct Surface {
    HRESULT BeginDraw(void*, int, winrt::com_ptr<ID3D11Texture2D>*, POINT*) { return -1; }
    HRESULT EndDraw() { return S_OK; }
};
struct SwapChain { HRESULT Present(int, UINT) { Require(!renderOpen, "render must end before Present"); events.push_back("present"); return S_OK; } };
struct AdaptivePresenter {
    ReflexController* _reflex = nullptr;
    uint64_t _reflexFrameId = 0, _reflexPresentId = 0;
    bool _reflexRendering = false, _reflexGenerated = false, _frameCapacityBusy = false, _isDCompPresenting = false;
    FrameLatencyGate _frameLatencyGate;
    Handle _frameLatencyWaitableObject;
    SwapChain swap; winrt::com_ptr<SwapChain> _dxgiSwapChain{&swap}; Surface* _dcompSurface = nullptr;
    HRESULT _PresentWithVRR(SwapChain* chain, bool) { return chain->Present(0, 0); }
    Resources* _deviceResources = nullptr;
    std::chrono::steady_clock::time_point _lastSubmissionTime{};
    winrt::com_ptr<ID3D11Texture2D> _backBuffer{&texture};
    winrt::com_ptr<ID3D11RenderTargetView> _backBufferRtv{&target};
    void SetReflexFrame(uint64_t, uint64_t, bool) noexcept;
    void BeginReflexRender() noexcept; void CancelReflexRender() noexcept;
    bool PrepareFrame() noexcept;
    // Recovery state is covered by AdaptivePresenterRecoveryTests; keep this
    // fixture focused on production admission and marker ordering.
    void _TryRecoverSwapChain() noexcept {}
    bool HasFatalError() const noexcept { return false; }
    void _Fail(const char*, HRESULT) noexcept {}
    bool _CheckDeviceFailure(const char*, HRESULT) noexcept { return false; }
    bool BeginFrame(winrt::com_ptr<ID3D11Texture2D>&, winrt::com_ptr<ID3D11RenderTargetView>&, POINT&) noexcept;
    bool WasFrameCapacityBusy() const { return _frameCapacityBusy; }
    template<class... T> void SetSourceTiming(T...) {} template<class... T> void SetFrameGuidance(T...) {}
    void SetCaptureAdmissionTime(int64_t) {} void SetOutputHistoryRevision(uint64_t) {}
    void SetFrameGuidanceAvailability(int) {}
    bool BeginIdleFrame(winrt::com_ptr<ID3D11Texture2D>& tex, winrt::com_ptr<ID3D11RenderTargetView>& rtv, POINT& offset) noexcept {
        return BeginFrame(tex, rtv, offset);
    }
    bool EndFrame() {
/* PRESENT */
		(void)tracePresent;
        _frameLatencyGate.Reset(); return SUCCEEDED(presentResult);
    }
};
/* METHODS */
struct FrontendRenderTimings { std::chrono::steady_clock::duration beginFrame{}; bool capacityBusy = false; };
struct Renderer {
    enum class FrontendBaseResult { Ready, Retry, Dropped };
    uint32_t _sharedTextureSlotCount = 1;
    std::array<winrt::com_ptr<ID3D11Texture2D>, 1> _frontendSharedTextures{{{&texture}}}, _frontendSharedMotionTextures{{{&texture}}};
    IDXGIKeyedMutex mutex;
    std::array<winrt::com_ptr<IDXGIKeyedMutex>, 1> _frontendSharedTextureMutexes{{{&mutex}}}, _frontendSharedMotionTextureMutexes{};
    std::array<std::mutex, 1> _sharedTextureAccessMutexes;
    std::array<std::atomic<uint64_t>, 1> _sharedTextureMutexKeys{3}, _sharedTextureCaptureSequences{2}, _sharedTextureResourceGenerations{1}, _sharedTextureFrameIds{40}, _sharedMotionFrameIds{40};
    std::array<std::atomic<bool>, 1> _sharedMotionValid{true}, _sharedMotionReset{false};
    std::array<uint64_t, 1> _discardedFrontendKeys{}, _lastAccessMutexKeys{};
    std::atomic<uint64_t> _activeCaptureSequence{2}, _activeResourceGeneration{1}, _frameSyncAcknowledgedKey{2};
    std::atomic<uint32_t> _latestSharedTextureSlot{0};
    std::array<std::pair<uint64_t, uint64_t>, 1> _sharedReflexIds{{{40, 90}}};
    std::pair<uint64_t, uint64_t> _frontendReflexIds{};
    std::array<std::shared_ptr<int>, 1> _sharedScreenshotFrames{};
    std::shared_ptr<int> _frontendScreenshotFrame;
    std::array<Metadata, 1> _sharedFrameMetadata{};
    Metadata _frontendFrameMetadata, _frontendPresentedFrameMetadata;
    uint64_t _frontendCaptureFrameId = 0, _frontendMotionFrameId = 0;
    bool _frontendBaseValid = false, _frontendBaseNeedsPresent = false, _frontendPresentedBaseValid = false;
    bool _frameSyncEnabled = true, _frameSyncUsesSharedSlot = true, _isPassThroughActive = false;
    bool _frontendMotionValid = false, _frontendMotionReset = false;
    bool _isFSR3FrameGenerationActive = false;
    int _frontendMotionAvailability = 0;
    std::array<int, 1> _sharedMotionAvailability{};
    struct IdleClock { void Reset() {} } _idlePresentationClock;
    bool _HasProxyFrameGenerator() const noexcept { return false; }
    Handle _frameSyncConsumedEvent;
    RECT _destRect;
    Resources _frontendResources;
    PassThrough _passThroughFrames{_frontendResources.GetD3DDC()};
    AdaptivePresenter* _presenter;
    winrt::com_ptr<ID3D11Texture2D> _frontendBaseTexture, _frontendPresentedBaseTexture, _frontendMotionTexture;
    void SetPassThroughActive(bool value) { _isPassThroughActive = value; }
    // Diagnostic-only sampling is outside this marker boundary fixture.
    uint32_t _FrontendDiagnosticReasons() const noexcept { return 0; }
    void _ReportPresenterFailure() noexcept {}
    FrontendBaseResult _UpdateFrontendBase(uint32_t sharedTextureSlot) noexcept;
    bool Render(uint32_t sharedTextureSlot, FrontendRenderTimings* timings = nullptr, bool stableBaseOnly = false, bool* droppedFrame = nullptr, bool idleRedraw = false) {
/* ADMISSION */
        (void)diagnosticReasons;
        _frontendResources.GetD3DDC()->CopyResource(frameTex.get(), baseTexture);
        return _presenter->EndFrame();
    }
};
/* UPDATE */
static void Reset() {
    events.clear(); renderIds.clear(); renderOpen = coverageViolation = false; capacityQueries = 0;
    capacity = WAIT_OBJECT_0; acquireResult = fakeReleaseResult = createResult = S_OK;
}
int main() {
    try {
        ReflexController reflex; reflex.Initialize(std::make_unique<Driver>());
        for (bool generated : {false, true}) {
            Reset(); AdaptivePresenter presenter; Renderer renderer; renderer._presenter = &presenter;
            presenter._reflex = &reflex; presenter._deviceResources = &renderer._frontendResources;
            renderer._sharedFrameMetadata[0].generated = generated;
            Require(renderer.Render(0), "ready content must present");
            Require(!coverageViolation, "texture allocation and every GPU copy must follow the frontend render marker");
            Require(events == std::vector<std::string>{"render-start", "copy", "copy", "copy", "copy", "render-end", "present-start", "present", "present-end"},
                "one render interval must cover base, reference, motion and draw work before Present");
            Require(renderIds == std::vector<std::pair<uint64_t,uint64_t>>{{40,90}} && capacityQueries == 1,
                "immutable slot IDs and the capacity token must survive BeginFrame without a second marker/wait");
        }
        for (int failure = 0; failure < 5; ++failure) {
            Reset(); AdaptivePresenter presenter; Renderer renderer; renderer._presenter = &presenter;
            presenter._reflex = &reflex; presenter._deviceResources = &renderer._frontendResources;
            if (failure == 0) capacity = WAIT_TIMEOUT;
            if (failure == 1) acquireResult = -static_cast<int>(WAIT_TIMEOUT);
            if (failure == 2) renderer._sharedTextureCaptureSequences[0] = 1;
            if (failure == 3) createResult = -1;
            if (failure == 4) fakeReleaseResult = -1;
            FrontendRenderTimings timings; bool dropped = false;
            Require(!renderer.Render(0, &timings, false, &dropped), "capacity/acquisition/stale/allocation failure must retry or drop");
            Require(!renderOpen && !presenter._reflexRendering && std::count(events.begin(), events.end(), "present") == 0,
                "failure exits must close markers without inventing a Present");
            Require(failure >= 3 ? std::count(events.begin(), events.end(), "render-start") == 1 &&
                std::count(events.begin(), events.end(), "render-end") == 1 : events.empty(),
                "waits and stale slots must not start rendering; allocation failure must balance it");
            if (failure == 0) Require(timings.capacityBusy, "early capacity timeout must reach the outer pump");
            Reset(); renderer._sharedTextureCaptureSequences[0] = 2;
            renderer._sharedTextureMutexKeys[0] = 5;
            Require(renderer.Render(0), "retry must recover and present the correct IDs");
        }
        Reset(); AdaptivePresenter presenter; presenter._reflex = &reflex;
        presenter.SetReflexFrame(40,90,false); presenter.BeginReflexRender();
        presenter.CancelReflexRender(); presenter.CancelReflexRender();
        presenter.SetReflexFrame(0,0,false); presenter.BeginReflexRender();
        Require(events == std::vector<std::string>{"render-start", "render-end"}, "resize/cancel must close once and overlay must not invent content IDs");
        presenter._isDCompPresenting = true;
        presenter.SetReflexFrame(41,91,false); presenter.BeginReflexRender();
        Require(events.size() == 2, "DComp must not start a DXGI Reflex render interval");
        std::cout << "PASS: production frontend capacity-before-copy, immutable IDs, base/reference/motion/draw coverage, retry cleanup and DComp/overlay cancellation\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 42; }
}
