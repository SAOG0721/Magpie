#include "pch.h"
#include "FrameTrace.h"
#include "AdaptivePresenter.h"
#include "DeviceResources.h"
#include "ReflexController.h"
#include "Logger.h"
#include "ScalingWindow.h"
#include "Win32Helper.h"

namespace Magpie {

static bool ShouldLogPresentationDiagnostic(uint32_t count) noexcept {
	return count <= 3 || count % 120 == 0;
}

bool AdaptivePresenter::_Initialize(HWND hwndAttach) noexcept {
	if (ScalingWindow::Get().Options().IsDirectFlipDisabled()) {
		// 禁用 DirectFlip 时始终使用 DirectComposition 呈现
		if (!_ResizeDCompVisual(hwndAttach)) {
			Logger::Get().Error("_ResizeDCompVisual 失败");
			return false;
		}

		_isDCompPresenting = true;
		Logger::Get().Info("Presentation path: DirectComposition (DirectFlip disabled)");
		return true;
	}

	const uint32_t bufferCount = _CalcBufferCount();
	_VRRPathAvailable(true);

	const SIZE rendererSize = Win32Helper::GetSizeOfRect(ScalingWindow::Get().RendererRect());
	DXGI_SWAP_CHAIN_DESC1 sd{
		.Width = (UINT)rendererSize.cx,
		.Height = (UINT)rendererSize.cy,
		.Format = ScalingWindow::Get().Options().IsHdrCompatibilityEnabled()
			? DXGI_FORMAT_R16G16B16A16_FLOAT
			: DXGI_FORMAT_R8G8B8A8_UNORM,
		.SampleDesc = {
			.Count = 1
		},
		.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT,
		.BufferCount = bufferCount,
#ifdef _DEBUG
		// 我们应确保两种渲染方式可以无缝切换，DXGI_SCALING_NONE 使错误更容易观察到
		.Scaling = DXGI_SCALING_NONE,
#else
		// 如果两种渲染方式无法无缝切换，DXGI_SCALING_STRETCH 使视觉变化尽可能小
		.Scaling = DXGI_SCALING_STRETCH,
#endif
		// 渲染每帧之前都会清空后缓冲区，因此无需 DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL
		.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD,
		.AlphaMode = DXGI_ALPHA_MODE_IGNORE,
		// VRR is opt-in; creation and resize preserve the same tearing capability.
		.Flags = _vrr.CreationFlags() | DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT
	};

	ID3D11Device5* d3dDevice = _deviceResources->GetD3DDevice();
	winrt::com_ptr<IDXGISwapChain1> dxgiSwapChain;
	HRESULT hr = _deviceResources->GetDXGIFactory()->CreateSwapChainForHwnd(
		d3dDevice,
		hwndAttach,
		&sd,
		nullptr,
		nullptr,
		dxgiSwapChain.put()
	);
	if (hr == DXGI_ERROR_INVALID_CALL && (sd.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING)) {
		_vrr.RejectCreation();
		sd.Flags &= ~UINT(DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING);
		dxgiSwapChain = nullptr;
		Logger::Get().ComWarn("VRR swap-chain creation rejected; retrying once without tearing", hr);
		hr = _deviceResources->GetDXGIFactory()->CreateSwapChainForHwnd(
			d3dDevice, hwndAttach, &sd, nullptr, nullptr, dxgiSwapChain.put());
	}
	if (FAILED(hr)) {
		Logger::Get().ComError("创建交换链失败", hr);
		return false;
	}
	// Keep the known immutable flags even if later descriptor probing fails.
	_vrr.Created(sd.Flags);
	_dxgiSwapChain = dxgiSwapChain.try_as<IDXGISwapChain4>();
	if (!_dxgiSwapChain) {
		Logger::Get().Error("获取 IDXGISwapChain2 失败");
		return false;
	}
	_BindVRRSwapChain(_dxgiSwapChain.get());
	if (ScalingWindow::Get().Options().IsHdrCompatibilityEnabled()) {
		hr = _dxgiSwapChain->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709);
		if (FAILED(hr)) {
			Logger::Get().ComError("设置 HDR 交换链色彩空间失败", hr);
			return false;
		}
	}

