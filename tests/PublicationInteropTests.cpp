// Real D3D11/12 resource ordering, without Magpie, NGX, a swap chain or a window.
// This tests the proposed publication protocol; it does not measure game FPS.
#include <windows.h>
#include <d3d11_4.h>
#include <d3d11sdklayers.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace std::chrono_literals;

static void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

static void Check(HRESULT result, const char* operation) {
    if (FAILED(result)) {
        char message[256]{};
        std::snprintf(message, sizeof(message), "%s failed: 0x%08lx", operation,
            static_cast<unsigned long>(result));
        throw std::runtime_error(message);
    }
}

struct Handle {
    HANDLE value = nullptr;
    Handle() = default;
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() { if (value) CloseHandle(value); }
};

static void Wait(ID3D12Fence* fence, uint64_t value) {
    const auto completed = fence->GetCompletedValue();
    Require(completed != UINT64_MAX, "D3D12 device removed while checking a fence");
    if (completed >= value) return;
    Handle event;
    event.value = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Require(event.value != nullptr, "CreateEvent failed");
    Check(fence->SetEventOnCompletion(value, event.value), "D3D12 SetEventOnCompletion");
    Require(WaitForSingleObject(event.value, 5000) == WAIT_OBJECT_0, "D3D12 fence timed out");
    Require(fence->GetCompletedValue() != UINT64_MAX && fence->GetCompletedValue() >= value,
        "D3D12 fence woke without completion");
}

static void Wait(ID3D11Fence* fence, uint64_t value) {
    const auto completed = fence->GetCompletedValue();
    Require(completed != UINT64_MAX, "D3D11 device removed while checking a fence");
    if (completed >= value) return;
    Handle event;
    event.value = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Require(event.value != nullptr, "CreateEvent failed");
    Check(fence->SetEventOnCompletion(value, event.value), "D3D11 SetEventOnCompletion");
    Require(WaitForSingleObject(event.value, 5000) == WAIT_OBJECT_0, "D3D11 fence timed out");
    Require(fence->GetCompletedValue() != UINT64_MAX && fence->GetCompletedValue() >= value,
        "D3D11 fence woke without completion");
}

struct Devices {
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D11Device5> backend, frontend;
    ComPtr<ID3D11DeviceContext4> producer, consumer;
    ComPtr<ID3D12Device> device12;
    ComPtr<ID3D12CommandQueue> queue12;
    bool debug11 = false, debug12 = false;

    Devices() {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
            debug->EnableDebugLayer();
            debug12 = true;
        }
        ComPtr<IDXGIFactory4> factory;
        Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");
        for (UINT index = 0;; ++index) {
            ComPtr<IDXGIAdapter1> candidate;
            const auto result = factory->EnumAdapters1(index, &candidate);
            if (result == DXGI_ERROR_NOT_FOUND) break;
            Check(result, "EnumAdapters1");
            DXGI_ADAPTER_DESC1 description{};
            Check(candidate->GetDesc1(&description), "GetDesc1");
            if (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
            if (SUCCEEDED(D3D12CreateDevice(candidate.Get(), D3D_FEATURE_LEVEL_11_0,
                    IID_PPV_ARGS(&device12)))) {
                adapter = candidate;
                std::printf("Hardware adapter vendor=%04x device=%04x LUID=%08lx:%08lx debug12=%u\n",
                    description.VendorId, description.DeviceId,
                    static_cast<unsigned long>(description.AdapterLuid.HighPart),
                    static_cast<unsigned long>(description.AdapterLuid.LowPart), debug12 ? 1u : 0u);
                break;
            }
        }
        Require(adapter != nullptr, "No hardware adapter supports D3D12; no software fallback");
        Create11(backend, producer);
        Create11(frontend, consumer);
        D3D12_COMMAND_QUEUE_DESC queue{};
        queue.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        Check(device12->CreateCommandQueue(&queue, IID_PPV_ARGS(&queue12)), "CreateCommandQueue");
    }

