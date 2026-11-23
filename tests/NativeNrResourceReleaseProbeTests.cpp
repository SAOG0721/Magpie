// Uses the actual production callback/context/gate and SDK callback typedef.
// Only IUnknown work, trace collection and logging are deterministic CPU hosts.
#include <windows.h>
#include <unknwn.h>
#include <d3d12.h>
#include <winrt/base.h>
#include <nvsdk_ngx_defs.h>
#include "FrameTrace.h"
#include "include/NgxRuntimeGuard.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <list>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <type_traits>
#include <vector>

namespace FT = Magpie::FrameTrace;
namespace AllocationFailure {
thread_local int after = -1;
thread_local bool observed = false;
}
void* operator new(std::size_t bytes) {
    if (AllocationFailure::after == 0) {
        AllocationFailure::after = -1;
        AllocationFailure::observed = true;
        throw std::bad_alloc();
    }
    if (AllocationFailure::after > 0) --AllocationFailure::after;
    if (void* memory = std::malloc(bytes ? bytes : 1)) return memory;
    throw std::bad_alloc();
}
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }

static void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
namespace Observed {
struct Log {
    enum class Kind { Release, Identity, Retained };
    uint64_t call, owner, frame;
    uint32_t pass;
    const char* phase;
    DWORD thread;
    uintptr_t resource;
    ULONG remaining;
    double cpuUs;
    bool deferred;
    uint64_t retirementValue, completedValue;
    double retirementWaitUs;
    uint32_t retirementHr;
    Kind kind=Kind::Release;
    uint64_t width=0;
    uint32_t height=0, dimension=0, format=0, heapType=0, resourceFlags=0, heapFlags=0, identityHr=0, heapHr=0;
    uint64_t retained=0, released=0, resourceBytes=0, retainedBytes=0, limitBytes=0;
};
struct Event { FT::Event event; uint64_t frame; int64_t stage, pass; DWORD thread; };
std::mutex mutex;
std::vector<Log> logs;
std::vector<Event> events;
bool enabled = true, throwLog = false;
HANDLE retainedSeen=nullptr;
std::atomic<HANDLE> drainCaptured=nullptr;
std::atomic_uint64_t drainTarget=0;
void ObserveDrainTicket(uint64_t target) {
    if (auto event=drainCaptured.load()) { drainTarget=target; SetEvent(event); }
}
}
namespace fmt {
template<class... Args> Observed::Log format(const char*, Args... args) {
    auto values = std::tuple{args...};
    if constexpr (sizeof...(Args)==14) {
        return {std::get<0>(values),std::get<1>(values),std::get<2>(values),std::get<3>(values),
            std::get<4>(values),std::get<5>(values),std::get<6>(values),std::get<7>(values),std::get<8>(values),std::get<9>(values),
            std::get<10>(values),std::get<11>(values),std::get<12>(values),std::get<13>(values)};
    } else {
        static_assert(sizeof...(Args)==15 || sizeof...(Args)==16,"Production diagnostic fields changed");
        Observed::Log log{};
        log.call=std::get<0>(values); log.owner=std::get<1>(values); log.frame=std::get<2>(values);
        log.pass=std::get<3>(values); log.phase=std::get<4>(values);
        if constexpr (sizeof...(Args)==15) {
            log.kind=Observed::Log::Kind::Retained;
            log.thread=std::get<5>(values); log.resource=std::get<6>(values);
            log.retirementValue=std::get<7>(values); log.completedValue=std::get<8>(values);
            log.retirementWaitUs=std::get<9>(values); log.retained=std::get<10>(values); log.released=std::get<11>(values);
            log.resourceBytes=std::get<12>(values); log.retainedBytes=std::get<13>(values); log.limitBytes=std::get<14>(values);
        } else {
            log.kind=Observed::Log::Kind::Identity;
            log.resource=std::get<5>(values); log.identityHr=std::get<6>(values);
            log.width=std::get<7>(values); log.height=std::get<8>(values); log.dimension=std::get<9>(values);
            log.format=std::get<10>(values); log.heapType=std::get<11>(values); log.resourceFlags=std::get<12>(values);
            log.heapFlags=std::get<13>(values); log.heapHr=std::get<14>(values);
            log.resourceBytes=std::get<15>(values);
        }
        return log;
    }
}
}
namespace Magpie {
struct Logger {
    static Logger& Get() { static Logger logger; return logger; }
    void Info(const Observed::Log& log) {
        if (Observed::throwLog) throw std::runtime_error("simulated logging failure");
        std::lock_guard lock(Observed::mutex);
        Observed::logs.push_back(log);
        if (log.kind==Observed::Log::Kind::Retained && Observed::retainedSeen) SetEvent(Observed::retainedSeen);
    }
};
#define MAGPIE_NR_TEST_DRAIN_TICKET(target) ::Observed::ObserveDrainTicket(target)
#include "NativeNrResourceReleaseProbe.inl"
#undef MAGPIE_NR_TEST_DRAIN_TICKET
struct DLSSNRSettings {};
struct FrameGuidanceView {};
struct DLSSNRFilter {
    struct Impl { IUnknown* resource = nullptr; unsigned parameterCalls = 0; };
};
static void SetEvaluateParametersUnsafe(DLSSNRFilter::Impl& impl, const DLSSNRSettings&,
    const FrameGuidanceView&, bool) {
    ++impl.parameterCalls;
    auto* resource = impl.resource;
    impl.resource = nullptr;
    if (resource) NrResourceReleaseCallback(resource);
}
#include "NativeNrParametersWrapper.inl"
}
namespace Magpie::FrameTrace {
bool NativeNrStagesEnabled() noexcept { return Observed::enabled; }
int64_t Tick() noexcept { LARGE_INTEGER tick{}; QueryPerformanceCounter(&tick); return tick.QuadPart; }
void Record(Event event, int64_t, int64_t, uint64_t frame, int64_t stage, int64_t pass) noexcept {
    std::lock_guard lock(Observed::mutex);
    Observed::events.push_back({event,frame,stage,pass,GetCurrentThreadId()});
}
}
static_assert(std::is_convertible_v<decltype(&Magpie::NrResourceReleaseCallback), PFN_NVSDK_NGX_ResourceReleaseCallback>);

