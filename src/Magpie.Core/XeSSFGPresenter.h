#pragma once
#include "PresenterBase.h"
#include "ScalingOptions.h"

namespace Magpie {

enum class XeSSFGVariant : uint8_t {
	X2,
	MultiFrame
};

// XeSS-FG is exposed as an effect, but Intel's API generates frames inside a
// D3D12 proxy swap chain. This presenter bridges Magpie's D3D11 frontend into
// that swap chain while providing flat depth and optional optical-flow motion.
class XeSSFGPresenter final : public PresenterBase {
public:
	struct Impl;

	explicit XeSSFGPresenter(
		XeSSFGVariant variant = XeSSFGVariant::X2,
		uint32_t requestedMultiplier = 2,
		bool useExternalMotion = false
	);
	~XeSSFGPresenter() noexcept override;

	bool BeginFrame(
		winrt::com_ptr<ID3D11Texture2D>& frameTex,
		winrt::com_ptr<ID3D11RenderTargetView>& frameRtv,
		POINT& drawOffset
	) noexcept override;

	bool BeginIdleFrame(winrt::com_ptr<ID3D11Texture2D>& frameTex,
		winrt::com_ptr<ID3D11RenderTargetView>& frameRtv, POINT& drawOffset) noexcept override;
	bool EndFrame(bool waitForGpu = false) noexcept override;
	void SetProfilerEnabled(bool enabled) noexcept override;
	PresentationIntervals TakePresentationIntervals() noexcept override;
	bool SetBaseFrameRateLimit(double baseFPS) noexcept override;
	bool HasFatalError() const noexcept override;
	const PresenterFailure* GetFailure() const noexcept override;
	bool WaitForFrameCapacity(DWORD timeout) noexcept override;
	void SetSourceTiming(uint64_t frameId, uint64_t sequence, uint64_t generation, int64_t timestamp100ns) noexcept override;
	void SetFrameGuidance(
		ID3D11Texture2D* motion,
		FrameGuidanceFrameId frameId,
		bool requiresHistoryReset,
		const RECT& destinationRect
	) noexcept override;
	bool HasIndependentOverlay() const noexcept override;
	bool BeginOverlayFrame(
		winrt::com_ptr<ID3D11Texture2D>& frameTex,
		winrt::com_ptr<ID3D11RenderTargetView>& frameRtv,
		POINT& drawOffset
	) noexcept override;
	bool EndOverlayFrame() noexcept override;

	bool UsesFrameLatencyWaitableObject() const noexcept override {
		return true;
	}

	bool OnResize() noexcept override;
	ScalingError InitializationError() const noexcept {
		return _initializationError;
	}

protected:
	bool _Initialize(HWND hwndAttach) noexcept override;

private:
	bool _BeginFrame(winrt::com_ptr<ID3D11Texture2D>& frameTex,
		winrt::com_ptr<ID3D11RenderTargetView>& frameRtv, POINT& drawOffset, bool idle) noexcept;
	bool _ResizeOverlaySurface() noexcept;

	// 仅在 MP_ENABLE_XESS_FRAME_GENERATION 构建中使用；无 SDK 的 CI 构建里 ClangCL -Werror 会报未使用
	[[maybe_unused]] std::unique_ptr<Impl> _impl;
	[[maybe_unused]] XeSSFGVariant _variant = XeSSFGVariant::X2;
	[[maybe_unused]] uint32_t _requestedMultiplier = 2;
	[[maybe_unused]] bool _useExternalMotion = false;
	ScalingError _initializationError = ScalingError::ScalingFailedGeneral;
};

}
