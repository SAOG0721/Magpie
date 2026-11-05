// Production Start/Toggle/Restart methods and the production message-dispatch
// prefix. Dispatcher, windows and backends are doubles; no desktop/GPU work.
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <deque>
#include <functional>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
int checks = 0, dispatched = 0;
uintptr_t nextMessage = 1;
std::deque<MSG> messages;
std::map<uintptr_t, std::function<void()>> callbacks;
constexpr UINT DISPATCH_CALLBACK = WM_APP + 100;
void Check(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
void Enqueue(std::function<void()> callback) {
    const auto id = nextMessage++;
    callbacks.emplace(id, std::move(callback));
    MSG message{}; message.message = DISPATCH_CALLBACK; message.lParam = LPARAM(id);
    messages.push_back(message);
}
void EnqueueQuit() { MSG message{}; message.message = WM_QUIT; messages.push_back(message); }
BOOL FakePeekMessage(MSG* result, HWND, UINT first, UINT last, UINT remove) {
    const auto it = std::find_if(messages.begin(), messages.end(), [&](const MSG& message) {
        return message.message == WM_QUIT || (!first && !last) ||
            (message.message >= first && message.message <= last);
    });
    if (it == messages.end()) return FALSE;
    *result = *it;
    if (remove & PM_REMOVE) messages.erase(it);
    return TRUE;
}
BOOL FakeTranslateMessage(const MSG*) { return TRUE; }
LRESULT FakeDispatchMessage(const MSG* message) {
    ++dispatched;
    auto callback = callbacks.extract(uintptr_t(message->lParam));
    if (callback) callback.mapped()();
    return 0;
}
DWORD FakeWait(DWORD, const HANDLE*, DWORD, DWORD, DWORD) { return 0; }
}

namespace winrt {
struct DispatcherQueue {
    template<class Fn> bool TryEnqueue(Fn&& callback) const {
        Enqueue(std::forward<Fn>(callback));
        return true;
    }
};
struct ResourceLoader {
    static ResourceLoader GetForViewIndependentUse(int) { return {}; }
    std::wstring GetString(const wchar_t*) const { return {}; }
};
}

namespace Magpie {
#include "RuntimeQuitTypes.inc"
namespace CommonSharedConstants {
constexpr int APP_RESOURCE_MAP_ID = 0;
constexpr UINT WM_FRONTEND_RENDER_DLSSFG = WM_APP + 101;
constexpr UINT WM_FRONTEND_RENDER = WM_APP + 102;
}
namespace FrameTrace {
enum class Event { FrontendMessage };
struct Scope { Scope(Event, UINT) {} };
}
struct Logger {
    static Logger& Get() { static Logger log; return log; }
    void Warn(const char*) const {}
    void Error(const char*) const {}
};
struct EffectOption {};
struct FrameRefreshSettings {};
struct ScalingOptions {
    std::wstring screenshotsDir = L"fixture";
    std::function<void(HWND, const std::wstring&)> showToast = [](HWND, const std::wstring&) {};
    std::function<void()> showError = [] {};
    std::function<void()> save = [] {};
    bool IsWindowedMode() const { return false; }
};
struct ScalingWindow {
    struct Retirement { bool pending = false; bool Pending() const { return pending; } } _retirement;
    struct Source { HWND Handle() const { return reinterpret_cast<HWND>(uintptr_t(1)); } } source;
    struct RendererFixture {
        int suspended = 0;
        bool IsEditingParameters() const { return false; }
        void SuspendParameterInput() { ++suspended; }
    } renderer;
    bool live = false, cleanupReady = true, _isDestroying = false, waitingRestart = false;
    int backends = 0, starts = 0, restarts = 0, toggles = 0, completed = 0;
    static inline uint32_t runId = 10;
    static ScalingWindow& Get() { static ScalingWindow window; return window; }
    static uint32_t RunId() { return runId; }
    static void SessionWindowedMode(bool) {}
    explicit operator bool() const { return live; }
    HWND Handle() const { return live ? reinterpret_cast<HWND>(uintptr_t(2)) : nullptr; }
    const Source& SrcTracker() const { return source; }
    RendererFixture* TryGetRenderer() { return backends ? &renderer : nullptr; }
    RendererFixture& Renderer() { return renderer; }
    bool IsRetiring() const { return _retirement.pending; }
    bool HasUrgentOverlayInput() const { return false; }
    bool CompletePendingDestroy() {
        if (!_retirement.pending || !cleanupReady) return false;
        _retirement.pending = false; _isDestroying = false; backends = 0; ++completed;
        return true;
    }
    void Stop() { Destroy(); }
    void Destroy() {
        if (live || backends) { _retirement.pending = true; _isDestroying = true; }
        live = false;
        CompletePendingDestroy();
    }
    void Start(HWND, ScalingOptions&&) noexcept {
#include "RuntimeWindowStartAdmission.inc"
        live = true; backends = 1; ++starts; ++runId;
    }
    void ToggleScaling(bool) { ++toggles; }
    void RestartWithEffectParameters(std::vector<EffectOption>&&, FrameRefreshSettings) { ++restarts; waitingRestart = true; }
};
class ScalingRuntime {
public:
    bool Start(HWND hwndSrc, ScalingOptions&& options, bool force);
    void ToggleScaling(bool isWindowedMode);
    bool RestartWithEffectParameters(HWND hwndSource, HWND hwndScaling, uint32_t scalingRunId,
        std::vector<EffectOption>&& effects, FrameRefreshSettings frameRefresh);
    const winrt::DispatcherQueue& _Dispatcher() const { return _dispatcher; }
    ScalingState State() const { return state.load(); }
    void _State(ScalingState value) { state = value; }
    bool DispatchPass();
    winrt::DispatcherQueue _dispatcher;
    std::atomic<ScalingState> state = ScalingState::Scaling;
    std::atomic<uint64_t> _commandGeneration = 0;
    std::atomic<bool> _quitting = false;
    bool quitRequested = false;
};

#undef PeekMessage
#define PeekMessage FakePeekMessage
#define TranslateMessage FakeTranslateMessage
#undef DispatchMessage
#define DispatchMessage FakeDispatchMessage
#define MsgWaitForMultipleObjectsEx FakeWait
#include "RuntimeQuitCommandsProduction.inc"
bool ScalingRuntime::DispatchPass() {
    auto& scalingWindow = ScalingWindow::Get();
    MSG msg{};
    constexpr uint32_t MAX_MESSAGES_PER_PASS = 32;
#include "RuntimeQuitMessagePumpProduction.inc"
}
#undef PeekMessage
#undef TranslateMessage
#undef DispatchMessage
#undef MsgWaitForMultipleObjectsEx
}

