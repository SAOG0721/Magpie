// Production blocks are populated by prepare_reflex_capture_cycle_test.py.
#include "ReflexController.h"
#include "FramePacingOptions.h"
#include "FrameTrace.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
using namespace Magpie;

static void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
namespace wil {
template<class F> struct Exit { F f; ~Exit() { f(); } };
template<class F> Exit<F> scope_exit(F f) { return {f}; }
}
struct Logger {
    unsigned scopeMessages = 0;
    static Logger& Get() { static Logger logger; return logger; }
    void Info(std::string_view value) {
        if (value.find("Render covers accepted-content effects") != value.npos) ++scopeMessages;
    }
};
enum class CaptureMethod { GraphicsCapture, DesktopDuplication, GDI, DwmSharedSurface };
struct Options { CaptureMethod captureMethod = CaptureMethod::GraphicsCapture; };
struct ScalingWindow {
    static ScalingWindow& Get() { static ScalingWindow window; return window; }
    struct Options& Options() { return options; }
    struct Options options;
};
struct Event { std::string name; uint64_t frame = 0; };
struct Driver final : ReflexDriver {
    std::vector<Event> events;
    uint32_t interval = 0;
    int failure = 0;
    bool actualOn = true;
    ReflexConfigurationResult Configure(ReflexSettings settings) noexcept override {
        interval = settings.minimumIntervalUs;
        return {0, 0, true, settings.lowLatency && actualOn};
    }
    ReflexSleepStatus QueryStatus() noexcept override { return {0, actualOn}; }
    int Sleep() noexcept override { events.push_back({"sleep"}); return failure; }
    int Marker(ReflexMarker marker, uint64_t frame) noexcept override {
        constexpr const char* names[]{"sim-start", "sim-end", "render-start", "render-end"};
        events.push_back({names[static_cast<unsigned>(marker)], frame}); return 0;
    }
    int RegisterGenerationQueue(ID3D12CommandQueue*) noexcept override { return 0; }
    int Generation(ID3D12CommandQueue*, uint64_t, uint64_t, bool) noexcept override { return 0; }
    int FrontendRender(uint64_t, uint64_t, bool) noexcept override { return 0; }
    int Present(uint64_t, uint64_t, bool, bool) noexcept override { return 0; }
    void ReportFailure(const char*, int) noexcept override { events.push_back({"failure"}); }
    unsigned Count(std::string_view name) const {
        return static_cast<unsigned>(std::count_if(events.begin(), events.end(),
            [&](const auto& event) { return event.name == name; }));
    }
};
struct FrameSourceBase {
    ReflexController* _reflex = nullptr;
    uint64_t sequence = 7, generation = 9;
    int64_t timestamp = 100000;
    uint64_t CaptureSequence() const { return sequence; }
    uint64_t ResourceGeneration() const { return generation; }
    int64_t CaptureTimestamp100ns() const { return timestamp; }
    /* CAPTURE_SETTER */
    void _BeginCaptureRender() noexcept;
};
/* CAPTURE_HOOK */
struct Effect { std::string name; };
using FrameGuidanceFrameId = uint64_t;
struct GuidanceMetadata {
    uint64_t captureSequence = 0, resourceGeneration = 0;
    int64_t timestamp100ns = 0;
};
struct Guidance {
    struct { GuidanceMetadata metadata; } motion;
};
struct FrameGuidanceConsumerViews { Guidance produced, zero; };
struct OutputTarget {
    uint64_t frameId = 0, previousCaptureFrameId = 0, captureFrameIndex = 0, captureReflexFrameId = 0;
    int64_t captureAdmissionTime100ns = 0;
    Guidance frameGuidance, zeroFrameGuidance;
    bool isNewCaptureFrame = false;
};
struct Renderer {
    ReflexController _reflex;
    Driver* driver = nullptr;
    std::unique_ptr<FrameSourceBase> _frameSource = std::make_unique<FrameSourceBase>();
    FrameSyncBackend _frameSyncBackend = FrameSyncBackend::Reflex;
    bool _hasFrameGeneration = false;
    bool _pendingFrameGenerationInput = false, _pendingFrameSyncOutput = false;
    /* CAPTURE_FIELDS */
    int64_t _vfxPreviousTimestamp100ns = 0;
    uint32_t _captureEffectFrameCount = 0;
    /* PROCESSED_DEFINITION */
    ProcessedFrame _processedFrame;
    std::vector<Effect> _runtimeEffectOptions{{"DLSSNR\\DLSSNR_AI_Filter"}};
    explicit Renderer(uint32_t interval = 8334, bool actualOn = true) {
        auto fake = std::make_unique<Driver>(); driver = fake.get();
        fake->actualOn = actualOn;
        _reflex.Initialize(std::move(fake));
        _reflex.SetFrameRateLimit(interval);
    }
    void Wire() {
        /* WIRING */
    }
    void Draw(bool isNewCaptureFrame) {
        /* DRAW_START */
        /* PROCESSED_FRAME */
        driver->events.push_back({"effects", _processedFrame.reflexFrameId});
    }
    void Finish(bool isNewCaptureFrame) {
        /* FINISH */
    }
    void ExitBackend() {
        /* BACKEND_EXIT */
        _reflex.Stop();
    }
    uint64_t AcceptInput(bool hdr = false) {
        if (hdr) driver->events.push_back({"canonical", _reflex.CaptureFrameId()});
        ++_captureEffectFrameCount;
        _lastCapturedFrameTime += std::chrono::milliseconds(11);
        /* ACCEPT_IDENTITY */
        Draw(true); Finish(true);
        return _capturedFrameId;
    }
    void Redirect(const OutputTarget& target) {
        const auto& source = target.zeroFrameGuidance.motion.metadata;
        /* REDIRECT_OUTPUT */
    }
    uint64_t Poll(bool accepted, bool inputGpu = true, bool hdr = false, bool sameCaptureUpdate = false) {
        const auto candidate = _reflex.BeginCapture(_capturedFrameId + 1);
        if (!candidate) return 0;
        if (inputGpu) {
            _frameSource->_BeginCaptureRender();
            driver->events.push_back({"input-copy-compare", candidate});
        }
        /* DISCARD */
        if (accepted) {
            return AcceptInput(hdr);
        }
        if (sameCaptureUpdate) { Draw(false); Finish(false); }
        return 0;
    }
};

