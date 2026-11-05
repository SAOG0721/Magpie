#pragma once
#include "FrameSourceBase.h"
#include "CaptureTargetLifetime.h"
#include "WgcMinUpdateIntervalPolicy.h"
#include <memory>
#include <ShlObj.h>
#include <Windows.Graphics.Capture.Interop.h>
#include <winrt/Windows.Graphics.Capture.h>

namespace Magpie {

// 使用 Window Runtime 的 Windows.Graphics.Capture API 抓取窗口
// 见 https://docs.microsoft.com/en-us/windows/uwp/audio-video-camera/screen-capture
class GraphicsCaptureFrameSource final : public FrameSourceBase {
public:
	virtual ~GraphicsCaptureFrameSource();

	bool Start() noexcept override;

	FrameSourceWaitType WaitType() const noexcept override {
		return FrameSourceWaitType::WaitForEvent;
	}
	HANDLE FrameArrivedEvent() const noexcept override;

	const char* Name() const noexcept override {
		return "Graphics Capture";
	}

	void OnCursorVisibilityChanged(bool isVisible, bool onDestory) noexcept override;

protected:
	bool _Initialize() noexcept override;
	ColorDescription _GetSourceColorDescription() const noexcept override;

	FrameSourceState _Update() noexcept override;

private:
	struct FrameReadySignal;
	std::shared_ptr<FrameReadySignal> _frameReady;
	winrt::event_token _frameArrivedToken{};
	winrt::event_token _itemClosedToken{};
	std::shared_ptr<CaptureTargetLifetime> _targetLifetime;

	bool _StartCapture(const char* reason) noexcept;
	void _ApplyMinUpdateInterval();

	bool _StopCapture() noexcept;
	void _InterruptCapture(const char* reason) noexcept;
	FrameSourceState _FailCapture(const char* operation, HRESULT hr) noexcept;
	void _FinishRecovery() noexcept;

	bool _CaptureWindow(IGraphicsCaptureItemInterop* interop) noexcept;

	bool _TryCreateGraphicsCaptureItem(IGraphicsCaptureItemInterop* interop) noexcept;

	D3D11_BOX _frameBox{};
	WgcMinUpdateIntervalPolicy _minUpdateIntervalPolicy;
	uint64_t _captureSessionGeneration = 0;
	uint64_t _rejectedFrames = 0;
	int64_t _lastFrameTimestamp100ns = 0;
	std::chrono::steady_clock::time_point _recoveryStarted{};
	std::chrono::steady_clock::time_point _lastRecoveryGeometryCheck{};
	UINT_PTR _recoveryTimer = 0;
	bool _captureFailed = false;
	// Backend-thread terminal state; distinct from a recoverable cursor restart.
	bool _captureStopping = false;

	winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice _wrappedD3DDevice{ nullptr };
	winrt::Windows::Graphics::Capture::GraphicsCaptureItem _captureItem{ nullptr };
	winrt::Windows::Graphics::Capture::GraphicsCaptureSession _captureSession{ nullptr };
	winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool _captureFramePool{ nullptr };

	winrt::com_ptr<ITaskbarList> _taskbarList;
	bool _isSrcStyleChanged = false;
};

}
