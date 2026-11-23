#pragma once
#include <cstdint>
#include <dxgi.h>

namespace Magpie {

// An event can also signal for an older timed-out registration on the same handle.
// Keep the original timeout and require the caller's actual fence value.
template<class Fence>
HRESULT WaitForGpuFence(Fence* fence, uint64_t value, HANDLE event, DWORD timeout) noexcept {
	const ULONGLONG started = GetTickCount64();
	DWORD remaining = timeout;
	for (;;) {
		const DWORD result = WaitForSingleObject(event, remaining);
		const DWORD waitError = result == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
		const uint64_t completed = fence->GetCompletedValue();
		if (completed == UINT64_MAX) return DXGI_ERROR_DEVICE_REMOVED;
		if (completed >= value) return S_OK;
		if (result == WAIT_FAILED) return HRESULT_FROM_WIN32(waitError ? waitError : ERROR_GEN_FAILURE);
		if (result != WAIT_TIMEOUT && result != WAIT_OBJECT_0) return E_UNEXPECTED;
		if (timeout == INFINITE && result == WAIT_OBJECT_0) continue;
		const ULONGLONG elapsed = GetTickCount64() - started;
		if (result == WAIT_TIMEOUT || elapsed >= timeout) return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
		remaining = timeout - static_cast<DWORD>(elapsed);
	}
}

}
