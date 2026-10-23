#pragma once
// Adapted from Coldwood1026/OptiScaler, commit
// 70676c5f037c8c26f1ec355b250a72303cd268da, XeFGPacing.h (GPL-3.0).
// See docs/experimental/design/XESSFG-COMPATIBILITY-NOTICE.md.
#include <windows.h>
#include <intrin.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <array>
#include <algorithm>
#include "PresentationIntervals.h"
namespace Magpie::XeSSFGCompatibility::Pacing
{

constexpr uint32_t PresentThunkRva = 0x25C0;
constexpr uint32_t NativePresentRva = 0x21F730;
constexpr uint32_t PacedCallerRva = 0x2202ED;
constexpr uint32_t LastFrameCallerRva = 0x220467;
constexpr uint32_t LimiterEnabledOffset = 0x340;
constexpr uint32_t BurstLimiterField = 0x28;
constexpr uint32_t SchedThunkRva = 0x3100;
constexpr uint32_t SchedFnRva = 0x21EE30;
constexpr uint32_t BurstGateOffset = 0xC0;
constexpr uint32_t SchedLimiterOffset = 0x340;
constexpr uint32_t SchedEnableOffset = 0x341;
constexpr uint32_t RingSnapshotFnRva = 0x224CF0;
constexpr uint32_t RingOffset = 0x168;
constexpr uint32_t TimestampThunkRva = 0x3430;
constexpr uint32_t TimestampFnRva = 0x224B30;
constexpr uint32_t RingMeasuredOffset = 0x1B8;

inline const uint8_t TimestampThunkExpected[16] = {
    0xE9, 0xFB, 0x16, 0x22, 0x00, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
};

inline const uint8_t SchedThunkExpected[16] = {
    0xE9, 0x2B, 0xBD, 0x21, 0x00, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
};
constexpr uint32_t PresentThunkSize = 16;

inline const uint8_t PresentThunkExpected[PresentThunkSize] = {
    0xE9, 0x6B, 0xD1, 0x21, 0x00, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
};
using PresentFn = int64_t (*)(void*, uint32_t, uint32_t, uint64_t, void*, void*, uint64_t);
constexpr int32_t SampleCount = 15;

inline uint8_t* g_base = nullptr;
inline PresentFn g_native = nullptr;
inline bool g_enabled = false;
inline PresentationIntervalQueue outputProfiler;
inline std::atomic<uint32_t> activeCallbacks{0};
struct CallbackScope {
    CallbackScope() noexcept { activeCallbacks.fetch_add(1, std::memory_order_acq_rel); }
    ~CallbackScope() { activeCallbacks.fetch_sub(1, std::memory_order_release); }
};
inline LARGE_INTEGER g_freq {};
struct OutputRecord { int64_t before = 0, after = 0, result = 0; };
inline SRWLOCK outputLock = SRWLOCK_INIT;
inline std::array<OutputRecord, 512> outputRecords{};
inline size_t outputPosition = 0, outputSize = 0;
inline void RecordOutput(int64_t before, int64_t after, int64_t result) noexcept {
    if (!TryAcquireSRWLockExclusive(&outputLock)) return;
    outputRecords[outputPosition] = {before, after, result};
    outputPosition = (outputPosition + 1) % outputRecords.size();
    outputSize = std::min(outputSize + 1, outputRecords.size());
    ReleaseSRWLockExclusive(&outputLock);
}
struct OutputStats { size_t count = 0; double p50 = 0, p95 = 0, p99 = 0; };
inline OutputStats ReadOutputStats() noexcept {
    std::array<int64_t, 512> ticks{};
    if (!TryAcquireSRWLockShared(&outputLock)) return {};
    const auto count = outputSize;
    for (size_t i = 0; i < count; ++i) ticks[i] = outputRecords[i].before;
    ReleaseSRWLockShared(&outputLock);
    if (count < 2) return {};
    std::sort(ticks.begin(), ticks.begin() + count);
    for (size_t i = 0; i + 1 < count; ++i) ticks[i] = ticks[i + 1] - ticks[i];
    std::sort(ticks.begin(), ticks.begin() + count - 1);
    const auto ms = [&](size_t percentile) { return ticks[(count - 2) * percentile / 100] * 1000.0 / g_freq.QuadPart; };
    return {count - 1, ms(50), ms(95), ms(99)};
}
inline thread_local int64_t g_lastBurstQpc = 0;
inline thread_local int64_t g_periodNs = 0;
inline thread_local int64_t g_intervalQpc = 0;
inline thread_local int64_t g_targetQpc = 0;
inline thread_local int64_t g_samples[SampleCount] {};
inline thread_local int32_t g_sampleCount = 0;
inline thread_local int32_t g_samplePos = 0;
using SchedFn = bool (*)(void*, void*, uint8_t, void*, uint32_t);
using RingSnapshotFn = void* (*) (void*, void*);

inline SchedFn g_schedNative = nullptr;
inline RingSnapshotFn g_ringSnapshot = nullptr;

using TimestampFn = void* (*) (void*, int64_t*, void*, void*, uint32_t, uint32_t);

inline TimestampFn g_tsNative = nullptr;
inline thread_local uint8_t* g_ring = nullptr;
inline thread_local int64_t g_nextDeadlineNs = 0;
inline thread_local int64_t g_burstStepNs = 0;
inline thread_local uint32_t g_lastTsIndex = 0;
inline thread_local uint32_t g_lastTsCountPlus1 = 0;

inline bool SchedulerUsable(void* ctx)
{
    const auto* ctxBytes = static_cast<const uint8_t*>(ctx);

    return ctxBytes[SchedLimiterOffset] == 0 && ctxBytes[SchedEnableOffset] != 0;
}

inline int64_t NsFromQpc(int64_t delta) { return g_freq.QuadPart > 0 ? (delta / g_freq.QuadPart) * 1000000000LL + (delta % g_freq.QuadPart) * 1000000000LL / g_freq.QuadPart : 0; }

inline int64_t QpcFromNs(int64_t ns) { return g_freq.QuadPart > 0 ? (ns * g_freq.QuadPart) / 1000000000LL : 0; }

inline double MsFromQpc(int64_t qpc) { return g_freq.QuadPart > 0 ? (qpc * 1000.0) / g_freq.QuadPart : 0.0; }

inline std::atomic<uint64_t> resetEpoch{1};
inline std::atomic<int64_t> sourcePeriodNs{0};
inline std::atomic<uint64_t> outputCalls{0}, schedulerCalls{0}, extraWaitNs{0};
inline std::atomic<int64_t> diagnosticMedianNs{0}, diagnosticUnitNs{0}, diagnosticDeadlineShiftNs{0};
inline thread_local uint64_t workerEpoch = 0;
inline thread_local void* workerContext = nullptr;
inline thread_local int64_t workerLastQpc = 0;
inline void EnterContext(void* ctx) noexcept {
    LARGE_INTEGER now{}; QueryPerformanceCounter(&now);
    const auto epoch = resetEpoch.load(std::memory_order_acquire);
    if (workerEpoch != epoch || workerContext != ctx ||
        (workerLastQpc && NsFromQpc(now.QuadPart - workerLastQpc) >= 500000000)) {
        g_lastBurstQpc = {};
        g_periodNs = {};
        g_intervalQpc = {};
        g_targetQpc = {};
        for (auto& sample : g_samples) sample = 0;
        g_sampleCount = {};
        g_samplePos = {};
        g_ring = {};
        g_nextDeadlineNs = {};
        g_burstStepNs = {};
        g_lastTsIndex = {};
        g_lastTsCountPlus1 = {};
        workerEpoch = epoch; workerContext = ctx;
    }
    workerLastQpc = now.QuadPart;
    g_ring = static_cast<uint8_t*>(ctx) + RingOffset;
}
inline void PushPeriod(int64_t ns)
{
    if (ns <= 0)
        return;

    g_samples[g_samplePos] = ns;
    g_samplePos = (g_samplePos + 1) % SampleCount;

    if (g_sampleCount < SampleCount)
        g_sampleCount++;

    int64_t sorted[SampleCount];
    memcpy(sorted, g_samples, sizeof(int64_t) * g_sampleCount);

    for (int32_t i = 1; i < g_sampleCount; i++)
    {
        int64_t key = sorted[i];
        int32_t j = i - 1;

        while (j >= 0 && sorted[j] > key)
        {
            sorted[j + 1] = sorted[j];
            j--;
        }

        sorted[j + 1] = key;
    }

    g_periodNs = sorted[g_sampleCount / 2];
}
inline void NoteFrame(uint64_t index, uint64_t count, int64_t nowQpc)
{
    if (index > 1)
        return;

    if (g_lastBurstQpc != 0)
        PushPeriod(NsFromQpc(nowQpc - g_lastBurstQpc));
    g_lastBurstQpc = nowQpc;
    const auto source = sourcePeriodNs.load(std::memory_order_relaxed);
    if (source > 0) g_periodNs = source;

    if (g_periodNs > 0 && g_freq.QuadPart > 0)
        g_intervalQpc = QpcFromNs(g_periodNs / (static_cast<int64_t>(count) + 1));
}
inline void WaitUntil(int64_t targetQpc)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    const int64_t yieldBelow = QpcFromNs(200000); // 200 us

