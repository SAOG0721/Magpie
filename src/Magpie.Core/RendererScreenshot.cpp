#include "pch.h"
#include "Renderer.h"
#include "FrameSourceBase.h"
#include "ScalingWindow.h"
#include "SrcTracker.h"
#include "ScreenshotHelper.h"
#include "TextureHelper.h"
#include "StrHelper.h"
#include "Win32Helper.h"
#include "Logger.h"

using namespace std::chrono_literals;

namespace Magpie {

void Renderer::_QueueScreenshotCleanup(std::shared_ptr<SceneScreenshotRequest> request) noexcept {
	request->cleanupDispatcher.TryEnqueue([request] {
		auto& window = ScalingWindow::Get();
		if (!request->session->IsCurrent(ScalingWindow::RunId()) || !window || !window.TryGetRenderer()) return;
		auto& renderer = window.Renderer();
		request->output = nullptr;
		{
			std::lock_guard lock(request->inputMutex);
			request->input.reset();
		}
		renderer._backendThreadDispatcher.TryEnqueue([owner = &renderer, request] {
			if (owner->_backendScreenshotRequest == request) owner->_backendScreenshotRequest.reset();
		});
	});
}

void Renderer::_FailScreenshotRequest(const std::shared_ptr<SceneScreenshotRequest>& request,
	std::string_view stage, uint32_t code, bool pendingOnly) noexcept {
	if (pendingOnly) {
		if (!request->CancelPending()) return;
	} else if (request->state.exchange(SceneScreenshotState::Finished) == SceneScreenshotState::Finished) return;
	_QueueScreenshotCleanup(request);
	if (request->report) request->report(request->target, ScalingError::ScreenshotReadbackFailed,
		fmt::format("Screenshot request={} run={} / {} / {}", request->id,
			request->runId, StrHelper::UTF16ToUTF8(request->directory.native()), stage), code);
}

winrt::fire_and_forget Renderer::_WatchScreenshotRequest(std::shared_ptr<SceneScreenshotRequest> request) {
	co_await 10s;
	auto expected = SceneScreenshotState::Pending;
	if (request->state.compare_exchange_strong(expected, SceneScreenshotState::Finished)) {
		_QueueScreenshotCleanup(request);
		if (request->report) request->report(request->target, ScalingError::ScreenshotReadbackFailed,
			fmt::format("Screenshot request={} run={} / {} / waiting for input handoff or matching presented output timed out",
				request->id, request->runId, StrHelper::UTF16ToUTF8(request->directory.native())), WAIT_TIMEOUT);
	}
}

void Renderer::TakeDisplayedScreenshot() noexcept {
	if (IsStopping()) return;
	auto& window = ScalingWindow::Get();
	const auto& options = window.Options();
	if (_sceneScreenshotRequest && _sceneScreenshotRequest->state.load() != SceneScreenshotState::Finished) {
		if (options.showToast) options.showToast(window.SrcTracker().Handle(),
			window.GetLocalizedString(L"Message_ScreenshotBusy"));
		return;
	}
	auto request = std::make_shared<SceneScreenshotRequest>();
	request->id = ++_nextScreenshotRequestId;
	request->timestamp = ScreenshotHelper::CaptureTimestamp();
	request->runId = ScalingWindow::RunId();
	request->captureSequence = _activeCaptureSequence.load(std::memory_order_acquire);
	request->resourceGeneration = _activeResourceGeneration.load(std::memory_order_acquire);
	request->session = _sessionLifetime;
	request->cleanupDispatcher = ScalingWindow::Dispatcher();
	request->content = NormalizeScreenshotContent(static_cast<uint32_t>(options.screenshotContent));
	request->directory = options.screenshotsDir;
	request->report = options.reportErrorDetails;
	request->toast = options.showToast;
	request->target = window.SrcTracker().Handle();
	request->savedMessage = window.GetLocalizedString(L"Message_ScreenshotSaved");
	request->pairSavedMessage = window.GetLocalizedString(L"Message_ScreenshotPairSaved");
	request->partialMessage = window.GetLocalizedString(L"Message_ScreenshotPartialSaved");
	_sceneScreenshotRequest = request;
	Logger::Get().Info(fmt::format(
		"Screenshot requested: request={} run={} mode={} displayedFrame={} sequence={} generation={} generated={}",
		request->id, request->runId, static_cast<uint32_t>(request->content),
		_frontendPresentedFrameMetadata.frameId, request->captureSequence,
		request->resourceGeneration, _frontendPresentedFrameMetadata.generated));
	if (request->content != ScreenshotContent::InputOnly && !_frontendPresentedBaseValid) {
		_FailScreenshotRequest(request, "No successfully presented final scene available");
		return;
	}
	if (request->content == ScreenshotContent::OutputOnly) {
		winrt::com_ptr<ID3D11Device5> device;
		device.copy_from(_frontendResources.GetD3DDevice());
		winrt::com_ptr<ID3D11DeviceContext4> context;
		context.copy_from(_frontendResources.GetD3DDC());
		_SaveSceneScreenshot(request, std::move(device), std::move(context), ScalingWindow::Dispatcher(),
			_frontendPresentedBaseTexture, _frontendPresentedFrameMetadata);
		return;
	}
	try {
		if (request->content == ScreenshotContent::InputAndOutput) _FixScreenshotOutput(request);
	} catch (const winrt::hresult_error& error) {
		_FailScreenshotRequest(request, "Fix displayed screenshot output", static_cast<uint32_t>(error.code().value));
		return;
	}
	_WatchScreenshotRequest(request);
	if (!_backendThreadDispatcher.TryEnqueue([this, request] {
		if (request->session->IsStopping() || request->state.load() != SceneScreenshotState::Pending) return;
		_backendScreenshotRequest = request;
		// Service the retained capture directly. A screenshot is not a new
		// render, a temporal Evaluate, or an FG publication (even while paused).
		_SnapshotScreenshotInput();
	})) _FailScreenshotRequest(request, "Queue input snapshot", ERROR_CANCELLED);
}

void Renderer::_FixScreenshotOutput(const std::shared_ptr<SceneScreenshotRequest>& request) {
	D3D11_TEXTURE2D_DESC desc{};
	_frontendPresentedBaseTexture->GetDesc(&desc);
	winrt::com_ptr<ID3D11Texture2D> output;
	winrt::check_hresult(_frontendResources.GetD3DDevice()->CreateTexture2D(&desc, nullptr, output.put()));
	_frontendResources.GetD3DDC()->CopyResource(output.get(), _frontendPresentedBaseTexture.get());
	request->output = std::move(output);
	request->outputMetadata = _frontendPresentedFrameMetadata;
}

void Renderer::_TryCompleteSceneScreenshot() noexcept {
	const auto request = _sceneScreenshotRequest;
	if (!request || request->state.load() != SceneScreenshotState::Pending || IsStopping()) return;
	std::shared_ptr<SceneScreenshotFrame> input;
	{
		std::lock_guard lock(request->inputMutex);
		input = request->input;
	}
	if (!input) return;
	const auto& fixed = request->outputMetadata;
	const auto& presented = _frontendPresentedFrameMetadata;
	const auto selection = SelectScreenshotCandidate(request->content, input->Identity(),
		{ fixed.frameId, fixed.captureSequence, fixed.resourceGeneration }, fixed.generated,
		_frontendPresentedBaseValid ? ScreenshotFrameIdentity{ presented.frameId,
			presented.captureSequence, presented.resourceGeneration } : ScreenshotFrameIdentity{}, presented.generated);
	if (selection == ScreenshotSelection::Wait) return;
	if (selection == ScreenshotSelection::RetryInput) {
		{
			std::lock_guard lock(request->inputMutex);
			request->input.reset();
		}
		Logger::Get().Info(fmt::format("Screenshot request={} inputFrame={} skipped; presentedFrame={}, retry retained input",
			request->id, input->metadata.frameId, presented.frameId));
		if (!_backendThreadDispatcher.TryEnqueue([this, request] {
			if (_backendScreenshotRequest == request) _SnapshotScreenshotInput();
		})) _FailScreenshotRequest(request, "Queue newer input snapshot", ERROR_CANCELLED);
		return;
	}
	try {
		if (selection == ScreenshotSelection::PresentedOutput) _FixScreenshotOutput(request);
		winrt::com_ptr<ID3D11Device5> device;
		device.copy_from(_frontendResources.GetD3DDevice());
		winrt::com_ptr<ID3D11DeviceContext4> context;
		context.copy_from(_frontendResources.GetD3DDC());
		_SaveSceneScreenshot(request, std::move(device), std::move(context), ScalingWindow::Dispatcher(),
			request->output, request->content == ScreenshotContent::InputOnly ? input->metadata : request->outputMetadata, input);
	} catch (const winrt::hresult_error& error) {
		_FailScreenshotRequest(request, "Fix matching screenshot output", static_cast<uint32_t>(error.code().value));
	}
}

void Renderer::_SnapshotScreenshotInput() noexcept {
	const auto request = _backendScreenshotRequest;
	if (!request || request->state.load() != SceneScreenshotState::Pending || IsStopping()) {
		_backendScreenshotRequest.reset();
		return;
	}
	try {
		auto* source = _frameSource->GetPipelineTexture();
		if (!source || !_capturedFrameId || !_frameSource->IsHdrFrameReady() || _frameSource->IsCaptureInterrupted()) {
			_FailScreenshotRequest(request, fmt::format(
				"Retained input unavailable: frame={} source={} ready={} interrupted={}",
				_capturedFrameId, source != nullptr, _frameSource->IsHdrFrameReady(), _frameSource->IsCaptureInterrupted()), ERROR_CANCELLED, true);
			return;
		}
		if ((request->captureSequence != _captureSequence || _captureSequence != _frameSource->CaptureSequence()) ||
			(request->resourceGeneration != 0 && request->resourceGeneration != _frameSource->ResourceGeneration())) {
			_FailScreenshotRequest(request, fmt::format(
				"Capture epoch changed: requested={}/{} retained={}/{} sourceSequence={}",
				request->captureSequence, request->resourceGeneration, _captureSequence,
				_frameSource->ResourceGeneration(), _frameSource->CaptureSequence()), ERROR_CANCELLED, true);
			return;
		}
		{
			std::lock_guard lock(request->inputMutex);
			if (request->input) return;
		}
		auto frame = std::make_shared<SceneScreenshotFrame>();
		frame->requestId = request->id;
		frame->metadata = _frameSource->GetHdrFrameMetadata();
		frame->metadata.frameId = _capturedFrameId;
		frame->metadata.captureSequence = _captureSequence;
		frame->metadata.resourceGeneration = _frameSource->ResourceGeneration();
		frame->metadata.timestamp100ns = _acceptedCaptureTimestamp100ns;
		frame->metadata.generated = false;
		frame->metadata.valid = true;
		frame->metadata.stage = HdrFrameStage::CanonicalInput;
		frame->CopyInput(_backendResources.GetD3DDevice(), _backendResources.GetD3DDC(), source);
		Logger::Get().Info(fmt::format(
			"Screenshot input handed off: request={} frame={} sequence={} generation={} size={}x{}",
			request->id, frame->metadata.frameId, frame->metadata.captureSequence,
			frame->metadata.resourceGeneration, frame->metadata.width, frame->metadata.height));
		{
			std::lock_guard lock(request->inputMutex);
			if (request->state.load() != SceneScreenshotState::Pending) return;
			request->input = std::move(frame);
		}
		if (!request->cleanupDispatcher.TryEnqueue([request] {
			auto& window = ScalingWindow::Get();
			if (!request->session->IsCurrent(ScalingWindow::RunId()) || !window || !window.TryGetRenderer()) return;
			window.Renderer()._TryCompleteSceneScreenshot();
		})) _FailScreenshotRequest(request, "Queue screenshot handoff", ERROR_CANCELLED, true);
	} catch (const winrt::hresult_error& error) {
		_FailScreenshotRequest(request, "Create native-size input snapshot", static_cast<uint32_t>(error.code().value), true);
	} catch (...) { _FailScreenshotRequest(request, "Create native-size input snapshot", 0, true); }
}

winrt::fire_and_forget Renderer::_SaveSceneScreenshot(
	std::shared_ptr<SceneScreenshotRequest> request,
	winrt::com_ptr<ID3D11Device5> device,
	winrt::com_ptr<ID3D11DeviceContext4> context,
	winrt::DispatcherQueue dispatcher,
	winrt::com_ptr<ID3D11Texture2D> output, HdrFrameMetadata outputMetadata,
	std::shared_ptr<SceneScreenshotFrame> input) {
	auto expected = SceneScreenshotState::Pending;
	if (!request->state.compare_exchange_strong(expected, SceneScreenshotState::Reading)) co_return;
	const auto finished = wil::scope_exit([&] { request->state.store(SceneScreenshotState::Finished); });
	std::string stage = "Prepare screenshot";
	try {
		Logger::Get().Info(fmt::format(
			"Screenshot request={} run={} mode={} frame={} sequence={} generation={} generated={}",
			request->id, request->runId, static_cast<uint32_t>(request->content),
			outputMetadata.frameId, outputMetadata.captureSequence,
			outputMetadata.resourceGeneration, outputMetadata.generated));
		struct Image {
			std::wstring side;
			D3D11_TEXTURE2D_DESC desc{};
			ColorDescription color;
			winrt::com_ptr<ID3D11Texture2D> staging;
			std::vector<uint8_t> pixels;
			uint32_t rowPitch = 0;
		};
		std::vector<Image> images;
		auto snapshot = [&](ID3D11Texture2D* source, const ColorDescription& color, std::wstring side) {
			stage = StrHelper::UTF16ToUTF8(side) + " / Snapshot staging texture";
			if (!source) winrt::throw_hresult(E_PENDING);
			auto& image = images.emplace_back();
			image.side = std::move(side);
			image.color = color;
			source->GetDesc(&image.desc);
			image.desc.Usage = D3D11_USAGE_STAGING;
			image.desc.BindFlags = image.desc.MiscFlags = 0;
			image.desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			winrt::check_hresult(device->CreateTexture2D(&image.desc, nullptr, image.staging.put()));
			context->CopyResource(image.staging.get(), source);
		};
		if (request->content != ScreenshotContent::OutputOnly) {
			if (!input || input->requestId != request->id || !input->Identity().Matches(
				{ outputMetadata.frameId, outputMetadata.captureSequence, outputMetadata.resourceGeneration },
				outputMetadata.generated)) {
				_FailScreenshotRequest(request, "Input/output frame identity mismatch");
				co_return;
			}
			// The frontend device can be SINGLETHREADED. Only wait on the
			// producer's Win32 completion event off-thread; all DXGI/context
			// operations stay on the frontend dispatcher.
			co_await winrt::resume_background();
			const bool inputReady = input->readyEvent.wait(10000);
			co_await dispatcher;
			if (!inputReady) {
				_FailScreenshotRequest(request, "Input snapshot GPU completion timeout", WAIT_TIMEOUT);
				co_return;
			}
			if (request->session->IsStopping()) {
				_FailScreenshotRequest(request, "Scaling session stopped before input readback", ERROR_CANCELLED);
				co_return;
			}
			winrt::com_ptr<ID3D11Texture2D> original;
			stage = "input / Open shared native-size texture";
			winrt::check_hresult(device->OpenSharedResource1(input->inputHandle.get(), IID_PPV_ARGS(original.put())));
			auto mutex = original.as<IDXGIKeyedMutex>();
			const HRESULT acquired = mutex->AcquireSync(1, 0);
			if (acquired != S_OK) {
				_FailScreenshotRequest(request, "Acquire completed native input on frontend", static_cast<uint32_t>(acquired));
				co_return;
			}
			const auto release = wil::scope_exit([&] { mutex->ReleaseSync(2); });
			snapshot(original.get(), input->metadata.color, L"input");
		}
		if (request->content != ScreenshotContent::InputOnly)
			snapshot(output.get(), outputMetadata.color, L"output");
		// No Renderer, ScalingWindow, or mutable shared slot is touched by
		// readback/encoding. Both sides are immutable before the first suspension.
		winrt::com_ptr<ID3D11Fence> fence;
		stage = "input/output / Create and signal readback fence";
		winrt::check_hresult(device->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.put())));
		wil::unique_event_nothrow event;
		if (!event.try_create(wil::EventOptions::None, nullptr)) winrt::throw_last_error();
		winrt::check_hresult(context->Signal(fence.get(), 1));
		winrt::check_hresult(fence->SetEventOnCompletion(1, event.get()));
		context->Flush();
		_QueueScreenshotCleanup(request);
		co_await winrt::resume_background();
		const bool readbackReady = event.wait(10000);
		stage = "input/output / Resume readback dispatcher";
		co_await dispatcher;
		if (!readbackReady) { _FailScreenshotRequest(request, "GPU readback timeout", WAIT_TIMEOUT); co_return; }
		if (request->session->IsStopping()) {
			_FailScreenshotRequest(request, "Scaling session stopped during readback", ERROR_CANCELLED);
			co_return;
		}
		for (auto& image : images) {
			D3D11_MAPPED_SUBRESOURCE mapped{};
			const HRESULT mappedResult = context->Map(image.staging.get(), 0, D3D11_MAP_READ, 0, &mapped);
			if (FAILED(mappedResult)) {
				_FailScreenshotRequest(request, StrHelper::UTF16ToUTF8(image.side) + " / Map", static_cast<uint32_t>(mappedResult));
				co_return;
			}
			const auto unmap = wil::scope_exit([&] { context->Unmap(image.staging.get(), 0); });
			image.rowPitch = mapped.RowPitch;
			image.pixels.resize(size_t(mapped.RowPitch) * image.desc.Height);
			std::memcpy(image.pixels.data(), mapped.pData, image.pixels.size());
		}
		// Release frontend GPU objects on their owning dispatcher as well.
		for (auto& image : images) image.staging = nullptr;
		fence = nullptr;
		input.reset(); output = nullptr; context = nullptr; device = nullptr;
		co_await winrt::resume_background();
		std::lock_guard saveLock(ScreenshotHelper::SaveMutex());
		if (!Win32Helper::CreateDir(request->directory.c_str(), true)) {
			if (request->report) request->report(request->target, ScalingError::ScreenshotDirectoryFailed,
				fmt::format("Screenshot request={} run={} / {} / CreateDir", request->id,
					request->runId, StrHelper::UTF16ToUTF8(request->directory.native())), GetLastError());
			co_return;
		}
		const auto stem = ScreenshotHelper::FindUnusedScreenshotStem(request->directory, request->timestamp);
		if (stem.empty()) {
			if (request->report) request->report(request->target, ScalingError::ScreenshotDirectoryFailed,
				fmt::format("Screenshot request={} run={} / {} / Enumerate screenshot directory", request->id,
					request->runId, StrHelper::UTF16ToUTF8(request->directory.native())), 0);
			co_return;
		}
		std::vector<std::wstring> saved;
		std::string failures;
		std::wstring failedSide;
		ScalingError lastError = ScalingError::ScreenshotEncodeFailed;
		uint32_t lastCode = 0;
		for (auto& image : images) {
			const auto name = fmt::format(L"{}_{}.png", stem, image.side);
			const auto path = request->directory / name;
			TextureSaveError error;
			const bool hdr = image.color.transfer == HdrTransferFunction::Linear || image.color.transfer == HdrTransferFunction::PQ;
			if (TextureHelper::SaveTextureExclusive(path.c_str(), image.desc.Width, image.desc.Height,
				image.desc.Format, image.pixels, image.rowPitch, &error, hdr ? &image.color : nullptr)) {
				saved.push_back(name);
			} else {
				lastError = error.fileWriteFailed ? ScalingError::ScreenshotWriteFailed : ScalingError::ScreenshotEncodeFailed;
				lastCode = static_cast<uint32_t>(error.code);
				failedSide = image.side;
				failures += fmt::format("{} / {} / {} ({:#x}); ", StrHelper::UTF16ToUTF8(image.side),
					StrHelper::UTF16ToUTF8(path.native()), error.fileWriteFailed ? "write" : "encode", lastCode);
			}
		}
		Logger::Get().Info(fmt::format("Screenshot completed: request={} run={} saved={}/{}",
			request->id, request->runId, saved.size(), images.size()));
		if (!failures.empty() && request->report)
			request->report(request->target, lastError, fmt::format("Screenshot request={} run={} / saved={} / {}",
				request->id, request->runId, saved.size(), failures), lastCode);
		if (request->toast && !saved.empty()) {
			if (saved.size() == 2) request->toast(request->target,
				fmt::format(fmt::runtime(request->pairSavedMessage), saved[0], saved[1]));
			else if (images.size() == 2) request->toast(request->target,
				fmt::format(fmt::runtime(request->partialMessage), saved[0], failedSide));
			else request->toast(request->target, fmt::format(fmt::runtime(request->savedMessage), saved[0]));
		}
	} catch (const winrt::hresult_error& error) {
		_FailScreenshotRequest(request, stage, static_cast<uint32_t>(error.code().value));
	} catch (...) { _FailScreenshotRequest(request, stage); }
}

}
