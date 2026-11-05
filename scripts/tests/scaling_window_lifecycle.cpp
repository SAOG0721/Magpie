#include <windows.h>
#include <wct.h>
#include <cassert>
#include <atomic>
#include <array>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include <fmt/format.h>
#include "ScalingSessionLifetime.h"
#include "DlssNrStatus.h"
#include "ScalingWindowRetirement.h"
#include "ReflexController.h"
static bool failDetach;
static LONG_PTR SetOwnerForTest(HWND window,int index,LONG_PTR owner) {
    if(failDetach && index==GWLP_HWNDPARENT && !owner) { SetLastError(87); return 0; }
    return SetWindowLongPtrW(window,index,owner);
}
#define SetWindowLongPtrW SetOwnerForTest
#include "ScalingWindowOwner.h"
#undef SetWindowLongPtrW
using namespace Magpie;
struct NvidiaForegroundCompatibility {
    static void Retire() {}
    static void ReleaseReferenceFocus() {}
    static void FinishRetire() {}
};
struct ScalingWindow { static uint32_t RunId() { return 10; } };
static HANDLE readyEvent,releaseEvent,lockHeldEvent;
static int pausePhase=-1, saves, snapshots, resourceReleases, inputReleases;
static HWND retiringOutput;
static void Pause(int phase) {
    if(pausePhase!=phase) return;
    assert(!IsWindow(retiringOutput));
    SetEvent(readyEvent); assert(WaitForSingleObject(releaseEvent,8000)==WAIT_OBJECT_0);
}
struct Logger {
    static Logger& Get() { static Logger l; return l; }
    void Info(std::string_view text) {
        if(text.starts_with("Window retirement complete") && pausePhase==0) {
            assert(!IsWindow(retiringOutput)); SetEvent(readyEvent);
        }
    }
    void Warn(std::string_view) {}
    void Win32Error(std::string_view) { assert(false); }
};
namespace Win32Helper {
static std::wstring GetWindowClassName(HWND h) { wchar_t n[256]{}; GetClassNameW(h,n,256); return n; }
}
namespace StrHelper { static std::string UTF16ToUTF8(const std::wstring& s) { std::string out; for(wchar_t c:s) out.push_back(static_cast<char>(c)); return out; } }
namespace NgxRuntimeGuard {
static bool IsFaulted() { return false; }
static uintptr_t FaultAddress() { return 0; } static uint32_t FaultThread() { return 0; } static uint32_t FaultCode() { return 0; }
}
namespace FrameTrace { static void Stop() {} }
namespace wil { static bool handle_wait(HANDLE h,DWORD ms) { return WaitForSingleObject(h,ms)==WAIT_OBJECT_0; } }
static UINT WM_MAGPIE_SCALINGCHANGED=WM_APP+100;
static BOOL PostMessageForTest(HWND,UINT,WPARAM,LPARAM) { return TRUE; }
static UINT EndPeriodForTest(UINT) { return 0; }
enum class ScalingError { NgxRestartRequired };
class TestDriver : public ReflexDriver {
public:
    ReflexConfigurationResult Configure(ReflexSettings settings) noexcept override {
        if(pausePhase==0 && settings.lowLatency && settings.minimumIntervalUs) {
            SetEvent(lockHeldEvent); assert(WaitForSingleObject(releaseEvent,8000)==WAIT_OBJECT_0);
        }
        if(!settings.lowLatency) Pause(1);
        return {0,0,true,settings.lowLatency};
    }
    ReflexSleepStatus QueryStatus() noexcept override { return {0,true}; }
    int Sleep() noexcept override { return 0; }
    int Marker(ReflexMarker,uint64_t) noexcept override { return 0; }
    int RegisterGenerationQueue(ID3D12CommandQueue*) noexcept override { return 0; }
    int Generation(ID3D12CommandQueue*,uint64_t,uint64_t,bool) noexcept override { return 0; }
    int FrontendRender(uint64_t,uint64_t,bool) noexcept override { return 0; }
    int Present(uint64_t,uint64_t,bool,bool) noexcept override { return 0; }
    void ReportFailure(const char*,int) noexcept override { assert(false); }
};
struct Handle { HANDLE value=nullptr; explicit operator bool() const { return value!=nullptr; } HANDLE get() const { return value; } };
struct Hook { void reset() { if(pausePhase==2) { assert(!IsWindow(retiringOutput)); SetEvent(readyEvent); } } };
struct OwnedWindow {
    HWND value=nullptr;
    HWND release() { return std::exchange(value,nullptr); }
    void reset() { if(value) assert(DestroyWindow(std::exchange(value,nullptr))); }
};
namespace wil { using unique_hwnd=OwnedWindow; }
struct Mutex { explicit operator bool() const { return false; } void ReleaseMutex() {} void reset() {} };
struct ParameterSession { std::vector<int> Applied() { ++snapshots; return {42}; } };
enum class SceneScreenshotState { Pending, Failed };
struct SceneScreenshotRequest { std::atomic<SceneScreenshotState> state{SceneScreenshotState::Pending}; };
struct Options {
    bool windowed=false;
    bool IsWindowedMode() const { return windowed; }
    void IsWindowedMode(bool value) { windowed=value; }
    std::vector<int> effects;
    std::shared_ptr<ParameterSession> parameterSession=std::make_shared<ParameterSession>();
    std::function<void(HWND,ScalingError,std::string,uint32_t)> reportErrorDetails;
    std::function<void(const Options&,HWND)> save=[](const Options&,HWND) { ++saves; };
};
class TestRenderer {
public:
    TestRenderer() : _sessionLifetime(std::make_shared<ScalingSessionLifetime>(10)) {
        _dlssNrSession->Begin(_dlssNrRunId, {true});
        _reflex.Initialize(std::make_unique<TestDriver>());
        _frameSyncConsumedEvent.value=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        _backendThread=std::thread([this]{ MSG m; PeekMessageW(&m,nullptr,0,0,PM_NOREMOVE);
            _backendReady.store(true); _backendReady.notify_one();
            if(pausePhase==2) assert(WaitForSingleObject(releaseEvent,8000)==WAIT_OBJECT_0);
        });
        _backendReady.wait(false);
    }
    ~TestRenderer() noexcept;
    void BeginShutdown() noexcept;
    void ReleaseParameterInput() { assert(!IsWindow(retiringOutput) || !GetWindow(retiringOutput,GW_OWNER)); ++inputReleases; }
    void ClearOverlayStates() {}
    int CaptureOverlayState() { return 7; }
    bool _ReleaseNgxConsumers() { Pause(3); ++resourceReleases; CloseHandle(_frameSyncConsumedEvent.value); return true; }
    std::mutex _effectParameterMailboxMutex;
    std::vector<int> _effectParameterMailbox;
    std::shared_ptr<ScalingSessionLifetime> _sessionLifetime;
    std::shared_ptr<DlssNrSessionState> _dlssNrSession = std::make_shared<DlssNrSessionState>();
    uint32_t _dlssNrRunId = 10;
    std::function<void(uint32_t, uint32_t, const DlssNrStatus&)> _dlssNrStatusChanged;
    std::shared_ptr<SceneScreenshotRequest> _sceneScreenshotRequest;
    void _FailScreenshotRequest(const std::shared_ptr<SceneScreenshotRequest>& request, const char*, uint32_t code) {
        assert(code == ERROR_CANCELLED);
        request->state = SceneScreenshotState::Failed;
    }
    ReflexController _reflex;
    std::atomic<bool> _synchronousFramePresentationEnabled=true;
    Handle _frameSyncConsumedEvent;
    Hook _hKeyboardHook;
    std::thread _backendThread;
    std::atomic<bool> _backendReady=false;
    bool _frameTraceStarted=false;
};
struct Cursor {
    void BeginShutdown() {}
    bool ReleasePendingCursorReposition() { return true; }
    ~Cursor() { assert(!IsWindow(retiringOutput)); }
};
struct Source { HWND source=nullptr; HWND Handle() const { return source; } bool IsSessionForeground() const { return true; } };
struct Coordinator { void RetireOutput() {} };
struct Base { HWND _hWnd=nullptr; HWND Handle() const { return _hWnd; } };
class TestWindow : public Base {
public:
    using base_type=Base;
    std::unique_ptr<TestRenderer> _renderer;
    std::unique_ptr<Cursor> _cursorManager;
    ScalingWindowRetirement _retirement;
    Source _srcTracker;
    Coordinator _sourceCoordinator;
    Options _options;
    unsigned _windowCallbackDepth=0;
    bool _isDestroying=false,_stopRequested=false,_sourceStateCheckDeferred=false,_finalizingRetirement=false;
    bool _frontendRenderPending=false,_isSrcRepositioning=false;
    int _pendingSourceTransition=0;
    uint32_t _runId=10;
    HWND _hwndRenderer=nullptr;
    std::vector<int> _dlssFgFrameJobs;
    Mutex _exclModeMutex;
    std::array<OwnedWindow,4> _hwndTouchHoles{},_hwndResizeHelpers{};
    uint32_t RunId() { return _runId; }
    void _BeginDestroy(HWND) noexcept;
    void Destroy() noexcept;
    void ToggleScaling(bool) noexcept;
    bool HasHeldParameterInput() const { return false; }
    void Stop() { Destroy(); }
    static inline bool sessionWindowed;
    static void SessionWindowedMode(bool value) { sessionWindowed=value; }
    RECT _rendererRect{0,0,32,32};
    LONG _lastWindowedRendererWidth=0;
    std::optional<int> _repositionOverlayState;
    std::optional<bool> _pendingWindowedMode;
    void RestartAfterSrcRepositioned();
    bool CompletePendingDestroy() noexcept;
    void _FinishDestroy() noexcept;
    void CleanAfterSrcRepositioned() {}
    void ShowError(ScalingError) { assert(false); }
};
#undef PostMessage
#define PostMessage PostMessageForTest
#define timeEndPeriod EndPeriodForTest
#include "ScalingWindowLifecycle.inc"
#undef timeEndPeriod
#undef PostMessage
static LRESULT CALLBACK WindowProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp) {
    if(msg==WM_NCCREATE) {
        auto self=static_cast<TestWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        if(self) { self->_hWnd=hwnd; SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self)); }
    }
    auto self=reinterpret_cast<TestWindow*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
    if(self) {
        ++self->_windowCallbackDepth;
        if(msg==WM_DESTROY) {
            self->_BeginDestroy(hwnd);
            self->_hWnd=nullptr;
            assert(self->_renderer && !self->CompletePendingDestroy());
            assert(self->_renderer->_sessionLifetime->IsStopping());
            assert(!self->_renderer->_synchronousFramePresentationEnabled.load());
            assert(WaitForSingleObject(self->_renderer->_frameSyncConsumedEvent.get(),0)==WAIT_OBJECT_0);
        }
        if(msg==WM_NCDESTROY) { self->_retirement.NonclientDestroyed(); }
        --self->_windowCallbackDepth;
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}
static HWND CreateTestWindow(TestWindow* self,HWND owner=nullptr) {
    HWND h=CreateWindowExW(WS_EX_NOACTIVATE,L"MagpieCompleteRetirementTest",L"Hidden lifecycle test",WS_POPUP,
        0,0,32,32,owner,nullptr,GetModuleHandleW(nullptr),self);
    assert(h); return h;
}
void TestWindow::RestartAfterSrcRepositioned() {
    assert(!_retirement.Pending() && !_renderer && snapshots==1 && _options.effects==std::vector<int>{42});
    assert(_options.IsWindowedMode() && sessionWindowed && _repositionOverlayState==7);
    _isSrcRepositioning=false;
    retiringOutput=CreateTestWindow(this,_srcTracker.Handle());
    _renderer=std::make_unique<TestRenderer>(); _cursorManager=std::make_unique<Cursor>();
}
static void Pump() { MSG m; while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)) DispatchMessageW(&m); }
int wmain(int argc,wchar_t** argv) {
    WNDCLASSW wc{}; wc.hInstance=GetModuleHandleW(nullptr); wc.lpszClassName=L"MagpieCompleteRetirementTest"; wc.lpfnWndProc=WindowProc;
    assert(RegisterClassW(&wc));
    if(argc==6) {
        HWND source=reinterpret_cast<HWND>(_wcstoui64(argv[1],nullptr,16));
        readyEvent=OpenEventW(EVENT_MODIFY_STATE,FALSE,argv[2]); releaseEvent=OpenEventW(SYNCHRONIZE,FALSE,argv[3]);
        const unsigned mode=wcstoul(argv[4],nullptr,10); pausePhase=static_cast<int>(wcstol(argv[5],nullptr,10));
        lockHeldEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        TestWindow window; window._srcTracker.source=source;
        HWND popup=CreateTestWindow(&window,mode==0||mode==7?nullptr:source); retiringOutput=popup;
        const HWND independentHole=CreateTestWindow(nullptr);
        assert(!GetWindow(independentHole,GW_OWNER)); window._hwndTouchHoles[0].value=independentHole;
        window._renderer=std::make_unique<TestRenderer>(); window._cursorManager=std::make_unique<Cursor>();
        std::thread configuration;
        if(mode==0||mode==1) {
            pausePhase=-1; SetEvent(readyEvent); assert(WaitForSingleObject(releaseEvent,8000)==WAIT_OBJECT_0);
        } else if(pausePhase==0) {
            configuration=std::thread([&]{ window._renderer->_reflex.SetFrameRateLimit(123); });
            assert(WaitForSingleObject(lockHeldEvent,3000)==WAIT_OBJECT_0);
        }
        failDetach=mode==3||mode==5;
        if(mode==6) window.ToggleScaling(true);
        else if(mode==4||mode==5) { assert(DestroyWindow(popup)); assert(window.CompletePendingDestroy()); }
        else window.Destroy();
        if(configuration.joinable()) configuration.join();
        assert(!window._retirement.Pending() && !IsWindow(independentHole));
        assert(!window.CompletePendingDestroy() && window._runId==11 && resourceReleases==1);
        assert(inputReleases==(failDetach?1:2));
        if(mode==6) {
            assert(snapshots==1 && saves==0 && window._options.effects==std::vector<int>{42});
            pausePhase=-1; failDetach=false;
            assert(window._options.IsWindowedMode());
            window.Destroy(); assert(resourceReleases==2 && window._runId==12 && saves==1);
        } else assert(saves==1);
        assert(!window._renderer && !window._cursorManager);
        CloseHandle(lockHeldEvent); CloseHandle(readyEvent); CloseHandle(releaseEvent); return 0;
    }
    wchar_t exe[32768]; GetModuleFileNameW(nullptr,exe,32768);
    unsigned index=0;
    for(unsigned mode=0;mode!=8;++mode) for(int phase=0;phase!=4;++phase) {
        if((mode==0||mode==1) && phase!=0) continue;
        HWND source=CreateTestWindow(nullptr);
        auto prefix=L"Local\\MagpieRetirement-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(++index);
        auto readyName=prefix+L"-ready",releaseName=prefix+L"-release";
        HANDLE ready=CreateEventW(nullptr,TRUE,FALSE,readyName.c_str()),release=CreateEventW(nullptr,TRUE,FALSE,releaseName.c_str());
        wchar_t hex[32]; swprintf_s(hex,L"%llx",reinterpret_cast<unsigned long long>(source));
        std::wstring cmd=L"\""+std::wstring(exe)+L"\" "+hex+L" "+readyName+L" "+releaseName+L" "+std::to_wstring(mode)+L" "+std::to_wstring(phase);
        STARTUPINFOW startup{}; startup.cb=sizeof(startup); PROCESS_INFORMATION child{};
        assert(CreateProcessW(exe,cmd.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&child));
        auto deadline=GetTickCount64()+5000;
        while(WaitForSingleObject(ready,0)!=WAIT_OBJECT_0 && GetTickCount64()<deadline) { Pump(); Sleep(1); }
        assert(WaitForSingleObject(ready,0)==WAIT_OBJECT_0);
        std::atomic<bool> completed=false; DWORD caller=GetCurrentThreadId();
        std::thread watchdog([&]{
            for(unsigned i=0;i<200&&!completed.load();++i) Sleep(10);
            if(!completed.load()) {
                HWCT session=OpenThreadWaitChainSession(0,nullptr); WAITCHAIN_NODE_INFO nodes[WCT_MAX_NODE_COUNT]{};
                DWORD count=WCT_MAX_NODE_COUNT; BOOL cycle=FALSE;
                if(GetThreadWaitChain(session,0,WCTP_GETINFO_ALL_FLAGS,caller,&count,nodes,&cycle)) {
                    std::cout<<"positive_control_wct_nodes="<<count<<std::endl;
                    for(DWORD n=0;n<count;++n) std::cout<<"wct_type="<<nodes[n].ObjectType<<" status="<<nodes[n].ObjectStatus<<std::endl;
                }
                CloseThreadWaitChainSession(session);
            }
            SetEvent(release);
        });
        auto start=GetTickCount64();
        assert(SetWindowPos(source,HWND_TOP,0,0,0,0,SWP_NOACTIVATE|SWP_NOMOVE|SWP_NOSIZE));
        auto duration=GetTickCount64()-start; completed.store(true); watchdog.join();
        std::cout<<"mode="<<mode<<" phase="<<phase<<" source_zorder_ms="<<duration<<std::endl;
        assert(mode==1 ? duration>=1500 : duration<1000);
        deadline=GetTickCount64()+5000;
        while(WaitForSingleObject(child.hProcess,0)!=WAIT_OBJECT_0&&GetTickCount64()<deadline) { Pump(); Sleep(1); }
        assert(WaitForSingleObject(child.hProcess,0)==WAIT_OBJECT_0);
        DWORD exit=1; GetExitCodeProcess(child.hProcess,&exit); assert(exit==0);
        assert(DestroyWindow(source)); CloseHandle(child.hThread); CloseHandle(child.hProcess); CloseHandle(ready); CloseHandle(release);
    }
    std::cout<<"PASS: "<<index<<" two-process complete retirement probes (real Reflex lock/Configure, backend join, SDK boundary); positive control included"<<std::endl;
}