struct Counts {
    std::atomic_uint addRefs = 0, releases = 0;
    std::atomic_bool destroyed = false;
    DWORD releaseThread = 0;
    HANDLE entered = nullptr, permit = nullptr;
    bool* deviceAlive = nullptr;
    bool releasedBeforeDeviceTeardown = true, permitSucceeded = true;
    Magpie::NrResourceReleaseContext atRelease;
};
class Resource final : public IUnknown {
public:
    explicit Resource(Counts& counts) : _counts(counts) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** result) override { *result = nullptr; return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { ++_counts.addRefs; return ++_refs; }
    ULONG STDMETHODCALLTYPE Release() override {
        ++_counts.releases;
        _counts.atRelease = Magpie::g_nrResourceReleaseContext;
        _counts.releaseThread = GetCurrentThreadId();
        if (_counts.entered) SetEvent(_counts.entered);
        if (_counts.permit) _counts.permitSucceeded = WaitForSingleObject(_counts.permit, 5000) == WAIT_OBJECT_0;
        if (_counts.deviceAlive) _counts.releasedBeforeDeviceTeardown = *_counts.deviceAlive;
        const auto refs = --_refs;
        if (!refs) { _counts.destroyed = true; delete this; }
        return refs;
    }
private:
    Counts& _counts;
    ULONG _refs = 1;
};