static void TestScopedDuplicateCycle() {
    for (const unsigned rate : {60u, 90u, 120u}) for (const unsigned mode : {0u, 1u, 2u, 3u})
    for (const bool hdr : {false, true}) {
        (void)mode; // All four NR modes share this production capture wiring.
        ScalingWindow::Get().Options().captureMethod = CaptureMethod::GraphicsCapture;
        const auto messages = Logger::Get().scopeMessages;
        Renderer renderer(FrameSyncIntervalUs(rate)); renderer.Wire();
        Require(Logger::Get().scopeMessages == messages + 1,
            "ordinary NR/WGC/Reflex must describe accepted-content scope once");
        for (unsigned i = 0; i < 100; ++i) {
            Require(renderer.Poll(false) == 0, "pixel duplicates cannot become new content");
            Require(renderer._reflex.CaptureFrameId() == 1, "duplicate input must retain the same capture ID");
        }
        Require(renderer.driver->Count("sleep") == 1 && renderer.driver->Count("sim-start") == 1 &&
            renderer.driver->Count("render-start") == 0 && renderer.driver->Count("sim-end") == 0,
            "duplicate retries must not pay another driver period or leave Render open");
        for (unsigned i = 0; i < 20; ++i) renderer.Poll(false, false, hdr, true);
        Require(renderer._reflex.CaptureFrameId() == 1 && renderer.driver->Count("sleep") == 1 &&
            renderer.driver->Count("render-start") == 0,
            "same-capture parameter/quality output must not consume the next slept candidate");
        Require(renderer.Poll(true, true, hdr) == 1 && renderer._reflex.CaptureFrameId() == 0,
            "accepted content must use and complete the original slept ID");
        Require(renderer.driver->Count("sleep") == 1 && renderer.driver->Count("sim-start") == 1 &&
            renderer.driver->Count("sim-end") == 1 && renderer.driver->Count("render-start") == 1 &&
            renderer.driver->Count("render-end") == 1,
            "one accepted content frame must have one Sleep and one complete Simulation/Render pair");
        const auto start = std::find_if(renderer.driver->events.begin(), renderer.driver->events.end(),
            [](const auto& event) { return event.name == "render-start"; });
        Require(start != renderer.driver->events.end() && std::next(start)->name == "effects",
            "accepted-content Render must open immediately before effects");
        if (hdr) Require(std::prev(start)->name == "sim-end" && std::prev(start, 2)->name == "canonical",
            "canonical HDR input preparation must remain inside Simulation");
        Require(renderer.Poll(true, true, hdr) == 2 && renderer.driver->Count("sleep") == 2,
            "the next real content frame must obtain its own Sleep and monotonic ID");
        Require(renderer.driver->interval == FrameSyncIntervalUs(rate) && renderer._reflex.PacingAvailable(),
            "capture wiring must preserve the single driver limiter");
    }
}

