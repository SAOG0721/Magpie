// CPU-only behavior checks of the extracted production wrappers and real guard.
// No NGX library, D3D device or GPU work is loaded by this fixture.
#include <windows.h>
#include "FrameTrace.h"
#include "include/NgxRuntimeGuard.h"
#include <atomic>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace FT = Magpie::FrameTrace;

static void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

namespace Observed {
struct Row { FT::Event event; int64_t start, end; uint64_t frame; int64_t stage, pass; };
std::mutex mutex;
std::vector<Row> rows;
bool enabled = false;
std::atomic_uint ticks = 0;
HANDLE outerSeen = nullptr;
FT::NativeNrStage awaitedStage = FT::NativeNrStage::InferenceParameters;
std::atomic_bool guardHeld = false, invokedWhileHeld = false;

void Reset(bool option) {
    std::lock_guard lock(mutex);
    rows.clear(); ticks = 0; enabled = option;
    outerSeen = nullptr; invokedWhileHeld = false;
}
std::vector<Row> Copy() { std::lock_guard lock(mutex); return rows; }
}

#ifdef MP_ENABLE_FRAME_TRACE
namespace Magpie::FrameTrace {
bool NativeNrStagesEnabled() noexcept { return Observed::enabled; }
int64_t Tick() noexcept {
    ++Observed::ticks;
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}
void Record(Event event, int64_t start, int64_t end, uint64_t frame, int64_t stage, int64_t pass) noexcept {
    std::lock_guard lock(Observed::mutex);
    Observed::rows.push_back({event, start, end, frame, stage, pass});
    if (Observed::outerSeen && event == Event::NativeNrStageBegin &&
        stage == static_cast<int64_t>(Observed::awaitedStage)) SetEvent(Observed::outerSeen);
}
}
#endif

// The ABI/data shells below only replace the SDK dependencies of the wrappers.
struct ID3D12GraphicsCommandList {};
struct NVSDK_NGX_Handle {};
struct NVSDK_NGX_Parameter {};
using NVSDK_NGX_Result = uint32_t;
constexpr NVSDK_NGX_Result NVSDK_NGX_Result_FAIL_PlatformError = 0xbad00000u;
namespace Magpie {
struct DLSSNRSettings {};
struct FrameGuidanceView {};
struct DLSSNRFilter {
    struct Impl {
        using EvaluateFeatureFn = NVSDK_NGX_Result(*)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*,
            const NVSDK_NGX_Parameter*, void*);
        unsigned parameterCalls = 0;
        bool lastReset = false;
    };
};
static void SetEvaluateParametersUnsafe(DLSSNRFilter::Impl& impl, const DLSSNRSettings&,
    const FrameGuidanceView&, bool reset) {
    Observed::invokedWhileHeld = Observed::guardHeld.load();
    ++impl.parameterCalls; impl.lastReset = reset;
}
#include "NativeNrInferenceWrappers.inl"
}

static unsigned evaluateCalls = 0;
static NVSDK_NGX_Result evaluateResult = 0;
static ID3D12GraphicsCommandList list;
static NVSDK_NGX_Handle feature;
static NVSDK_NGX_Parameter parameters;
static NVSDK_NGX_Result Evaluate(ID3D12GraphicsCommandList* actualList, const NVSDK_NGX_Handle* actualFeature,
    const NVSDK_NGX_Parameter* actualParameters, void* progress) {
    Require(actualList == &list && actualFeature == &feature && actualParameters == &parameters && !progress,
        "The actual wrapper must preserve the SDK arguments");
    Observed::invokedWhileHeld = Observed::guardHeld.load();
    ++evaluateCalls;
    return evaluateResult;
}

