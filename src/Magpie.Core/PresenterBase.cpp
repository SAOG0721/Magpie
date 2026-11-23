#include "pch.h"
#include "PresenterBase.h"
#include "DeviceResources.h"
#include "Logger.h"
#include "ScalingWindow.h"
#include "NvidiaForegroundCompatibility.h"
#include "GpuFenceWait.h"
#include "Win32Helper.h"
#include "VRRPerformance.h"

namespace Magpie {

bool PresenterBase::Initialize(HWND hwndAttach, const DeviceResources& deviceResources) noexcept {
	_deviceResources = &deviceResources;
	_vrrWindow = hwndAttach;
	_vrrMonitor = MonitorFromWindow(hwndAttach, MONITOR_DEFAULTTONULL);
	_vrr.Configure(ScalingWindow::Get().Options().isVRREnabled, deviceResources.IsTearingSupported());
	_vrrAdapterFlags.reset();
	_vrrOutputProbe = {};
	_RefreshVRREnvironment(true);

	HRESULT hr = deviceResources.GetD3DDevice()->CreateFence(
		_fenceValue, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&_fence));
	if (FAILED(hr)) {
		// GH#979
		// 这个错误会在某些很旧的显卡上出现，似乎是驱动的 bug。文档中提到 ID3D11Device5::CreateFence 
		// 和 ID3D12Device::CreateFence 等价，但支持 DX12 的显卡也有失败的可能，如 GH#1013
		Logger::Get().ComError("CreateFence 失败", hr);
		return false;
	}

	if (!_fenceEvent.try_create(wil::EventOptions::None, nullptr)) {
		Logger::Get().Win32Error("CreateEvent 失败");
		return false;
	}

	const bool initialized = _Initialize(hwndAttach);
	_vrrInitializationComplete = true;
	_UpdateForegroundCompatibility();
	_LogVRRState(initialized ? "initialize" : "initialize-failed");
	return initialized;
}

VRRUnavailableReason PresenterBase::_QualifyVRROutput(const VRROutputProbeKey& key, bool force) noexcept {
	if (key.remote) return VRRUnavailableReason::RemoteSession;
	if (!key.topologyCurrent) return VRRUnavailableReason::StaleDisplayTopology;
	IDXGIAdapter4* adapter = _deviceResources->GetGraphicsAdapter();
	if (!adapter) return VRRUnavailableReason::OutputUnknown;
	if (!_vrrAdapterFlags) {
		DXGI_ADAPTER_DESC1 desc{};
		if (FAILED(adapter->GetDesc1(&desc))) return VRRUnavailableReason::OutputUnknown;
		_vrrAdapterFlags = desc.Flags;
	}
	if (*_vrrAdapterFlags & DXGI_ADAPTER_FLAG_SOFTWARE) return VRRUnavailableReason::SoftwareAdapter;
	if (!key.geometryValid) return VRRUnavailableReason::OutputUnknown;
	const RECT windowRect{ key.windowRect[0], key.windowRect[1], key.windowRect[2], key.windowRect[3] };
	const RECT monitorRect{ key.monitorRect[0], key.monitorRect[1], key.monitorRect[2], key.monitorRect[3] };
	RECT intersection{};
	if (!IntersectRect(&intersection, &windowRect, &monitorRect) ||
		!EqualRect(&intersection, &windowRect)) return VRRUnavailableReason::SpanningOutputs;
	if (!_vrrOutputProbe.NeedsEnumeration(key, GetTickCount64(), force)) return _vrrEnumeratedOutput;
	_vrrEnumeratedOutput = VRRUnavailableReason::OutputUnknown;
	for (UINT index = 0; ; ++index) {
		winrt::com_ptr<IDXGIOutput> output;
		const HRESULT hr = adapter->EnumOutputs(index, output.put());
		if (hr == DXGI_ERROR_NOT_FOUND) break;
		if (FAILED(hr)) return VRRUnavailableReason::OutputUnknown;
		DXGI_OUTPUT_DESC desc{};
		if (FAILED(output->GetDesc(&desc))) return VRRUnavailableReason::OutputUnknown;
		if (desc.AttachedToDesktop && uintptr_t(desc.Monitor) == key.monitor)
			return _vrrEnumeratedOutput = VRRUnavailableReason::None;
	}
	return _vrrEnumeratedOutput = VRRUnavailableReason::CrossAdapter;
}