static void TestScopeExclusions() {
    for (const auto backend : {FrameSyncBackend::None, FrameSyncBackend::FrontEdge, FrameSyncBackend::Async,
        FrameSyncBackend::Reflex, FrameSyncBackend::XeLL})
    for (const auto method : {CaptureMethod::GraphicsCapture, CaptureMethod::DesktopDuplication,
        CaptureMethod::GDI, CaptureMethod::DwmSharedSurface})
    for (const bool fg : {false, true}) for (const char* effect : {
        "DLSSNR\\DLSSNR_AI_Filter", "DLSS\\DLSS_SR", "Sharpen", "DLSSNR\\DLSSNR_AI_Filter_extra"}) {
        ScalingWindow::Get().Options().captureMethod = method;
        Renderer renderer; renderer._frameSyncBackend = backend; renderer._hasFrameGeneration = fg;
        renderer._runtimeEffectOptions = {{effect}};
        const auto messages = Logger::Get().scopeMessages;
        renderer.Wire();
        const bool scoped = backend == FrameSyncBackend::Reflex && method == CaptureMethod::GraphicsCapture &&
            !fg && std::string_view(effect) == "DLSSNR\\DLSSNR_AI_Filter";
        Require((renderer._frameSource->_reflex == nullptr) == scoped,
            "the new boundary must be restricted to ordinary NR plus WGC plus requested Reflex");
        Require(Logger::Get().scopeMessages == messages + scoped, "unmodified paths must not report the new scope");
        if (!scoped && backend == FrameSyncBackend::Reflex) {
            renderer.Poll(false); renderer.Poll(true);
            Require(renderer.driver->Count("sleep") == 2 && renderer.driver->Count("render-start") == 2 &&
                renderer.driver->Count("render-end") == 2,
                "FG, non-WGC and non-NR paths must retain their old pre-capture GPU marker boundary");
        }
    }
    ScalingWindow::Get().Options().captureMethod = CaptureMethod::GraphicsCapture;
    Renderer unavailable; unavailable._reflex.Stop(); unavailable.Wire();
    Require(unavailable._frameSource->_reflex == &unavailable._reflex,
        "an unavailable Reflex driver must preserve the legacy capture hook");
}

