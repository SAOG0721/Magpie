#pragma once
#include "SmallVector.h"
#include "ProfilerHistory.h"
#include <array>
#include <chrono>

namespace Magpie {

class DeviceResources;

struct EffectsProfilerSnapshot {
	SmallVector<float> passTimings;
	ProfilerHistory history;
	ProfilerHistory::Time sampleTime{};
	uint64_t generation = 0;
	bool gpuAvailable = false;
	bool partial = false;
};

class EffectsProfiler {
public:
	EffectsProfiler() = default;

	EffectsProfiler(const EffectsProfiler&) = delete;
	EffectsProfiler(EffectsProfiler&&) = delete;

	void Start(ID3D11Device* d3dDevice, uint32_t passCount, bool partial = false) noexcept;

	void Stop() noexcept;

	bool IsProfiling() const noexcept;

	void SetPassCount(ID3D11Device* d3dDevice, uint32_t passCount, bool partial = false) noexcept;
	void OnFrame(ID3D11Device* device, uint64_t captureSequence, uint64_t resourceGeneration,
		bool isNewCaptureFrame) noexcept;
	void SetSuspended(ID3D11Device* device, bool suspended) noexcept;

	void OnBeginEffects(ID3D11DeviceContext* d3dDC) noexcept;

	void OnEndPass(ID3D11DeviceContext* d3dDC, bool executed = true) noexcept;

	void OnEndEffects(ID3D11DeviceContext* d3dDC) noexcept;

	void QueryTimings(ID3D11DeviceContext* d3dDC) noexcept;
	void Poll(ID3D11DeviceContext* d3dDC) noexcept;

	// 从前端线程调用
	EffectsProfilerSnapshot GetSnapshot() noexcept;

private:
	struct QuerySlot {
		winrt::com_ptr<ID3D11Query> disjoint;
		winrt::com_ptr<ID3D11Query> start;
		std::vector<winrt::com_ptr<ID3D11Query>> passes;
		std::vector<uint8_t> executed;
		std::chrono::steady_clock::time_point submitted{};
		ProfilerHistory::Time frameTime{};
	};
	void _Disable(const char* operation, HRESULT hr) noexcept;
	bool _ReadQuery(ID3D11DeviceContext* d3dDC, ID3D11Query* query,
		void* data, UINT size, std::chrono::steady_clock::time_point submitted) noexcept;

	void _PublishHistory(ProfilerHistory::Time now, bool force = false) noexcept;
	void _ClearQueries() noexcept;
	EffectsProfilerSnapshot _snapshot;
	wil::srwlock _timingsLock;
	ProfilerHistory _history;
	ProfilerHistory::Time _frameTime{}, _lastPublished{};
	uint64_t _generation = 0, _captureSequence = 0, _resourceGeneration = 0;
	uint32_t _requestedPassCount = 0;
	bool _enabled = false, _partial = false, _contextSet = false;

	// Backend-owned FIFO. Busy slots skip samples instead of blocking rendering.
	std::array<QuerySlot, 3> _slots;
	uint32_t _passCount = 0;
	uint32_t _readSlot = 0;
	uint32_t _writeSlot = 0;
	uint32_t _pendingCount = 0;
	uint64_t _skippedSamples = 0;
	bool _recording = false;
	uint32_t _curPass = 0;
};

}
