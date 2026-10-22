#include "pch.h"
#include "SceneScreenshot.h"
#include <cassert>
#include <future>
#include <iostream>

using namespace Magpie;

static void CheckSelection() {
	const ScreenshotFrameIdentity paused{ 12, 0, 3 };
	// No successful Present event occurs after request dispatch: the independent
	// handoff still finishes against the output fixed before pausing.
	assert(SelectScreenshotCandidate(ScreenshotContent::InputAndOutput,
		paused, paused, false, {}, false) == ScreenshotSelection::FixedOutput);
	// InputOnly neither needs an output nor waits for a presentation.
	assert(SelectScreenshotCandidate(ScreenshotContent::InputOnly,
		paused, {}, true, {}, true) == ScreenshotSelection::Input);
	// Backend has advanced: never pair frame 13 with fixed output 12. Generated
	// output 13 is also insufficient; only successful real presentation 13 wins.
	const ScreenshotFrameIdentity ahead{ 13, 0, 3 };
	assert(SelectScreenshotCandidate(ScreenshotContent::InputAndOutput,
		ahead, paused, false, paused, false) == ScreenshotSelection::Wait);
	assert(SelectScreenshotCandidate(ScreenshotContent::InputAndOutput,
		ahead, paused, false, ahead, true) == ScreenshotSelection::Wait);
	assert(SelectScreenshotCandidate(ScreenshotContent::InputAndOutput,
		ahead, paused, false, ahead, false) == ScreenshotSelection::PresentedOutput);
	// Frame 13 was dropped before handoff: retry retained input 14, preserving
	// the request. It may complete while paused with no new capture/Present.
	const ScreenshotFrameIdentity newer{ 14, 0, 3 };
	assert(SelectScreenshotCandidate(ScreenshotContent::InputAndOutput,
		ahead, paused, false, newer, false) == ScreenshotSelection::RetryInput);
	assert(SelectScreenshotCandidate(ScreenshotContent::InputAndOutput,
		newer, paused, false, newer, false) == ScreenshotSelection::PresentedOutput);
	for (auto stale : { ScreenshotFrameIdentity{12, 1, 3}, ScreenshotFrameIdentity{12, 0, 4} })
		assert(SelectScreenshotCandidate(ScreenshotContent::InputAndOutput,
			paused, stale, false, stale, false) == ScreenshotSelection::Wait);
}