void PresenterBase::_RefreshVRREnvironment(bool force) noexcept {
	if (!_vrr.Requested() || !_vrr.TearingSupported()) return;
	const auto now = std::chrono::steady_clock::now();
	if (!force && now < _vrrNextProbe) return;
	_vrrNextProbe = now + std::chrono::milliseconds(500);
	VRROutputProbeKey key;
	key.remote = GetSystemMetrics(SM_REMOTESESSION) != 0;
	key.topologyCurrent = _deviceResources->GetDXGIFactory()->IsCurrent() != FALSE;
	RECT windowRect{};
	MONITORINFO monitorInfo{};
	monitorInfo.cbSize = sizeof(monitorInfo);
	if (GetWindowRect(_vrrWindow, &windowRect) && !IsRectEmpty(&windowRect)) {
		key.windowRect = { windowRect.left, windowRect.top, windowRect.right, windowRect.bottom };
		_vrrMonitor = MonitorFromRect(&windowRect, MONITOR_DEFAULTTONULL);
		key.monitor = uintptr_t(_vrrMonitor);
		if (_vrrMonitor && GetMonitorInfo(_vrrMonitor, &monitorInfo)) {
			key.geometryValid = true;
			const auto& rect = monitorInfo.rcMonitor;
			key.monitorRect = { rect.left, rect.top, rect.right, rect.bottom };
		}
	} else {
		_vrrMonitor = nullptr;
	}
	_vrr.Environment(_QualifyVRROutput(key, force));
	_UpdateForegroundCompatibility();
	_LogVRRState("output-probe");
}

void PresenterBase::_VRRPathAvailable(bool available) noexcept {
	_vrr.PathAvailable(available);
	_vrrMonitor = MonitorFromWindow(_vrrWindow, MONITOR_DEFAULTTONULL);
	_RefreshVRREnvironment(true);
	_LogVRRState("path-change");
}

void PresenterBase::_LogVRRState(std::string_view event) noexcept {
	_UpdateForegroundCompatibility();
	if (event != "first-present" && _lastLoggedVRR && *_lastLoggedVRR == _vrr && _lastLoggedVRRMonitor == _vrrMonitor &&
		_lastLoggedVRRInitializationComplete == _vrrInitializationComplete) return;
	_lastLoggedVRR = _vrr;
	_lastLoggedVRRMonitor = _vrrMonitor;
	_lastLoggedVRRInitializationComplete = _vrrInitializationComplete;
	const char* reason = "none";
	switch (_vrr.Reason()) {
	case VRRUnavailableReason::None: break;
	case VRRUnavailableReason::Disabled: reason = "disabled"; break;
	case VRRUnavailableReason::TearingUnsupported: reason = "tearing-unsupported"; break;
	case VRRUnavailableReason::PathUnavailable: reason = "non-DXGI-path"; break;
	case VRRUnavailableReason::OutputUnknown: reason = "output-unknown"; break;
	case VRRUnavailableReason::SpanningOutputs: reason = "spanning-or-outside-output"; break;
	case VRRUnavailableReason::SoftwareAdapter: reason = "software-adapter"; break;
	case VRRUnavailableReason::RemoteSession: reason = "remote-session"; break;
	case VRRUnavailableReason::CrossAdapter: reason = "cross-adapter-output"; break;
	case VRRUnavailableReason::CreationRejected: reason = "creation-rejected"; break;
	case VRRUnavailableReason::AltEnterUnsafe: reason = "alt-enter-guard-failed"; break;
	case VRRUnavailableReason::PresentRejected: reason = "present-contract-rejected"; break;
	case VRRUnavailableReason::SwapChainWithoutTearing: reason = "swap-chain-without-tearing-restart-required"; break;
	case VRRUnavailableReason::StaleDisplayTopology: reason = "display-topology-stale-restart-required"; break;
	}
	if (!_vrrInitializationComplete && _vrr.Requested() && _vrr.TearingSupported() &&
		(_vrr.Reason() == VRRUnavailableReason::PathUnavailable || _vrr.Reason() == VRRUnavailableReason::AltEnterUnsafe))
		reason = "initializing";
	DXGI_ADAPTER_DESC1 adapterDesc{};
	if (auto adapter = _deviceResources->GetGraphicsAdapter()) adapter->GetDesc1(&adapterDesc);
	Logger::Get().Info(fmt::format(
		"VRR: event={} requested={} tearingSupported={} eligible={} creationFlags=0x{:x} "
		"syncInterval={} presentFlags=0x{:x} active=unknown vendorCompatibilityRequested={} "
		"reason={} hwnd=0x{:x} monitor=0x{:x} adapterVendor=0x{:x} adapterLuid={:x}:{:x} realForeground=0x{:x}",
		event, _vrr.Requested(), _vrr.TearingSupported(),
		_vrr.Reason() == VRRUnavailableReason::None, _vrr.ResizeFlags(), _PresentSyncInterval(_vrr.PresentFlags()), _vrr.PresentFlags(),
		_vrr.Requested() && adapterDesc.VendorId == 0x10de,
		reason, reinterpret_cast<uintptr_t>(_vrrWindow), reinterpret_cast<uintptr_t>(_vrrMonitor),
		adapterDesc.VendorId, uint32_t(adapterDesc.AdapterLuid.HighPart), adapterDesc.AdapterLuid.LowPart,
		uintptr_t(Win32Helper::GetRealForegroundWindow())));
}