    while (now.QuadPart < targetQpc)
    {
        if ((targetQpc - now.QuadPart) > yieldBelow)
            Sleep(0);
        else
            YieldProcessor();

        QueryPerformanceCounter(&now);
    }
}
inline void PaceFrame(uint64_t index, uint64_t count)
{
    const int64_t multiplier = static_cast<int64_t>(count) + 1;
    if (multiplier <= 2)
        return;

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    NoteFrame(index, count, now.QuadPart);

    if (index <= 1)
    {
        if (g_periodNs <= 0 || g_intervalQpc <= 0)
            return;

        g_targetQpc = now.QuadPart + g_intervalQpc;

    }
    else
    {
        if (g_intervalQpc <= 0)
            return;

        g_targetQpc += g_intervalQpc;
    }
    const int64_t latest = now.QuadPart + QpcFromNs(g_periodNs);

    if (g_targetQpc > latest)
        g_targetQpc = latest;
    // An expired slot has already been consumed by GPU/Present work. Do not
    // add another interval, or feed that extra wait into the next input period.
    // At most three slots can catch up; index 1 reanchors the next burst.
    WaitUntil(g_targetQpc);

    LARGE_INTEGER end;
    QueryPerformanceCounter(&end);
    extraWaitNs.fetch_add(NsFromQpc(end.QuadPart - now.QuadPart), std::memory_order_relaxed);
}
inline void ScheduleFrame(void* ctx, uint8_t* burst, uint64_t index)
{
    if (g_schedNative == nullptr || g_ringSnapshot == nullptr)
        return;

    g_ring = reinterpret_cast<uint8_t*>(ctx) + RingOffset;
    alignas(16) uint8_t timing[0x20] {};

    g_ringSnapshot(reinterpret_cast<uint8_t*>(ctx) + RingOffset, timing);

    const uint8_t gate = burst[BurstGateOffset] & 1;
    const uint64_t count = *reinterpret_cast<uint64_t*>(burst + 8);

    LARGE_INTEGER before;
    QueryPerformanceCounter(&before);

    NoteFrame(index, count, before.QuadPart);

    const bool scheduled = g_schedNative(ctx, burst, gate, timing, static_cast<uint32_t>(index));

    LARGE_INTEGER after;
    QueryPerformanceCounter(&after);

    (void)scheduled;

    schedulerCalls.fetch_add(1, std::memory_order_relaxed);
    extraWaitNs.fetch_add(NsFromQpc(after.QuadPart - before.QuadPart), std::memory_order_relaxed);

}
inline bool ProviderPacesLastFrame(void* ctx, const uint8_t* burst, uint64_t count)
{
    if (count <= 2)
        return false;

    const auto* ctxBytes = reinterpret_cast<const uint8_t*>(ctx);

    return ctxBytes[LimiterEnabledOffset] != 0 && *reinterpret_cast<const uint32_t*>(burst + BurstLimiterField) == 0;
}
// Only the pinned provider burst-loop and last-frame callers enter this path.
inline void TryPace(void* ctx, void* arg5, void* arg6, uint64_t arg7, bool isLast)
{
    if (arg5 == nullptr)
        return;
    const uint8_t flag = static_cast<uint8_t>(arg7);

    if (flag != (isLast ? 0 : 1))
        return;

    auto* burst = reinterpret_cast<uint8_t*>(arg5) - 0x38;
    const uint64_t count = *reinterpret_cast<uint64_t*>(burst + 8);
    if (count < 2 || count > 3)
        return;

    uint64_t index = count;

    if (!isLast)
    {
        auto* frames = *reinterpret_cast<uint8_t**>(burst);

        if (frames == nullptr || reinterpret_cast<uint8_t*>(arg6) < frames)
            return;

        const uint64_t offset = reinterpret_cast<uint8_t*>(arg6) - frames;

        if ((offset % 8) != 0)
            return;

        index = offset / 8;
        if (index < 1 || index >= count)
            return;
    }
    if (g_schedNative != nullptr && SchedulerUsable(ctx))
    {
        if (!isLast)
            ScheduleFrame(ctx, burst, index);

        return;
    }
    if (isLast && ProviderPacesLastFrame(ctx, burst, count))
        return;

    PaceFrame(index, count);
}