static void CheckPausedGpuHandoff(DXGI_FORMAT format, uint32_t pixelBytes) {
	winrt::com_ptr<ID3D11Device> backend, frontend;
	winrt::com_ptr<ID3D11DeviceContext> producer, consumer;
	winrt::check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
		nullptr, 0, D3D11_SDK_VERSION, backend.put(), nullptr, producer.put()));
	winrt::check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_SINGLETHREADED,
		nullptr, 0, D3D11_SDK_VERSION, frontend.put(), nullptr, consumer.put()));
	D3D11_TEXTURE2D_DESC desc{ 37, 19, 1, 1, format, {1, 0}, D3D11_USAGE_DEFAULT,
		D3D11_BIND_SHADER_RESOURCE, 0, 0 };
	std::vector<uint8_t> pixels(size_t(desc.Width) * desc.Height * pixelBytes);
	for (size_t i = 0; i < pixels.size(); ++i) pixels[i] = uint8_t(i * 37 + 5);
	D3D11_SUBRESOURCE_DATA data{ pixels.data(), desc.Width * pixelBytes, 0 };
	winrt::com_ptr<ID3D11Texture2D> source;
	winrt::check_hresult(backend->CreateTexture2D(&desc, &data, source.put()));
	SceneScreenshotFrame input;
	input.requestId = 7;
	input.metadata.frameId = 12;
	input.metadata.captureSequence = 0;
	input.metadata.resourceGeneration = 3;
	// Exercise the production snapshot code, with NO render, FG slot publication,
	// extra producer Flush or new capture following the request.
	input.CopyInput(backend.as<ID3D11Device5>().get(), producer.as<ID3D11DeviceContext4>().get(), source.get());
	assert(input.metadata.width == 37 && input.metadata.height == 19);
	assert(input.metadata.sourceFormat == format);
	assert(input.Identity().Matches({12, 0, 3}, false));
	// Background activity is restricted to Win32 events. Simultaneously submit
	// foreground context work on the SINGLETHREADED frontend, then consume the
	// production copy-completion event before any shared-resource DXGI access.
	wil::unique_event workerStarted, workerMayFinish;
	workerStarted.create(); workerMayFinish.create();
	auto ready = std::async(std::launch::async, [event = input.readyEvent.get(),
		started = workerStarted.get(), finish = workerMayFinish.get()] {
		SetEvent(started);
		const bool completed = WaitForSingleObject(event, 10000) == WAIT_OBJECT_0;
		assert(WaitForSingleObject(finish, 10000) == WAIT_OBJECT_0);
		return completed;
	});
	assert(workerStarted.wait(10000));
	winrt::com_ptr<ID3D11Texture2D> foregroundA, foregroundB;
	winrt::check_hresult(frontend->CreateTexture2D(&desc, &data, foregroundA.put()));
	winrt::check_hresult(frontend->CreateTexture2D(&desc, nullptr, foregroundB.put()));
	for (uint32_t i = 0; i < 128; ++i) consumer->CopyResource(foregroundB.get(), foregroundA.get());
	consumer->Flush();
	workerMayFinish.SetEvent();
	assert(ready.get());
	winrt::com_ptr<ID3D11Texture2D> shared;
	winrt::check_hresult(frontend.as<ID3D11Device1>()->OpenSharedResource1(
		input.inputHandle.get(), IID_PPV_ARGS(shared.put())));
	auto mutex = shared.as<IDXGIKeyedMutex>();
	assert(mutex->AcquireSync(1, 0) == S_OK);
	desc.Usage = D3D11_USAGE_STAGING;
	desc.BindFlags = 0;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	winrt::com_ptr<ID3D11Texture2D> staging;
	winrt::check_hresult(frontend->CreateTexture2D(&desc, nullptr, staging.put()));
	consumer->CopyResource(staging.get(), shared.get());
	winrt::check_hresult(mutex->ReleaseSync(2));
	winrt::com_ptr<ID3D11Fence> fence;
	winrt::check_hresult(frontend.as<ID3D11Device5>()->CreateFence(0,
		D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.put())));
	wil::unique_event event;
	event.create();
	winrt::check_hresult(consumer.as<ID3D11DeviceContext4>()->Signal(fence.get(), 1));
	winrt::check_hresult(fence->SetEventOnCompletion(1, event.get()));
	consumer->Flush();
	assert(event.wait(10000));
	D3D11_MAPPED_SUBRESOURCE mapped{};
	winrt::check_hresult(consumer->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped));
	for (uint32_t row = 0; row < desc.Height; ++row)
		assert(std::memcmp(static_cast<const uint8_t*>(mapped.pData) + row * mapped.RowPitch,
			pixels.data() + row * desc.Width * pixelBytes, desc.Width * pixelBytes) == 0);
	consumer->Unmap(staging.get(), 0);
}

static void CheckPendingCancellation() {
	SceneScreenshotRequest request;
	// A stale Pending observation cannot cancel a save that already claimed
	// immutable input/output, reopen Busy admission, or emit a false failure.
	assert(request.state.load() == SceneScreenshotState::Pending);
	request.state.store(SceneScreenshotState::Reading);
	assert(!request.CancelPending());
	assert(request.state.load() == SceneScreenshotState::Reading);
	request.state.store(SceneScreenshotState::Pending);
	assert(request.CancelPending());
	assert(!request.CancelPending());
	for (uint32_t attempt = 0; attempt < 256; ++attempt) {
		request.state.store(SceneScreenshotState::Pending);
		std::atomic_bool start = false;
		auto cancel = std::async(std::launch::async, [&] {
			while (!start.load()) std::this_thread::yield();
			return request.CancelPending();
		});
		start.store(true);
		auto expected = SceneScreenshotState::Pending;
		const bool reading = request.state.compare_exchange_strong(expected, SceneScreenshotState::Reading);
		const bool cancelled = cancel.get();
		assert(reading != cancelled);
		assert(request.state.load() == (reading ? SceneScreenshotState::Reading : SceneScreenshotState::Finished));
	}
}

int main() {
	winrt::init_apartment(winrt::apartment_type::multi_threaded);
	CheckSelection();
	CheckPendingCancellation();
	CheckPausedGpuHandoff(DXGI_FORMAT_B8G8R8A8_UNORM, 4);
	CheckPausedGpuHandoff(DXGI_FORMAT_R16G16B16A16_FLOAT, 8);
	std::cout << "PASS: paused independent production input copy across two WARP devices, native SDR/FP16 pixels, SINGLETHREADED frontend with concurrent event wait, fence/ownership, pairing/drop/epoch and cancellation races\n";
}
