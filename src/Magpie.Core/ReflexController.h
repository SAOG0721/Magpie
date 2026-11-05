#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>

struct ID3D11Device;
struct ID3D12CommandQueue;

namespace Magpie {

enum class ReflexMarker { SimulationStart, SimulationEnd, RenderStart, RenderEnd };

enum class ReflexState { Unavailable, Active, DriverOff, Paused, Faulted, Stopped };

// Clear means no nonzero interval was requested, or SetSleepMode accepted zero.
// During a transition or failed cap cleanup the renderer cannot start Async.
enum class ReflexPacingState { Clear, Configuring, Active, CleanupFailed };

// Optional frontend out-of-band GPU tracking is independent of the driver
// interval and low-latency query. A failed first marker disables this scope.
enum class ReflexRenderTrackingState { Unavailable, Unchecked, Supported, Disabled };

struct ReflexConfigurationResult {
	int setStatus = 0;
	int queryStatus = 0;
	bool queried = false;
	bool lowLatency = false;
};

struct ReflexSleepStatus {
	int status = 0;
	bool lowLatency = false;
};

struct ReflexSettings {
	bool lowLatency = true;
	bool boost = false;
	uint32_t minimumIntervalUs = 0;
	bool operator==(const ReflexSettings&) const = default;
};

// Injectable driver boundary: tests exercise the production frame lifecycle
// without loading a display driver or running NGX.
class ReflexDriver {
public:
	virtual ~ReflexDriver() = default;
	virtual ReflexConfigurationResult Configure(ReflexSettings settings) noexcept = 0;
	virtual ReflexSleepStatus QueryStatus() noexcept = 0;
	virtual int Sleep() noexcept = 0;
	virtual int Marker(ReflexMarker marker, uint64_t frameId) noexcept = 0;
	virtual int RegisterGenerationQueue(ID3D12CommandQueue* queue) noexcept = 0;
	virtual int Generation(ID3D12CommandQueue* queue, uint64_t frameId,
		uint64_t presentId, bool start) noexcept = 0;
	virtual int FrontendRender(uint64_t frameId, uint64_t presentId, bool start) noexcept = 0;
	virtual int Present(uint64_t frameId, uint64_t presentId, bool generated, bool start) noexcept = 0;
	virtual void ReportFailure(const char* operation, int status) noexcept = 0;
};

std::unique_ptr<ReflexDriver> CreateNvReflexDriver(ID3D11Device* presentDevice) noexcept;

// Owned by Renderer, initialized before either rendering thread can use it and
// destroyed after the backend joins. The driver object stays immutable/alive
// even after a failure. Sleep never holds the configuration mutex: Present on
// the other thread must be able to make progress while the driver sleeps.
class ReflexController {
public:
	~ReflexController() { Stop(); }
	void Initialize(std::unique_ptr<ReflexDriver> driver, ReflexSettings settings = {}) noexcept {
		_driver = std::move(driver);
		_settings = settings;
		_frontendRenderTracking.store(_driver ? ReflexRenderTrackingState::Unchecked :
			ReflexRenderTrackingState::Unavailable);
		if (_driver) SetPresentationAvailable(true);
	}
	void SetPresentationAvailable(bool available) noexcept {
		if (!_driver || _stopped.load() || _presentationAvailable.load() == available) return;
		std::scoped_lock lock(_configurationMutex);
		if (!_driver || _stopped.load() || _presentationAvailable.load() == available) return;
		_presentationAvailable.store(available);
		_ConfigureLocked(available ? _settings : ReflexSettings{ .lowLatency = false });
	}
	void SetFrameRateLimit(uint32_t intervalUs) noexcept {
		std::scoped_lock lock(_configurationMutex);
		if (!_driver || _stopped.load() || _settings.minimumIntervalUs == intervalUs) return;
		_settings.minimumIntervalUs = intervalUs;
		if (_presentationAvailable.load()) _ConfigureLocked(_settings);
	}
	void Stop() noexcept {
		std::scoped_lock lock(_configurationMutex);
		_StopLocked(nullptr, 0);
	}
	ReflexState State() const noexcept { return _state.load(); }
	bool Available() const noexcept { return State() == ReflexState::Active; }
	ReflexPacingState PacingState() const noexcept { return _pacingState.load(); }
	bool PacingAvailable() const noexcept { return PacingState() == ReflexPacingState::Active; }
	bool CanUseAsync() const noexcept { return PacingState() == ReflexPacingState::Clear; }
	bool CaptureBlocked() const noexcept {
		const auto state = PacingState();
		return state == ReflexPacingState::Configuring || state == ReflexPacingState::CleanupFailed;
	}
	bool CanResume() const noexcept { return _driver && !_stopped.load(); }
	ReflexRenderTrackingState FrontendRenderTrackingState() const noexcept {
		return _frontendRenderTracking.load();
	}

