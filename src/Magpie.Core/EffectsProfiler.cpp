#include "pch.h"
#include "EffectsProfiler.h"
#include "Logger.h"

namespace Magpie {

void EffectsProfiler::Start(ID3D11Device* d3dDevice, uint32_t passCount, bool partial) noexcept {
	if (IsProfiling()) return;
	_enabled = true;
	_partial = partial;
	_requestedPassCount = passCount;
	_contextSet = false;
	++_generation;
	_history.Reset(ProfilerHistory::Clock::now());
	_PublishHistory(ProfilerHistory::Clock::now(), true);
	if (!passCount) {
		_Disable("Effects profiler: no passes", E_INVALIDARG);
		return;
	}
	for (QuerySlot& slot : _slots) {
		slot.passes.resize(passCount);
		slot.executed.resize(passCount);
		D3D11_QUERY_DESC desc{ .Query = D3D11_QUERY_TIMESTAMP_DISJOINT };
		HRESULT hr = d3dDevice->CreateQuery(&desc, slot.disjoint.put());
		if (FAILED(hr)) {
			_Disable("Effects profiler: create disjoint query", hr);
			return;
		}
		desc.Query = D3D11_QUERY_TIMESTAMP;
		hr = d3dDevice->CreateQuery(&desc, slot.start.put());
		if (FAILED(hr)) {
			_Disable("Effects profiler: create start query", hr);
			return;
		}
		for (auto& query : slot.passes) {
			hr = d3dDevice->CreateQuery(&desc, query.put());
			if (FAILED(hr)) {
				_Disable("Effects profiler: create pass query", hr);
				return;
			}
		}
	}
	_passCount = passCount;
	_PublishHistory(ProfilerHistory::Clock::now(), true);
	Logger::Get().Info(fmt::format("Effects profiler started: passes={} querySlots={}",
		passCount, _slots.size()));
}

void EffectsProfiler::Stop() noexcept {
	if (_passCount) Logger::Get().Info(fmt::format(
		"Effects profiler stopped: skippedSamples={} pending={}", _skippedSamples, _pendingCount));
	_ClearQueries();
	_enabled = false;
	_history = {};
	++_generation;
	auto lock = _timingsLock.lock_exclusive();
	_snapshot = {};
	_snapshot.generation = _generation;
}

void EffectsProfiler::_ClearQueries() noexcept {
	_slots = {};
	_passCount = _readSlot = _writeSlot = _pendingCount = _curPass = 0;
	_skippedSamples = 0;
	_recording = false;
}

bool EffectsProfiler::IsProfiling() const noexcept {
	return _enabled;
}

void EffectsProfiler::SetPassCount(ID3D11Device* d3dDevice, uint32_t passCount, bool partial) noexcept {
	if (!IsProfiling()) return;
	// Called between frames. Retire old queries rather than resize pending slots
	// whose timestamps still describe the previous effect chain.
	Stop();
	Start(d3dDevice, passCount, partial);
}

void EffectsProfiler::OnFrame(ID3D11Device* device, uint64_t captureSequence, uint64_t resourceGeneration,
	bool isNewCaptureFrame) noexcept {
	if (!_enabled) return;
	if (_contextSet && (_captureSequence != captureSequence || _resourceGeneration != resourceGeneration))
		SetPassCount(device, _requestedPassCount, _partial);
	_contextSet = true;
	_captureSequence = captureSequence;
	_resourceGeneration = resourceGeneration;
	_frameTime = ProfilerHistory::Clock::now();
	// Reprocessing an old capture still has effect work, but is not a new input.
	if (isNewCaptureFrame) _history.Frame(_frameTime);
	else _history.Observe(_frameTime);
	_PublishHistory(_frameTime);
}

void EffectsProfiler::SetSuspended(ID3D11Device* device, bool suspended) noexcept {
	if (!_enabled || suspended == _history.Suspended()) return;
	if (!suspended) SetPassCount(device, _requestedPassCount, _partial);
	_history.Suspend(suspended, ProfilerHistory::Clock::now());
	_PublishHistory(ProfilerHistory::Clock::now(), true);
}

void EffectsProfiler::_PublishHistory(ProfilerHistory::Time now, bool force) noexcept {
	if (!force && now - _lastPublished < ProfilerHistory::BucketDuration) return;
	_lastPublished = now;
	auto lock = _timingsLock.lock_exclusive();
	_snapshot.history = _history;
	_snapshot.generation = _generation;
	_snapshot.partial = _partial;
	_snapshot.gpuAvailable = _passCount != 0;
}

void EffectsProfiler::OnBeginEffects(ID3D11DeviceContext* d3dDC) noexcept {
	QueryTimings(d3dDC);
	if (!_passCount) return;
	if (_pendingCount == _slots.size()) {
		if (++_skippedSamples == 1) Logger::Get().Info(
			"Effects profiler query slots busy; skipping samples while effects continue");
		return;
	}
	QuerySlot& slot = _slots[_writeSlot];
	slot.frameTime = _frameTime;
	d3dDC->Begin(slot.disjoint.get());
	d3dDC->End(slot.start.get());
	_recording = true;
	_curPass = 0;
}

void EffectsProfiler::OnEndPass(ID3D11DeviceContext* d3dDC, bool executed) noexcept {
	if (!_recording) return;
	QuerySlot& slot = _slots[_writeSlot];
	if (_curPass >= _passCount) {
		d3dDC->End(slot.disjoint.get());
		_Disable("Effects profiler: more passes than expected", E_UNEXPECTED);
		return;
	}
	slot.executed[_curPass] = executed;
	d3dDC->End(slot.passes[_curPass++].get());
}

void EffectsProfiler::OnEndEffects(ID3D11DeviceContext* d3dDC) noexcept {
	if (!_recording) return;
	QuerySlot& slot = _slots[_writeSlot];
	d3dDC->End(slot.disjoint.get());
	_recording = false;
	if (_curPass != _passCount) {
		_Disable("Effects profiler: fewer passes than expected", E_UNEXPECTED);
		return;
	}
	slot.submitted = std::chrono::steady_clock::now();
	++_pendingCount;
	_writeSlot = (_writeSlot + 1) % uint32_t(_slots.size());
	// Normal texture publication submits these commands. GetData never flushes
	// or waits; incomplete results remain in the slot for subsequent polls.
}

void EffectsProfiler::_Disable(const char* operation, HRESULT hr) noexcept {
	Logger::Get().ComError(operation, hr);
	Logger::Get().Warn("Effects GPU timing disabled for this profiling session; effects continue");
	_ClearQueries();
	{
		auto lock = _timingsLock.lock_exclusive();
		_snapshot.passTimings.clear();
	}
	_PublishHistory(ProfilerHistory::Clock::now(), true);
}

bool EffectsProfiler::_ReadQuery(ID3D11DeviceContext* d3dDC, ID3D11Query* query,
	void* data, UINT size, std::chrono::steady_clock::time_point submitted) noexcept {
	const HRESULT hr = d3dDC->GetData(query, data, size, D3D11_ASYNC_GETDATA_DONOTFLUSH);
	if (hr == S_OK) return true;
	if (hr == S_FALSE) {
		if (std::chrono::steady_clock::now() - submitted >= 5s) {
			_Disable("Effects profiler: GPU query timeout", HRESULT_FROM_WIN32(ERROR_TIMEOUT));
		}
	} else {
		_Disable("Effects profiler: GetData failed", FAILED(hr) ? hr : E_UNEXPECTED);
	}
	return false;
}

void EffectsProfiler::QueryTimings(ID3D11DeviceContext* d3dDC) noexcept {
	// At most three samples per call. A single incomplete query returns at once.
	while (_pendingCount) {
		QuerySlot& slot = _slots[_readSlot];
		D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
		if (!_ReadQuery(d3dDC, slot.disjoint.get(), &disjoint, sizeof(disjoint), slot.submitted)) return;
		if (!disjoint.Disjoint) {
			if (!disjoint.Frequency) {
				_Disable("Effects profiler: zero timestamp frequency", E_UNEXPECTED);
				return;
			}
			uint64_t previous = 0;
			if (!_ReadQuery(d3dDC, slot.start.get(), &previous, sizeof(previous), slot.submitted)) return;
			SmallVector<float> timings;
			timings.resize(_passCount);
			const double toMs = 1000.0 / double(disjoint.Frequency);
			double total = 0;
			for (uint32_t i = 0; i < _passCount; ++i) {
				uint64_t timestamp = 0;
				if (!_ReadQuery(d3dDC, slot.passes[i].get(), &timestamp, sizeof(timestamp), slot.submitted)) return;
				if (timestamp < previous) {
					_Disable("Effects profiler: non-monotonic timestamp", E_UNEXPECTED);
					return;
				}
				timings[i] = slot.executed[i] ? float(double(timestamp - previous) * toMs) : 0.0f;
				total += timings[i];
				previous = timestamp;
			}
			_history.Effects(slot.frameTime, total);
			_PublishHistory(ProfilerHistory::Clock::now());
			// All GPU queries and aggregation are outside the frontend lock.
			auto lock = _timingsLock.lock_exclusive();
			_snapshot.passTimings = std::move(timings);
			_snapshot.sampleTime = slot.frameTime;
		}
		// Completed disjoint samples are invalid but their slots can be reused.
		--_pendingCount;
		_readSlot = (_readSlot + 1) % uint32_t(_slots.size());
	}
}

void EffectsProfiler::Poll(ID3D11DeviceContext* d3dDC) noexcept {
	if (!_enabled) return;
	QueryTimings(d3dDC);
	const auto now = ProfilerHistory::Clock::now();
	_history.Observe(now);
	_PublishHistory(now);
}

EffectsProfilerSnapshot EffectsProfiler::GetSnapshot() noexcept {
	auto lock = _timingsLock.lock_exclusive();
	EffectsProfilerSnapshot result;
	result.passTimings = std::move(_snapshot.passTimings);
	_snapshot.passTimings.clear();
	result.history = _snapshot.history;
	result.sampleTime = _snapshot.sampleTime;
	result.generation = _snapshot.generation;
	result.gpuAvailable = _snapshot.gpuAvailable;
	result.partial = _snapshot.partial;
	return result;
}

}