    void Create11(ComPtr<ID3D11Device5>& device, ComPtr<ID3D11DeviceContext4>& context) {
        ComPtr<ID3D11Device> baseDevice;
        ComPtr<ID3D11DeviceContext> baseContext;
        auto result = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
            D3D11_CREATE_DEVICE_DEBUG, nullptr, 0, D3D11_SDK_VERSION,
            &baseDevice, nullptr, &baseContext);
        if (result == DXGI_ERROR_SDK_COMPONENT_MISSING) {
            result = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                0, nullptr, 0, D3D11_SDK_VERSION, &baseDevice, nullptr, &baseContext);
        } else if (SUCCEEDED(result)) {
            debug11 = true;
        }
        Check(result, "D3D11CreateDevice on same hardware adapter");
        Check(baseDevice.As(&device), "ID3D11Device5");
        Check(baseContext.As(&context), "ID3D11DeviceContext4");
    }

    void CheckErrors() {
        Check(backend->GetDeviceRemovedReason(), "Backend device health");
        Check(frontend->GetDeviceRemovedReason(), "Frontend device health");
        Check(device12->GetDeviceRemovedReason(), "D3D12 device health");
        for (auto* device : {backend.Get(), frontend.Get()}) {
            ComPtr<ID3D11InfoQueue> info;
            if (FAILED(device->QueryInterface(IID_PPV_ARGS(&info)))) continue;
            for (UINT64 index = 0; index < info->GetNumStoredMessagesAllowedByRetrievalFilter(); ++index) {
                SIZE_T size = 0;
                Check(info->GetMessage(index, nullptr, &size), "D3D11 message length");
                std::vector<uint8_t> storage(size);
                auto* message = reinterpret_cast<D3D11_MESSAGE*>(storage.data());
                Check(info->GetMessage(index, message, &size), "D3D11 debug message");
                if (message->Severity == D3D11_MESSAGE_SEVERITY_CORRUPTION ||
                    message->Severity == D3D11_MESSAGE_SEVERITY_ERROR) {
                    throw std::runtime_error(std::string("D3D11 debug error: ") + message->pDescription);
                }
            }
            info->ClearStoredMessages();
        }
        ComPtr<ID3D12InfoQueue> info;
        if (SUCCEEDED(device12.As(&info))) {
            for (UINT64 index = 0; index < info->GetNumStoredMessagesAllowedByRetrievalFilter(); ++index) {
                SIZE_T size = 0;
                Check(info->GetMessage(index, nullptr, &size), "D3D12 message length");
                std::vector<uint8_t> storage(size);
                auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
                Check(info->GetMessage(index, message, &size), "D3D12 debug message");
                if (message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION ||
                    message->Severity == D3D12_MESSAGE_SEVERITY_ERROR) {
                    throw std::runtime_error(std::string("D3D12 debug error: ") + message->pDescription);
                }
            }
            info->ClearStoredMessages();
        }
    }
};

struct SharedFence {
    ComPtr<ID3D11Fence> fence11;
    ComPtr<ID3D12Fence> fence12;
    explicit SharedFence(Devices& devices) {
        Check(devices.backend->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence11)), "Create shared fence");
        Handle shared;
        Check(fence11->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &shared.value), "Create fence NT handle");
        Check(devices.device12->OpenSharedHandle(shared.value, IID_PPV_ARGS(&fence12)), "Open shared fence in D3D12");
    }
};

struct Destination {
    ComPtr<ID3D11Texture2D> backend, frontend, readback;
    ComPtr<IDXGIKeyedMutex> writeMutex, readMutex;
    uint64_t key = 0;
    uint32_t frame = 0;
};