	const auto& options = ScalingWindow::Get().Options();
	uint32_t maximumFrameLatency = options.isFrontEdgeSyncEnabled && !options.IsBenchmarkMode()
		? 1u : bufferCount - 1;
	for (const EffectOption& effect : ScalingWindow::Get().Options().effects) {
		if (effect.name == "DLSSFG\\DLSS_FrameGeneration") {
			maximumFrameLatency = 1;
			break;
		}
	}
	// Bound driver-side queuing when the application already paces submissions.
	hr = _dxgiSwapChain->SetMaximumFrameLatency(maximumFrameLatency);
	if (FAILED(hr)) {
		Logger::Get().ComError("SetMaximumFrameLatency failed", hr);
		return false;
	}
	Logger::Get().Info(fmt::format("Swap-chain maximum frame latency: {}", maximumFrameLatency));

	_frameLatencyWaitableObject.reset(_dxgiSwapChain->GetFrameLatencyWaitableObject());
	if (!_frameLatencyWaitableObject) {
		Logger::Get().Error("GetFrameLatencyWaitableObject 失败");
		return false;
	}

	hr = _deviceResources->GetDXGIFactory()->MakeWindowAssociation(
		hwndAttach, DXGI_MWA_NO_ALT_ENTER);
	_vrr.AltEnterDisabled(SUCCEEDED(hr));
	_UpdateForegroundCompatibility();
	if (FAILED(hr)) {
		Logger::Get().ComError("MakeWindowAssociation 失败", hr);
	}

	hr = _dxgiSwapChain->GetBuffer(0, IID_PPV_ARGS(_backBuffer.put()));
	if (FAILED(hr)) {
		Logger::Get().ComError("获取后缓冲区失败", hr);
		return false;
	}

	hr = d3dDevice->CreateRenderTargetView(_backBuffer.get(), nullptr, _backBufferRtv.put());
	if (FAILED(hr)) {
		Logger::Get().ComError("CreateRenderTargetView 失败", hr);
		return false;
	}

	Logger::Get().Info("Presentation path: DXGI HWND flip-discard");
	return true;
}

void AdaptivePresenter::SetReflexController(ReflexController* controller) noexcept {
	_reflex = controller;
	if (_reflex) _reflex->SetPresentationAvailable(!_isDCompPresenting && !!_dxgiSwapChain);
}

void AdaptivePresenter::SetReflexFrame(uint64_t frameId, uint64_t presentId, bool generated) noexcept {
	if (_reflexFrameId != frameId || _reflexPresentId != presentId) CancelReflexRender();
	_reflexFrameId = frameId;
	_reflexPresentId = presentId;
	_reflexGenerated = generated;
}

void AdaptivePresenter::BeginReflexRender() noexcept {
	if (!_reflexRendering && !_isDCompPresenting && _reflex && _reflexFrameId && _reflexPresentId) {
		_reflex->FrontendRender(_reflexFrameId, _reflexPresentId, true);
		_reflexRendering = true;
	}
}

void AdaptivePresenter::CancelReflexRender() noexcept {
	if (_reflexRendering) {
		_reflex->FrontendRender(_reflexFrameId, _reflexPresentId, false);
		_reflexRendering = false;
	}
}

