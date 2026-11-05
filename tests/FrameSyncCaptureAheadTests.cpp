#include <d3d11_4.h>
#include <d3d11sdklayers.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>
#include "FramePacingOptions.h"
#include "FramePresentationTiming.h"

using Microsoft::WRL::ComPtr;
static unsigned checks = 0;
static void Check(bool passed, const char* message) {
	++checks;
	if (!passed) { std::cerr << "FAILED: " << message << '\n'; std::exit(42); }
}
static void Hr(HRESULT result) {
	if (FAILED(result)) std::cerr << "HRESULT=" << std::hex << result << std::dec << '\n';
	Check(SUCCEEDED(result), "D3D operation");
}

namespace Magpie {
struct TextureRef {
	ComPtr<ID3D11Texture2D> texture;
	void copy_from(ID3D11Texture2D* value) { texture = value; }
	ID3D11Texture2D* get() const { return texture.Get(); }
	explicit operator bool() const { return texture != nullptr; }
	TextureRef& operator=(std::nullptr_t) { texture.Reset(); return *this; }
};
struct WarpDevice {
	ComPtr<ID3D11Device5> device;
	ComPtr<ID3D11DeviceContext4> dc;
	ComPtr<ID3D11InfoQueue> debug;
	ComPtr<ID3D11Fence> fence;
	HANDLE completed = nullptr;
	uint64_t fenceValue = 0;
	WarpDevice() {
		ComPtr<ID3D11Device> base;
		ComPtr<ID3D11DeviceContext> context;
		Hr(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_DEBUG,
			nullptr, 0, D3D11_SDK_VERSION, &base, nullptr, &context));
		Hr(base.As(&device)); Hr(context.As(&dc)); Hr(device.As(&debug));
		Hr(device->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
		completed = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		Check(completed != nullptr, "fence event created");
	}
	~WarpDevice() { if (completed) CloseHandle(completed); }
	ID3D11DeviceContext4* GetD3DDC() const { return dc.Get(); }
	void Drain() {
		Hr(dc->Signal(fence.Get(), ++fenceValue));
		Hr(fence->SetEventOnCompletion(fenceValue, completed));
		dc->Flush();
		Check(WaitForSingleObject(completed, 10'000) == WAIT_OBJECT_0, "publication GPU fence completes");
	}
	ComPtr<ID3D11Texture2D> Texture(bool shared = false, bool staging = false) {
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = desc.Height = 4;
		desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
		desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		desc.Usage = staging ? D3D11_USAGE_STAGING : D3D11_USAGE_DEFAULT;
		desc.BindFlags = staging ? 0 : D3D11_BIND_SHADER_RESOURCE;
		desc.CPUAccessFlags = staging ? D3D11_CPU_ACCESS_READ : 0;
		desc.MiscFlags = shared ? D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX : 0;
		ComPtr<ID3D11Texture2D> texture;
		Hr(device->CreateTexture2D(&desc, nullptr, &texture));
		return texture;
	}
	void Verify(ID3D11Texture2D* image, uint32_t expected) {
		auto readback = Texture(false, true);
		dc->CopyResource(readback.Get(), image);
		D3D11_MAPPED_SUBRESOURCE mapped{};
		Hr(dc->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped));
		for (unsigned y = 0; y < 4; ++y) for (unsigned x = 0; x < 4; ++x) {
			uint32_t pixel = 0;
			std::memcpy(&pixel, static_cast<const char*>(mapped.pData) + y * mapped.RowPitch + x * 4, 4);
			Check(pixel == expected, "shared/staged/front pixels retain exact frame identity");
		}
		dc->Unmap(readback.Get(), 0);
	}
	void CheckDebug() {
		for (UINT64 i = 0; i < debug->GetNumStoredMessages(); ++i) {
			SIZE_T size = 0; Hr(debug->GetMessage(i, nullptr, &size));
			std::vector<char> bytes(size);
			auto* message = reinterpret_cast<D3D11_MESSAGE*>(bytes.data());
			Hr(debug->GetMessage(i, message, &size));
			if (message->Severity <= D3D11_MESSAGE_SEVERITY_WARNING) {
				std::cerr << message->pDescription << '\n';
				Check(false, "D3D debug warning/error");
			}
		}
	}
};

struct Identity {
	uint64_t frame = 0, previous = 0, sequence = 0, generation = 0, history = 0, reflex = 0;
	int64_t timestamp = 0;
	FrameUpdateReason reason = FrameUpdateReason::NewCapture;
	bool operator==(const Identity&) const = default;
};
struct Effect { std::string name; };
struct FrameGeneration { bool enabled = false; bool HasFrameGeneration() const { return enabled; } };
struct Presenter { bool deferred = true; bool SupportsDeferredPresent() const { return deferred; } };
struct Renderer {
	bool _frameSyncEnabled = true, _frameSyncUsesSharedSlot = true;
	bool _frameSyncCaptureAheadEnabled = false, _backendMayDeferFG = true;
	FrameSyncBackend _frameSyncBackend = FrameSyncBackend::FrontEdge;
	TextureRef _pendingFrameSyncOutput;
	bool _pendingFrameSyncHasPhysicalCapture = false;
	std::atomic<uint64_t> _frameSyncAcknowledgedKey = 0;
	std::array<std::atomic<uint64_t>, 1> _sharedTextureMutexKeys{};
	std::atomic<bool> _backendContentInFlight = false;
	WarpDevice _backendResources, frontend;
	ComPtr<ID3D11Texture2D> effectOutput, shared, opened, frontendBase;
	ComPtr<IDXGIKeyedMutex> producerMutex, consumerMutex;
	Presenter presenter;
	Presenter* _presenter = &presenter;
	uint64_t _captureSequence = 7, copiedKey = 0;
	unsigned accepted = 0, publications = 0, successfulPresents = 0, failedPresents = 0;
	Identity _processedFrame{}, published{}, copied{};
	HANDLE consumed = nullptr;
	Renderer() {
		consumed = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		Check(consumed != nullptr, "consumption event created");
		effectOutput = _backendResources.Texture();
		shared = _backendResources.Texture(true);
		Hr(shared.As(&producerMutex));
		ComPtr<IDXGIResource> resource;
		Hr(shared.As(&resource)); HANDLE handle = nullptr; Hr(resource->GetSharedHandle(&handle));
		Hr(frontend.device->OpenSharedResource(handle, IID_PPV_ARGS(&opened)));
		Hr(opened.As(&consumerMutex));
		frontendBase = frontend.Texture();
		Configure({}, {{"DLSSNR\\DLSSNR_AI_Filter"}});
	}
	~Renderer() { if (consumed) CloseHandle(consumed); }
#include "FrameSyncCaptureAhead.members.h"
	bool _StageFrameSyncOutput(ID3D11Texture2D*, bool) noexcept;
	bool _PublishPendingFrameSyncOutput() noexcept;
	void Configure(const FrameGeneration&, const std::vector<Effect>&);
	void _CompleteBackendFrame(ID3D11Texture2D*, bool, uint64_t) noexcept;
	bool Process(uint64_t id, bool physical = true, FrameUpdateReason reason = FrameUpdateReason::NewCapture) {
		if (_FrameSyncBlocksCapture()) return false;
		Check(!_pendingFrameSyncOutput, "a second capture cannot overwrite one staged output");
		++accepted;
		_processedFrame = {id, id - 1, _captureSequence, 11, id * 3, id * 17,
			static_cast<int64_t>(id * 100'000), reason};
		std::array<uint32_t, 16> pixels; pixels.fill(static_cast<uint32_t>(id));
		_backendResources.dc->UpdateSubresource(effectOutput.Get(), 0, nullptr, pixels.data(), 16, 0);
		_backendContentInFlight.store(true);
		if (!_StageFrameSyncOutput(effectOutput.Get(), physical)) {
			_CompleteBackendFrame(effectOutput.Get(), physical, _captureSequence);
			_backendContentInFlight.store(false);
		}
		return true;
	}
	void Consume();
	void Present(bool success) {
		if (success) {
			++successfulPresents;
			_frameSyncAcknowledgedKey.store(copiedKey, std::memory_order_release);
			SetEvent(consumed);
		} else ++failedPresents;
	}
	void VerifyShared(uint32_t id);
	void CancelForResizeOrStop() {
		_backendResources.Drain();
#include "FrameSyncCaptureAhead.cancel.inl"
	}
};
#include "FrameSyncCaptureAhead.production.inl"

void Renderer::_CompleteBackendFrame(ID3D11Texture2D* output, bool, uint64_t sequence) noexcept {
	Check(!_IsFrameSyncPublicationPending(), "publication retains original Present acknowledgement");
	Check(sequence == _processedFrame.sequence, "staged source sequence retained");
	const auto current = _sharedTextureMutexKeys[0].load();
	const std::array<IDXGIKeyedMutex*, 3> mutexes{producerMutex.Get(), nullptr, nullptr};
	Hr(AcquirePresentationTextures(mutexes, current, 1000));
	_backendResources.dc->CopyResource(shared.Get(), output);
	published = _processedFrame;
	Hr(ReleasePresentationTextures(mutexes, current + 1));
	_sharedTextureMutexKeys[0].store(current + 1, std::memory_order_release);
	_backendResources.Drain();
	++publications;
}
void Renderer::Consume() {
	const auto current = _sharedTextureMutexKeys[0].load();
	const std::array<IDXGIKeyedMutex*, 3> mutexes{consumerMutex.Get(), nullptr, nullptr};
	Hr(AcquirePresentationTextures(mutexes, current, 1000));
	frontend.dc->CopyResource(frontendBase.Get(), opened.Get());
	copied = published;
	Hr(ReleasePresentationTextures(mutexes, current + 1));
	copiedKey = current + 1;
	_sharedTextureMutexKeys[0].store(copiedKey, std::memory_order_release);
	frontend.Verify(frontendBase.Get(), static_cast<uint32_t>(copied.frame));
}
void Renderer::VerifyShared(uint32_t id) {
	const auto current = _sharedTextureMutexKeys[0].load();
	const std::array<IDXGIKeyedMutex*, 3> mutexes{producerMutex.Get(), nullptr, nullptr};
	Hr(AcquirePresentationTextures(mutexes, current, 1000));
	_backendResources.Verify(shared.Get(), id);
	Hr(ReleasePresentationTextures(mutexes, current));
}
}

int main() {
	using namespace Magpie;
	for (int fps : {60, 90, 120}) {
		Renderer render;
		Check(render._frameSyncCaptureAheadEnabled, "ordinary NR FrontEdge enables bounded preparation");
		FrontEdgeSyncClock clock;
		const auto period = std::chrono::nanoseconds(1'000'000'000 / fps);
		clock.SetInterval(period);
		auto now = FrontEdgeSyncClock::Clock::time_point(std::chrono::seconds(1));
		clock.Submitted(now);
		Check(render.Process(1), "initial image accepted");
		for (uint64_t current = 1; current <= 24; ++current) {
			render.Consume();
			Check(render.copied.frame == current, "frontend receives publication order");
			const auto old = render.copied;
			const auto beforePresent = render.successfulPresents;
			Check(render.Process(current + 1), "next capture is prepared before previous Present");
			Check(render._pendingFrameSyncOutput && render._backendContentInFlight.load(), "one staged output retains backend work");
			const auto prepared = render._processedFrame;
			for (unsigned poll = 0; poll < 8; ++poll) {
				Check(!render.Process(current + 2), "bounded prefetch blocks all further captures");
				Check(!render._PublishPendingFrameSyncOutput(), "shared publication awaits Present acknowledgement");
				Check(render._processedFrame == prepared, "staged metadata remains immutable");
			}
			render.VerifyShared(static_cast<uint32_t>(current));
			render.frontend.Verify(render.frontendBase.Get(), static_cast<uint32_t>(current));
			render._backendResources.Verify(render._pendingFrameSyncOutput.get(), static_cast<uint32_t>(current + 1));
			Check(render.copied == old && render.successfulPresents == beforePresent, "preparation changes no frontend identity or Present counters");
			if (current % 5 == 0) {
				render.Present(false);
				Check(render._IsFrameSyncPublicationPending() && !render._PublishPendingFrameSyncOutput(), "failed Present cannot consume or replace the queued image");
				Check(render.successfulPresents == beforePresent, "failed Present does not advance content clock");
			}
			const auto due = clock.Due(now);
			Check(now + std::chrono::milliseconds(2) < due, "preparation can overlap the original target deadline");
			now = due;
			render.Present(true);
			clock.Submitted(now);
			Check(clock.Due(now) - now == period, "successful Present preserves target maxFPS cadence");
			Check(render._PublishPendingFrameSyncOutput(), "acknowledged slot publishes prepared output");
			Check(!render._pendingFrameSyncOutput && !render._backendContentInFlight.load(), "drained output releases staged ownership");
			Check(render.published == prepared, "source/time/history/reflex/update reason match staged pixels");
			render.VerifyShared(static_cast<uint32_t>(current + 1));
		}
		Check(render.accepted == 25 && render.publications == 25 && render.successfulPresents == 24, "capture/publication/Present counters stay distinct and bounded");
		// A completion wake is serviced once; no message remains to spin the owner.
		ResetEvent(render.consumed);
		MSG message{}; PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE);
		Check(PostThreadMessageW(GetCurrentThreadId(), WM_NULL, 0, 0) != 0, "completion wake posted");
		Check(MsgWaitForMultipleObjectsEx(1, &render.consumed, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE) == WAIT_OBJECT_0 + 1, "pending output still services completion messages");
		while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
		Check(MsgWaitForMultipleObjectsEx(1, &render.consumed, 1, QS_ALLINPUT, MWMO_INPUTAVAILABLE) == WAIT_TIMEOUT, "drained completion wake returns to blocking wait");
		Check(render.Process(26, false, FrameUpdateReason::QualityRefresh), "same-capture work uses the same bounded slot");
		Check(!render._pendingFrameSyncHasPhysicalCapture, "quality refresh retains non-capture identity");
		render.effectOutput.Reset();
		render._backendResources.Verify(render._pendingFrameSyncOutput.get(), 26);
		render.CancelForResizeOrStop();
		Check(!render._pendingFrameSyncOutput && !render._pendingFrameSyncHasPhysicalCapture && !render._backendContentInFlight.load(), "resize/stop drains and discards retained output without acknowledgement");
		render._backendResources.CheckDebug(); render.frontend.CheckDebug();
	}
	Renderer policy;
	for (auto backend : {FrameSyncBackend::None, FrameSyncBackend::FrontEdge, FrameSyncBackend::Async, FrameSyncBackend::Reflex, FrameSyncBackend::XeLL}) {
		for (bool fg : {false, true}) for (bool deferred : {false, true}) for (bool nr : {false, true}) {
			policy._frameSyncBackend = backend;
			policy.presenter.deferred = deferred;
			policy.Configure({fg}, {{nr ? "DLSSNR\\DLSSNR_AI_Filter" : "CAS\\CAS"}});
			Check(policy._frameSyncCaptureAheadEnabled == (backend == FrameSyncBackend::FrontEdge && !fg && deferred && nr), "FG/Reflex/Async/unrelated effects preserve existing admission");
		}
	}
	policy._frameSyncCaptureAheadEnabled = false;
	Check(policy.Process(1), "legacy initial publication");
	Check(policy._FrameSyncBlocksCapture() && !policy.Process(2), "legacy path still waits before accepting another capture");
	policy._backendResources.CheckDebug(); policy.frontend.CheckDebug();
	std::cout << "PASS: " << checks << " CPU/WARP checks; two software D3D11 devices, real shared keyed mutexes/copies/fences, one staged image, failed Present, 60/90/120 cadence, wake waits, resize/stop and mode isolation\n";
}
