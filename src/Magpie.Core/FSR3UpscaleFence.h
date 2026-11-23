#pragma once
#include <atomic>
#include <chrono>
#include <d3d12.h>
#include <dxgi.h>

namespace Magpie {

// Both D3D12 upscalers use the same AMD runtime. Once an undrained owner
// has been retained, subsequent sessions must not add more.
inline std::atomic<bool> fsrUpscaleRestartRequired = false;

inline HRESULT WaitForFsrUpscaleFence(
	ID3D12Fence* fence, ID3D12Device* device, HANDLE event, uint64_t value
) noexcept {
	using namespace std::chrono;
	const auto deadline = steady_clock::now() + seconds(3);
	bool armed = false;
	for (;;) {
		const uint64_t completed = fence->GetCompletedValue();
		if (completed == UINT64_MAX) {
			const HRESULT reason = device->GetDeviceRemovedReason();
			return FAILED(reason) ? reason : DXGI_ERROR_DEVICE_REMOVED;
		}
		if (completed >= value) return S_OK;
		const auto now = steady_clock::now();
		if (now >= deadline) return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
		if (!armed) {
			const HRESULT hr = fence->SetEventOnCompletion(value, event);
			if (FAILED(hr)) return hr;
			armed = true;
		}
		// A previous timed-out registration can wake the same event. Confirm
		// the intended fence value before reusing GPU resources.
		const DWORD timeout = static_cast<DWORD>(ceil<milliseconds>(deadline - now).count());
		const DWORD result = WaitForSingleObject(event, timeout);
		if (result == WAIT_FAILED) return HRESULT_FROM_WIN32(GetLastError());
		if (result != WAIT_OBJECT_0 && result != WAIT_TIMEOUT) return E_FAIL;
	}
}

}