bool AdaptivePresenter::PrepareFrame() noexcept {
	_frameCapacityBusy = false;
	_TryRecoverSwapChain();
	if (HasFatalError()) return false;
	if (_reflex) _reflex->SetPresentationAvailable(!_isDCompPresenting && !!_dxgiSwapChain);
	if (_isDCompPresenting) return true;
	// FrameLatencyGate retains an acquired token across shared-slot retries and
	// the subsequent BeginFrame. No GPU work or Reflex render marker on timeout.
	const DWORD waitResult = _frameLatencyGate.TryAcquire(_frameLatencyWaitableObject.get());
	if (waitResult == WAIT_TIMEOUT) {
		_frameCapacityBusy = true;
		FrameTrace::Mark(FrameTrace::Event::CapacityBusy);
		return false;
	} else if (waitResult == WAIT_FAILED) {
		_Fail("Swap-chain frame-latency wait failed", HRESULT_FROM_WIN32(GetLastError()));
		return false;
	} else if (waitResult != WAIT_OBJECT_0) {
		_Fail("Unexpected swap-chain frame-latency wait result", E_UNEXPECTED);
		return false;
	}
	return true;
}

bool AdaptivePresenter::BeginFrame(
	winrt::com_ptr<ID3D11Texture2D>& frameTex,
	winrt::com_ptr<ID3D11RenderTargetView>& frameRtv,
	POINT& drawOffset
) noexcept {
	if (!PrepareFrame()) return false;
	if (_isDCompPresenting) {
		HRESULT hr = _dcompSurface->BeginDraw(nullptr, IID_PPV_ARGS(&frameTex), &drawOffset);
		if (FAILED(hr)) {
			Logger::Get().ComError("BeginDraw 失败", hr);
			_CheckDeviceFailure("DirectComposition BeginDraw", hr);
			return false;
		}

		hr = _deviceResources->GetD3DDevice()->CreateRenderTargetView(
			frameTex.get(), nullptr, frameRtv.put());
		if (FAILED(hr)) {
			Logger::Get().ComError("CreateRenderTargetView 失败", hr);
			_CheckDeviceFailure("DirectComposition CreateRenderTargetView", hr);
			// BeginDraw succeeded; close it even when the view could not be
			// created so the fallback remains drawable on the next attempt.
			const HRESULT endResult = _dcompSurface->EndDraw();
			if (FAILED(endResult)) _CheckDeviceFailure("DirectComposition EndDraw after view failure", endResult);
			return false;
		}
	} else {
		drawOffset = {};

		frameTex = _backBuffer;
		frameRtv = _backBufferRtv;
		BeginReflexRender();
	}
	
	return true;
}

bool AdaptivePresenter::WaitForFrameCapacity(DWORD timeout) noexcept {
	if (_isDCompPresenting || HasFatalError()) return false;
	DWORD result = WAIT_TIMEOUT;
	if (!_frameLatencyGate.Wait(_frameLatencyWaitableObject.get(), timeout, result)) return false;
	if (result == WAIT_FAILED) {
		_Fail("Swap-chain capacity event wait failed", HRESULT_FROM_WIN32(GetLastError()));
		return false;
	}
	return true;
}

