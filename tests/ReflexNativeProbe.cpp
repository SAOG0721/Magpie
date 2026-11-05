// Small native diagnostic. Run each stage/sequence in a fresh process: the
// managed community proxy intentionally stays loaded until process exit.
#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <psapi.h>
#include <nvapi.h>
#include <nvapi_interface.h>
#include <nvsdk_ngx.h>
#include "DlssgSm86Package.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {
struct Options {
    std::wstring stage = L"plain", sequence = L"split";
    fs::path community;
    unsigned frames = 120, interval = 16667;
    bool visible = false, renderPresentIdZero = false;
};

void Require(bool condition, const char* text) {
    if (!condition) throw std::runtime_error(text);
}
void Check(HRESULT result, const char* operation) {
    if (FAILED(result)) {
        std::printf("HRESULT operation=%s result=0x%08lx\n", operation, result);
        throw std::runtime_error(operation);
    }
}
fs::path ModulePath(HMODULE module) {
    std::wstring text(32768, L'\0');
    const auto size = GetModuleFileNameW(module, text.data(), static_cast<DWORD>(text.size()));
    Require(size && size < text.size(), "GetModuleFileNameW");
    text.resize(size);
    return text;
}
void Pointer(const char* stage, const char* name, const void* pointer) {
    HMODULE module{};
    const bool found = pointer && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(pointer), &module);
    std::printf("POINTER stage=%s name=%s address=%p module=%ls bytes=", stage, name,
        pointer, found ? ModulePath(module).c_str() : L"unknown");
    // Copy only readable bytes in this process; never follow/patch a trampoline.
    std::array<unsigned char, 16> bytes{};
    SIZE_T read{};
    if (pointer && ReadProcessMemory(GetCurrentProcess(), pointer, bytes.data(), bytes.size(), &read))
        for (size_t i = 0; i < read; ++i) std::printf("%02x", bytes[i]);
    std::puts("");
}
void Modules(const char* stage) {
    std::array<HMODULE, 2048> modules{};
    DWORD size{};
    Require(K32EnumProcessModules(GetCurrentProcess(), modules.data(), sizeof(modules), &size) &&
        size <= sizeof(modules), "EnumProcessModules");
    for (size_t i = 0; i < size / sizeof(HMODULE); ++i) {
        const auto path = ModulePath(modules[i]);
        auto name = path.filename().wstring();
        std::transform(name.begin(), name.end(), name.begin(), towlower);
        if (name.find(L"rtss") != name.npos || name.find(L"nvapi") != name.npos ||
            name.find(L"ngx") != name.npos || name.find(L"sm86") != name.npos ||
            GetProcAddress(modules[i], "DlssgProxy_Name"))
            std::printf("MODULE stage=%s path=%ls\n", stage, path.c_str());
    }
}

