#pragma once
#include <cstdint>

namespace Magpie::FrameTrace {

// CPU wall times only. Nested events overlap; none is a GPU execution timer.
enum class Event : uint16_t {
    BackendWait, BackendMessages, CaptureUpdate, CaptureResult, WgcAcquire,
    WgcFrame, WgcRejected, WgcClose, WgcStart, DuplicateCheck, DuplicateReadback,
    BackendRender, Guidance, NvofSubmit, NativeEffect, ReferencePrepare,
    ReferencePublish, ReferenceConsume, ReferencePresented, Publication,
    PublicationTransaction, PublicationFence, FrontendTick, FrontendPrepare,
    CursorUpdate, FocusProbe, HitTestRequest, HitTestComplete, FrontendAcquire,
    FrontendAcquireBusy, FrontendBase, FrontendDraw, BeginFrame, CapacityBusy,
    Present, PresentGap, ContentSubmit, OverlaySubmit, DcompCommit, PresentGpuWait,
    RenderDecision, CaptureAccepted, FrontendMessage, WgcNotificationWait,
    WgcDequeue, CaptureWake, OverlayDeferred, InputBackpressure, GenerationFence,
    FgQueued, FgDequeued, CaptureClassification, RenderReason, EffectReuse, EffectExecute,
    HdrCapture, CursorPublish, OutputUpdate, EnhancementReady,
    WgcNotification, WgcNotificationOverflow, CaptureDeadline, CaptureTimerWait, CaptureSourceWait,
    FrameSyncAck, NativeAsyncService, OverlayContentionWait, FrontendWait,
    FrontendSceneCopy, FrontendOverlaySnapshot, FrontendOverlayDraw, FrontendCursorDraw, CaptureStart,
    CursorRepositionRequest, CursorRepositionWait, CursorRepositionComplete, CursorRepositionCancel,
    CursorShapeRequest, CursorShapeComplete, CursorShapeCancel, ReflexSleep,
    NativeNrStageBegin, NativeNrStageEnd, Count
};

// ReflexSleep is the CPU interval of the real NVAPI Sleep call; a is its raw
// result. frame_id is still a pre-acceptance capture candidate at this point.

// NativeNrStageBegin is an immediate checkpoint, so an API that has not returned
// remains visible. NativeNrStageEnd spans that stage. a is NativeNrStage, b is
// the raw HRESULT when the wrapped API has one; frame_id is the draw context ID.
// These fine stages are off unless MAGPIE_NATIVE_NR_STAGES=1 at trace Start.
enum class NativeNrStage : int64_t {
    FilterTimingBegin, FilterDraw, PerformanceLog, FilterTimingEnd, FilterRetirementSignal,
    TemporalDraw, TemporalInputView, TemporalOutputView, TemporalMotionWait, TemporalMotionView,
    TemporalConstants, TemporalConstantBind, TemporalReduceBind, TemporalReduceDispatch,
    TemporalReduceUnbind, TemporalMainBind, TemporalMainDispatch, TemporalUnbind
};

// BackendWait.a identifies the existing wait owner; nested capture waits split
// source arrival from a software limiter. No value implies GPU execution time.
enum class BackendWaitReason : int64_t {
    None, Capture, ReflexCapture, FrameGenerationCapacity, FrameGenerationDeadline, PublicationAck
};
enum class FrontendWaitReason : int64_t { None, IdleReplay, Deadline, Capacity, Message };

// CursorRepositionWait is elapsed hold/dispatcher time. Shutdown may include the
// final synchronous coordinate restoration, after active presentation has stopped.
// Request/complete/cancel.a is the request ID; .b is this reason (request.b is
// whether the source cursor is captured). Shape spans include background hit test.
enum class CursorRepositionReason : int64_t {
    Completed, Superseded, Shutdown, ExternalClip, ClipQueryFailed, ClipSetFailed,
    TimerUnavailable, CaptureEnded, CursorMoved, SourceChanged, ParameterInput
};

#ifdef MP_ENABLE_FRAME_TRACE
bool Start() noexcept; // Frontend, before starting the backend thread.
void BindBackend() noexcept;
void Stop() noexcept;  // Frontend, AFTER joining the backend thread.
bool Enabled() noexcept;
bool NativeNrStagesEnabled() noexcept;
int64_t Tick() noexcept;
void SetFrame(uint64_t frameId) noexcept;
uint64_t Frame() noexcept;
void Record(Event event, int64_t start, int64_t end, uint64_t frameId,
    int64_t a = 0, int64_t b = 0) noexcept;
void Mark(Event event, int64_t a = 0, int64_t b = 0) noexcept;
void Presentation(int64_t start, int64_t end, int64_t result, uintptr_t swapchain) noexcept;
#else
inline bool Start() noexcept { return false; }
inline void BindBackend() noexcept {}
inline void Stop() noexcept {}
inline bool Enabled() noexcept { return false; }
inline bool NativeNrStagesEnabled() noexcept { return false; }
inline int64_t Tick() noexcept { return 0; }
inline void SetFrame(uint64_t) noexcept {}
inline uint64_t Frame() noexcept { return 0; }
inline void Record(Event, int64_t, int64_t, uint64_t, int64_t = 0, int64_t = 0) noexcept {}
inline void Mark(Event, int64_t = 0, int64_t = 0) noexcept {}
inline void Presentation(int64_t, int64_t, int64_t, uintptr_t) noexcept {}
#endif

class Scope {
public:
    explicit Scope(Event event, int64_t a = 0, int64_t b = 0) noexcept
        : _event(event), _start(Enabled() ? Tick() : 0), _frame(Frame()), _a(a), _b(b) {}
    ~Scope() noexcept { End(); }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    void Data(int64_t a, int64_t b = 0) noexcept { _a = a; _b = b; }
    void FrameId(uint64_t frame) noexcept { _frame = frame; }
    void End() noexcept {
        if (!_start) return;
        Record(_event, _start, Tick(), _frame, _a, _b);
        _start = 0;
    }
private:
    Event _event;
    int64_t _start;
    uint64_t _frame;
    int64_t _a, _b;
};

class NativeNrScope {
public:
    NativeNrScope(NativeNrStage stage, uint64_t frameId) noexcept
        : _stage(static_cast<int64_t>(stage)), _frame(frameId),
          _start(NativeNrStagesEnabled() ? Tick() : 0) {
        if (_start) Record(Event::NativeNrStageBegin, _start, _start, _frame, _stage);
    }
    ~NativeNrScope() noexcept {
        if (_start) Record(Event::NativeNrStageEnd, _start, Tick(), _frame, _stage, _result);
    }
    NativeNrScope(const NativeNrScope&) = delete;
    NativeNrScope& operator=(const NativeNrScope&) = delete;
    void Result(int64_t result) noexcept { _result = result; }
private:
    int64_t _stage;
    uint64_t _frame;
    int64_t _start, _result = 0;
};

}
