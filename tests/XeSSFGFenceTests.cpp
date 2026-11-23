#include <windows.h>
#include "GpuFenceWait.h"
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <stdexcept>
#include <thread>

namespace Magpie {
struct TestEvent {
    HANDLE value = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    TestEvent() { if (!value) throw std::runtime_error("CreateEvent failed"); }
    ~TestEvent() { if (value) CloseHandle(value); }
    HANDLE get() const noexcept { return value; }
    explicit operator bool() const noexcept { return value != nullptr; }
};

// Only GPU completion is faked. Registration failure, stale wakeups and the
// eventual completion use the production function and actual Win32 events.
struct TestFence {
    enum class Action { Complete, DelayedComplete, EarlyWake, RemovedOnWake, RegistrationFailure, WaitFailure };
    std::atomic<uint64_t> completed{0};
    Action action = Action::Complete;
    TestEvent* owner = nullptr;
    unsigned registrations = 0;
    std::thread worker;
    ~TestFence() { if (worker.joinable()) worker.join(); }
    uint64_t GetCompletedValue() const noexcept { return completed.load(); }
    HRESULT SetEventOnCompletion(uint64_t target, HANDLE event) noexcept {
        ++registrations;
        switch (action) {
        case Action::RegistrationFailure: return E_FAIL;
        case Action::WaitFailure:
            CloseHandle(event);
            owner->value = nullptr;
            return S_OK; // Waiting on the invalid handle must return WAIT_FAILED.
        case Action::DelayedComplete:
            worker = std::thread([this, target, event] {
                Sleep(2);
                completed.store(target);
                SetEvent(event);
            });
            return S_OK;
        case Action::Complete: completed.store(target); break;
        case Action::RemovedOnWake: completed.store(UINT64_MAX); break;
        case Action::EarlyWake: break;
        }
        return SetEvent(event) ? S_OK : E_FAIL;
    }
};
struct XeSSFGPresenter {
    struct Impl {
        TestFence* fence12 = nullptr;
        TestEvent fenceEvent;
    };
};

// The runner extracts this exact function from XeSSFGPresenter.cpp. No copy
// of its decision logic is maintained by the fixture.
#include "XeSSFGFence.production.inl"
}

static void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

int main() try {
    using namespace Magpie;
    using Action = TestFence::Action;
    {
        TestEvent ready;
        TestFence fence;
        fence.action = Action::DelayedComplete;
        SetEvent(ready.get());
        Require(SUCCEEDED(fence.SetEventOnCompletion(10, ready.get())), "Failed to register fixture completion");
        Require(WaitForGpuFence(&fence, 10, ready.get(), 1000) == S_OK && fence.completed >= 10,
            "Shared GPU wait must ignore a stale notification and observe this target");
        Require(WaitForGpuFence(&fence, 11, ready.get(), 0) == HRESULT_FROM_WIN32(ERROR_TIMEOUT),
            "A timeout must not be reported as target completion");
        fence.completed = UINT64_MAX;
        Require(WaitForGpuFence(&fence, 11, ready.get(), 0) == DXGI_ERROR_DEVICE_REMOVED,
            "The device-removed sentinel must not be reported as target completion");
    }
    {
        TestFence fence;
        XeSSFGPresenter::Impl impl{ &fence };
        fence.owner = &impl.fenceEvent;
        fence.completed = UINT64_MAX;
        Require(!WaitForFence(impl, 10), "Device removed was accepted as completed");
        Require(fence.registrations == 0, "Removed device must fail before registration");
    }
    {
        TestFence fence;
        XeSSFGPresenter::Impl impl{ &fence };
        fence.owner = &impl.fenceEvent;
        Require(WaitForFence(impl, 0), "Zero target must not wait on a healthy device");
        fence.completed = 11;
        Require(WaitForFence(impl, 10), "Already-completed target was rejected");
        Require(fence.registrations == 0, "Already-completed target registered a wait");
    }
    for (const auto action : {Action::Complete, Action::DelayedComplete, Action::EarlyWake,
                             Action::RemovedOnWake, Action::RegistrationFailure, Action::WaitFailure}) {
        TestFence fence;
        XeSSFGPresenter::Impl impl{ &fence };
        fence.owner = &impl.fenceEvent;
        fence.action = action;
        // Emulate a stale auto-reset signal from a previous registration. A
        // normal delayed completion must clear it and wait for its own target.
        if (action == Action::DelayedComplete) SetEvent(impl.fenceEvent.get());
        const bool result = WaitForFence(impl, 10);
        if (fence.worker.joinable()) fence.worker.join();
        const bool expected = action == Action::Complete || action == Action::DelayedComplete;
        Require(result == expected, "Fence wakeup result did not match completion state");
        if (expected) Require(fence.completed.load() >= 10, "Wait returned before the target completed");
        Require(fence.registrations == 1, "Missing completion registration");
    }
    std::puts("PASS XeSSFG production fence: device removal, zero/already complete, normal/stale events, early wake, registration and wait failures.");
    return 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "XeSSFG fence test failed: %s\n", error.what());
    return 1;
}
