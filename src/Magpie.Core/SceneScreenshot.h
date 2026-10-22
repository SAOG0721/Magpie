#pragma once
#include "ScalingOptions.h"
#include "HdrFrame.h"
#include "ScalingSessionLifetime.h"
#include <atomic>
#include <mutex>

namespace Magpie {

// Request options are immutable. Output is frontend-only; input handoff uses
// inputMutex and state is atomic. Admission covers readback, encoding and disk writing.
enum class SceneScreenshotState { Pending, Reading, Finished };
struct SceneScreenshotFrame;
struct SceneScreenshotRequest {
	uint64_t id = 0;
	std::wstring timestamp;
	uint32_t runId = 0;
	uint64_t captureSequence = 0;
	uint64_t resourceGeneration = 0;
	ScreenshotContent content = ScreenshotContent::OutputOnly;
	// Frontend-owned output is fixed before dispatching the input request.
	winrt::com_ptr<ID3D11Texture2D> output;
	HdrFrameMetadata outputMetadata;
	// Independent one-candidate handoff; never waits for an FG publication slot.
	std::mutex inputMutex;
	std::shared_ptr<SceneScreenshotFrame> input;
	std::atomic<SceneScreenshotState> state = SceneScreenshotState::Pending;
	bool CancelPending() noexcept {
		auto expected = SceneScreenshotState::Pending;
		return state.compare_exchange_strong(expected, SceneScreenshotState::Finished);
	}
	std::shared_ptr<ScalingSessionLifetime> session;
	winrt::DispatcherQueue cleanupDispatcher{ nullptr };
	std::filesystem::path directory;
	decltype(ScalingOptions::reportErrorDetails) report = nullptr;
	decltype(ScalingOptions::showToast) toast = nullptr;
	HWND target = nullptr;
	std::wstring savedMessage, pairSavedMessage, partialMessage;
};

// A new immutable native-size input candidate exists only while requested.
// The request mutex protects transfer; the texture is immutable after release key 1.
struct SceneScreenshotFrame {
	uint64_t requestId = 0;
	winrt::com_ptr<ID3D11Texture2D> input;
	wil::unique_handle inputHandle;
	winrt::com_ptr<ID3D11Fence> readyFence;
	wil::unique_event_nothrow readyEvent;
	HdrFrameMetadata metadata;
	// The caller owns the backend immediate context. This one-off copy must
	// submit its own work: a paused source may never publish another frame.
	void CopyInput(ID3D11Device5* device, ID3D11DeviceContext4* context, ID3D11Texture2D* source) {
		D3D11_TEXTURE2D_DESC desc{};
		source->GetDesc(&desc);
		metadata.width = desc.Width;
		metadata.height = desc.Height;
		metadata.sourceFormat = desc.Format;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		desc.CPUAccessFlags = 0;
		desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
		winrt::check_hresult(device->CreateTexture2D(&desc, nullptr, input.put()));
		winrt::check_hresult(input.as<IDXGIResource1>()->CreateSharedHandle(nullptr,
			DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, inputHandle.put()));
		const auto mutex = input.as<IDXGIKeyedMutex>();
		const HRESULT acquired = mutex->AcquireSync(0, 0);
		// WAIT_TIMEOUT is not a failed HRESULT.
		if (acquired != S_OK) winrt::throw_hresult(HRESULT_FROM_WIN32(acquired));
		context->CopyResource(input.get(), source);
		winrt::check_hresult(mutex->ReleaseSync(1));
		// Background consumers wait only this Win32 event, never a DXGI
		// object belonging to the single-threaded frontend device.
		winrt::check_hresult(device->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(readyFence.put())));
		if (!readyEvent.try_create(wil::EventOptions::ManualReset, nullptr)) winrt::throw_last_error();
		winrt::check_hresult(context->Signal(readyFence.get(), 1));
		winrt::check_hresult(readyFence->SetEventOnCompletion(1, readyEvent.get()));
		context->Flush();
	}
	ScreenshotFrameIdentity Identity() const noexcept {
		return { metadata.frameId, metadata.captureSequence, metadata.resourceGeneration };
	}
};

}