static void CheckPair(bool parametersCall, uint64_t frame, uint32_t pass, bool enabled) {
    const auto rows = Observed::Copy();
    if (!enabled) {
        Require(rows.empty() && Observed::ticks == 0, "Disabled stages must not sample the clock or emit events");
        return;
    }
    const auto outer = parametersCall ? FT::NativeNrStage::InferenceParameters : FT::NativeNrStage::InferenceEvaluate;
    const auto inner = parametersCall ? FT::NativeNrStage::InferenceParametersLocked : FT::NativeNrStage::InferenceEvaluateLocked;
    Require(rows.size() == 4, "One wrapper must produce an outer/locked begin and matching end");
    for (const auto& row : rows) Require(row.frame == frame && row.pass == pass,
        "Explicit frame and one-based pass must survive both begin and end");
    Require(rows[0].event == FT::Event::NativeNrStageBegin && rows[0].stage == static_cast<int64_t>(outer) &&
        rows[1].event == FT::Event::NativeNrStageBegin && rows[1].stage == static_cast<int64_t>(inner) &&
        rows[2].event == FT::Event::NativeNrStageEnd && rows[2].stage == static_cast<int64_t>(inner) &&
        rows[3].event == FT::Event::NativeNrStageEnd && rows[3].stage == static_cast<int64_t>(outer),
        "Locked work must nest inside its original wrapper scope");
    Require(rows[0].start <= rows[1].start && rows[1].start == rows[2].start &&
        rows[2].end <= rows[3].end && rows[3].start == rows[0].start,
        "Nested intervals must preserve real entry/return order");
}

static void Invoke(bool parametersCall, uint64_t frame, uint32_t pass) {
    DWORD seh = 123;
    if (parametersCall) {
        Magpie::DLSSNRFilter::Impl impl;
        Require(Magpie::SetEvaluateParametersSafely(impl, {}, {}, true, &seh, frame, pass) &&
            impl.parameterCalls == 1 && impl.lastReset, "Parameter wrapper must call the original body once");
    } else {
        const auto previous = evaluateCalls;
        Require(Magpie::CallEvaluateFeatureSafely(Evaluate, &list, &feature, &parameters, &seh, frame, pass) == evaluateResult &&
            evaluateCalls == previous + 1, "Evaluate wrapper must preserve the raw result and call count");
    }
    Require(seh == 0 && !Magpie::NgxRuntimeGuard::IsFaulted(), "Tracing must preserve normal guard/SEH state");
}

#ifdef MP_ENABLE_FRAME_TRACE
static void Contention(bool parametersCall) {
    HANDLE held = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE outer = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    Require(held && release && outer, "Cannot create contention fixture events");
    Observed::Reset(true);
    Observed::outerSeen = outer;
    Observed::awaitedStage = parametersCall ? FT::NativeNrStage::InferenceParameters : FT::NativeNrStage::InferenceEvaluate;
    std::atomic_bool heldOk = false, observedOuterOnly = false;
    std::thread holder([&] {
        DWORD seh = 0;
        Magpie::NgxRuntimeGuard::Invoke([&] {
            Observed::guardHeld = true;
            SetEvent(held);
            heldOk = WaitForSingleObject(release, 5000) == WAIT_OBJECT_0;
            Observed::guardHeld = false;
            return 0;
        }, -1, &seh);
    });
    const bool holderReady = WaitForSingleObject(held, 5000) == WAIT_OBJECT_0;
    std::thread observer([&] {
        if (WaitForSingleObject(outer, 5000) == WAIT_OBJECT_0) {
            const auto rows = Observed::Copy();
            observedOuterOnly = rows.size() == 1 && Observed::guardHeld.load();
        }
        SetEvent(release); // Release even on failure; never leave the host stuck.
    });
    Invoke(parametersCall, 1189, 3);
    holder.join(); observer.join();
    Observed::outerSeen = nullptr;
    CloseHandle(held); CloseHandle(release); CloseHandle(outer);
    Require(holderReady && heldOk && observedOuterOnly && !Observed::invokedWhileHeld,
        "Only wrapper entry may appear while the real NGX guard is held by another caller");
    CheckPair(parametersCall, 1189, 3, true);
}
#endif

int main() try {
    for (bool parametersCall : {true, false}) {
        for (const auto result : {0u, 0xbad00020u}) {
            evaluateResult = result;
            for (bool enabled : {false, true}) {
                Observed::Reset(enabled);
                Invoke(parametersCall, 4001, 2);
#ifdef MP_ENABLE_FRAME_TRACE
                CheckPair(parametersCall, 4001, 2, enabled);
#else
                CheckPair(parametersCall, 4001, 2, false);
#endif
            }
        }
#ifdef MP_ENABLE_FRAME_TRACE
        Contention(parametersCall);
#endif
    }
#ifdef MP_ENABLE_FRAME_TRACE
    std::puts("Native NR stages: production wrappers preserve calls/results; frame/pass, opt-in and real guard contention passed.");
#else
    std::puts("Native NR stages compiled out: production wrapper calls/results preserved; no clock sampling or trace events.");
#endif
    return 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "Native NR stage test failed: %s\n", error.what());
    return 1;
}