using namespace Magpie;
static void Reset() {
    callbacks.clear(); messages.clear(); dispatched = 0;
    ScalingWindow::Get() = {};
}
static void RetainOldSession() {
    auto& window = ScalingWindow::Get();
    window.live = true; window.backends = 1; window.cleanupReady = false;
}
static void QueueRestoreCommands(ScalingRuntime& runtime) {
    auto& window = ScalingWindow::Get();
    runtime._State(ScalingState::Scaling);
    Check(runtime.RestartWithEffectParameters(window.SrcTracker().Handle(), window.Handle(), window.RunId(), {}, {}),
        "Pre-quit parameter restart was not queued");
    runtime.ToggleScaling(true);
}

int main() try {
    const HWND source = reinterpret_cast<HWND>(uintptr_t(1));
    {
        Reset(); RetainOldSession(); ScalingRuntime runtime;
        Check(runtime.Start(source, {}, true), "Pre-quit Start was not queued");
        QueueRestoreCommands(runtime);
        const auto generation = runtime._commandGeneration.load();
        EnqueueQuit();
        Check(!runtime.DispatchPass() && runtime._quitting && runtime.quitRequested,
            "Quit did not freeze command admission while cleanup remained pending");
        Check(runtime._commandGeneration == generation + 1 && runtime.State() == ScalingState::Stopping,
            "Quit did not supersede queued command generations/state");
        auto& window = ScalingWindow::Get();
        window.cleanupReady = true;
        const int before = dispatched;
        Check(runtime.DispatchPass() && dispatched == before && window.starts == 0 && window.backends == 0 && !window.IsRetiring(),
            "Loop-head cleanup dispatched a queued Start before returning");
    }
    {
        Reset(); RetainOldSession(); ScalingRuntime runtime;
        auto& window = ScalingWindow::Get();
        bool newStartAccepted = true, newRestartAccepted = true;
        Enqueue([&] {
            window.cleanupReady = true;
            window.CompletePendingDestroy();
            // Commands arriving inside the same dispatch batch as recovery must
            // also be refused, independently of the loop-head early return.
            newStartAccepted = runtime.Start(source, {}, true);
            newRestartAccepted = runtime.RestartWithEffectParameters(source,
                reinterpret_cast<HWND>(uintptr_t(2)), window.RunId(), {}, {});
            runtime.ToggleScaling(false);
        });
        Check(runtime.Start(source, {}, true), "Old Start was not queued for same-batch recovery");
        QueueRestoreCommands(runtime);
        EnqueueQuit();
        Check(!runtime.DispatchPass() && window.IsRetiring(), "Pending quit completed before recovery callback");
        Check(runtime.DispatchPass() && dispatched >= 4 && !newStartAccepted && !newRestartAccepted,
            "Same-batch recovery admitted a new Start/restart or skipped queued-command coverage");
        Check(window.starts == 0 && window.restarts == 0 && window.toggles == 0 && window.backends == 0 && !window.live,
            "A dispatched Start/restore command created work after quit intent");
    }
    {
        Reset(); ScalingRuntime runtime;
        Check(runtime.Start(source, {}, true), "Ordinary Start was rejected before quit");
        Check(!runtime.DispatchPass() && ScalingWindow::Get().starts == 1 && ScalingWindow::Get().backends == 1,
            "Quit admission guard changed normal Start behavior");
        QueueRestoreCommands(runtime);
        Check(!runtime.DispatchPass() && ScalingWindow::Get().restarts == 1 && ScalingWindow::Get().toggles == 1,
            "Quit admission guard changed normal recovery commands");
        EnqueueQuit();
        Check(runtime.DispatchPass() && ScalingWindow::Get().backends == 0 && !ScalingWindow::Get().live,
            "Quit returned with an existing backend still live");
        const auto queued = messages.size();
        Check(!runtime.Start(source, {}, true) && !runtime.RestartWithEffectParameters(source, nullptr, 0, {}, {}),
            "Post-quit public command entry admitted a replacement session");
        runtime.ToggleScaling(false);
        Check(messages.size() == queued, "Post-quit recovery queued another dispatcher callback");
    }
    std::cout << "Production runtime quit admission/message dispatch: " << checks << " checks passed; no UI/GPU.\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
