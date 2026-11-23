#pragma once
#include <atomic>
#include <d3d12.h>
#include <dxgi.h>
#include "GpuFenceWait.h"

namespace Magpie {

// Retained GPU/SDK owners must live until process exit. Both XeSS SR paths
// stop accepting sessions if a previous owner could not be safely retired.
inline std::atomic<bool> xessUpscaleRestartRequired{ false };

inline HRESULT WaitForXeSSUpscaleFence(
	ID3D12Device* device, ID3D12Fence* fence, HANDLE event, uint64_t value
) noexcept {
	const uint64_t completed = fence->GetCompletedValue();
	HRESULT result;
	if (completed == UINT64_MAX) {
		result = DXGI_ERROR_DEVICE_REMOVED;
	} else if (completed >= value) {
		return S_OK;
	} else {
		result = fence->SetEventOnCompletion(value, event);
		if (SUCCEEDED(result)) result = WaitForGpuFence(fence, value, event, 5000);
	}
	if (FAILED(result)) {
		const HRESULT reason = device->GetDeviceRemovedReason();
		if (FAILED(reason)) return reason;
	}
	return result;
}

}