struct ResourceProperties {
    D3D12_RESOURCE_DESC desc{};
    D3D12_HEAP_PROPERTIES heap{};
    D3D12_HEAP_FLAGS heapFlags=D3D12_HEAP_FLAG_NONE;
    HRESULT qiHr=S_OK,heapHr=S_OK;
};
static ResourceProperties TextureProperties() {
    ResourceProperties result;
    result.desc.Width=1280; result.desc.Height=720; result.desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    result.desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM; result.desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    result.heap.Type=D3D12_HEAP_TYPE_DEFAULT;
    return result;
}
static ResourceProperties UploadBufferProperties(uint64_t width,uint64_t alignment=0) {
    ResourceProperties result;
    auto& desc=result.desc;
    desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width=width; desc.Alignment=alignment;
    desc.Height=1; desc.DepthOrArraySize=1; desc.MipLevels=1; desc.SampleDesc.Count=1;
    desc.Format=DXGI_FORMAT_UNKNOWN; desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    result.heap.Type=D3D12_HEAP_TYPE_UPLOAD;
    return result;
}
class Resource12 final : public ID3D12Resource {
public:
    explicit Resource12(Counts& counts,ResourceProperties properties=TextureProperties()) : _counts(counts),_properties(properties) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** result) override {
        *result=nullptr;
        if (FAILED(_properties.qiHr)) return _properties.qiHr;
        if (iid!=__uuidof(ID3D12Resource) && iid!=__uuidof(IUnknown)) return E_NOINTERFACE;
        *result=static_cast<ID3D12Resource*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { ++_counts.addRefs; return ++_refs; }
    ULONG STDMETHODCALLTYPE Release() override {
        ++_counts.releases;
        _counts.releaseThread=GetCurrentThreadId();
        if (_refs==1) {
            if (_counts.entered) SetEvent(_counts.entered);
            if (_counts.permit) _counts.permitSucceeded=WaitForSingleObject(_counts.permit,5000)==WAIT_OBJECT_0;
        }
        const auto refs=--_refs;
        if (!refs) { _counts.destroyed=true; delete this; }
        return refs;
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID,UINT*,void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID,UINT,const void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID,const IUnknown*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID,void** result) override { *result=nullptr; return E_NOINTERFACE; }
    HRESULT STDMETHODCALLTYPE Map(UINT,const D3D12_RANGE*,void**) override { return E_NOTIMPL; }
    void STDMETHODCALLTYPE Unmap(UINT,const D3D12_RANGE*) override {}
    D3D12_RESOURCE_DESC STDMETHODCALLTYPE GetDesc() override { return _properties.desc; }
    D3D12_GPU_VIRTUAL_ADDRESS STDMETHODCALLTYPE GetGPUVirtualAddress() override { return 0; }
    HRESULT STDMETHODCALLTYPE WriteToSubresource(UINT,const D3D12_BOX*,const void*,UINT,UINT) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ReadFromSubresource(void*,UINT,UINT,UINT,const D3D12_BOX*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetHeapProperties(D3D12_HEAP_PROPERTIES* heap,D3D12_HEAP_FLAGS* flags) override {
        *heap=_properties.heap; *flags=_properties.heapFlags; return _properties.heapHr;
    }
private:
    Counts& _counts;
    ResourceProperties _properties;
    ULONG _refs=1;
};

// A CPU host of the real ID3D12Fence ABI. Only the completion source is
// controlled; production batching, COM ownership and worker waits are intact.
class Fence final : public ID3D12Fence {
public:
    explicit Fence(uint64_t completed = 1) : _completed(completed) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** result) override { *result=nullptr; return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++_refs; }
    ULONG STDMETHODCALLTYPE Release() override { const auto refs=--_refs; if (!refs) delete this; return refs; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID,UINT*,void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID,UINT,const void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID,const IUnknown*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID,void** result) override { *result=nullptr; return E_NOINTERFACE; }
    UINT64 STDMETHODCALLTYPE GetCompletedValue() override { return _completed.load(); }
    HRESULT STDMETHODCALLTYPE SetEventOnCompletion(UINT64 value,HANDLE event) override {
        Require(!event,"The production worker must use its own blocking fence wait");
        waitCalls.fetch_add(1);
        if (waitEntered) SetEvent(waitEntered);
        if (failWait) return E_OUTOFMEMORY;
        std::unique_lock lock(_mutex);
        _changed.wait(lock,[&] { return _completed.load()>=value; });
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Signal(UINT64 value) override {
        { std::lock_guard lock(_mutex); _completed=value; }
        _changed.notify_all();
        return S_OK;
    }
    HANDLE waitEntered=nullptr;
    bool failWait=false;
    std::atomic_uint waitCalls=0;
private:
    std::atomic_ulong _refs=1;
    std::atomic_uint64_t _completed;
    std::mutex _mutex;
    std::condition_variable _changed;
};
static winrt::com_ptr<Fence> MakeFence(uint64_t completed=1) {
    winrt::com_ptr<Fence> fence;
    fence.attach(new Fence(completed));
    return fence;
}

static void Gate() {
    for (const auto* option : {L"",L"0",L"01",L"true",L"1 "}) {
        SetEnvironmentVariableW(L"MAGPIE_DIAG_NR_RELEASE_CALLBACK", option);
        Require(!Magpie::NrSynchronousReleaseRequested(true), "Probe must require exact opt-in");
    }
    SetEnvironmentVariableW(L"MAGPIE_DIAG_NR_RELEASE_CALLBACK", L"1");
    Require(!Magpie::NrSynchronousReleaseRequested(false), "Probe must not apply to non-signed backends");
    Observed::enabled = false;
    Require(!Magpie::NrSynchronousReleaseRequested(true), "Probe must not apply without fine stages");
    Observed::enabled = true;
    Require(Magpie::NrSynchronousReleaseRequested(true), "Signed NR/fine-stage/exact opt-in must enable the probe");
    SetEnvironmentVariableW(L"MAGPIE_DIAG_NR_RELEASE_CALLBACK", nullptr);
}

static void DirectReleaseAndAttribution() {
    Counts counts;
    int owner = 0;
    auto* resource = new Resource(counts);
    const auto pointer = reinterpret_cast<uintptr_t>(resource);
    const auto previous = Magpie::g_nrResourceReleaseCalls.load();
    {
        Magpie::NrResourceReleaseScope scope(&owner, 1189, 2, "evaluate");
        Magpie::NrResourceReleaseCallback(resource);
        Require(counts.destroyed, "Probe must finish synchronous destruction before returning");
    }
    Require(counts.addRefs == 0 && counts.releases == 1 && counts.atRelease.owner == &owner &&
        counts.atRelease.frame == 1189 && counts.atRelease.pass == 2, "Probe must transfer no reference or owner");
    Require(Magpie::g_nrResourceReleaseContext.owner == nullptr, "Owner context must restore after the API call");
    const auto& log = Observed::logs.back();
    Require(log.call == previous + 1 && log.owner == reinterpret_cast<uintptr_t>(&owner) && log.frame == 1189 &&
        log.pass == 2 && std::string(log.phase) == "evaluate" && log.thread == GetCurrentThreadId() &&
        log.resource == pointer && log.remaining == 0 && log.cpuUs >= 0, "Per-call release identity/timing record changed");
    const auto size = Observed::events.size();
    Require(size >= 2 && Observed::events[size-2].event == FT::Event::NativeNrStageBegin &&
        Observed::events[size-1].event == FT::Event::NativeNrStageEnd &&
        Observed::events[size-1].stage == static_cast<int64_t>(FT::NativeNrStage::InferenceResourceRelease) &&
        Observed::events[size-1].frame == 1189 && Observed::events[size-1].pass == 2,
        "Actual Release must retain its own matching frame/pass trace scope");
}

static void NestedAndOtherThread() {
    int first = 0, second = 0;
    Magpie::NrResourceReleaseOwner queue;
    Require(queue.Start(), "Cannot start the native release owner");
    Magpie::NrResourceReleaseBatch batch(&queue);
    {
        Magpie::NrResourceReleaseScope outer(&first, 10, 1, "evaluate", &batch, true);
        { Magpie::NrResourceReleaseScope inner(&second, 20, 2, "feature-release");
          Require(Magpie::g_nrResourceReleaseContext.owner == &second, "Nested SDK owner must become current"); }
        Require(Magpie::g_nrResourceReleaseContext.owner == &first, "Nested SDK owner must restore");
        Counts counts;
        std::thread worker([&] { Magpie::NrResourceReleaseCallback(new Resource(counts)); });
        worker.join();
        Require(counts.releases == 1 && counts.destroyed && counts.atRelease.owner == nullptr &&
            counts.atRelease.frame == 0 && counts.atRelease.pass == 0, "SDK worker must remain explicitly unbound");
        const auto& log = Observed::logs.back();
        Require(log.owner == 0 && log.frame == 0 && log.pass == 0 && std::string(log.phase) == "unbound" &&
            log.thread != GetCurrentThreadId(), "Worker callback must keep its actual thread and unknown owner");
    }
    Require(queue.Drain().accepted == 0, "An unbound callback thread must never borrow the caller's queue");
    queue.Stop();
}

static void OriginalReferenceAndGuardIndependence() {
    Magpie::NrResourceReleaseOwner owner;
    Require(owner.Start(), "Cannot start the native release owner");
    bool deviceAlive = true;
    Counts counts;
    counts.entered = CreateEventW(nullptr,TRUE,FALSE,nullptr);
    counts.permit = CreateEventW(nullptr,TRUE,FALSE,nullptr);
    counts.deviceAlive = &deviceAlive;
    Require(counts.entered && counts.permit, "Cannot create release barrier events");
    int featureOwner = 0;
    auto fence = MakeFence();
    Magpie::NrResourceReleaseBatch batch(&owner);
    bool enteredWhileGuardHeld = false;
    DWORD seh = 0;
    const auto submitted = Magpie::NgxRuntimeGuard::Invoke([&] {
        Magpie::NrResourceReleaseScope call(&featureOwner,1201,1,"evaluate",&batch,true);
        Magpie::NrResourceReleaseCallback(new Resource(counts));
        Require(batch.Submit(fence.get(),1),"A submitted fence must transfer the job-local references");
        // A worker that also takes the NGX lock cannot enter Release here.
        enteredWhileGuardHeld = WaitForSingleObject(counts.entered,1000) == WAIT_OBJECT_0;
        return 1;
    },0,&seh);
    const bool callbackReturnedBeforeRelease = !counts.destroyed.load();
    std::atomic_bool drained = false;
    HANDLE drainEntered = CreateEventW(nullptr,TRUE,FALSE,nullptr);
    HANDLE drainFinished = CreateEventW(nullptr,TRUE,FALSE,nullptr);
    std::thread drain([&] { SetEvent(drainEntered); owner.Drain(); drained = true; SetEvent(drainFinished); });
    const bool drainerStarted = WaitForSingleObject(drainEntered,1000) == WAIT_OBJECT_0;
    const bool heldUntilRelease = WaitForSingleObject(drainFinished,10) == WAIT_TIMEOUT;
    SetEvent(counts.permit);
    drain.join();
    const auto stats = owner.Drain();
    owner.Stop();
    deviceAlive = false;
    CloseHandle(counts.entered); CloseHandle(counts.permit);
    CloseHandle(drainEntered); CloseHandle(drainFinished);
    Require(submitted == 1 && seh == 0 && enteredWhileGuardHeld && callbackReturnedBeforeRelease,
        "Evaluate must return without waiting for the transferred reference, independent of the NGX guard");
    Require(drainerStarted && heldUntilRelease && drained && stats.accepted == 1 && stats.completed == 1 &&
        counts.addRefs == 0 && counts.releases == 1 && counts.destroyed && counts.permitSucceeded &&
        counts.releasedBeforeDeviceTeardown && counts.releaseThread != GetCurrentThreadId(),
        "Drain must retire the original reference exactly once before SDK/device teardown");
    const auto& log = Observed::logs.back();
    Require(log.deferred && log.owner == reinterpret_cast<uintptr_t>(&featureOwner) && log.frame == 1201 && log.pass == 1,
        "Deferred Release must preserve callback identity after its TLS context has ended");
}

static void StopAndFailureFallback() {
    int featureOwner = 0;
    Magpie::NrResourceReleaseOwner unavailable;
    Magpie::NrResourceReleaseBatch unavailableBatch(&unavailable);
    Counts fallback;
    {
        Magpie::NrResourceReleaseScope call(&featureOwner,2,1,"evaluate",&unavailableBatch,true);
        Magpie::NrResourceReleaseCallback(new Resource(fallback));
    }
    Require(fallback.releases == 1 && fallback.destroyed && fallback.releaseThread == GetCurrentThreadId() &&
        unavailable.Drain().fallback == 1, "An unavailable owner must leave the original reference for synchronous release");

    Magpie::NrResourceReleaseOwner owner;
    Require(owner.Start(), "Cannot start release owner for stop test");
    auto fence=MakeFence();
    Magpie::NrResourceReleaseBatch batch(&owner);
    Counts pending;
    pending.entered = CreateEventW(nullptr,TRUE,FALSE,nullptr);
    pending.permit = CreateEventW(nullptr,TRUE,FALSE,nullptr);
    {
        Magpie::NrResourceReleaseScope call(&featureOwner,3,1,"evaluate",&batch,true);
        Magpie::NrResourceReleaseCallback(new Resource(pending));
    }
    Require(batch.Submit(fence.get(),1),"Cannot submit the stop fixture's retired references");
    const bool workerEntered = WaitForSingleObject(pending.entered,1000) == WAIT_OBJECT_0;
    HANDLE stopped = CreateEventW(nullptr,TRUE,FALSE,nullptr);
    std::thread stop([&] { owner.Stop(); SetEvent(stopped); });
    const bool stopWaited = WaitForSingleObject(stopped,10) == WAIT_TIMEOUT;
    SetEvent(pending.permit);
    stop.join();
    CloseHandle(pending.entered); CloseHandle(pending.permit); CloseHandle(stopped);
    Require(workerEntered && stopWaited && pending.destroyed && pending.releases == 1 && !owner.Start(),
        "Stop must finish accepted work and join the worker before closing the owner");
    Counts late;
    {
        Magpie::NrResourceReleaseScope call(&featureOwner,4,1,"evaluate",&batch,true);
        Magpie::NrResourceReleaseCallback(new Resource(late));
    }
    Require(late.releases == 1 && late.destroyed && late.releaseThread == GetCurrentThreadId(),
        "A callback after owner closure must synchronously consume its reference once");

    Magpie::NrResourceReleaseOwner logging;
    Require(logging.Start(), "Cannot start release owner for logging failure test");
    Magpie::NrResourceReleaseBatch loggingBatch(&logging);
    Counts released;
    Observed::throwLog = true;
    {
        Magpie::NrResourceReleaseScope call(&featureOwner,5,1,"evaluate",&loggingBatch,true);
        Magpie::NrResourceReleaseCallback(new Resource(released));
    }
    Require(loggingBatch.Submit(fence.get(),1),"Cannot submit the logging failure fixture");
    const auto stats = logging.Drain();
    logging.Stop();
    Observed::throwLog = false;
    Require(released.releases == 1 && released.destroyed && stats.accepted == stats.completed,
        "Diagnostic failure must not lose a reference or prevent a lifecycle drain");
}

static void FactoryFailureAndDefaultPolicy() {
    for (int failAfter : {0,1,2,3}) {
        AllocationFailure::observed = false;
        AllocationFailure::after = failAfter;
        auto unavailable = Magpie::CreateNrResourceReleaseOwner();
        AllocationFailure::after = -1;
        Require(AllocationFailure::observed && !unavailable,
            "Owner allocation/initialization failure must be contained by its noexcept factory");
        Counts fallback;
        int featureOwner = 0;
        Magpie::NrResourceReleaseBatch batch(unavailable.get());
        {
            Magpie::NrResourceReleaseScope call(&featureOwner,6,1,"evaluate",&batch,true);
            Magpie::NrResourceReleaseCallback(new Resource(fallback));
        }
        Require(fallback.releases == 1 && fallback.destroyed && fallback.releaseThread == GetCurrentThreadId() &&
            !Magpie::NgxRuntimeGuard::IsFaulted(), "Missing owner must preserve synchronous Release without faulting NGX");
    }

    Observed::enabled = false;
    Magpie::g_nrReleaseDiagnostics = false;
    const auto logs = Observed::logs.size(), events = Observed::events.size();
    auto owner = Magpie::CreateNrResourceReleaseOwner();
    Require(bool(owner), "Default owner creation failed");
    Counts deferred;
    int featureOwner = 0;
    auto fence=MakeFence();
    Magpie::NrResourceReleaseBatch batch(owner.get());
    {
        Magpie::NrResourceReleaseScope call(&featureOwner,7,1,"evaluate",&batch,true);
        Magpie::NrResourceReleaseCallback(new Resource(deferred));
    }
    Require(batch.Submit(fence.get(),1),"Cannot submit the default-policy fixture");
    const auto stats = owner->Drain();
    owner->Stop();
    Require(stats.accepted == 1 && stats.completed == 1 && deferred.destroyed && deferred.releases == 1 &&
        deferred.addRefs == 0 && deferred.releaseThread != GetCurrentThreadId() &&
        Observed::logs.size() == logs && Observed::events.size() == events,
        "Formal deferred policy must work without trace, fine stages or the diagnostic environment");
    Observed::enabled = true;
    Magpie::g_nrReleaseDiagnostics = true;
}

static void ParameterWrapperNeverQueues() {
    auto owner = Magpie::CreateNrResourceReleaseOwner();
    Require(bool(owner), "Cannot create a release owner for parameter scope test");
    Counts counts;
    Magpie::DLSSNRFilter::Impl impl{new Resource(counts)};
    DWORD seh = 123;
    int featureOwner = 0;
    Magpie::NrResourceReleaseBatch batch(owner.get());
    const auto firstEvent = Observed::events.size();
    bool synchronous = false, outerRestored = false;
    {
        // Even an enclosing Evaluate context must not leak defer permission
        // into the production parameter wrapper.
        Magpie::NrResourceReleaseScope outer(&featureOwner,1201,1,"evaluate",&batch,true);
        const bool success = Magpie::SetEvaluateParametersSafely(impl,{}, {},false,&seh,1202,2);
        synchronous = success && seh == 0 && impl.parameterCalls == 1 && counts.destroyed &&
            counts.releases == 1 && counts.addRefs == 0 && counts.releaseThread == GetCurrentThreadId() &&
            counts.atRelease.owner == &impl && counts.atRelease.frame == 1202 && counts.atRelease.pass == 2 &&
            std::string(counts.atRelease.phase) == "parameters" && !counts.atRelease.batch && !counts.atRelease.defer;
        outerRestored = Magpie::g_nrResourceReleaseContext.batch == &batch && Magpie::g_nrResourceReleaseContext.defer;
    }
    const auto stats = owner->Drain();
    owner->Stop();
    Require(synchronous && outerRestored && stats.accepted == 0 && stats.completed == 0 && stats.fallback == 0,
        "Parameter callbacks must synchronously release once, preserve frame/pass and never touch a release queue");
    const auto& log = Observed::logs.back();
    Require(!log.deferred && log.frame == 1202 && log.pass == 2 && std::string(log.phase) == "parameters",
        "Parameter release diagnostics must keep their synchronous identity");
    for (size_t i=firstEvent;i<Observed::events.size();++i)
        Require(Observed::events[i].frame == 1202 && Observed::events[i].pass == 2 &&
            Observed::events[i].stage != static_cast<int64_t>(FT::NativeNrStage::InferenceResourceReleaseQueue),
            "Parameter scope must retain diagnostic context without queue-stage events");
}

static void SubmittedTicketRetiresBeforeRelease() {
    Magpie::NrResourceReleaseOwner owner;
    Require(owner.Start(),"Cannot start the fence retirement fixture");
    auto fence=MakeFence(0);
    fence->waitEntered=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    Counts counts;
    counts.entered=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    int featureOwner=0;
    Magpie::NrResourceReleaseBatch batch(&owner);
    {
        Magpie::NrResourceReleaseScope call(&featureOwner,1301,2,"evaluate",&batch,true);
        Magpie::NrResourceReleaseCallback(new Resource(counts));
    }
    const bool heldBeforeSubmission=WaitForSingleObject(counts.entered,10)==WAIT_TIMEOUT;
    Require(heldBeforeSubmission && owner.Drain().accepted==0 && !batch.Empty(),
        "An SDK-transferred reference must stay in the job until Execute+Signal succeeds");
    Require(batch.Submit(fence.get(),9) && batch.Empty(),"A successful submission must transfer the whole batch");
    const bool workerWaiting=WaitForSingleObject(fence->waitEntered,1000)==WAIT_OBJECT_0;
    const bool heldBeforeRetirement=WaitForSingleObject(counts.entered,10)==WAIT_TIMEOUT;
    HANDLE stopped=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    std::thread stop([&] { owner.Stop(); SetEvent(stopped); });
    const bool stopWaited=WaitForSingleObject(stopped,10)==WAIT_TIMEOUT;
    fence->Signal(9);
    stop.join();
    const auto stats=owner.Drain();
    CloseHandle(stopped); CloseHandle(counts.entered); CloseHandle(fence->waitEntered);
    Require(workerWaiting && heldBeforeRetirement && stopWaited && counts.destroyed && counts.releases==1 &&
        counts.addRefs==0 && stats.accepted==1 && stats.completed==1 && stats.fallback==0,
        "Stop must wait for the submitted GPU ticket, then release exactly the SDK's original reference");
    const auto& log=Observed::logs.back();
    Require(log.frame==1301 && log.pass==2 && log.deferred && log.retirementValue==9 &&
        log.completedValue==9 && log.retirementWaitUs>0 && log.retirementHr==S_OK,
        "Actual completion and its CPU wait must remain associated with the callback frame/pass");
}

static void UnsubmittedAndWaitFailure() {
    auto owner=Magpie::CreateNrResourceReleaseOwner();
    Require(bool(owner),"Cannot create the failure retirement fixture");
    int featureOwner=0;
    Counts unsubmitted;
    {
        Magpie::NrResourceReleaseBatch batch(owner.get());
        Magpie::NrResourceReleaseScope call(&featureOwner,1302,1,"evaluate",&batch,true);
        Magpie::NrResourceReleaseCallback(new Resource(unsubmitted));
        Require(!unsubmitted.destroyed && !batch.Submit(nullptr,0) && owner->Drain().accepted==0,
            "Failed Close/Signal must not publish a never-signalled GPU ticket");
        // No GPU work was submitted by this host. Production first drains
        // accepted work on its failure boundary, then destroys this same batch.
    }
    Require(unsubmitted.destroyed && unsubmitted.releases==1 && unsubmitted.addRefs==0 &&
        unsubmitted.releaseThread==GetCurrentThreadId() && owner->Drain().fallback==1,
        "A failed job must consume every unsubmitted original reference exactly once");

    for (bool removed : {false,true}) {
        auto fence=MakeFence(0);
        fence->waitEntered=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        fence->failWait=true;
        Counts counts;
        counts.entered=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        Magpie::NrResourceReleaseBatch batch(owner.get());
        {
            Magpie::NrResourceReleaseScope call(&featureOwner,1303,1,"evaluate",&batch,true);
            Magpie::NrResourceReleaseCallback(new Resource(counts));
        }
        Require(batch.Submit(fence.get(),10),"Cannot publish the wait-failure fixture");
        const bool waitAttempted=WaitForSingleObject(fence->waitEntered,1000)==WAIT_OBJECT_0;
        const bool held=WaitForSingleObject(counts.entered,10)==WAIT_TIMEOUT;
        fence->Signal(removed ? UINT64_MAX : 10);
        const auto stats=owner->Drain();
        CloseHandle(counts.entered); CloseHandle(fence->waitEntered);
        Require(waitAttempted && held && counts.destroyed && counts.releases==1 && counts.addRefs==0 &&
            stats.accepted==stats.completed,
            "A failed event wait must still observe real completion or device removal before releasing");
        const auto& log=Observed::logs.back();
        Require(log.retirementValue==10 && log.completedValue==(removed ? UINT64_MAX : 10) &&
            log.retirementHr==static_cast<uint32_t>(E_OUTOFMEMORY),
            "The fallback completion path must report the real fence and wait failure");
    }
    owner->Stop();
}

static void EmptyBatchAndStagingAllocationFailure() {
    auto owner=Magpie::CreateNrResourceReleaseOwner();
    Require(bool(owner),"Cannot create the lazy batch fixture");
    AllocationFailure::observed=false;
    AllocationFailure::after=100;
    { Magpie::NrResourceReleaseBatch empty(owner.get()); }
    const int remaining=AllocationFailure::after;
    AllocationFailure::after=-1;
    Require(remaining==100 && !AllocationFailure::observed,
        "Constructing and destroying a callback-free job batch must perform no allocation");

    Observed::enabled=false;
    Magpie::g_nrReleaseDiagnostics=false;
    const auto logs=Observed::logs.size(),events=Observed::events.size();
    int featureOwner=0;
    for (int failure : {0,1,2}) {
        Counts counts;
        Magpie::NrResourceReleaseBatch batch(owner.get());
        auto* resource=new Resource(counts);
        AllocationFailure::observed=false;
        AllocationFailure::after=failure;
        {
            Magpie::NrResourceReleaseScope call(&featureOwner,1304,1,"evaluate",&batch,true);
            Magpie::NrResourceReleaseCallback(resource);
        }
        AllocationFailure::after=-1;
        Require(AllocationFailure::observed && batch.Empty() && counts.destroyed && counts.releases==1 &&
            counts.addRefs==0 && counts.releaseThread==GetCurrentThreadId(),
            "List wrapper, MSVC sentinel or node allocation failure must synchronously consume the original reference once");
    }
    const auto stats=owner->Drain();
    owner->Stop();
    Require(stats.accepted==0 && stats.completed==0 && stats.fallback==3 &&
        Observed::logs.size()==logs && Observed::events.size()==events,
        "Failed staging must leave no owner work, leaks or disabled diagnostic work");
    Observed::enabled=true;
    Magpie::g_nrReleaseDiagnostics=true;
}

static void BoundedUploadRetentionAndLifecycle() {
    constexpr uint64_t actualSdkWidth=113907200, expectedCharge=113967104;
    constexpr uint64_t limit=Magpie::NR_UPLOAD_RETENTION_LIMIT_BYTES;
    Observed::retainedSeen=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    Counts first,second,excess,laterCounts;
    excess.entered=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    auto owner=Magpie::CreateNrResourceReleaseOwner();
    Require(bool(owner) && Observed::retainedSeen && excess.entered,"Cannot create upload retention fixture");
    auto fence=MakeFence(0);
    fence->waitEntered=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    int firstFeature=0,secondFeature=0;
    const auto firstLog=Observed::logs.size();
    Magpie::NrResourceReleaseBatch firstBatch(owner.get()),secondBatch(owner.get());
    auto firstProperties=UploadBufferProperties(actualSdkWidth);
    firstProperties.heapFlags=static_cast<D3D12_HEAP_FLAGS>(0xc0);
    {
        Magpie::NrResourceReleaseScope call(&firstFeature,1501,2,"evaluate",&firstBatch,true);
        Magpie::NrResourceReleaseCallback(new Resource12(first,firstProperties));
    }
    Require(firstBatch.Submit(fence.get(),12),"Cannot publish first feature's upload candidate");
    {
        Magpie::NrResourceReleaseScope call(&secondFeature,1502,1,"evaluate",&secondBatch,true);
        Magpie::NrResourceReleaseCallback(new Resource12(second,UploadBufferProperties(limit-expectedCharge)));
        Magpie::NrResourceReleaseCallback(new Resource12(excess,UploadBufferProperties(1)));
    }
    Require(secondBatch.Submit(fence.get(),12),"Cannot publish second feature's upload candidates");
    const bool waitedForGpu=WaitForSingleObject(fence->waitEntered,1000)==WAIT_OBJECT_0 &&
        first.releases==0 && second.releases==0 && excess.releases==0;
    fence->Signal(12);
    // Processing this over-budget resource proves both earlier resources
    // have passed inspection and share the same runtime budget.
    const bool excessReleased=WaitForSingleObject(excess.entered,1000)==WAIT_OBJECT_0;
    const bool originalsRetained=first.addRefs==1 && first.releases==1 && !first.destroyed &&
        second.addRefs==1 && second.releases==1 && !second.destroyed;
    Observed::Log identity{},retention{};
    {
        std::lock_guard lock(Observed::mutex);
        for (size_t i=firstLog;i<Observed::logs.size();++i) {
            const auto& log=Observed::logs[i];
            if (log.kind==Observed::Log::Kind::Identity && log.frame==1501) identity=log;
            if (log.kind==Observed::Log::Kind::Retained) retention=log;
        }
    }
    auto stats=owner->Drain();
    Require(waitedForGpu && excessReleased && originalsRetained && excess.destroyed &&
        excess.addRefs==1 && excess.releases==2,
        "GPU retirement and a shared 128MiB budget must precede retaining or releasing the original reference");
    Require(identity.kind==Observed::Log::Kind::Identity && identity.frame==1501 && identity.pass==2 &&
        identity.identityHr==S_OK && identity.heapHr==S_OK && identity.width==actualSdkWidth && identity.height==1 &&
        identity.dimension==D3D12_RESOURCE_DIMENSION_BUFFER && identity.format==DXGI_FORMAT_UNKNOWN &&
        identity.heapType==D3D12_HEAP_TYPE_UPLOAD && identity.resourceFlags==0 && identity.heapFlags==0xc0 &&
        identity.resourceBytes==expectedCharge && retention.retained==2 && retention.released==0 &&
        retention.retainedBytes==limit && retention.limitBytes==limit,
        "Aligned bytes, resource identity and current retention must report the actual budget without claiming Release");
    Require(stats.accepted==3 && stats.completed==3 && stats.retained==0 && stats.retainedBytes==0 &&
        stats.retainedTotal==2 && stats.releasedFromRetention==2 && stats.maxRetainedBytes==limit &&
        first.destroyed && first.releases==2 && second.destroyed && second.releases==2,
        "Drain must physically release retained originals and refund their entire shared budget");

    ResetEvent(Observed::retainedSeen);
    Magpie::NrResourceReleaseBatch later(owner.get());
    {
        Magpie::NrResourceReleaseScope call(&firstFeature,1503,1,"evaluate",&later,true);
        Magpie::NrResourceReleaseCallback(new Resource12(laterCounts,UploadBufferProperties(limit)));
    }
    Require(later.Submit(fence.get(),12),"Cannot publish a resource after a prior drain");
    const bool retainedAgain=WaitForSingleObject(Observed::retainedSeen,1000)==WAIT_OBJECT_0;
    const bool stillOriginal=(!laterCounts.destroyed && laterCounts.addRefs==1 && laterCounts.releases==1);
    owner->Stop();
    stats=owner->Drain();
    Require(retainedAgain && stillOriginal && stats.accepted==4 && stats.completed==4 && stats.retained==0 &&
        stats.retainedBytes==0 && stats.retainedTotal==3 && stats.releasedFromRetention==3 &&
        stats.maxRetainedBytes==limit && laterCounts.destroyed && laterCounts.releases==2,
        "Budget must be reusable after Drain and every remaining original must be released by Stop");
    CloseHandle(excess.entered); CloseHandle(fence->waitEntered);
    CloseHandle(Observed::retainedSeen); Observed::retainedSeen=nullptr;
}

static void IneligibleResourceAndBudgetFailures() {
    constexpr uint64_t limit=Magpie::NR_UPLOAD_RETENTION_LIMIT_BYTES;
    std::vector<ResourceProperties> cases;
    auto properties=TextureProperties(); properties.heap.Type=D3D12_HEAP_TYPE_UPLOAD; cases.push_back(properties);
    properties=UploadBufferProperties(65536); properties.heap.Type=D3D12_HEAP_TYPE_DEFAULT; cases.push_back(properties);
    properties=UploadBufferProperties(65536); properties.qiHr=E_NOINTERFACE; cases.push_back(properties);
    properties=UploadBufferProperties(65536); properties.heapHr=E_FAIL; cases.push_back(properties);
    cases.push_back(UploadBufferProperties(0));
    properties=UploadBufferProperties(65536); properties.desc.Height=2; cases.push_back(properties);
    properties=UploadBufferProperties(65536); properties.desc.DepthOrArraySize=2; cases.push_back(properties);
    properties=UploadBufferProperties(65536); properties.desc.MipLevels=0; cases.push_back(properties);
    properties=UploadBufferProperties(65536); properties.desc.SampleDesc.Count=2; cases.push_back(properties);
    properties=UploadBufferProperties(65536); properties.desc.SampleDesc.Quality=1; cases.push_back(properties);
    properties=UploadBufferProperties(65536); properties.desc.Format=DXGI_FORMAT_R8_UNORM; cases.push_back(properties);
    properties=UploadBufferProperties(65536); properties.desc.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN; cases.push_back(properties);
    cases.push_back(UploadBufferProperties(65536,3));
    cases.push_back(UploadBufferProperties(UINT64_MAX));
    cases.push_back(UploadBufferProperties(limit+1));
    cases.push_back(UploadBufferProperties(1,UINT64_C(1)<<63));
    auto owner=Magpie::CreateNrResourceReleaseOwner();
    Require(bool(owner),"Cannot create resource eligibility fixture");
    auto fence=MakeFence();
    int featureOwner=0;
    for (const auto& value:cases) {
        Counts current;
        current.entered=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        Magpie::NrResourceReleaseBatch batch(owner.get());
        {
            Magpie::NrResourceReleaseScope call(&featureOwner,1601,1,"evaluate",&batch,true);
            Magpie::NrResourceReleaseCallback(new Resource12(current,value));
        }
        Require(batch.Submit(fence.get(),1),"Cannot publish resource eligibility case");
        const bool releasedWithoutDrain=WaitForSingleObject(current.entered,1000)==WAIT_OBJECT_0;
        const auto stats=owner->Drain();
        CloseHandle(current.entered);
        Require(releasedWithoutDrain && stats.retained==0 && stats.retainedBytes==0 && stats.retainedTotal==0 &&
            current.destroyed && current.releases==current.addRefs+1 && current.addRefs==(FAILED(value.qiHr)?0u:1u),
            "Unknown, failed, non-upload, malformed, overflowing or over-budget resources must use fenced Release");
    }
    owner->Stop();
}

static void NormalTraceOffRetainsUploads() {
    Observed::enabled=false;
    Magpie::g_nrReleaseDiagnostics=false;
    const auto firstLog=Observed::logs.size(),firstEvent=Observed::events.size();
    Counts upload,barrier;
    barrier.entered=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    auto owner=Magpie::CreateNrResourceReleaseOwner();
    Require(bool(owner),"Cannot create ordinary-mode upload retention fixture");
    auto fence=MakeFence();
    int featureOwner=0;
    Magpie::NrResourceReleaseBatch batch(owner.get());
    {
        Magpie::NrResourceReleaseScope call(&featureOwner,1701,1,"evaluate",&batch,true);
        Magpie::NrResourceReleaseCallback(new Resource12(upload,UploadBufferProperties(1)));
        Magpie::NrResourceReleaseCallback(new Resource(barrier));
    }
    Require(batch.Submit(fence.get(),1),"Cannot publish ordinary-mode retention fixture");
    const bool processed=WaitForSingleObject(barrier.entered,1000)==WAIT_OBJECT_0;
    const bool retained=upload.addRefs==1 && upload.releases==1 && !upload.destroyed;
    const auto stats=owner->Drain();
    owner->Stop();
    CloseHandle(barrier.entered);
    Require(processed && retained && stats.accepted==2 && stats.completed==2 && stats.retained==0 &&
        stats.retainedBytes==0 && stats.retainedTotal==1 && stats.releasedFromRetention==1 &&
        stats.maxRetainedBytes==65536 && upload.destroyed && upload.releases==2 &&
        barrier.destroyed && barrier.addRefs==0 && barrier.releases==1 && Observed::events.size()==firstEvent &&
        Observed::logs.size()==firstLog+1 && Observed::logs.back().kind==Observed::Log::Kind::Retained &&
        Observed::logs.back().retainedBytes==65536,
        "The bounded policy must work by default without trace or fine stages, with an explicit retention receipt");
    Observed::enabled=true;
    Magpie::g_nrReleaseDiagnostics=true;
}

static void DrainPrefixWithConcurrentFeatureSubmission() {
    Counts retained,beforeBarrier,afterBarrier;
    retained.entered=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    retained.permit=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    beforeBarrier.entered=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    beforeBarrier.permit=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    afterBarrier.entered=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    HANDLE captured=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    HANDLE drained=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    auto owner=Magpie::CreateNrResourceReleaseOwner();
    Require(bool(owner) && retained.entered && retained.permit && beforeBarrier.entered && beforeBarrier.permit &&
        afterBarrier.entered && captured && drained,"Cannot create mixed Drain barrier fixture");
    auto fence=MakeFence();
    int firstFeature=0,otherFeature=0;
    Magpie::NrResourceReleaseBatch original(owner.get());
    {
        Magpie::NrResourceReleaseScope call(&firstFeature,1801,1,"evaluate",&original,true);
        Magpie::NrResourceReleaseCallback(new Resource12(retained,UploadBufferProperties(65536)));
        Magpie::NrResourceReleaseCallback(new Resource(beforeBarrier));
    }
    const bool firstSubmitted=original.Submit(fence.get(),1);
    const bool beforeEntered=WaitForSingleObject(beforeBarrier.entered,1000)==WAIT_OBJECT_0;
    Observed::drainTarget=0;
    Observed::drainCaptured=captured;
    Magpie::NrResourceReleaseOwner::Statistics drainStats{};
    std::thread drain([&] { drainStats=owner->Drain(); SetEvent(drained); });
    const bool ticketCaptured=WaitForSingleObject(captured,1000)==WAIT_OBJECT_0;
    const auto ticket=Observed::drainTarget.load();
    Observed::drainCaptured=nullptr;
    Magpie::NrResourceReleaseBatch later(owner.get());
    {
        // The observer fired inside real Drain's mutex. Both the callback's
        // Accepting check and Submit now use that same mutex after its unlock.
        Magpie::NrResourceReleaseScope call(&otherFeature,1802,1,"evaluate",&later,true);
        Magpie::NrResourceReleaseCallback(new Resource(afterBarrier));
    }
    const bool laterSubmitted=later.Submit(fence.get(),1);
    SetEvent(beforeBarrier.permit);
    const bool retainedEntered=WaitForSingleObject(retained.entered,1000)==WAIT_OBJECT_0;
    const bool newerStillQueued=WaitForSingleObject(afterBarrier.entered,0)==WAIT_TIMEOUT;
    const bool drainStillWaiting=WaitForSingleObject(drained,0)==WAIT_TIMEOUT;
    SetEvent(retained.permit);
    drain.join();
    owner->Stop();
    const auto finalStats=owner->Drain();
    for (HANDLE event : {retained.entered,retained.permit,beforeBarrier.entered,beforeBarrier.permit,
        afterBarrier.entered,captured,drained}) CloseHandle(event);
    Require(firstSubmitted && beforeEntered && ticketCaptured && ticket==2 && laterSubmitted &&
        retainedEntered && newerStillQueued && drainStillWaiting && drainStats.completed>=2 &&
        finalStats.accepted==3 && finalStats.completed==3 && finalStats.retainedBytes==0 &&
        finalStats.retainedTotal==1 && finalStats.releasedFromRetention==1 &&
        retained.destroyed && retained.addRefs==1 && retained.releases==2 && retained.permitSucceeded &&
        beforeBarrier.destroyed && beforeBarrier.releases==1 && beforeBarrier.permitSucceeded &&
        afterBarrier.destroyed && afterBarrier.releases==1,
        "A later feature's completed Release must never substitute for a retained reference in Drain's captured prefix");
}

int main() try {
    Magpie::g_nrReleaseDiagnostics = true;
    Gate(); DirectReleaseAndAttribution(); NestedAndOtherThread();
    OriginalReferenceAndGuardIndependence(); StopAndFailureFallback();
    FactoryFailureAndDefaultPolicy();
    ParameterWrapperNeverQueues();
    SubmittedTicketRetiresBeforeRelease(); UnsubmittedAndWaitFailure();
    EmptyBatchAndStagingAllocationFailure();
    BoundedUploadRetentionAndLifecycle(); IneligibleResourceAndBudgetFailures(); NormalTraceOffRetainsUploads();
    DrainPrefixWithConcurrentFeatureSubmission();
    Counts counts;
    Observed::throwLog = true;
    Magpie::NrResourceReleaseCallback(new Resource(counts));
    Require(counts.releases == 1 && counts.destroyed, "Logging failure must not skip or repeat the real Release");
    Observed::throwLog = false;
    Magpie::NrResourceReleaseCallback(nullptr);
    Require(Observed::logs.back().resource == 0, "Null callback input must be represented explicitly");
    std::puts("NR release owner: original-reference transfer, GPU retirement, lazy/OOM fallback, bounded upload retention, QI identity, trace-off default policy, lifecycle drain/stop, guard and parameter contracts passed.");
    return 0;
} catch (const std::exception& error) {
    std::fprintf(stderr,"NR release probe failed: %s\n",error.what()); return 1;
}