	// Backend only. Polls before Render starts retain the slept candidate,
	// including explicitly scoped NR/WGC input validation. A rejected attempt
	// that already opened Render closes that interval before waiting again.
	uint64_t BeginCapture(uint64_t minimumFrameId = 1) noexcept {
		const auto revision = _configurationRevision.load();
		if (_captureFrameId && _captureRevision != revision) CompleteCapture();
		if (CaptureBlocked() || !_presentationAvailable.load() || _stopped.load()) return 0;
		if (_captureFrameId) return _captureFrameId;
		// Keep Sleep/markers while the interface is usable even if the driver's
		// low-latency query is Off (including an explicit zero application cap).
		if (!_Usable() || !_Check("Sleep", _driver->Sleep()) ||
			CaptureBlocked() || _stopped.load() || !_presentationAvailable.load() ||
			_configurationRevision.load() != revision) return 0;
		_captureRevision = revision;
		_nextFrameId = std::max(_nextFrameId + 1, minimumFrameId);
		_captureFrameId = _nextFrameId;
		_renderStarted = false;
		_renderEnded = false;
		_Marker(ReflexMarker::SimulationStart);
		// Magpie has no access to the source application's simulation or input.
		// This is its own capture/processing cycle; do not invent INPUT_SAMPLE.
		return _captureFrameId;
	}
	// Full capture scopes call before capture GPU work. Input-validated NR/WGC
	// instead calls at accepted-content effects, after capture preparation.
	void BeginCaptureRender() noexcept {
		if (!_captureFrameId || _renderStarted || _renderEnded) return;
		_renderStarted = true;
		_Marker(ReflexMarker::SimulationEnd);
		_Marker(ReflexMarker::RenderStart);
	}
	uint64_t CaptureFrameId() const noexcept { return _captureFrameId; }
	uint64_t NextPresentId() noexcept { return _captureFrameId ? ++_nextPresentId : 0; }
	// A buffered output can be presented after its capture cycle has completed.
	// Keep that immutable capture ID instead of borrowing the current cycle.
	uint64_t NextPresentIdForFrame(uint64_t frameId) noexcept { return frameId ? ++_nextPresentId : 0; }
	void EndCaptureRender() noexcept {
		if (_captureFrameId && !_renderEnded) {
			_Marker(_renderStarted ? ReflexMarker::RenderEnd : ReflexMarker::SimulationEnd);
			_renderEnded = true;
		}
	}
	void DiscardCaptureRender() noexcept {
		if (_renderStarted) CompleteCapture();
	}
	void CompleteCapture() noexcept {
		EndCaptureRender();
		_captureFrameId = 0;
	}
	void RegisterGenerationQueue(ID3D12CommandQueue* queue) noexcept {
		if (_Usable()) _Check("NotifyOutOfBandCommandQueue", _driver->RegisterGenerationQueue(queue));
	}
	void Generation(ID3D12CommandQueue* queue, uint64_t frameId,
		uint64_t presentId, bool start) noexcept {
		if (frameId && presentId && _Usable())
			_Check("D3D12 async generation marker", _driver->Generation(queue, frameId, presentId, start));
	}
	// Frontend only. IDs come from the immutable published ring slot, never
	// from the backend's currently running (possibly newer) capture.
	void FrontendRender(uint64_t frameId, uint64_t presentId, bool start) noexcept {
		if (!frameId || !presentId || !_Usable()) return;
		const auto tracking = _frontendRenderTracking.load();
		if (tracking == ReflexRenderTrackingState::Disabled) return;
		const int status = _driver->FrontendRender(frameId, presentId, start);
		if (tracking == ReflexRenderTrackingState::Unchecked && start) {
			if (status) {
				// No interval was opened. A failed initial optional tracking call
				// must not revoke working Sleep/capture/present APIs or the cap.
				// Keep the raw error visible; do not claim that NVAPI accepted it.
				_frontendRenderTracking.store(ReflexRenderTrackingState::Disabled);
				_driver->ReportFailure("D3D11 async render tracking disabled after initial marker failure; "
					"driver pacing and present tracking retained", status);
				return;
			}
			_frontendRenderTracking.store(ReflexRenderTrackingState::Supported);
		}
		// Once tracking opened successfully, any later failure retains the
		// original cleanup contract, including an unsuccessful closing marker.
		_Check("D3D11 async render marker", status);
	}
	void Present(uint64_t frameId, uint64_t presentId, bool generated, bool start) noexcept {
		if (frameId && presentId && _Usable() &&
			_Check("D3D11 async present marker", _driver->Present(frameId, presentId, generated, start)) &&
			!generated && !start) _ObserveSleepStatus();
	}

private:
	// Frontend only. A cold Off query may become On once presentation is warm.
	// At most three read-only checks per configuration, never a repeated On
	// request. The count is completed base-present marker pairs, not scanout.
	void _ObserveSleepStatus() noexcept {
		if (State() != ReflexState::DriverOff) return;
		const auto revision = _configurationRevision.load();
		if (_statusObservationRevision != revision) {
			_statusObservationRevision = revision;
			_statusObservationCount = 0;
		}
		if (_statusObservationCount >= 512) return;
		const auto count = ++_statusObservationCount;
		if (count != 32 && count != 128 && count != 512) return;
		std::scoped_lock lock(_configurationMutex);
		if (!_Usable() || !_presentationAvailable.load() ||
			_configurationRevision.load() != revision || State() != ReflexState::DriverOff) return;
		const auto result = _driver->QueryStatus();
		if (result.status) {
			// This optional observation cannot revoke the accepted interval.
			_statusObservationCount = 512;
			_driver->ReportFailure("read-only warm GetSleepStatus; driver pacing retained", result.status);
			return;
		}
		_state.store(result.lowLatency ? ReflexState::Active : ReflexState::DriverOff);
	}
	void _ConfigureLocked(ReflexSettings settings) noexcept {
		_pacingState.store(ReflexPacingState::Configuring);
		++_configurationRevision;
		// Even a failed nonzero request may have partially changed driver state.
		// A session that has only requested zero never installed an application cap.
		if (settings.minimumIntervalUs) _capMayBeSet = true;
		const auto result = _driver->Configure(settings);
		if (!result.setStatus && !settings.minimumIntervalUs) _capMayBeSet = false;
		if (result.setStatus || (result.queried && result.queryStatus)) {
			_StopLocked(result.setStatus ? "SetSleepMode" : "GetSleepStatus",
				result.setStatus ? result.setStatus : result.queryStatus);
			return;
		}
		// A successful request does not guarantee driver activation (for example,
		// a control-panel override). Keep the interface alive without re-requesting
		// On every frame or pretending that the driver returned NOT_SUPPORTED.
		_state.store(!_presentationAvailable.load() ? ReflexState::Paused :
			(result.queried && result.lowLatency ? ReflexState::Active : ReflexState::DriverOff));
		// NVAPI's interval and low-latency switch are independent. A successful
		// Off query cannot revoke an accepted frame limit.
		_pacingState.store(settings.minimumIntervalUs ? ReflexPacingState::Active : ReflexPacingState::Clear);
	}
	bool _Usable() const noexcept { return _driver && !_stopped.load(); }
	void _Marker(ReflexMarker marker) noexcept {
		if (_Usable()) _Check("SetLatencyMarker", _driver->Marker(marker, _captureFrameId));
	}
	bool _Check(const char* operation, int status) noexcept {
		if (!status) return true;
		std::scoped_lock lock(_configurationMutex);
		_StopLocked(operation, status);
		return false;
	}
	void _StopLocked(const char* operation, int status) noexcept {
		if (!_driver || _stopped.exchange(true)) return;
		_pacingState.store(ReflexPacingState::Configuring);
		_state.store(operation ? ReflexState::Faulted : ReflexState::Stopped);
		if (operation) _driver->ReportFailure(operation, status);
		const auto reset = _driver->Configure(ReflexSettings{ .lowLatency = false });
		if (reset.setStatus) _driver->ReportFailure("restore SleepMode Off", reset.setStatus);
		else if (reset.queried && reset.queryStatus)
			_driver->ReportFailure("query restored SleepMode", reset.queryStatus);
		// GetSleepStatus reports low latency, not the interval. Set success is
		// the cleanup contract; a failed query cannot make a cleared cap unsafe.
		if (!reset.setStatus) _capMayBeSet = false;
		_pacingState.store(_capMayBeSet ? ReflexPacingState::CleanupFailed : ReflexPacingState::Clear);
	}
	std::unique_ptr<ReflexDriver> _driver;
	ReflexSettings _settings;
	bool _capMayBeSet = false; // Protected by _configurationMutex.
	std::mutex _configurationMutex;
	std::atomic<ReflexState> _state = ReflexState::Unavailable;
	std::atomic<ReflexPacingState> _pacingState = ReflexPacingState::Clear;
	std::atomic<ReflexRenderTrackingState> _frontendRenderTracking = ReflexRenderTrackingState::Unavailable;
	std::atomic<uint64_t> _configurationRevision = 0;
	uint64_t _statusObservationRevision = 0; // Frontend only.
	uint32_t _statusObservationCount = 0; // Frontend only, bounded per revision.
	uint64_t _captureRevision = 0;
	std::atomic<bool> _presentationAvailable = false;
	std::atomic<bool> _stopped = false;
	uint64_t _nextFrameId = 0;
	uint64_t _nextPresentId = 0;
	uint64_t _captureFrameId = 0;
	bool _renderStarted = false;
	bool _renderEnded = false;
};

}