bool AdaptivePresenter::EndFrame(bool waitForGpu) noexcept {
	HRESULT endDrawResult = S_OK;
	if (_isDCompPresenting) {
		endDrawResult = _dcompSurface->EndDraw();
		if (FAILED(endDrawResult)) {
			Logger::Get().ComError("DirectComposition EndDraw failed", endDrawResult);
			_CheckDeviceFailure("DirectComposition EndDraw", endDrawResult);
		}
	}

	if (waitForGpu || _isResized) {
		_isResized = false;

		// 下面两个调用用于减少调整窗口尺寸时的边缘闪烁。
		// 
		// 我们希望 DWM 绘制新的窗口框架时刚好合成新帧，但这不是我们能控制的，尤其是混合架构
		// 下需要在显卡间传输帧数据，无法预测 Present/Commit 后多久 DWM 能收到。我们只能尽
		// 可能为 DWM 合成新帧预留时间，这包括两个步骤：
		// 
		// 1. 首先等待渲染完成，确保新帧对 DWM 随时可用。
		// 2. 然后在新一轮合成开始时提交，这让 DWM 有更多时间合成新帧。
		// 
		// 目前看来除非像 UWP 一般有 DWM 协助，否则彻底摆脱闪烁是不可能的。
		// 
		// https://github.com/Blinue/Magpie/pull/1071#issuecomment-2718314731 讨论了 UWP
		// 调整尺寸的方法，测试表明可以彻底解决闪烁问题。不过它使用了很不稳定的私有接口，没有
		// 实用价值。

		// 等待渲染完成
		FrameTrace::Scope traceGpuWait(FrameTrace::Event::PresentGpuWait);
		_WaitForGpu();

		// 等待 DWM 开始合成新一帧
		Win32Helper::WaitForDwmComposition();
	}

	if (_isDCompPresenting) {
		FrameTrace::Scope traceCommit(FrameTrace::Event::DcompCommit);
		_lastSubmissionTime = std::chrono::steady_clock::now();
		const HRESULT commitResult = _dcompDevice->Commit();
		traceCommit.Data(commitResult);
		traceCommit.End();
		if (FAILED(commitResult)) {
			Logger::Get().ComError("DirectComposition Commit failed", commitResult);
			_CheckDeviceFailure("DirectComposition Commit", commitResult);
		}
		_lastPresentedFrameCount = SUCCEEDED(endDrawResult) && SUCCEEDED(commitResult) ? 1u : 0u;
		return SUCCEEDED(endDrawResult) && SUCCEEDED(commitResult);
	} else {
		// 两个垂直同步之间允许渲染数帧，SyncInterval = 0 只呈现最新的一帧，旧帧被丢弃
		const auto tracePresent = FrameTrace::Tick();
		_lastSubmissionTime = std::chrono::steady_clock::now();
		if (_reflexRendering) {
			_reflex->FrontendRender(_reflexFrameId, _reflexPresentId, false);
			_reflex->Present(_reflexFrameId, _reflexPresentId, _reflexGenerated, true);
		}
		const HRESULT presentResult = _PresentWithVRR(_dxgiSwapChain.get(), true);
		if (_reflexRendering) {
			_reflex->Present(_reflexFrameId, _reflexPresentId, _reflexGenerated, false);
			_reflexRendering = false;
		}
		FrameTrace::Presentation(tracePresent, FrameTrace::Tick(), presentResult,
			reinterpret_cast<uintptr_t>(_dxgiSwapChain.get()));
		_lastPresentedFrameCount = presentResult == S_OK ? 1u : 0u;
		if (presentResult == DXGI_STATUS_OCCLUDED) {
			++_presentOccludedCount;
			if (ShouldLogPresentationDiagnostic(_presentOccludedCount)) {
				const ScalingWindow& scalingWindow = ScalingWindow::Get();
				Logger::Get().Info(fmt::format(
					"Swap-chain Present is occluded: count={} visible={} firstFrame={}",
					_presentOccludedCount,
					IsWindowVisible(scalingWindow.Handle()) != FALSE,
					scalingWindow.IsFirstFramePending()));
			}
		} else if (FAILED(presentResult)) {
			_CheckDeviceFailure("DXGI Present", presentResult);
			++_presentFailureCount;
			if (ShouldLogPresentationDiagnostic(_presentFailureCount)) {
				const ScalingWindow& scalingWindow = ScalingWindow::Get();
				Logger::Get().ComError(fmt::format(
					"Swap-chain Present failed: count={} visible={} firstFrame={}",
					_presentFailureCount,
					IsWindowVisible(scalingWindow.Handle()) != FALSE,
					scalingWindow.IsFirstFramePending()), presentResult);
			}
		}
		_frameLatencyGate.Reset();

		// 丢弃渲染目标的内容
		_deviceResources->GetD3DDC()->DiscardView(_backBufferRtv.get());

		if (_isSwitchingToSwapChain) {
			if (presentResult == S_OK) {
				_TryCompleteSwapChainSwitch();
			} else {
				// OCCLUDED is accepted by EndFrame for initial window showing,
				// but it does not establish a replacement for the DComp image.
				_KeepDCompFallback("DXGI recovery Present did not produce an image", presentResult);
			}
		}

		return SUCCEEDED(presentResult);
	}
}