struct NvApi {
    using Query = void* (__cdecl*)(unsigned);
    HMODULE module{};
    Query query{};
    decltype(&NvAPI_Initialize) init{};
    decltype(&NvAPI_Unload) unload{};
    decltype(&NvAPI_D3D_SetSleepMode) configure{};
    decltype(&NvAPI_D3D_GetSleepStatus) state{};
    decltype(&NvAPI_D3D_Sleep) sleep{};
    decltype(&NvAPI_D3D_SetLatencyMarker) marker{};
    decltype(&NvAPI_D3D11_SetAsyncFrameMarker) async{};
    decltype(&NvAPI_SYS_GetDriverAndBranchVersion) driver{};
    template<class T> void Resolve(T& function, const char* name) {
        for (const auto& entry : nvapi_interface_table) if (!std::strcmp(entry.func, name)) {
            function = reinterpret_cast<T>(query(entry.id));
            Require(function != nullptr, name);
            return;
        }
        throw std::runtime_error(name);
    }
    void Initialize() {
        module = LoadLibraryExW(L"nvapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        Require(module != nullptr, "Load system nvapi64.dll");
        query = reinterpret_cast<Query>(GetProcAddress(module, "nvapi_QueryInterface"));
        Require(query != nullptr, "nvapi_QueryInterface");
        Resolve(init, "NvAPI_Initialize"); Resolve(unload, "NvAPI_Unload");
        Resolve(configure, "NvAPI_D3D_SetSleepMode"); Resolve(state, "NvAPI_D3D_GetSleepStatus");
        Resolve(sleep, "NvAPI_D3D_Sleep"); Resolve(marker, "NvAPI_D3D_SetLatencyMarker");
        Resolve(async, "NvAPI_D3D11_SetAsyncFrameMarker");
        Resolve(driver, "NvAPI_SYS_GetDriverAndBranchVersion");
        const auto result = init();
        std::printf("INITIALIZE status=%d\n", result);
        Require(result == NVAPI_OK, "NvAPI_Initialize");
        NvU32 version{}; NvAPI_ShortString branch{};
        std::printf("DRIVER status=%d ", driver(&version, branch));
        std::printf("version=%u branch=%s\n", version, branch);
    }
    void Pointers(const char* stage) {
        Pointer(stage, "query", reinterpret_cast<const void*>(query));
        Pointer(stage, "sleep", reinterpret_cast<const void*>(sleep));
        Pointer(stage, "configure", reinterpret_cast<const void*>(configure));
        Pointer(stage, "marker", reinterpret_cast<const void*>(marker));
        Pointer(stage, "async", reinterpret_cast<const void*>(async));
        // Production caches pointers before NGX preparation. Also show what a
        // later QueryInterface returns without replacing those cached pointers.
        decltype(async) later{}; Resolve(later, "NvAPI_D3D11_SetAsyncFrameMarker");
        Pointer(stage, "async-requery", reinterpret_cast<const void*>(later));
    }
    int Configure(ID3D11Device* device, bool enabled, unsigned interval) {
        NV_SET_SLEEP_MODE_PARAMS parameters{};
        parameters.version = NV_SET_SLEEP_MODE_PARAMS_VER;
        parameters.bLowLatencyMode = enabled;
        parameters.minimumIntervalUs = interval;
        const int result = configure(device, &parameters);
        NV_GET_SLEEP_STATUS_PARAMS actual{}; actual.version = NV_GET_SLEEP_STATUS_PARAMS_VER;
        const int queried = state(device, &actual);
        std::printf("CONFIGURE on=%u intervalUs=%u status=%d query=%d actualOn=%u\n",
            enabled, interval, result, queried, actual.bLowLatencyMode);
        return result;
    }
    void State(ID3D11Device* device, unsigned frame) {
        NV_GET_SLEEP_STATUS_PARAMS actual{}; actual.version = NV_GET_SLEEP_STATUS_PARAMS_VER;
        const int result = state(device, &actual);
        std::printf("STATE frame=%u status=%d actualOn=%u\n", frame, result, actual.bLowLatencyMode);
    }
};

// Load only the already prepared, hash-pinned managed cache. This follows the
// production proxy -> private missing runtime request without changing its INI.
// All file handles/modules intentionally stay alive until this process exits.
fs::path LoadCommunity(const fs::path& root) {
    namespace Package = Magpie::DlssgSm86Package;
    for (const auto& file : Package::Files) {
        const HANDLE handle = Magpie::PinnedFile::Open(root / file.name);
        Require(Magpie::PinnedFile::Verify(handle, file.size, file.sha256), "Managed package identity");
    }
    const auto config = root / L"dlssg_sm86.ini";
    Require(Magpie::PinnedFile::Verify(config, Package::Files[1].size, Package::Files[1].sha256) ||
        Magpie::PinnedFile::Verify(config, Package::Files[2].size, Package::Files[2].sha256), "Managed config identity");
    const auto proxy = LoadLibraryExW((root / L"version.dll").c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    Require(proxy && GetProcAddress(proxy, "DlssgProxy_Name"), "Managed proxy load");
    const auto request = fs::current_path() / (L"reflex-request-" + std::to_wstring(GetCurrentProcessId()) +
        L"-" + std::to_wstring(GetTickCount64()));
    Require(CreateDirectoryW(request.c_str(), nullptr), "Private runtime request directory");
    const auto runtime = LoadLibraryExW((request / L"nvngx_dlssg.dll").c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    RemoveDirectoryW(request.c_str()); // Only the empty directory just created.
    Require(runtime != nullptr, "Managed runtime request");
    const auto expected = (root / Package::Runtime.name).lexically_normal();
    Require(!_wcsicmp(ModulePath(runtime).c_str(), expected.c_str()), "Managed runtime mapping");
    std::printf("COMMUNITY runtime=%ls\n", expected.c_str());
    return expected.parent_path();
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    return DefWindowProcW(window, message, wparam, lparam);
}
struct Device {
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain1> chain;
    ComPtr<ID3D11RenderTargetView> target;
    HWND window{};
    void Initialize(bool visible) {
        ComPtr<IDXGIFactory2> factory;
        Check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");
        for (unsigned i = 0;; ++i) {
            Check(factory->EnumAdapters1(i, &adapter), "EnumAdapters1");
            DXGI_ADAPTER_DESC1 desc{}; Check(adapter->GetDesc1(&desc), "GetDesc1");
            if (desc.VendorId == 0x10de) {
                std::printf("ADAPTER name=%ls luid=%08lx:%08lx\n", desc.Description,
                    desc.AdapterLuid.HighPart, desc.AdapterLuid.LowPart);
                break;
            }
            adapter.Reset();
        }
        const D3D_FEATURE_LEVEL levels[]{ D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
        Check(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            levels, 2, D3D11_SDK_VERSION, &device, nullptr, &context), "D3D11CreateDevice");
        WNDCLASSW cls{}; cls.lpfnWndProc = WindowProc; cls.hInstance = GetModuleHandleW(nullptr);
        cls.lpszClassName = L"MagpieReflexNativeProbe";
        Require(RegisterClassW(&cls), "RegisterClassW");
        window = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, cls.lpszClassName,
            L"Magpie Reflex diagnostic", WS_OVERLAPPEDWINDOW, 32, 32, 256, 160,
            nullptr, nullptr, cls.hInstance, nullptr);
        Require(window != nullptr, "CreateWindowExW");
        DXGI_SWAP_CHAIN_DESC1 desc{}; desc.Width = 128; desc.Height = 72;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; desc.BufferCount = 2;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        Check(factory->CreateSwapChainForHwnd(device.Get(), window, &desc, nullptr, nullptr, &chain),
            "CreateSwapChainForHwnd");
        ComPtr<IDXGISwapChain2> chain2; Check(chain.As(&chain2), "IDXGISwapChain2");
        Check(chain2->SetMaximumFrameLatency(1), "SetMaximumFrameLatency");
        ComPtr<ID3D11Texture2D> backbuffer; Check(chain->GetBuffer(0, IID_PPV_ARGS(&backbuffer)), "GetBuffer");
        Check(device->CreateRenderTargetView(backbuffer.Get(), nullptr, &target), "CreateRenderTargetView");
        if (visible) ShowWindow(window, SW_SHOWNOACTIVATE);
    }
    ~Device() {
        if (context) { context->ClearState(); context->Flush(); }
        target.Reset(); chain.Reset(); context.Reset(); device.Reset();
        if (window) DestroyWindow(window);
    }
};
struct Ngx {
    ComPtr<ID3D12Device> device;
    bool initialized = false;
    void Initialize(IDXGIAdapter* adapter, const fs::path& featureDirectory) {
        Check(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "D3D12CreateDevice");
        const auto data = fs::current_path().wstring();
        const auto feature = featureDirectory.wstring();
        const wchar_t* paths[]{ feature.empty() ? data.c_str() : feature.c_str(), data.c_str() };
        NVSDK_NGX_FeatureCommonInfo info{};
        info.PathListInfo = { paths, feature.empty() ? 1u : 2u };
        const auto result = NVSDK_NGX_D3D12_Init_with_ProjectID("7c134ab9-9677-4af5-a2b2-bca943350861",
            NVSDK_NGX_ENGINE_TYPE_CUSTOM, "Magpie-Experimental-0.5.7", data.c_str(), device.Get(),
            &info, NVSDK_NGX_Version_API);
        std::printf("NGX initialize=0x%x community=%u\n", result, !feature.empty());
        initialized = NVSDK_NGX_SUCCEED(result);
        Require(initialized, "NGX initialization");
    }
    ~Ngx() { if (initialized) std::printf("NGX shutdown=0x%x\n", NVSDK_NGX_D3D12_Shutdown1(device.Get())); }
};

struct Stat { unsigned count = 0, failed = 0; int first = 0, last = 0; };
std::map<std::string, Stat> stats;
void Record(const char* operation, int status, uint64_t frame) {
    auto& stat = stats[operation];
    if (!stat.count || status != stat.last || (status && stat.failed < 3))
        std::printf("CALL frame=%llu operation=%s status=%d\n", frame, operation, status);
    if (!stat.count) stat.first = status;
    ++stat.count; stat.failed += status != 0; stat.last = status;
}
int Run(const Options& options) {
    std::printf("RUN stage=%ls sequence=%ls frames=%u intervalUs=%u visible=%u renderPresentIdZero=%u pid=%lu\n",
        options.stage.c_str(), options.sequence.c_str(), options.frames, options.interval,
        options.visible, options.renderPresentIdZero, GetCurrentProcessId());
    NvApi api; api.Initialize(); api.Pointers("initial");
    Device render; render.Initialize(options.visible); Modules("device");
    const auto reset = [&] { api.Configure(render.device.Get(), false, 0); };
    struct Cleanup { decltype(reset)& action; ~Cleanup() { action(); } } cleanup{ reset };
    Require(!api.Configure(render.device.Get(), true, 0), "Initial SetSleepMode");
    Require(!api.Configure(render.device.Get(), true, options.interval), "SetSleepMode interval");
    fs::path featureDirectory;
    if (options.stage == L"proxy" || options.stage == L"community-ngx")
        featureDirectory = LoadCommunity(options.community);
    Ngx ngx;
    if (options.stage == L"ngx" || options.stage == L"community-ngx")
        ngx.Initialize(render.adapter.Get(), featureDirectory);
    api.Pointers("prepared"); Modules("prepared");
    std::vector<double> intervals;
    Clock::time_point previous{};
    const auto regular = [&](NV_LATENCY_MARKER_TYPE type, const char* name, uint64_t frame) {
        NV_LATENCY_MARKER_PARAMS parameters{};
        parameters.version = NV_LATENCY_MARKER_PARAMS_VER; parameters.frameID = frame; parameters.markerType = type;
        Record(name, api.marker(render.device.Get(), &parameters), frame);
    };
    const auto async = [&](NV_LATENCY_MARKER_TYPE type, const char* name, uint64_t frame) {
        NV_ASYNC_FRAME_MARKER_PARAMS parameters{};
        parameters.version = NV_ASYNC_FRAME_MARKER_PARAMS_VER;
        parameters.frameID = frame; parameters.presentFrameID = frame; parameters.markerType = type;
        if (options.renderPresentIdZero && (type == OUT_OF_BAND_RENDERSUBMIT_START ||
            type == OUT_OF_BAND_RENDERSUBMIT_END)) parameters.presentFrameID = 0;
        Record(name, api.async(render.device.Get(), &parameters), frame);
    };
    for (uint64_t frame = 1; frame <= options.frames; ++frame) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
        Record("Sleep", api.sleep(render.device.Get()), frame);
        const auto now = Clock::now();
        if (frame > 16) intervals.push_back(std::chrono::duration<double, std::milli>(now - previous).count());
        previous = now;
        regular(SIMULATION_START, "SimulationStart", frame);
        regular(SIMULATION_END, "SimulationEnd", frame);
        regular(RENDERSUBMIT_START, "RenderStart", frame);
        if (options.sequence != L"standard") regular(RENDERSUBMIT_END, "RenderEnd", frame);
        if (options.sequence == L"split") async(OUT_OF_BAND_RENDERSUBMIT_START, "AsyncRenderStart", frame);
        const float color[]{ .05f, .08f, .12f, 1.f };
        render.context->ClearRenderTargetView(render.target.Get(), color);
        if (options.sequence == L"split") async(OUT_OF_BAND_RENDERSUBMIT_END, "AsyncRenderEnd", frame);
        if (options.sequence == L"standard") {
            regular(RENDERSUBMIT_END, "RenderEnd", frame);
            regular(PRESENT_START, "PresentStart", frame);
        } else async(PRESENT_START, "AsyncPresentStart", frame);
        Record("DXGIPresent", render.chain->Present(0, 0), frame);
        if (options.sequence == L"standard") regular(PRESENT_END, "PresentEnd", frame);
        else async(PRESENT_END, "AsyncPresentEnd", frame);
        if (frame == 1 || frame == 16 || frame == options.frames) api.State(render.device.Get(), static_cast<unsigned>(frame));
    }
    api.Pointers("completed"); Modules("completed");
    bool failed = false;
    for (const auto& [name, stat] : stats) {
        std::printf("SUMMARY operation=%s count=%u failed=%u first=%d last=%d\n",
            name.c_str(), stat.count, stat.failed, stat.first, stat.last);
        failed |= stat.failed != 0;
    }
    if (!intervals.empty()) {
        std::sort(intervals.begin(), intervals.end());
        std::printf("CPU_INTERVAL samples=%zu minMs=%.4f p50Ms=%.4f p95Ms=%.4f maxMs=%.4f\n",
            intervals.size(), intervals.front(), intervals[intervals.size() / 2],
            intervals[intervals.size() * 95 / 100], intervals.back());
    }
    // A nonzero diagnostic result preserves real API failures. This process
    // never converts an unsupported/failed marker into a successful call.
    return failed ? 2 : 0;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        Options options;
        for (int i = 1; i < argc; ++i) {
            const std::wstring argument = argv[i];
            if (argument == L"--visible") options.visible = true;
            else if (argument == L"--render-present-id-zero") options.renderPresentIdZero = true;
            else {
                Require(i + 1 < argc, "Missing argument value");
                const std::wstring value = argv[++i];
                if (argument == L"--stage") options.stage = value;
                else if (argument == L"--sequence") options.sequence = value;
                else if (argument == L"--community-root") options.community = fs::absolute(value);
                else if (argument == L"--frames") options.frames = std::stoul(value);
                else if (argument == L"--interval-us") options.interval = std::stoul(value);
                else throw std::runtime_error("Unknown argument");
            }
        }
        Require(options.stage == L"plain" || options.stage == L"ngx" || options.stage == L"proxy" ||
            options.stage == L"community-ngx", "Unknown stage");
        Require(options.sequence == L"split" || options.sequence == L"standard" ||
            options.sequence == L"async-present", "Unknown sequence");
        Require(options.frames >= 1 && options.frames <= 600, "Frame range 1..600");
        if (options.stage == L"proxy" || options.stage == L"community-ngx")
            Require(!options.community.empty(), "Managed cache directory is required");
        return Run(options);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "ERROR %s win32=%lu\n", error.what(), GetLastError());
        return 1;
    }
}
