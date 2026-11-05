#pragma once
#include <fstream>
#include <iomanip>
#include <map>
#include <numeric>

// Test-only timestamp collector. The generated source copy inserts these
// scopes; no product source or runtime behavior is replaced.
namespace CostBenchmark {
struct Sample {
    uint64_t frame = 0;
    std::string stage, domain;
    uint32_t width = 0, height = 0;
    double milliseconds = 0;
    uint64_t startTick = 0, endTick = 0, frequency = 0;
};
struct GpuTrace {
    struct Event {
        winrt::com_ptr<ID3D11Query> start, end;
        const char* stage = nullptr;
        uint32_t width = 0, height = 0;
    };
    struct Slot {
        winrt::com_ptr<ID3D11Query> disjoint;
        std::array<Event, 64> events;
        size_t count = 0;
        uint64_t frame = 0;
        bool pending = false;
    };
    std::array<Slot, 32> slots;
    Slot* active = nullptr;
    ID3D11DeviceContext* context = nullptr;
    std::vector<Sample> samples;
    uint64_t droppedFrames = 0, droppedEvents = 0, disjointFrames = 0;
    uint64_t currentFrame = 0;
    void Initialize(ID3D11Device* device, ID3D11DeviceContext* dc) {
        context = dc;
        for (auto& slot : slots) {
            D3D11_QUERY_DESC desc{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
            winrt::check_hresult(device->CreateQuery(&desc, slot.disjoint.put()));
            desc.Query = D3D11_QUERY_TIMESTAMP;
            for (auto& event : slot.events) {
                winrt::check_hresult(device->CreateQuery(&desc, event.start.put()));
                winrt::check_hresult(device->CreateQuery(&desc, event.end.put()));
            }
        }
    }
    void Poll(bool flush = false) {
        for (auto& slot : slots) if (slot.pending) {
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT data{};
            const UINT flags=flush?0:D3D11_ASYNC_GETDATA_DONOTFLUSH;
            if (context->GetData(slot.disjoint.get(), &data, sizeof(data), flags) != S_OK) continue;
            if (data.Disjoint || !data.Frequency) { slot.pending = false; ++disjointFrames; continue; }
            bool complete = true;
            std::array<std::pair<uint64_t,uint64_t>, 64> times{};
            for (size_t i = 0; i < slot.count; ++i) {
                complete &= context->GetData(slot.events[i].start.get(), &times[i].first, sizeof(uint64_t), flags) == S_OK;
                complete &= context->GetData(slot.events[i].end.get(), &times[i].second, sizeof(uint64_t), flags) == S_OK;
            }
            if (!complete) continue;
            for (size_t i = 0; i < slot.count; ++i) if (times[i].second >= times[i].first) {
                const auto& event = slot.events[i];
                samples.push_back({slot.frame, event.stage, "D3D11_timeline", event.width, event.height,
                    double(times[i].second-times[i].first)*1000./double(data.Frequency),times[i].first,times[i].second,data.Frequency});
            }
            slot.pending = false;
        }
    }
    void Begin(uint64_t frame) {
        Poll(); active = nullptr; currentFrame=frame;
        for (auto& slot : slots) if (!slot.pending) {
            active = &slot; slot.frame = frame; slot.count = 0;
            context->Begin(slot.disjoint.get()); return;
        }
        ++droppedFrames;
    }
    size_t Pending() const {
        return std::count_if(slots.begin(),slots.end(),[](const auto& slot){return slot.pending;});
    }
    void End() {
        if (!active) return;
        context->End(active->disjoint.get()); active->pending = true; active = nullptr;
    }
    Event* Start(const char* stage, uint32_t width, uint32_t height) {
        if (!active) return nullptr;
        if (active->count == active->events.size()) { ++droppedEvents; return nullptr; }
        auto& event = active->events[active->count++];
        event.stage = stage; event.width = width; event.height = height;
        context->End(event.start.get()); return &event;
    }
};
inline GpuTrace* trace = nullptr;
struct Region {
    GpuTrace::Event* event = nullptr;
    bool automaticBatch = false;
    Region(const char* stage, ID3D11DeviceContext*, uint32_t width, uint32_t height) {
        if (trace) {
            if (!trace->active) { trace->Begin(trace->currentFrame); automaticBatch=true; }
            event = trace->Start(stage, width, height);
        }
    }
    void Stop() {
        if (event) trace->context->End(event->end.get());
        if (automaticBatch) trace->End();
        event=nullptr; automaticBatch=false;
    }
    ~Region() { Stop(); }
};
struct CpuRegion {
    const char* stage;
    uint32_t width,height;
    std::chrono::steady_clock::time_point started;
    CpuRegion(const char* name,uint32_t w,uint32_t h) : stage(name),width(w),height(h),started(std::chrono::steady_clock::now()) {}
    ~CpuRegion() {
        if (trace) trace->samples.push_back({trace->currentFrame,stage,"CPU",width,height,
            std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count()});
    }
};
inline double Milliseconds(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
}
}