// One producer output is deliberately reused every frame. Destination slots
// protect frontend consumption separately, exactly as the product ring does.
class PublicationRun {
public:
    PublicationRun(Devices& devices, uint32_t slots, uint32_t generation)
        : d(devices), timeline(devices), gate(devices), width(127 + generation * 2),
          height(73 + generation * 2), generation(generation), destinations(slots) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width; desc.Height = height; desc.MipLevels = 1; desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count = 1;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
        Check(d.backend->CreateTexture2D(&desc, nullptr, &output11), "Create shared producer output");
        ComPtr<IDXGIResource1> dxgi;
        Check(output11.As(&dxgi), "Producer IDXGIResource1");
        Handle shared;
        Check(dxgi->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &shared.value), "Share producer output");
        Check(d.device12->OpenSharedHandle(shared.value, IID_PPV_ARGS(&output12)), "Open producer output in D3D12");
        for (auto& slot : destinations) {
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
            Check(d.backend->CreateTexture2D(&desc, nullptr, &slot.backend), "Create bounded publication slot");
            Check(slot.backend.As(&slot.writeMutex), "Backend keyed mutex");
            ComPtr<IDXGIResource> resource;
            Check(slot.backend.As(&resource), "Publication IDXGIResource");
            HANDLE legacy = nullptr;
            Check(resource->GetSharedHandle(&legacy), "Get legacy presentation handle");
            Check(d.frontend->OpenSharedResource(legacy, IID_PPV_ARGS(&slot.frontend)), "Open frontend slot");
            Check(slot.frontend.As(&slot.readMutex), "Frontend keyed mutex");
            desc.BindFlags = 0; desc.MiscFlags = 0;
            desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            Check(d.frontend->CreateTexture2D(&desc, nullptr, &slot.readback), "Create frontend readback");
            desc.Usage = D3D11_USAGE_DEFAULT; desc.CPUAccessFlags = 0;
        }
        Check(d.backend->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&drainFence)), "Create lifecycle drain fence");
        D3D12_DESCRIPTOR_HEAP_DESC heap{};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap.NumDescriptors = 1;
        Check(d.device12->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&cpuHeap)), "Create CPU UAV heap");
        heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        Check(d.device12->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&gpuHeap)), "Create GPU UAV heap");
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        d.device12->CreateUnorderedAccessView(output12.Get(), nullptr, &uav, cpuHeap->GetCPUDescriptorHandleForHeapStart());
        d.device12->CreateUnorderedAccessView(output12.Get(), nullptr, &uav, gpuHeap->GetCPUDescriptorHandleForHeapStart());
        for (auto& command : commands) {
            Check(d.device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&command.allocator)), "Create command allocator");
            Check(d.device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, command.allocator.Get(),
                nullptr, IID_PPV_ARGS(&command.list)), "Create command list");
            Check(command.list->Close(), "Close initial command list");
        }
    }

    ~PublicationRun() {
        // Keep an intentionally delayed test from leaving the GPU gate closed
        // if a preceding assertion fails. Normal destruction follows Drain().
        if (!gateReleased.exchange(true)) gate.fence12->Signal(1);
        if (gateWatchdog.joinable()) {
            gateWatchdog.request_stop();
            gateWatchdog.join();
        }
        if (!drained) {
            try { Drain(); } catch (...) { }
        }
    }

    void Run(uint32_t frames) {
        auto ready = Produce(1, 0);
        uint32_t batchBegin = 0;
        for (uint32_t frame = 1; frame <= frames; ++frame) {
            auto& slot = destinations[(frame - 1) % destinations.size()];
            Require(slot.frame == 0, "Destination slot reused before frontend consumption");
            Publish(slot, frame, ready, frame == 1);
            Require(!gateWatchdogFired, "Driver blocked the publishing CPU inside the controlled gate; no async protocol verdict");
            // This is the next input-ready ticket. It is queued AFTER the
            // preceding D3D11 read/copy, before the next D3D12 overwrite.
            const auto inputReady = ++value;
            Check(d.producer->Signal(timeline.fence11.Get(), inputReady), "Signal next NR input-ready");
            d.producer->Flush();
            ready = Produce(frame + 1, inputReady); // Last frame also gets an overwrite sentinel.
            if (frame == 1) {
                std::this_thread::sleep_for(2ms);
                Require(timeline.fence12->GetCompletedValue() < ready,
                    "Overwrite passed the unreleased D3D11 copy gate");
                ReleaseGate();
            }
            if (frame % destinations.size() == 0 || frame == frames) {
                std::this_thread::sleep_for((frame % 3 == 0) ? 7ms : 2ms);
                // Explicit lifecycle drain while the final publication/sentinel
                // may still be outstanding, rather than a wait on every frame.
                if (frame == frames) Drain();
                const auto count = frame - batchBegin;
                for (uint32_t index = 0; index < count; ++index) {
                    auto& pending = destinations[index];
                    Require(pending.frame == batchBegin + index + 1, "Publication FIFO identity changed");
                    Require(Consume(pending) == 0, "Pixels were overwritten by a later producer frame");
                    ++checkedFrames;
                }
                batchBegin = frame;
            }
        }
        Drain();
        Require(checkedFrames == frames, "Not every publication was consumed");
        std::printf("PASS generation=%u slots=%zu frames=%u pixels=%llu publicationCpuWaits=0 allocatorWaits=%u frontendRetries=%u\n",
            generation, destinations.size(), checkedFrames,
            static_cast<unsigned long long>(checkedPixels), allocatorWaits, frontendRetries);
    }

    void NegativeControl() {
        const auto first = Produce(11, 0);
        auto& slot = destinations.front();
        Publish(slot, 11, first, true);
        Require(!gateWatchdogFired, "Driver blocked the negative-control publisher; overwrite sensitivity could not be exercised");
        // Intentionally omit the next-input dependency. The producer finishes
        // frame12 before the held D3D11 copy starts; expected frame11 must fail.
        const auto overwritten = Produce(12, 0);
        Wait(timeline.fence12.Get(), overwritten);
        ReleaseGate();
        d.producer->Flush();
        const auto mismatches = Consume(slot);
        Drain();
        Require(mismatches == uint64_t(width) * height, "Negative control did not detect every stale publication pixel");
        std::printf("PASS negative-control: omitted cross-queue dependency detected %llu overwritten pixels\n",
            static_cast<unsigned long long>(mismatches));
    }

    void Drain() {
        // One backend D3D11 drain, then retire the D3D12 producer. Both are
        // required before releasing or rebuilding shared resources.
        Check(d.producer->Signal(drainFence.Get(), ++drainValue), "Signal lifecycle D3D11 drain");
        d.producer->Flush();
        Wait(drainFence.Get(), drainValue);
        const auto finished = ++value;
        Check(d.queue12->Signal(timeline.fence12.Get(), finished), "Signal lifecycle D3D12 drain");
        Wait(timeline.fence12.Get(), finished);
        drained = true;
    }