static void TestAcceptedInputReflexIdentity() {
    ScalingWindow::Get().Options().captureMethod = CaptureMethod::GraphicsCapture;
    Renderer renderer; renderer.Wire();
    Require(renderer.Poll(true) == 1 && renderer._processedFrame.frameId == 1 &&
        renderer._processedFrame.reflexFrameId == 1 && renderer._processedFrame.newContent &&
        renderer._reflex.CaptureFrameId() == 0,
        "the real processed-frame construction must freeze the accepted input's completed Reflex ID");
    renderer.Poll(false);
    Require(renderer._reflex.CaptureFrameId() == 2, "a future Simulation candidate must be open for this test");
    renderer.Draw(false);
    Require(renderer._processedFrame.frameId == 1 && renderer._processedFrame.reflexFrameId == 1 &&
        !renderer._processedFrame.newContent && renderer._acceptedCaptureReflexFrameId == 1,
        "parameter/quality redraws must use the accepted input ID while a future Simulation candidate waits");
    renderer._frameSource->timestamp += 110000;
    Require(renderer.Poll(true) == 2 && renderer._processedFrame.previousFrameId == 1 &&
        renderer._processedFrame.reflexFrameId == 2 &&
        renderer._processedFrame.timestamp100ns == renderer._frameSource->timestamp,
        "a new accepted physical capture must advance both pixel and Reflex provenance");
    renderer.Poll(false);
    const OutputTarget older{
        .frameId = 1, .previousCaptureFrameId = 0, .captureFrameIndex = 1, .captureReflexFrameId = 1,
        .captureAdmissionTime100ns = 110000,
        .zeroFrameGuidance = {.motion = {.metadata = {7, 9, 100000}}},
        .isNewCaptureFrame = false
    };
    renderer.Redirect(older);
    Require(renderer._processedFrame.frameId == 1 && renderer._processedFrame.reflexFrameId == 1 &&
        renderer._processedFrame.timestamp100ns == 100000 && renderer._processedFrame.redirected &&
        renderer._capturedFrameId == 2 && renderer._acceptedCaptureReflexFrameId == 2,
        "Sparse output redirection must keep its own old target without changing accepted input provenance");
    renderer.Draw(false);
    Require(renderer._processedFrame.frameId == 2 && renderer._processedFrame.reflexFrameId == 2 &&
        !renderer._processedFrame.redirected && !renderer._processedFrame.newContent &&
        renderer._reflex.CaptureFrameId() == 3,
        "the next input context must restore physical capture 2, not old output 1 or future candidate 3");
    renderer._reflex.CompleteCapture();
    renderer.Poll(false); renderer.Draw(false);
    Require(renderer._reflex.CaptureFrameId() == 4 && renderer._processedFrame.reflexFrameId == 2,
        "canceling/replacing a candidate must not relabel existing captured pixels");
    ++renderer._frameSource->sequence; ++renderer._frameSource->generation;
    renderer._frameSource->timestamp += 110000;
    Require(renderer.Poll(true) == 4 && renderer._processedFrame.previousFrameId == 2 &&
        renderer._processedFrame.reflexFrameId == 4 && renderer._processedFrame.captureSequence == 8 &&
        renderer._processedFrame.resourceGeneration == 10,
        "the next accepted source generation must freeze its current monotonic ID and metadata");
    renderer._reflex.Stop();
    Require(renderer.AcceptInput() == 5 && renderer._processedFrame.reflexFrameId == 0 &&
        renderer._acceptedCaptureReflexFrameId == 0,
        "a capture accepted after Reflex fallback must clear the old input's marker identity");
}

static void TestCancelPauseAndHotTarget() {
    ScalingWindow::Get().Options().captureMethod = CaptureMethod::GraphicsCapture;
    Renderer renderer; renderer.Wire(); renderer.Poll(false);
    renderer._reflex.CompleteCapture(); // Same production interrupt/error/backend-exit cancellation.
    Require(renderer._reflex.CaptureFrameId() == 0 && renderer.driver->Count("sim-end") == 1 &&
        renderer.driver->Count("render-end") == 0, "canceling input preparation must end only Simulation");
    renderer.Poll(false);
    const auto interrupted = renderer._reflex.CaptureFrameId();
    renderer._reflex.SetPresentationAvailable(false);
    Require(renderer.Poll(true) == 0 && renderer._reflex.CaptureFrameId() == 0,
        "a paused presentation cannot accept stale prepared input");
    renderer._reflex.SetPresentationAvailable(true);
    renderer.Poll(false);
    const auto resumed = renderer._reflex.CaptureFrameId();
    Require(resumed > interrupted, "resume must obtain a new slept candidate");
    const auto sleeps = renderer.driver->Count("sleep");
    renderer._reflex.SetFrameRateLimit(11112);
    renderer.Poll(false);
    Require(renderer._reflex.CaptureFrameId() > resumed && renderer.driver->Count("sleep") == sleeps + 1,
        "a real target revision must reclaim the old candidate and Sleep once for the new configuration");
    renderer.driver->failure = -1;
    renderer._reflex.CompleteCapture();
    Require(renderer.Poll(true) == 0 && renderer._reflex.State() == ReflexState::Faulted &&
        renderer._reflex.CanUseAsync() && renderer.driver->interval == 0,
        "a genuine Sleep failure must retain the existing cap cleanup and fallback contract");
    Renderer stopped; stopped.Wire(); stopped.Poll(false);
    const auto stoppedSleeps = stopped.driver->Count("sleep");
    stopped.ExitBackend();
    Require(stopped._reflex.CaptureFrameId() == 0 && stopped._reflex.State() == ReflexState::Stopped &&
        stopped.driver->interval == 0 && stopped.Poll(true) == 0 && stopped.driver->Count("sleep") == stoppedSleeps,
        "stopping after indefinitely duplicate input must retire the candidate and cap without another Sleep");
}