void PresenterBase::_UpdateForegroundCompatibility() noexcept {
	const bool potential = _vrrInitializationComplete || _vrr.HasSwapChain() ?
		_vrr.PresentFlags() != 0 : _vrr.CreationFlags() != 0;
	NvidiaForegroundCompatibility::PresentationState(potential, _vrr.PresentFlags() != 0);
}

void PresenterBase::_BindVRRSwapChain(IDXGISwapChain4* swapChain) noexcept {
	DXGI_SWAP_CHAIN_DESC1 desc{};
	HWND window = nullptr;
	const HRESULT descHr = swapChain->GetDesc1(&desc);
	HRESULT windowHr = swapChain->GetHwnd(&window);
	if (FAILED(windowHr)) {
		// Some proxy implementations expose the HWND through the older descriptor.
		DXGI_SWAP_CHAIN_DESC legacyDesc{};
		windowHr = swapChain->GetDesc(&legacyDesc);
		if (SUCCEEDED(windowHr)) window = legacyDesc.OutputWindow;
	}
	if (SUCCEEDED(descHr)) _vrr.Created(desc.Flags);
	const bool valid = SUCCEEDED(descHr) && SUCCEEDED(windowHr) && window == _vrrWindow &&
		(desc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD || desc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL);
	Logger::Get().Info(fmt::format(
		"VRR swap-chain binding: expected=0x{:x} actual=0x{:x} descHr=0x{:x} windowHr=0x{:x} flags=0x{:x} valid={}",
		uintptr_t(_vrrWindow), uintptr_t(window), uint32_t(descHr), uint32_t(windowHr), desc.Flags, valid));
	if (!valid || !NvidiaForegroundCompatibility::BindPresentationWindow(window)) {
		NvidiaForegroundCompatibility::Reject("swap-chain-binding-unavailable");
		_VRRPathAvailable(false);
	}
	_UpdateForegroundCompatibility();
}

HRESULT PresenterBase::_PresentWithVRR(IDXGISwapChain4* swapChain, bool retryWithoutTearing) noexcept {
	_RefreshVRREnvironment();
	const UINT flags = _vrr.PresentFlags();
	_UpdateForegroundCompatibility();
	NvidiaForegroundCompatibility::Phase(_firstPresentComplete ? ForegroundQueryPhase::Present : ForegroundQueryPhase::FirstPresent);
	HRESULT hr;
	{
		VRRPerformance::PresentScope performance(flags != 0);
		hr = swapChain->Present(_PresentSyncInterval(flags), flags);
	}
	if (flags && hr == DXGI_ERROR_INVALID_CALL) {
		// Device loss is deliberately excluded. Latch for this swap-chain session;
		// ResizeBuffers must still retain its original immutable creation flags.
		_vrr.RejectPresent();
		_UpdateForegroundCompatibility();
		Logger::Get().ComWarn("VRR Present contract rejected; disabling tearing submissions for this session", hr);
		_LogVRRState("present-rejected");
		// An SDK proxy may already have consumed its tagged FG input. Only the
		// ordinary DXGI endpoint can safely retry the same image once here.
		if (retryWithoutTearing) {
			VRRPerformance::PresentScope performance(false);
			hr = swapChain->Present(_PresentSyncInterval(0), 0);
		}
	}
	if (FAILED(hr)) NvidiaForegroundCompatibility::Reject("present-failed");
	if (!_firstPresentComplete && SUCCEEDED(hr)) {
		_firstPresentComplete = true;
		_LogVRRState("first-present");
	}
	NvidiaForegroundCompatibility::Phase(ForegroundQueryPhase::Present);
	return hr;
}

uint32_t PresenterBase::_CalcBufferCount() noexcept {
	// 缓冲区数量取决于 ScalingRuntime::_ScalingThreadProc 中检查光标移动的频率
	return ScalingWindow::Get().Options().Is3DGameMode() ? 4 : 8;
}

HRESULT PresenterBase::_WaitForGpu() noexcept {
	ID3D11DeviceContext4* d3dDC = _deviceResources->GetD3DDC();

	// 等待渲染完成
	HRESULT hr = d3dDC->Signal(_fence.get(), ++_fenceValue);
	if (SUCCEEDED(hr)) hr = _fence->SetEventOnCompletion(_fenceValue, _fenceEvent.get());
	if (FAILED(hr)) return hr;

	d3dDC->Flush();
	const HRESULT completion = WaitForGpuFence(_fence.get(), _fenceValue, _fenceEvent.get(), 1000);
	if (completion == DXGI_ERROR_DEVICE_REMOVED) {
		const HRESULT reason = _deviceResources->GetD3DDevice()->GetDeviceRemovedReason();
		if (FAILED(reason)) return reason;
	}
	return completion;
}

}