private:
    struct Command {
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        uint64_t completion = 0;
    };
    Devices& d;
    SharedFence timeline, gate;
    uint32_t width, height, generation;
    std::vector<Destination> destinations;
    ComPtr<ID3D11Texture2D> output11;
    ComPtr<ID3D12Resource> output12;
    ComPtr<ID3D11Fence> drainFence;
    ComPtr<ID3D12DescriptorHeap> cpuHeap, gpuHeap;
    std::array<Command, 4> commands;
    uint64_t value = 0, drainValue = 0, checkedPixels = 0;
    uint32_t nextCommand = 0, allocatorWaits = 0, frontendRetries = 0, checkedFrames = 0;
    std::atomic<bool> gateReleased = false, gateWatchdogFired = false;
    bool drained = false;
    std::jthread gateWatchdog;

    std::array<uint8_t, 4> Pattern(uint32_t frame, uint32_t quadrant) const {
        return {static_cast<uint8_t>(frame), static_cast<uint8_t>(quadrant * 53),
            static_cast<uint8_t>(generation ^ 0xa5), static_cast<uint8_t>(frame * 17 + quadrant * 29 + generation)};
    }

    uint64_t Produce(uint32_t frame, uint64_t priorCopy) {
        drained = false;
        auto& command = commands[nextCommand++ % commands.size()];
        if (command.completion && timeline.fence12->GetCompletedValue() < command.completion) {
            ++allocatorWaits;
            Wait(timeline.fence12.Get(), command.completion);
        }
        Check(command.allocator->Reset(), "Reset retired allocator");
        Check(command.list->Reset(command.allocator.Get(), nullptr), "Reset command list");
        if (priorCopy) Check(d.queue12->Wait(timeline.fence12.Get(), priorCopy), "D3D12 wait for prior D3D11 publication copy");
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {output12.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
        command.list->ResourceBarrier(1, &barrier);
        ID3D12DescriptorHeap* heaps[]{gpuHeap.Get()};
        command.list->SetDescriptorHeaps(1, heaps);
        for (uint32_t quadrant = 0; quadrant < 4; ++quadrant) {
            const auto pattern = Pattern(frame, quadrant);
            const float color[]{pattern[0] / 255.f, pattern[1] / 255.f, pattern[2] / 255.f, pattern[3] / 255.f};
            const D3D12_RECT rect{quadrant & 1 ? LONG(width / 2) : 0,
                quadrant & 2 ? LONG(height / 2) : 0,
                quadrant & 1 ? LONG(width) : LONG(width / 2),
                quadrant & 2 ? LONG(height) : LONG(height / 2)};
            command.list->ClearUnorderedAccessViewFloat(gpuHeap->GetGPUDescriptorHandleForHeapStart(),
                cpuHeap->GetCPUDescriptorHandleForHeapStart(), output12.Get(), color, 1, &rect);
        }
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        command.list->ResourceBarrier(1, &barrier);
        Check(command.list->Close(), "Close producer commands");
        ID3D12CommandList* lists[]{command.list.Get()};
        d.queue12->ExecuteCommandLists(1, lists);
        command.completion = ++value;
        Check(d.queue12->Signal(timeline.fence12.Get(), command.completion), "Signal NR output-ready");
        return command.completion;
    }

    void Publish(Destination& slot, uint32_t frame, uint64_t ready, bool delayCopy) {
        // AcquireSync timeouts are nonnegative HRESULTs; only S_OK owns it.
        Require(slot.writeMutex->AcquireSync(slot.key, 3000) == S_OK, "Backend slot AcquireSync failed");
        Check(d.producer->Wait(timeline.fence11.Get(), ready), "D3D11 wait for NR output-ready");
        if (delayCopy) {
            // A driver may itself block in ReleaseSync. An independent bounded
            // release makes that a reported fixture failure instead of a hang.
            gateWatchdog = std::jthread([this](std::stop_token stop) {
                const auto deadline = std::chrono::steady_clock::now() + 1s;
                while (!stop.stop_requested() && !gateReleased && std::chrono::steady_clock::now() < deadline)
                    std::this_thread::sleep_for(5ms);
                if (!stop.stop_requested() && !gateReleased.exchange(true)) {
                    gateWatchdogFired = true;
                    gate.fence12->Signal(1);
                }
            });
            Check(d.producer->Wait(gate.fence11.Get(), 1), "Hold D3D11 publication copy");
        }
        d.producer->CopyResource(slot.backend.Get(), output11.Get());
        Check(slot.writeMutex->ReleaseSync(++slot.key), "Release backend publication slot");
        slot.frame = frame;
        d.producer->Flush(); // Submit, but never wait for this publication on the CPU.
    }

    void ReleaseGate() {
        if (!gateReleased.exchange(true)) Check(gate.fence12->Signal(1), "Release controlled copy delay");
        if (gateWatchdog.joinable()) {
            gateWatchdog.request_stop();
            gateWatchdog.join();
        }
    }

    uint64_t Consume(Destination& slot) {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        HRESULT acquired = E_PENDING;
        do {
            acquired = slot.readMutex->AcquireSync(slot.key, 0);
            if (acquired == S_OK) break;
            Require(acquired == static_cast<HRESULT>(WAIT_TIMEOUT), "Frontend keyed mutex failed");
            ++frontendRetries;
            std::this_thread::sleep_for(1ms);
        } while (std::chrono::steady_clock::now() < deadline);
        Require(acquired == S_OK, "Frontend slot retry timed out");
        d.consumer->CopyResource(slot.readback.Get(), slot.frontend.Get());
        Check(slot.readMutex->ReleaseSync(++slot.key), "Release consumed frontend slot");
        d.consumer->Flush();
        D3D11_MAPPED_SUBRESOURCE mapped{};
        Check(d.consumer->Map(slot.readback.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Read frontend pixels");
        uint64_t mismatches = 0;
        for (uint32_t y = 0; y < height; ++y) {
            const auto* row = static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch;
            for (uint32_t x = 0; x < width; ++x) {
                const auto expected = Pattern(slot.frame, (x >= width / 2 ? 1u : 0u) | (y >= height / 2 ? 2u : 0u));
                if (std::memcmp(row + size_t(x) * 4, expected.data(), 4) != 0) ++mismatches;
                ++checkedPixels;
            }
        }
        d.consumer->Unmap(slot.readback.Get(), 0);
        slot.frame = 0;
        return mismatches;
    }
};

int main() try {
    const auto started = std::chrono::steady_clock::now();
    Devices devices;
    {
        PublicationRun negative(devices, 1, 9);
        negative.NegativeControl();
    }
    devices.CheckErrors();
    // Each generation releases and reconstructs the producer shared texture,
    // imported handles, allocator ring and frontend slots after a full drain.
    for (uint32_t generation = 1; generation <= 3; ++generation) {
        for (uint32_t slots : {1u, 2u, 4u}) {
            PublicationRun run(devices, slots, generation);
            run.Run(48);
        }
        devices.CheckErrors();
    }
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    std::printf("Publication interop: 432 ordered frames, 9 drained resource lifecycles, negative-control sensitivity passed; debug11=%u debug12=%u elapsed=%.3fs.\n",
        devices.debug11 ? 1u : 0u, devices.debug12 ? 1u : 0u, elapsed);
    std::puts("Scope: source-overwrite ordering and bounded keyed destinations; no NGX, capture, Present or game-performance claim.");
    return 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "Publication interop FAILED: %s\n", error.what());
    return 1;
}
