#pragma once
#include "FrameGuidanceTypes.h"
#include "PresentationFrameRate.h"
#include "FrameGuidanceAvailability.h"
#include "PresenterFailure.h"
#include "VRRPresentationPolicy.h"
#include "VRROutputProbe.h"

namespace Magpie {

class DeviceResources;
class ReflexController;

class PresenterBase {
public:
	virtual ~PresenterBase() noexcept {}

	bool Initialize(HWND hwndAttach, const DeviceResources& deviceResources) noexcept;
	void OutputEnvironmentChanged(bool topologyChanged = false) noexcept {
		_vrrNextProbe = {};
		if (topologyChanged) _vrrOutputProbe.Invalidate();
	}
	// Reserve DXGI capacity before shared-texture GPU work. Other presenters
	// keep their existing BeginFrame admission path.
	virtual bool PrepareFrame() noexcept { return true; }

	virtual bool BeginFrame(
		winrt::com_ptr<ID3D11Texture2D>& frameTex,
		winrt::com_ptr<ID3D11RenderTargetView>& frameRtv,
		POINT& drawOffset
	) noexcept = 0;
	// Replay a completed output without supplying a new capture/motion sample.
	// Plain presenters have no temporal state. SDK proxies explicitly disable
	// generation for this submission and reset temporal history on resumption.
	virtual bool BeginIdleFrame(winrt::com_ptr<ID3D11Texture2D>& frameTex,
		winrt::com_ptr<ID3D11RenderTargetView>& frameRtv, POINT& drawOffset) noexcept {
		return BeginFrame(frameTex, frameRtv, drawOffset);
	}

	// Returns true when the frame was submitted to the presentation backend.
	// Successful status codes such as DXGI_STATUS_OCCLUDED still count as a
	// submission so a deliberately hidden first-frame window can be shown.
	virtual bool EndFrame(bool waitForGpu = false) noexcept = 0;
	// Only stable DXGI images may remain prepared across message-pump passes.
	virtual bool SupportsDeferredPresent() const noexcept { return false; }
	// SDK-owned frame generators implement their own input limiter.
	virtual bool SetBaseFrameRateLimit(double) noexcept { return true; }
	virtual void SetReflexController(ReflexController*) noexcept {}
	virtual void SetReflexFrame(uint64_t, uint64_t, bool) noexcept {}
	virtual void BeginReflexRender() noexcept {}
	virtual void CancelReflexRender() noexcept {}
	// Called only from the outer message pump, after BeginFrame found no capacity.
	virtual bool WaitForFrameCapacity(DWORD) noexcept { return false; }
	virtual bool WasFrameCapacityBusy() const noexcept { return false; }
	// Fatal SDK/device errors must stop the session instead of retrying forever.
	virtual bool HasFatalError() const noexcept { return false; }
	virtual const PresenterFailure* GetFailure() const noexcept { return nullptr; }
	std::chrono::steady_clock::time_point LastSubmissionTime() const noexcept { return _lastSubmissionTime; }
	std::optional<uint32_t> LastPresentedFrameCount() const noexcept { return _lastPresentedFrameCount; }

	virtual ContentFrameCount LastContentFrameCount() const noexcept {
		return { _lastPresentedFrameCount, ContentFrameCountSource::Reported };
	}
	// Async SDK output timestamps are consumed independently of batch counts.
	virtual void SetProfilerEnabled(bool) noexcept {}
	virtual PresentationIntervals TakePresentationIntervals() noexcept { return {}; }

	// Supplies motion that belongs to the next real base frame. Presenters that
	// do not consume frame guidance intentionally ignore this call.
	virtual void SetSourceTiming(uint64_t, uint64_t, uint64_t, int64_t) noexcept {}
	virtual void SetCaptureAdmissionTime(int64_t) noexcept {}
	virtual void SetOutputHistoryRevision(uint64_t) noexcept {}
	virtual void SetFrameGuidanceAvailability(FrameGuidanceAvailability) noexcept {}

	virtual void SetFrameGuidance(
		ID3D11Texture2D*,
		FrameGuidanceFrameId,
		bool,
		const RECT&
	) noexcept {}

	// Frame-generation presenters may expose a transparent composition surface
	// above their SDK-owned colour swap chain. UI-only presents use this surface
	// and therefore never masquerade as a new game input frame.
	virtual bool HasIndependentOverlay() const noexcept { return false; }
	virtual bool BeginOverlayFrame(
		winrt::com_ptr<ID3D11Texture2D>&,
		winrt::com_ptr<ID3D11RenderTargetView>&,
		POINT&
	) noexcept { return false; }
	virtual bool EndOverlayFrame() noexcept { return false; }

	// A presenter with a frame-latency waitable object already provides queue
	// capacity pacing in BeginFrame. DLSSFG must not add a DWM wait on top of it.
	virtual bool UsesFrameLatencyWaitableObject() const noexcept {
		return false;
	}

	virtual bool OnResize() noexcept = 0;

	virtual void OnEndResize(bool& shouldRedraw) noexcept {
		shouldRedraw = false;
	}

protected:
	virtual bool _Initialize(HWND hwndAttach) noexcept = 0;

	virtual UINT _PresentSyncInterval(UINT) const noexcept { return 0; }
	void _WaitForGpu() noexcept;
	void _RefreshVRREnvironment(bool force = false) noexcept;
	void _LogVRRState(std::string_view event) noexcept;
	void _VRRPathAvailable(bool available) noexcept;
	void _UpdateForegroundCompatibility() noexcept;
	void _BindVRRSwapChain(IDXGISwapChain4* swapChain) noexcept;
	HRESULT _PresentWithVRR(IDXGISwapChain4* swapChain, bool retryWithoutTearing) noexcept;
	VRRPresentationPolicy _vrr;

	static uint32_t _CalcBufferCount() noexcept;

	const DeviceResources* _deviceResources = nullptr;
	std::optional<uint32_t> _lastPresentedFrameCount = 0;
	std::chrono::steady_clock::time_point _lastSubmissionTime{};

private:
	VRRUnavailableReason _QualifyVRROutput(const VRROutputProbeKey& key, bool force) noexcept;
	VRROutputProbe _vrrOutputProbe;
	VRRUnavailableReason _vrrEnumeratedOutput = VRRUnavailableReason::OutputUnknown;
	std::optional<UINT> _vrrAdapterFlags;
	HWND _vrrWindow = NULL;
	HMONITOR _vrrMonitor = NULL;
	HMONITOR _lastLoggedVRRMonitor = NULL;
	std::chrono::steady_clock::time_point _vrrNextProbe{};
	std::optional<VRRPresentationPolicy> _lastLoggedVRR;
	bool _vrrInitializationComplete = false, _lastLoggedVRRInitializationComplete = false;
	bool _firstPresentComplete = false;
	winrt::com_ptr<ID3D11Fence> _fence;
	uint64_t _fenceValue = 0;
	wil::unique_event_nothrow _fenceEvent;
};

}
