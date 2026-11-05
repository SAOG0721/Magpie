#include "pch.h"
#include "FrameTrace.h"
#ifdef MP_ENABLE_FRAME_TRACE
#include "Logger.h"
#include <array>
#include <cstdio>
#include <memory>
#include <stdexcept>

namespace Magpie::FrameTrace {
namespace {
constexpr size_t CAPACITY = 262144;
constexpr size_t SLOW_CAPACITY = 2048;
constexpr size_t EVENT_COUNT = static_cast<size_t>(Event::Count);
constexpr std::array<const char*, EVENT_COUNT> NAMES{
    "BackendWait", "BackendMessages", "CaptureUpdate", "CaptureResult", "WgcAcquire",
    "WgcFrame", "WgcRejected", "WgcClose", "WgcStart", "DuplicateCheck", "DuplicateReadback",
    "BackendRender", "Guidance", "NvofSubmit", "NativeEffect", "ReferencePrepare",
    "ReferencePublish", "ReferenceConsume", "ReferencePresented", "Publication",
    "PublicationTransaction", "PublicationFence", "FrontendTick", "FrontendPrepare",
    "CursorUpdate", "FocusProbe", "HitTestRequest", "HitTestComplete", "FrontendAcquire",
    "FrontendAcquireBusy", "FrontendBase", "FrontendDraw", "BeginFrame", "CapacityBusy",
    "Present", "PresentGap", "ContentSubmit", "OverlaySubmit", "DcompCommit", "PresentGpuWait",
    "RenderDecision", "CaptureAccepted", "FrontendMessage", "WgcNotificationWait",
    "WgcDequeue", "CaptureWake", "OverlayDeferred", "InputBackpressure", "GenerationFence",
    "FgQueued", "FgDequeued", "CaptureClassification", "RenderReason", "EffectReuse", "EffectExecute",
    "HdrCapture", "CursorPublish", "OutputUpdate", "EnhancementReady",
    "WgcNotification", "WgcNotificationOverflow", "CaptureDeadline", "CaptureTimerWait", "CaptureSourceWait",
    "FrameSyncAck", "NativeAsyncService", "OverlayContentionWait", "FrontendWait",
    "FrontendSceneCopy", "FrontendOverlaySnapshot", "FrontendOverlayDraw", "FrontendCursorDraw", "CaptureStart",
    "CursorRepositionRequest", "CursorRepositionWait", "CursorRepositionComplete", "CursorRepositionCancel",
    "CursorShapeRequest", "CursorShapeComplete", "CursorShapeCancel", "ReflexSleep",
    "NativeNrStageBegin", "NativeNrStageEnd"
};
struct Entry {
    int64_t start, duration;
    uint64_t frame;
    int64_t a, b;
    Event event;
};
struct Statistics {
    uint64_t count = 0, over50 = 0;
    int64_t total = 0, maximum = 0, maximumStart = 0;
};
struct Lane {
    std::array<Entry, CAPACITY> recent{};
    std::array<Entry, SLOW_CAPACITY> slow{};
    std::array<Statistics, EVENT_COUNT> statistics{};
    uint64_t total = 0, slowTotal = 0, frame = 0;
    DWORD threadId = 0;
    int64_t lastPresent = 0;
    uintptr_t lastSwapchain = 0;
    int64_t origin = 0;
};
struct Session {
    Lane frontend, backend;
    bool nativeNrStages = false;
    int64_t origin = 0, frequency = 0;
    uint64_t utcFileTime = 0;
    SYSTEMTIME localTime{};
};
// Each lane has exactly one writer. Stop reads only after backend.join(), on
// the frontend thread. No lock, allocation, formatting, I/O or GPU query in Record.
std::unique_ptr<Session> session;
thread_local Lane* lane = nullptr;
int64_t slowTicks = 0;
uint32_t serial = 0;

bool NativeNrStagesRequested() noexcept {
    wchar_t option[2]{};
    return GetEnvironmentVariableW(L"MAGPIE_NATIVE_NR_STAGES", option, 2) == 1 && option[0] == L'1';
}

double Microseconds(int64_t ticks) noexcept {
    return double(ticks) * 1'000'000.0 / double(session->frequency);
}

void WriteLane(FILE* file, const char* name, const Lane& source) noexcept {
    std::fprintf(file, "#lane,%s,%lu,%llu,%llu,%zu,%zu\n", name, source.threadId,
        source.total, source.slowTotal, CAPACITY, SLOW_CAPACITY);
    for (size_t i = 0; i < EVENT_COUNT; ++i) {
        const auto& s = source.statistics[i];
        if (!s.count) continue;
        std::fprintf(file, "#stat,%s,%s,%llu,%.3f,%.3f,%llu,%.3f\n", name, NAMES[i],
            s.count, Microseconds(s.total), Microseconds(s.maximum), s.over50,
            s.maximum ? Microseconds(s.maximumStart - session->origin) : 0.0);
    }
    const auto write = [&](const auto& ring, uint64_t total, const char* stream) {
        const uint64_t first = total > ring.size() ? total - ring.size() : 0;
        for (uint64_t n = first; n < total; ++n) {
            const auto& e = ring[n % ring.size()];
            std::fprintf(file, "%s,%s,%s,%.3f,%.3f,%llu,%lld,%lld\n", stream, name,
                NAMES[static_cast<size_t>(e.event)], Microseconds(e.start - session->origin),
                Microseconds(e.duration), e.frame, e.a, e.b);
        }
    };
    write(source.recent, source.total, "recent");
    write(source.slow, source.slowTotal, "slow");
}
}

bool Start() noexcept {
    if (session) return false;
    wchar_t option[8]{};
    if (GetEnvironmentVariableW(L"MAGPIE_FRAME_TRACE", option, ARRAYSIZE(option)) == 1 &&
        option[0] == L'0') return false;
    try {
        session = std::make_unique<Session>();
        session->nativeNrStages = NativeNrStagesRequested();
        LARGE_INTEGER frequency{};
        if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) {
            session.reset();
            return false;
        }
        session->frequency = frequency.QuadPart;
        LARGE_INTEGER origin{};
        QueryPerformanceCounter(&origin);
        session->origin = origin.QuadPart;
        session->frontend.origin = session->backend.origin = origin.QuadPart;
        FILETIME utc{};
        GetSystemTimePreciseAsFileTime(&utc);
        session->utcFileTime = (uint64_t(utc.dwHighDateTime) << 32) | utc.dwLowDateTime;
        GetLocalTime(&session->localTime);
        slowTicks = frequency.QuadPart / 20; // 50 ms, independent of source frame rate.
        lane = &session->frontend;
        lane->threadId = GetCurrentThreadId();
        Logger::Get().Info("r9 frame trace enabled: bounded memory, export to logs/frame-traces on scaling stop");
        return true;
    } catch (...) {
        lane = nullptr;
        session.reset();
        Logger::Get().Warn("Frame trace allocation failed; continuing without tracing");
        return false;
    }
}

