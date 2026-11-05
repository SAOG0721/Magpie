#pragma once
#include <chrono>
#include <algorithm>

// Test input pacing only. A 100us sleep_for can round to the Windows timer
// quantum and manufacture 15/30ms input intervals. This timer is private to
// the probe; it does not change the product or the system timer-resolution API.
class DLSSNRBenchmarkPacing {
public:
    DLSSNRBenchmarkPacing() {
        _timer = CreateWaitableTimerExW(nullptr, nullptr,
            CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        winrt::check_bool(_timer != nullptr);
    }
    ~DLSSNRBenchmarkPacing() { if (_timer) CloseHandle(_timer); }
    DLSSNRBenchmarkPacing(const DLSSNRBenchmarkPacing&) = delete;
    template<class Service>
    void WaitUntil(std::chrono::steady_clock::time_point deadline, Service&& service) {
        using Clock = std::chrono::steady_clock;
        while (Clock::now() < deadline) {
            service();
            const double remainingUs = std::chrono::duration<double,std::micro>(deadline-Clock::now()).count();
            if (remainingUs > 300.) {
                LARGE_INTEGER due{};
                due.QuadPart = -static_cast<LONGLONG>(std::min(remainingUs-200., 1000.)*10.);
                winrt::check_bool(SetWaitableTimerEx(_timer, &due, 0, nullptr, nullptr, nullptr, 0));
                const auto status = WaitForSingleObject(_timer, 5000);
                winrt::check_bool(status == WAIT_OBJECT_0);
            } else {
                YieldProcessor();
            }
        }
    }
private:
    HANDLE _timer = nullptr;
};
