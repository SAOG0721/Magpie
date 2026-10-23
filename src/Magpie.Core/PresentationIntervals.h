#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>

namespace Magpie {

struct PresentationIntervals {
    using Time = std::chrono::steady_clock::time_point;
    struct Interval { Time begin{}, end{}; };
    std::array<Interval, 64> samples{};
    size_t count = 0;
};

// Bridges asynchronous successful output submissions to the frontend. Each
// sample carries both actual endpoints; losing a callback never creates an
// interval across that gap. The producer never waits for the consumer.
class PresentationIntervalQueue {
public:
    using Clock = std::chrono::steady_clock;
    using Time = Clock::time_point;
    struct Submission { uint64_t epoch = 0, serial = 0; Time time{}; };

    bool Active() const noexcept { return (_epoch.load(std::memory_order_acquire) & 1) != 0; }

    void SetActive(bool active) noexcept {
        if (bool(_epoch.load(std::memory_order_acquire) & 1) != active) Reset(active);
    }
    void Reset(bool active) noexcept {
        std::lock_guard lock(_mutex);
        const uint64_t epoch = _epoch.load(std::memory_order_relaxed);
        _epoch.store(((epoch & ~uint64_t(1)) + 2) | uint64_t(active), std::memory_order_release);
        _lastTime.reset(); _lastSerial = 0; _head = 0; _pending.count = 0;
    }
    Submission Begin(Time now = Clock::now()) noexcept {
        const uint64_t epoch = _epoch.load(std::memory_order_acquire);
        if (!(epoch & 1)) return {};
        return {epoch, _serial.fetch_add(1, std::memory_order_relaxed) + 1, now};
    }
    void Complete(const Submission& submission, bool successful) noexcept {
        if (!submission.serial) return;
        std::unique_lock lock(_mutex, std::try_to_lock);
        if (!lock || submission.epoch != _epoch.load(std::memory_order_relaxed)) return;
        if (submission.serial <= _lastSerial) {
            _lastTime.reset(); // Overlapping submissions completed out of order.
            return;
        }
        if (successful && _lastTime && submission.serial == _lastSerial + 1 && submission.time > *_lastTime) {
            const size_t index = (_head + _pending.count) % _pending.samples.size();
            _pending.samples[index] = {*_lastTime, submission.time};
            if (_pending.count == _pending.samples.size()) _head = (_head + 1) % _pending.samples.size();
            else ++_pending.count;
        }
        _lastSerial = submission.serial;
        _lastTime = successful ? std::optional(submission.time) : std::nullopt;
    }
    PresentationIntervals Take() noexcept {
        std::lock_guard lock(_mutex);
        PresentationIntervals result;
        result.count = _pending.count;
        for (size_t i = 0; i < result.count; ++i)
            result.samples[i] = _pending.samples[(_head + i) % _pending.samples.size()];
        _head = 0; _pending.count = 0;
        return result;
    }
private:
    std::mutex _mutex;
    std::atomic<uint64_t> _epoch{0}, _serial{0};
    std::optional<Time> _lastTime;
    uint64_t _lastSerial = 0;
    PresentationIntervals _pending;
    size_t _head = 0;
};

}