void BindBackend() noexcept {
    if (!session) return;
    lane = &session->backend;
    lane->threadId = GetCurrentThreadId();
}

bool Enabled() noexcept { return lane != nullptr; }
bool NativeNrStagesEnabled() noexcept { return Enabled() && session->nativeNrStages; }
int64_t Tick() noexcept {
    if (!lane) return 0;
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}
void SetFrame(uint64_t frameId) noexcept { if (lane) lane->frame = frameId; }
uint64_t Frame() noexcept { return lane ? lane->frame : 0; }

void Record(Event event, int64_t start, int64_t end, uint64_t frameId, int64_t a, int64_t b) noexcept {
    // Ignore async completions originating in a previous scaling session.
    if (!lane || !start || start < lane->origin || end < start) return;
    const Entry entry{ start, end - start, frameId, a, b, event };
    lane->recent[lane->total++ % CAPACITY] = entry;
    auto& stats = lane->statistics[static_cast<size_t>(event)];
    ++stats.count;
    stats.total += entry.duration;
    if (entry.duration > stats.maximum) {
        stats.maximum = entry.duration;
        stats.maximumStart = start;
    }
    if (entry.duration >= slowTicks) {
        ++stats.over50;
        lane->slow[lane->slowTotal++ % SLOW_CAPACITY] = entry;
    }
}

void Mark(Event event, int64_t a, int64_t b) noexcept {
    if (!lane) return;
    const auto now = Tick();
    Record(event, now, now, lane->frame, a, b);
}

void Presentation(int64_t start, int64_t end, int64_t result, uintptr_t swapchain) noexcept {
    if (!lane) return;
    Record(Event::Present, start, end, lane->frame, result, static_cast<int64_t>(swapchain));
    // S_OK only; occlusion and failures break the interval sequence.
    if (result != S_OK) {
        lane->lastPresent = 0;
        return;
    }
    if (lane->lastPresent && lane->lastSwapchain == swapchain) {
        Record(Event::PresentGap, lane->lastPresent, start, lane->frame, 0,
            static_cast<int64_t>(swapchain));
    }
    lane->lastPresent = start;
    lane->lastSwapchain = swapchain;
}

void Stop() noexcept {
    const int64_t stopped = Tick();
    lane = nullptr;
    if (!session) return;
    try {
        std::wstring module(32768, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, module.data(), DWORD(module.size()));
        if (!length || length >= module.size()) throw std::runtime_error("module path");
        module.resize(length);
        const auto directory = std::filesystem::path(module).parent_path() / L"logs" / L"frame-traces";
        std::filesystem::create_directories(directory);
        const auto& t = session->localTime;
        const auto path = directory / fmt::format(
            L"trace-{:04}{:02}{:02}-{:02}{:02}{:02}-{:03}-{}-{}.csv",
            t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
            GetCurrentProcessId(), ++serial);
        FILE* raw = nullptr;
        if (_wfopen_s(&raw, path.c_str(), L"wbx") || !raw) throw std::runtime_error("open trace");
        std::unique_ptr<FILE, decltype(&std::fclose)> file(raw, &std::fclose);
        std::fprintf(raw, "#magpie_frame_trace,1\n#process,%lu\n#clock,%lld,%lld,%llu\n#duration_us,%.3f\n",
            GetCurrentProcessId(), session->frequency, session->origin, session->utcFileTime,
            Microseconds(stopped - session->origin));
        std::fprintf(raw, "stream,thread,event,start_us,duration_us,frame_id,a,b\n");
        WriteLane(raw, "frontend", session->frontend);
        WriteLane(raw, "backend", session->backend);
        std::fprintf(raw, "#complete,1\n");
        bool failed = std::ferror(raw) != 0;
        if (std::fclose(file.release()) != 0) failed = true;
        if (failed) throw std::runtime_error("write trace");
        Logger::Get().Info(fmt::format("Frame trace exported to logs/frame-traces: {}", path.filename().string()));
    } catch (...) {
        Logger::Get().Warn("Frame trace export failed; rendering session has already stopped");
    }
    session.reset();
}
}
#endif