bool AdaptivePresenter::OnResize() noexcept {
	if (HasFatalError()) return false;
	const bool hadDCompContent = _isDCompPresenting || _isSwitchingToSwapChain;
	_nextRecoveryAttempt.reset();
	_recoveryAttempts = 0;
	_switchCompletionQuery = nullptr;
	_isSwitchingToSwapChain = false;
	if (_reflex) {
		CancelReflexRender();
		_reflex->SetPresentationAvailable(false);
	}
	_isResized = true;

	if (ScalingWindow::Get().IsResizingOrMoving() || !_dxgiSwapChain) {
		// 切换到 DirectComposition 呈现，失败则回落到交换链
		_isDCompPresenting = _ResizeDCompVisual();
		if (_isDCompPresenting) {
			_VRRPathAvailable(false);
			if (!hadDCompContent && _dxgiSwapChain) {
				Logger::Get().Info("Presentation path: temporary DirectComposition during resize/move");
			}
			return true;
		}

		Logger::Get().Error("_ResizeDCompVisual 失败");

		// 禁用 DirectFlip 时不存在交换链
		if (!_dxgiSwapChain) {
			return false;
		}
	}

	if (!_ResizeSwapChain()) {
		Logger::Get().Error("_ResizeSwapChain 失败");
		return false;
	}
	if (hadDCompContent) {
		_isDCompPresenting = false;
		_isSwitchingToSwapChain = true;
		_nextRecoveryAttempt = std::chrono::steady_clock::now();
	}
	_VRRPathAvailable(!_isDCompPresenting);
	
	return true;
}

void AdaptivePresenter::OnEndResize(bool& shouldRedraw) noexcept {
	shouldRedraw = false;
	if (!_isDCompPresenting || !_dxgiSwapChain || HasFatalError()) return;
	_recoveryAttempts = 0;
	_nextRecoveryAttempt = std::chrono::steady_clock::now();
	Logger::Get().Info("DXGI recovery scheduled after resize/move; retaining DirectComposition until a valid Present");
	_TryRecoverSwapChain();
	shouldRedraw = _isSwitchingToSwapChain && !HasFatalError();
}

bool AdaptivePresenter::_ResizeSwapChain(DWORD capacityTimeout) noexcept {
	assert(_dxgiSwapChain);
	_RefreshVRREnvironment(true);

	// ResizeBuffers retains this swap chain and its already acquired token.
	const DWORD capacity = _frameLatencyGate.TryAcquire(_frameLatencyWaitableObject.get(), capacityTimeout);
	if (capacity != WAIT_OBJECT_0) {
		if (capacity == WAIT_FAILED) {
			_Fail("Swap-chain resize capacity wait failed", HRESULT_FROM_WIN32(GetLastError()));
		} else if (capacity != WAIT_TIMEOUT) {
			_Fail("Unexpected swap-chain resize capacity wait result", E_UNEXPECTED);
		} else {
			_CheckDeviceFailure("Swap-chain resize capacity timeout", S_OK);
			Logger::Get().Info("DXGI resize deferred: frame capacity is busy");
		}
		return false;
	}

	// Failed/occluded Present may leave indirect RTV references bound. Every
	// resize attempt releases those as well as our owned buffer/view references.
	_deviceResources->GetD3DDC()->ClearState();
	_backBuffer = nullptr;
	_backBufferRtv = nullptr;
	_deviceResources->GetD3DDC()->Flush();

	const RECT& swapChainRect = ScalingWindow::Get().RendererRect();
	const SIZE swapChainSize = Win32Helper::GetSizeOfRect(swapChainRect);
	HRESULT hr = _dxgiSwapChain->ResizeBuffers(
		0,
		(UINT)swapChainSize.cx,
		(UINT)swapChainSize.cy,
		DXGI_FORMAT_UNKNOWN,
		_vrr.ResizeFlags()
	);
	if (FAILED(hr)) {
		Logger::Get().ComError("ResizeBuffers 失败", hr);
		_CheckDeviceFailure("ResizeBuffers", hr);
		return false;
	}

	hr = _dxgiSwapChain->GetBuffer(0, IID_PPV_ARGS(_backBuffer.put()));
	if (FAILED(hr)) {
		Logger::Get().ComError("获取后缓冲区失败", hr);
		_CheckDeviceFailure("GetBuffer after resize", hr);
		return false;
	}

	hr = _deviceResources->GetD3DDevice()->CreateRenderTargetView(
		_backBuffer.get(), nullptr, _backBufferRtv.put());
	if (FAILED(hr)) {
		Logger::Get().ComError("CreateRenderTargetView 失败", hr);
		_CheckDeviceFailure("CreateRenderTargetView after resize", hr);
		return false;
	}

	return true;
}