inline int64_t Detour(void* ctx, uint32_t a2, uint32_t a3, uint64_t a4, void* arg5, void* arg6, uint64_t arg7) noexcept
{
    CallbackScope callback;
    if (!g_enabled && !outputProfiler.Active())
        return g_native(ctx, a2, a3, a4, arg5, arg6, arg7);
    if (g_enabled)
    {
        EnterContext(ctx);
        auto* caller = reinterpret_cast<uint8_t*>(_ReturnAddress());

        if (caller == g_base + PacedCallerRva)
            TryPace(ctx, arg5, arg6, arg7, false);
        else if (caller == g_base + LastFrameCallerRva)
            TryPace(ctx, arg5, arg6, arg7, true);
    }

    outputCalls.fetch_add(1, std::memory_order_relaxed);
    LARGE_INTEGER before{}, after{};
    QueryPerformanceCounter(&before);
    const auto submission = outputProfiler.Begin();
    const auto result = g_native(ctx, a2, a3, a4, arg5, arg6, arg7);
    outputProfiler.Complete(submission, static_cast<HRESULT>(result) == S_OK);
    QueryPerformanceCounter(&after);
    RecordOutput(before.QuadPart, after.QuadPart, result);
    return result;
}
inline bool SchedForwarder(void* ctx, void* burst, uint8_t gate, void* timing, uint32_t index) noexcept
{
    CallbackScope callback;
    EnterContext(ctx);
    return g_schedNative != nullptr && g_schedNative(ctx, burst, gate, timing, index);
}
// Restore the median step removed by the native minimum clamp, then use
// one fixed schedule per burst, entirely in the SDK timestamp domain. Expired
// slots are left to the native scheduler; do not map a future deadline to QPC
// "now" or roll each expired slot forward. Only this burst's <=3 slots can catch
// up, and the next burst takes a new native anchor (no carried timing debt).
inline void* TsDetour(void* a1, int64_t* out, void* lookup, void* timing, uint32_t index, uint32_t countPlus1) noexcept
{
    CallbackScope callback;
    void* const result = g_tsNative(a1, out, lookup, timing, index, countPlus1);
    if (!g_enabled) return result;
    if (out == nullptr || timing == nullptr || index == 0 || index > 3 || countPlus1 < 3 || countPlus1 > 4 || index >= countPlus1)
        return result;

    const int64_t median = *reinterpret_cast<const int64_t*>(reinterpret_cast<const uint8_t*>(timing) + 8);
    const int64_t inputPeriod = sourcePeriodNs.load(std::memory_order_relaxed);
    // Prefer the accepted-capture / attributed-submit estimate over the provider
    // ring. Both may include backpressure: neither proves the game's production
    // cadence, so it is essential not to extend expired slots below.
    const int64_t unit = (inputPeriod > 0 ? inputPeriod : median) / static_cast<int64_t>(countPlus1);

    if (unit <= 0)
        return result;

    const bool fresh = g_lastTsIndex == 0 || index == 1 || index <= g_lastTsIndex || countPlus1 != g_lastTsCountPlus1;
    const int64_t originalDeadline = *out;
    diagnosticMedianNs.store(median, std::memory_order_relaxed);
    diagnosticUnitNs.store(unit, std::memory_order_relaxed);

    if (fresh)
    {
        int64_t nativeUnit = median / static_cast<int64_t>(countPlus1);

        if (g_ring != nullptr)
        {
            float f = *reinterpret_cast<const float*>(g_ring + RingMeasuredOffset);

            if (!(f >= 0.125f) || f > 500.0f)
                f = 500.0f;

            const int64_t fNs = static_cast<int64_t>(f * 1000000.0f);
            const int64_t clampedUnit = fNs / static_cast<int64_t>(countPlus1);

            if (clampedUnit < nativeUnit)
            {
                nativeUnit = clampedUnit;

            }
        }

        g_nextDeadlineNs = *out + static_cast<int64_t>(index) * (unit - nativeUnit);
        g_burstStepNs = unit;
    }
    else
    {
        g_nextDeadlineNs += static_cast<int64_t>(index - g_lastTsIndex) * g_burstStepNs;
    }

    g_lastTsIndex = index;
    g_lastTsCountPlus1 = countPlus1;

    *out = g_nextDeadlineNs;
    diagnosticDeadlineShiftNs.store(*out - originalDeadline, std::memory_order_relaxed);

    return result;
}

}