static void TestColdDriverStatusAndInterval() {
    ScalingWindow::Get().Options().captureMethod = CaptureMethod::GraphicsCapture;
    Renderer renderer(0, false); renderer.Wire();
    Require(renderer._reflex.State() == ReflexState::DriverOff && !renderer._reflex.PacingAvailable() &&
        renderer._frameSource->_reflex == nullptr,
        "cold Off/zero-interval state must not hide the requested Reflex scope during initialization");
    renderer._reflex.SetFrameRateLimit(8334);
    renderer.Poll(false); renderer.Poll(false);
    Require(renderer.Poll(true) == 1 && renderer.driver->Count("sleep") == 1 &&
        renderer._reflex.PacingAvailable() && !renderer._reflex.Available(),
        "independent accepted driver cap must retain one slept candidate while low latency still reports Off");
}

#ifdef MP_ENABLE_FRAME_TRACE
namespace Magpie::FrameTrace {
struct TraceEntry { Event event; int64_t start, end; uint64_t frame; int64_t status, result; };
std::vector<TraceEntry> recorded;
int64_t now = 100;
unsigned tickCalls = 0, nativeStageEnvironmentReads = 0;
bool traceEnabled = true;
std::wstring nativeStageEnvironment;
struct StageSession { bool nativeNrStages = false; } stageSession;
StageSession* session = &stageSession;
bool Enabled() noexcept { return traceEnabled; }
unsigned GetEnvironmentVariableW(const wchar_t* name, wchar_t* option, unsigned capacity) {
    Require(std::wstring_view(name) == L"MAGPIE_NATIVE_NR_STAGES", "only the internal native stage option is requested here");
    ++nativeStageEnvironmentReads;
    const auto length = static_cast<unsigned>(nativeStageEnvironment.size());
    if (length >= capacity) return length + 1;
    std::copy(nativeStageEnvironment.begin(), nativeStageEnvironment.end(), option);
    option[length] = L'\0';
    return length;
}
/* NATIVE_STAGES_REQUEST */
/* NATIVE_STAGES_ENABLED */
void InitializeNativeStages() { /* NATIVE_STAGES_INIT */ }
int64_t Tick() noexcept { ++tickCalls; return now; }
uint64_t Frame() noexcept { return 17; }
void Record(Event event, int64_t start, int64_t end, uint64_t frame, int64_t a, int64_t b) noexcept {
    recorded.push_back({event, start, end, frame, a, b});
}
}
#endif
struct SleepBoundary { virtual ~SleepBoundary() = default; virtual int Sleep() noexcept = 0; };
struct NativeSleepWrapper : SleepBoundary {
    struct Device { void* get() { return this; } } _device;
    std::function<int(void*)> _sleep;
    /* SLEEP */
};
static void TestRealSleepTraceBoundary() {
    for (const int status : {0, -1}) {
        NativeSleepWrapper driver;
        unsigned calls = 0;
        driver._sleep = [&](void* device) {
            ++calls; Require(device == driver._device.get(), "native Sleep must receive the same presentation device");
#ifdef MP_ENABLE_FRAME_TRACE
            FrameTrace::now += 55;
#endif
            return status;
        };
        Require(driver.Sleep() == status && calls == 1, "Sleep trace must preserve the single raw API call and result");
#ifdef MP_ENABLE_FRAME_TRACE
        const auto& entry = FrameTrace::recorded.back();
        Require(entry.event == FrameTrace::Event::ReflexSleep && entry.end - entry.start == 55 &&
            entry.frame == 17 && entry.status == status,
            "ReflexSleep must bracket the real API and retain its raw status and candidate trace ID");
#endif
    }
}
static void TestNativeStageEntrySurvivesIncompleteCall() {
#ifdef MP_ENABLE_FRAME_TRACE
    FrameTrace::nativeStageEnvironment = L"1";
    FrameTrace::InitializeNativeStages();
    const auto before = FrameTrace::recorded.size();
    const auto stage = static_cast<int64_t>(FrameTrace::NativeNrStage::FilterRetirementSignal);
    const auto start = FrameTrace::now;
#endif
    {
        FrameTrace::NativeNrScope trace(FrameTrace::NativeNrStage::FilterRetirementSignal, 120);
#ifdef MP_ENABLE_FRAME_TRACE
        Require(FrameTrace::recorded.size() == before + 1,
            "an incomplete native API must already have an entry checkpoint before it returns");
        const auto& entry = FrameTrace::recorded.back();
        Require(entry.event == FrameTrace::Event::NativeNrStageBegin && entry.start == start &&
            entry.end == start && entry.frame == 120 && entry.status == stage,
            "stage entry must identify the draw context and API boundary, not the next capture candidate");
        FrameTrace::now += 123;
#endif
        trace.Result(-1);
    }
#ifdef MP_ENABLE_FRAME_TRACE
    Require(FrameTrace::recorded.size() == before + 2, "a returned native API must add one matching end span");
    const auto& end = FrameTrace::recorded.back();
    Require(end.event == FrameTrace::Event::NativeNrStageEnd && end.start == start &&
        end.end - start == 123 && end.frame == 120 && end.status == stage && end.result == -1,
        "returned native stage must retain the original interval, context ID and raw HRESULT");
#endif
}
static void TestNativeStageOptInGate() {
#ifdef MP_ENABLE_FRAME_TRACE
    for (const auto* option : {L"", L"0", L"01", L"10", L"true", L"1 "}) {
        FrameTrace::nativeStageEnvironment = option;
        const auto reads = FrameTrace::nativeStageEnvironmentReads;
        FrameTrace::InitializeNativeStages();
        Require(FrameTrace::nativeStageEnvironmentReads == reads + 1 && !FrameTrace::NativeNrStagesEnabled(),
            "native stages must be off by default and accept only the exact value 1 at trace initialization");
        const auto ticks = FrameTrace::tickCalls;
        const auto entries = FrameTrace::recorded.size();
        FrameTrace::nativeStageEnvironment = L"1";
        for (unsigned i = 0; i < 18; ++i) {
            FrameTrace::NativeNrScope trace(FrameTrace::NativeNrStage::FilterDraw, 120);
            trace.Result(-1);
        }
        Require(FrameTrace::tickCalls == ticks && FrameTrace::recorded.size() == entries &&
            FrameTrace::nativeStageEnvironmentReads == reads + 1,
            "disabled stages must neither read the environment per scope nor call Tick/Record");
    }
    FrameTrace::nativeStageEnvironment = L"1";
    FrameTrace::InitializeNativeStages();
    const auto reads = FrameTrace::nativeStageEnvironmentReads;
    const auto ticks = FrameTrace::tickCalls;
    const auto entries = FrameTrace::recorded.size();
    FrameTrace::nativeStageEnvironment = L"0";
    {
        FrameTrace::NativeNrScope trace(FrameTrace::NativeNrStage::FilterDraw, 120);
        FrameTrace::now += 7;
    }
    Require(FrameTrace::tickCalls == ticks + 2 && FrameTrace::recorded.size() == entries + 2 &&
        FrameTrace::nativeStageEnvironmentReads == reads,
        "enabled stage state must remain cached until the next trace initialization");
    FrameTrace::traceEnabled = false;
    FrameTrace::session = nullptr;
    {
        FrameTrace::NativeNrScope trace(FrameTrace::NativeNrStage::FilterDraw, 120);
        trace.Result(-1);
    }
    Require(FrameTrace::tickCalls == ticks + 2 && FrameTrace::recorded.size() == entries + 2 &&
        !FrameTrace::NativeNrStagesEnabled(),
        "an unstarted frame trace must disable fine stages even after a previously opted-in session");
    FrameTrace::session = &FrameTrace::stageSession;
    FrameTrace::traceEnabled = true;
#endif
}
int main() {
    try {
        TestScopedDuplicateCycle(); TestScopeExclusions(); TestAcceptedInputReflexIdentity(); TestCancelPauseAndHotTarget();
        TestColdDriverStatusAndInterval(); TestRealSleepTraceBoundary();
        TestNativeStageOptInGate(); TestNativeStageEntrySurvivesIncompleteCall();
        std::cout << "PASS production NR/WGC/Reflex capture wiring, four modes at 60/90/120 SDR/HDR, accepted-input/output identities, legacy exclusions, cancellation/pause/hot target and native trace boundaries\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n'; return 42;
    }
}