bool AdaptivePresenter::NeedsRedraw() const noexcept {
	return _nextRecoveryAttempt && !HasFatalError() &&
		!ScalingWindow::Get().IsResizingOrMoving() &&
		std::chrono::steady_clock::now() >= *_nextRecoveryAttempt;
}

void AdaptivePresenter::_TryRecoverSwapChain() noexcept {
	if (!NeedsRedraw()) return;
	if (_CheckDeviceFailure("DXGI recovery", S_OK)) return;
	// Eight attempts with zero-timeout capacity checks, separated by pump passes. Keep
	// rendering through DComp after exhaustion; a new resize starts a new cycle.
	if (_recoveryAttempts >= 8) {
		_nextRecoveryAttempt.reset();
		_KeepDCompFallback("DXGI recovery exhausted; staying on DirectComposition until the next resize or restart", S_FALSE);
		return;
	}
	++_recoveryAttempts;
	_nextRecoveryAttempt = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
	if (!_isDCompPresenting || !_ResizeSwapChain(0)) return;
	_isDCompPresenting = false;
	_isSwitchingToSwapChain = true;
	_VRRPathAvailable(true);
	Logger::Get().Info(fmt::format("DXGI recovery buffers ready: attempt={}; awaiting a valid Present", _recoveryAttempts));
}

void AdaptivePresenter::_TryCompleteSwapChainSwitch() noexcept {
	// A status-only event query replaces the old blocking GPU wait. Do not
	// restart it on every frame: it fences the first successful replacement.
	auto* context = _deviceResources->GetD3DDC();
	if (!_switchCompletionQuery) {
		const D3D11_QUERY_DESC desc{ D3D11_QUERY_EVENT, 0 };
		const HRESULT hr = _deviceResources->GetD3DDevice()->CreateQuery(&desc, _switchCompletionQuery.put());
		if (FAILED(hr)) {
			_KeepDCompFallback("DXGI recovery completion query creation failed", hr);
			return;
		}
		context->End(_switchCompletionQuery.get());
		context->Flush();
	}
	HRESULT hr = context->GetData(_switchCompletionQuery.get(), nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH);
	if (hr == S_FALSE) return;
	if (FAILED(hr)) {
		_KeepDCompFallback("DXGI recovery completion query failed", hr);
		return;
	}
	Win32Helper::WaitForDwmComposition();
	hr = _dcompVisual->SetContent(nullptr);
	if (SUCCEEDED(hr)) hr = _dcompDevice->Commit();
	if (FAILED(hr)) {
		// Undo the pending detach, too: a later DComp Commit must not expose a
		// half-completed transition after this Commit failed.
		const HRESULT restoreResult = _dcompVisual->SetContent(_dcompSurface.get());
		_KeepDCompFallback("DXGI recovery could not detach DirectComposition", hr);
		if (FAILED(restoreResult)) _Fail("DirectComposition fallback restoration failed", restoreResult);
		return;
	}
	_isSwitchingToSwapChain = false;
	_switchCompletionQuery = nullptr;
	_nextRecoveryAttempt.reset();
	Logger::Get().Info(fmt::format("Presentation path: DXGI restored after {} attempt(s); DirectComposition detached", _recoveryAttempts));
}

