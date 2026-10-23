#pragma once
#include "PresenterBase.h"
#include <dcomp.h>
#include "FramePacingWait.h"

namespace Magpie {

// 根据需要在交换链和 DirectComposition 两种呈现方式间切换。交换链可以触发
// DirectFlip/IndependentFlip 以最小化延迟，DirectComposition 在调整尺寸
// 时闪烁更少，这个呈现器旨在结合两者的优势。
class AdaptivePresenter final : public PresenterBase {
protected:
	bool _Initialize(HWND hwndAttach) noexcept override;

public:
	bool PrepareFrame() noexcept override;
	bool BeginFrame(
		winrt::com_ptr<ID3D11Texture2D>& frameTex,
		winrt::com_ptr<ID3D11RenderTargetView>& frameRtv,
		POINT& drawOffset
	) noexcept override;

	bool EndFrame(bool waitForGpu = false) noexcept override;
	ContentFrameCount LastContentFrameCount() const noexcept override {
		return { _lastPresentedFrameCount, ContentFrameCountSource::Reported,
			_lastPresentedFrameCount == 1u ? std::optional(_lastSubmissionTime) : std::nullopt };
	}
	void SetReflexController(ReflexController* controller) noexcept override;
	void SetReflexFrame(uint64_t frameId, uint64_t presentId, bool generated) noexcept override;
	void BeginReflexRender() noexcept override;
	void CancelReflexRender() noexcept override;
	bool WaitForFrameCapacity(DWORD timeout) noexcept override;
	bool WasFrameCapacityBusy() const noexcept override { return _frameCapacityBusy; }
	bool SupportsDeferredPresent() const noexcept override {
		return !_isDCompPresenting && !_isResized && !_isSwitchingToSwapChain;
	}

	bool UsesFrameLatencyWaitableObject() const noexcept override {
		return !_isDCompPresenting && !!_frameLatencyWaitableObject;
	}

	bool OnResize() noexcept override;

	void OnEndResize(bool& shouldRedraw) noexcept override;

private:
	bool _frameCapacityBusy = false;
	ReflexController* _reflex = nullptr;
	uint64_t _reflexFrameId = 0;
	uint64_t _reflexPresentId = 0;
	bool _reflexGenerated = false;
	bool _reflexRendering = false;
	bool _ResizeSwapChain() noexcept;

	bool _ResizeDCompVisual(HWND hwndAttach = NULL) noexcept;

	winrt::com_ptr<IDXGISwapChain4> _dxgiSwapChain;
	wil::unique_event_nothrow _frameLatencyWaitableObject;
	winrt::com_ptr<ID3D11Texture2D> _backBuffer;
	winrt::com_ptr<ID3D11RenderTargetView> _backBufferRtv;

	// 调整大小或禁用 DirectFlip 时使用
	winrt::com_ptr<IDCompositionDesktopDevice> _dcompDevice;
	winrt::com_ptr<IDCompositionTarget> _dcompTarget;
	winrt::com_ptr<IDCompositionVisual2> _dcompVisual;
	winrt::com_ptr<IDCompositionVirtualSurface> _dcompSurface;
	
	bool _isDCompPresenting = false;
	bool _isResized = false;
	FrameLatencyGate _frameLatencyGate;
	bool _isSwitchingToSwapChain = false;
	// 当前未被任何代码读取（等待超时计数未接线）；ClangCL -Werror、-Wunused-private-field 会报错
	[[maybe_unused]] uint32_t _frameLatencyWaitTimeoutCount = 0;
	uint32_t _presentOccludedCount = 0;
	uint32_t _presentFailureCount = 0;
};

}