void AdaptivePresenter::_KeepDCompFallback(const char* context, HRESULT hr) noexcept {
	_CheckDeviceFailure(context, hr);
	_isDCompPresenting = true;
	_isSwitchingToSwapChain = false;
	_switchCompletionQuery = nullptr;
	_VRRPathAvailable(false);
	if (_reflex) _reflex->SetPresentationAvailable(false);
	Logger::Get().ComWarn(context, hr);
}

void AdaptivePresenter::_Fail(const char* context, HRESULT hr) noexcept {
	if (HasFatalError()) return;
	_failure.Record(context, static_cast<uint32_t>(hr));
	_nextRecoveryAttempt.reset();
	CancelReflexRender();
	Logger::Get().ComError(context, hr);
}

bool AdaptivePresenter::_CheckDeviceFailure(const char* context, HRESULT hr) noexcept {
	if (HasFatalError()) return true;
	const HRESULT removedReason = _deviceResources->GetD3DDevice()->GetDeviceRemovedReason();
	if (FAILED(removedReason)) {
		_Fail(context, removedReason);
	} else if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET ||
		hr == DXGI_ERROR_DEVICE_HUNG || hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR) {
		_Fail(context, hr);
	}
	return HasFatalError();
}

bool AdaptivePresenter::_ResizeDCompVisual(HWND hwndAttach) noexcept {
	const SIZE rendererSize = Win32Helper::GetSizeOfRect(ScalingWindow::Get().RendererRect());

	if (_dcompSurface) {
		// 使用 IDCompositionVirtualSurface 而不是 IDCompositionSurface 的原因是
		// IDCompositionDevice2::CreateSurface 有时相当慢，最坏情况下要几十毫秒。
		HRESULT hr = _dcompSurface->Resize((UINT)rendererSize.cx, (UINT)rendererSize.cy);
		if (FAILED(hr)) {
			Logger::Get().ComError("Resize 失败", hr);
			return false;
		}
	} else {
		// 初始化 DirectComposition
		HRESULT hr = DCompositionCreateDevice3(
			_deviceResources->GetD3DDevice(), IID_PPV_ARGS(&_dcompDevice));
		if (FAILED(hr)) {
			Logger::Get().ComError("DCompositionCreateDevice3 失败", hr);
			return false;
		}

		if (!hwndAttach) {
			// 没有禁用 DirectFlip 时才会在调整大小时初始化，因此必定存在交换链
			hr = _dxgiSwapChain->GetHwnd(&hwndAttach);
			if (FAILED(hr)) {
				Logger::Get().ComError("GetHwnd 失败", hr);
				return false;
			}
		}

		hr = _dcompDevice->CreateTargetForHwnd(hwndAttach, TRUE, _dcompTarget.put());
		if (FAILED(hr)) {
			Logger::Get().ComError("CreateTargetForHwnd 失败", hr);
			return false;
		}

		hr = _dcompDevice->CreateVisual(_dcompVisual.put());
		if (FAILED(hr)) {
			Logger::Get().ComError("CreateVisual 失败", hr);
			return false;
		}

		hr = _dcompTarget->SetRoot(_dcompVisual.get());
		if (FAILED(hr)) {
			Logger::Get().ComError("SetRoot 失败", hr);
			return false;
		}

		hr = _dcompDevice->CreateVirtualSurface(
			(UINT)rendererSize.cx,
			(UINT)rendererSize.cy,
			ScalingWindow::Get().Options().IsHdrCompatibilityEnabled()
				? DXGI_FORMAT_R16G16B16A16_FLOAT
				: DXGI_FORMAT_R8G8B8A8_UNORM,
			DXGI_ALPHA_MODE_IGNORE,
			_dcompSurface.put()
		);
		if (FAILED(hr)) {
			Logger::Get().ComError("CreateVirtualSurface 失败", hr);
			return false;
		}
	}

	HRESULT hr = _dcompVisual->SetContent(_dcompSurface.get());
	if (FAILED(hr)) {
		Logger::Get().ComError("SetContent 失败", hr);
		return false;
	}

	return true;
}

}
