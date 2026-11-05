#include "pch.h"
#include "FrameTrace.h"
#include "GraphicsCaptureFrameSource.h"
#include "CommonSharedConstants.h"
#include "DeviceResources.h"
#include "DirectXHelper.h"
#include "Logger.h"
#include "ScalingWindow.h"
#include "StrHelper.h"
#include "Win32Helper.h"
#include <dwmapi.h>
#include <Windows.Graphics.DirectX.Direct3D11.interop.h>
#include <array>
#include <mutex>

namespace winrt {
using namespace Windows::Graphics;
using namespace Windows::Graphics::Capture;
using namespace Windows::Graphics::DirectX;
using namespace Windows::Graphics::DirectX::Direct3D11;
}

namespace Magpie {

// The callback owns only this signal, never the capture source or D3D context.
// A late callback after Close/restart cannot touch the new session or a freed
// object, and its handle stays alive until the callback returns.
struct GraphicsCaptureFrameSource::FrameReadySignal {
	wil::unique_event_nothrow event;
#ifdef MP_ENABLE_FRAME_TRACE
	bool traceEnabled = false;
	std::atomic<int64_t> firstNotification = 0;
	std::atomic<uint64_t> notifications = 0;
	struct Notification { int64_t tick = 0; uint64_t serial = 0; };
	// Only diagnostic timestamps cross the callback/owner boundary. The source
	// and immediate context remain owner-only. Overflow is explicit, never
	// silently mistaken for a complete delivery timeline.
	std::mutex notificationMutex;
	std::array<Notification, 1024> recentNotifications;
	size_t notificationCount = 0;
	uint64_t notificationSerial = 0, droppedNotifications = 0;
	bool notificationsClosed = false;
	void TraceNotification(int64_t tick) noexcept {
		std::lock_guard lock(notificationMutex);
		if (notificationsClosed) return;
		const auto serial = ++notificationSerial;
		if (notificationCount < recentNotifications.size()) recentNotifications[notificationCount++] = {tick, serial};
		else ++droppedNotifications;
	}
	void DrainNotifications(uint64_t generation, bool close = false) noexcept {
		if (!traceEnabled) return;
		std::array<Notification, 1024> batch;
		size_t count = 0;
		uint64_t dropped = 0;
		{
			std::lock_guard lock(notificationMutex);
			count = std::exchange(notificationCount, 0);
			std::copy_n(recentNotifications.begin(), count, batch.begin());
			dropped = std::exchange(droppedNotifications, 0);
			notificationsClosed |= close;
		}
		for (size_t i = 0; i < count; ++i) {
			const auto& sample = batch[i];
			// Frame zero: this is a delivery event, not an accepted-capture ID.
			FrameTrace::Record(FrameTrace::Event::WgcNotification, sample.tick, sample.tick,
				0, sample.serial, generation);
		}
		if (dropped) FrameTrace::Mark(FrameTrace::Event::WgcNotificationOverflow, dropped, generation);
	}
#endif
};

HANDLE GraphicsCaptureFrameSource::FrameArrivedEvent() const noexcept {
	return _frameReady ? _frameReady->event.get() : nullptr;
}

bool GraphicsCaptureFrameSource::_Initialize() noexcept {
	ID3D11Device5* d3dDevice = _deviceResources->GetD3DDevice();

	HRESULT hr;

	winrt::com_ptr<IGraphicsCaptureItemInterop> interop;
	try {
		if (!winrt::GraphicsCaptureSession::IsSupported()) {
			Logger::Get().Error("当前不支持 WinRT 捕获");
			return false;
		}

		winrt::com_ptr<IDXGIDevice> dxgiDevice;
		d3dDevice->QueryInterface<IDXGIDevice>(dxgiDevice.put());

		hr = CreateDirect3D11DeviceFromDXGIDevice(
			dxgiDevice.get(),
			reinterpret_cast<::IInspectable**>(winrt::put_abi(_wrappedD3DDevice))
		);
		if (FAILED(hr)) {
			Logger::Get().ComError("创建 IDirect3DDevice 失败", hr);
			return false;
		}

		// 从窗口句柄获取 GraphicsCaptureItem
		interop = winrt::get_activation_factory<winrt::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
		if (!interop) {
			Logger::Get().Error("获取 IGraphicsCaptureItemInterop 失败");
			return false;
		}
	} catch (const winrt::hresult_error& e) {
		Logger::Get().Error(StrHelper::Concat("初始化 WinRT 失败: ", StrHelper::UTF16ToUTF8(e.message())));
		return false;
	}

	if (!_CaptureWindow(interop.get())) {
		Logger::Get().Error("窗口捕获失败");
		return false;
	}

	_output = DirectXHelper::CreateTexture2D(
		d3dDevice,
		ScalingWindow::Get().Options().IsHdrCaptureEnabled()
			? DXGI_FORMAT_R16G16B16A16_FLOAT
			: DXGI_FORMAT_B8G8R8A8_UNORM,
		_frameBox.right - _frameBox.left,
		_frameBox.bottom - _frameBox.top,
		D3D11_BIND_SHADER_RESOURCE
	);
	if (!_output) {
		Logger::Get().Error("创建纹理失败");
		return false;
	}

	// Freeze the choice once per scaling session. A cursor-triggered restart
	// reapplies it without rereading the environment or retrying a failed zero.
	wchar_t intervalOverride[8]{};
	const DWORD overrideLength = GetEnvironmentVariableW(L"MAGPIE_WGC_MIN_UPDATE_INTERVAL_US",
		intervalOverride, DWORD(std::size(intervalOverride)));
	const std::wstring_view overrideValue = overrideLength < std::size(intervalOverride)
		? std::wstring_view(intervalOverride, overrideLength) : L"invalid";
	_minUpdateIntervalPolicy.Configure(ScalingWindow::Get().Options().isVRREnabled,
		Win32Helper::GetOSVersion() >= Version(10, 0, 26200), overrideValue);
	if (_minUpdateIntervalPolicy.InvalidOverride()) {
		Logger::Get().Warn("MAGPIE_WGC_MIN_UPDATE_INTERVAL_US accepts only 0 or 1000; ignoring invalid override");
	}

	Logger::Get().Info("GraphicsCaptureFrameSource 初始化完成");
	return true;
}

ColorDescription GraphicsCaptureFrameSource::_GetSourceColorDescription() const noexcept {
	ColorDescription result = FrameSourceBase::_GetSourceColorDescription();
	if (!ScalingWindow::Get().Options().IsHdrCaptureEnabled()) {
		return result;
	}

	// WGC's FP16 capture surface uses linear scRGB. The monitor metadata still
	// supplies the display peak used by normalization.
	result.dxgiColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
	result.primaries = HdrColorPrimaries::Rec709;
	result.transfer = HdrTransferFunction::Linear;
	result.range = HdrColorRange::SceneLinear;
	result.isSceneReferred = true;
	result.isInferred = true;
	result.preExposure = 1.0f;
	return result;
}

bool GraphicsCaptureFrameSource::Start() noexcept {
	_DisableRoundCornerInWin11();
	return _StartCapture("initial start");
}

FrameSourceState GraphicsCaptureFrameSource::_Update() noexcept {
	FrameTrace::Scope traceAcquire(FrameTrace::Event::WgcAcquire);
	if (_captureStopping) return FrameSourceState::Waiting;
	if (_captureFailed) return FrameSourceState::Error;
	if (_targetLifetime && _targetLifetime->IsClosed()) return _FailCapture("WGC capture target closed or replaced", RO_E_CLOSED);
	if (!_captureSession || !_captureFramePool) {
		return _FailCapture("WGC capture session unavailable", E_UNEXPECTED);
	}

	try {
		// Reset BEFORE inspecting the pool. Notifications concurrent with the
		// drain remain signaled, including a frame arriving after the final poll.
		// Resetting after the drain would lose that wakeup.
		ResetEvent(_frameReady->event.get());
#ifdef MP_ENABLE_FRAME_TRACE
		const auto notification = _frameReady->firstNotification.exchange(0);
		const auto notifications = _frameReady->notifications.exchange(0);
		if (notification) FrameTrace::Record(FrameTrace::Event::WgcNotificationWait,
			notification, FrameTrace::Tick(), FrameTrace::Frame(), notifications);
		_frameReady->DrainNotifications(_captureSessionGeneration);
#endif
		winrt::Direct3D11CaptureFrame frame = _captureFramePool.TryGetNextFrame();
		if (frame) {
			uint32_t dequeued = 1;
			// Drain at most the pool capacity, so a fast producer cannot keep us here.
			for (uint32_t i = 1; i < 4; ++i) {
				auto nextFrame = _captureFramePool.TryGetNextFrame();
				if (!nextFrame) break;
				frame.Close();
				frame = std::move(nextFrame);
				++dequeued;
			}
			FrameTrace::Mark(FrameTrace::Event::WgcDequeue, dequeued, _captureSessionGeneration);

			const auto content = frame.ContentSize();
			const int64_t timestamp = frame.SystemRelativeTime().count();
			FrameTrace::Mark(FrameTrace::Event::WgcFrame, timestamp, _captureSequence);
			const auto access = frame.Surface().as<
				::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
			winrt::com_ptr<ID3D11Texture2D> texture;
			winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(&texture)));
			D3D11_TEXTURE2D_DESC desc{};
			texture->GetDesc(&desc);
			// Content may legitimately exceed pool size. Only the copied crop must
			// fit both the valid content and the actual surface allocation.
			const bool valid = content.Width > 0 && content.Height > 0 &&
				_frameBox.left < _frameBox.right && _frameBox.top < _frameBox.bottom &&
				_frameBox.front == 0 && _frameBox.back == 1 &&
				_frameBox.right <= UINT(content.Width) && _frameBox.bottom <= UINT(content.Height) &&
				_frameBox.right <= desc.Width && _frameBox.bottom <= desc.Height;
			const bool staleTimestamp = timestamp <= 0 ||
				(_lastFrameTimestamp100ns && timestamp < _lastFrameTimestamp100ns);
			if (!valid || staleTimestamp) {
				FrameTrace::Mark(FrameTrace::Event::WgcRejected, !valid ? 1 : 2, timestamp);
				_InterruptCapture(!valid ? "invalid content bounds" : "non-advancing capture timestamp");
				if (++_rejectedFrames == 1) {
					Logger::Get().Warn(fmt::format(
						"WGC rejected frame: session={} sequence={} content={}x{} surface={}x{} crop={},{},{},{} timestamp100ns={} previousTimestamp100ns={}",
						_captureSessionGeneration, _captureSequence, content.Width, content.Height,
						desc.Width, desc.Height, _frameBox.left, _frameBox.top, _frameBox.right,
						_frameBox.bottom, timestamp, _lastFrameTimestamp100ns));
					// The frontend's existing SrcTracker path owns geometry changes and
					// effect-chain rebuilding. Wake it even when no frame can be published.
					PostMessage(ScalingWindow::Get().Handle(),
						CommonSharedConstants::WM_FRONTEND_RENDER, 0, 0);
					_lastRecoveryGeometryCheck = std::chrono::steady_clock::now();
				}
				frame.Close();
			} else if (timestamp == _lastFrameTimestamp100ns) {
				// WGC may repeat a timestamp, including after a screenshot. This is
				// not a discontinuity: preserve temporal history and wait for new input.
				// Fall through so an existing recovery still checks its deadline.
				frame.Close();
			} else {
				// Preserve the long-pause optimization, with a 5-second debounce.
				// WGC can legitimately skip hundreds of milliseconds for static or
				// throttled windows; those gaps must not reset FG history and flash.
				if (_lastFrameTimestamp100ns &&
					timestamp - _lastFrameTimestamp100ns >= 50'000'000) {
					_InterruptCapture("capture long-pause discontinuity");
				}
				_BeginCaptureRender();
				_deviceResources->GetD3DDC()->CopySubresourceRegion(
					_output.get(), 0, 0, 0, 0, texture.get(), 0, &_frameBox);
				frame.Close();
				_captureTimestamp100ns = timestamp;
				_lastFrameTimestamp100ns = timestamp;
				if (_captureInterrupted) {
					Logger::Get().Info(fmt::format(
						"WGC valid capture restored: session={} sequence={} content={}x{} surface={}x{} rejected={} timestamp100ns={} recoveryMs={:.3f}",
						_captureSessionGeneration, _captureSequence, content.Width, content.Height,
						desc.Width, desc.Height, _rejectedFrames, timestamp,
						std::chrono::duration<double, std::milli>(
							std::chrono::steady_clock::now() - _recoveryStarted).count()));
					_FinishRecovery();
				}
				return FrameSourceState::NewFrame;
			}
		}

		// Only an explicitly interrupted sequence has a deadline. A healthy static
		// window is allowed to provide no new frames indefinitely.
		if (_captureInterrupted && !_recoveryTimer) {
			return _FailCapture("WGC recovery timer creation", _captureErrorCode);
		}
		if (_captureInterrupted && std::chrono::steady_clock::now() - _lastRecoveryGeometryCheck >= 250ms) {
			// A transition frame can arrive before Win32 reports the new geometry.
			// Continue the existing frontend size/minimize checks while no output
			// is published (including when the minimum-FPS timer is overdue).
			PostMessage(ScalingWindow::Get().Handle(),
				CommonSharedConstants::WM_FRONTEND_RENDER, 0, 0);
			_lastRecoveryGeometryCheck = std::chrono::steady_clock::now();
		}
		if (_captureInterrupted && std::chrono::steady_clock::now() - _recoveryStarted >= 5s) {
			return _FailCapture(_rejectedFrames ? "WGC valid content/timestamp recovery timed out" :
				"WGC first frame after start/restart timed out", HRESULT_FROM_WIN32(ERROR_TIMEOUT));
		}
		return FrameSourceState::Waiting;
	} catch (const winrt::hresult_error& e) {
		return _FailCapture("WGC acquire/copy/close frame", e.code());
	}
}

void GraphicsCaptureFrameSource::OnCursorVisibilityChanged(bool isVisible, bool onDestory) noexcept {
	if (_captureStopping) return;
	if (onDestory) {
		_captureStopping = true;
		_FinishRecovery();
	}
	// 显示光标时必须重启捕获
	if (isVisible) {
		const bool stopped = _StopCapture();
		
		if (onDestory) {
			// FIXME: 这里尝试修复拖动窗口时光标不显示的问题，但有些环境下不起作用
			SystemParametersInfo(SPI_SETCURSORS, 0, nullptr, 0);
		} else {
			if (!stopped) {
				_FailCapture("WGC close before cursor restart", _captureErrorCode);
			} else if (!_captureFailed) {
				// _StartCapture records a persistent failure; _Update propagates it.
				_StartCapture("cursor visible");
			}
		}
	}
}

static bool CalcWindowCapturedFrameBounds(HWND hWnd, RECT& rect) noexcept {
	// Graphics Capture 的捕获区域没有文档记录，这里的计算是我实验了多种窗口后得出的，
	// 高度依赖实现细节，未来可能会失效。
	// Win10 和 Win11 24H2 开始捕获区域为 extended frame bounds；Win11 24H2 前
	// DwmGetWindowAttribute 对最大化的窗口返回值和 Win10 不同，可能是 OS 的 bug，
	// 应进一步处理。
	const auto& srcTracker = ScalingWindow::Get().SrcTracker();
	rect = srcTracker.WindowFrameRect();
	
	if (!srcTracker.IsZoomed() ||
		Win32Helper::GetOSVersion().IsWin10() ||
		Win32Helper::GetOSVersion().Is24H2OrNewer())
	{
		return true;
	}

	// 如果窗口禁用了非客户区域绘制则捕获区域为 extended frame bounds
	BOOL hasBorder = TRUE;
	HRESULT hr = DwmGetWindowAttribute(hWnd, DWMWA_NCRENDERING_ENABLED, &hasBorder, sizeof(hasBorder));
	if (FAILED(hr)) {
		Logger::Get().ComError("DwmGetWindowAttribute 失败", hr);
		return false;
	}

	if (!hasBorder) {
		return true;
	}

	RECT clientRect;
	if (!Win32Helper::GetClientScreenRect(hWnd, clientRect)) {
		Logger::Get().Error("GetClientScreenRect 失败");
		return false;
	}

	// 有些窗口最大化后有部分客户区在屏幕外，如 UWP 和资源管理器，它们的捕获区域
	// 是整个客户区。否则捕获区域不会超出屏幕
	HMONITOR hMon = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
	MONITORINFO mi{ .cbSize = sizeof(mi) };
	if (!GetMonitorInfo(hMon, &mi)) {
		Logger::Get().Win32Error("GetMonitorInfo 失败");
		return false;
	}

	if (clientRect.top < mi.rcWork.top) {
		rect = clientRect;
	} else {
		Win32Helper::IntersectRect(rect, rect, mi.rcWork);
	}

	return true;
}

// 部分使用 Kirikiri 引擎的游戏有着这样的架构: 游戏窗口并非顶级窗口，而是被一个零尺寸
// 的窗口所有。此时 Alt+Tab 列表中的窗口和任务栏图标实际上是所有者窗口，这会导致 WGC
// 捕获失败。我们特殊处理这类窗口。
static bool IsKirikiriWindow(HWND hwndSrc) noexcept {
	const HWND hwndOwner = GetWindowOwner(hwndSrc);
	if (!hwndOwner) {
		return false;
	}

	RECT ownerRect;
	if (!GetWindowRect(hwndOwner, &ownerRect)) {
		Logger::Get().Win32Error("GetWindowRect 失败");
		return false;
	}

	// 所有者窗口尺寸为零，而且是顶级窗口
	return ownerRect.left == ownerRect.right && ownerRect.top == ownerRect.bottom &&
		!GetWindowOwner(hwndOwner);
}

bool GraphicsCaptureFrameSource::_CaptureWindow(IGraphicsCaptureItemInterop* interop) noexcept {
	const SrcTracker& srcTracker = ScalingWindow::Get().SrcTracker();
	const HWND hwndSrc = srcTracker.Handle();
	const RECT& srcRect = srcTracker.SrcRect();

	RECT frameBounds;
	if (!CalcWindowCapturedFrameBounds(hwndSrc, frameBounds)) {
		Logger::Get().Error("CalcWindowCapturedFrameBounds 失败");
		return false;
	}

	if (srcRect.left < frameBounds.left || srcRect.top < frameBounds.top) {
		Logger::Get().Error("裁剪边框错误");
		return false;
	}

	// 在源窗口存在 DPI 缩放时有时会有一像素的偏移（取决于窗口在屏幕上的位置）
	// 可能是 DwmGetWindowAttribute 的 bug
	_frameBox = {
		UINT(srcRect.left - frameBounds.left),
		UINT(srcRect.top - frameBounds.top),
		0,
		UINT(srcRect.right - frameBounds.left),
		UINT(srcRect.bottom - frameBounds.top),
		1
	};

	const DWORD srcExStyle = GetWindowExStyle(hwndSrc);
	// WS_EX_APPWINDOW 样式使窗口始终在 Alt+Tab 列表中显示
	if (srcExStyle & WS_EX_APPWINDOW) {
		return _TryCreateGraphicsCaptureItem(interop);
	}

	const bool isSrcKirikiri = IsKirikiriWindow(hwndSrc);
	if (isSrcKirikiri) {
		Logger::Get().Info("源窗口有零尺寸的所有者窗口");
	} else {
		// 第一次尝试捕获。Kirikiri 窗口必定失败，无需尝试
		if (_TryCreateGraphicsCaptureItem(interop)) {
			return true;
		}
	}

	// 添加 WS_EX_APPWINDOW 样式
	if (!SetWindowLongPtr(hwndSrc, GWL_EXSTYLE, srcExStyle | WS_EX_APPWINDOW)) {
		Logger::Get().Win32Error("SetWindowLongPtr 失败");
		return false;
	}

	Logger::Get().Info("已改变源窗口样式");
	_isSrcStyleChanged = true;

	// Kirikiri 窗口改变样式后所有者窗口和游戏窗口将同时出现在 Alt+Tab 列表和任务栏中。
	// 虽然所有窗口都会如此，但 Kirikiri 的特殊之处在于两个窗口的图标和标题相同，为了不
	// 引起困惑应隐藏所有者窗口的图标。
	if (isSrcKirikiri) {
		_taskbarList = winrt::try_create_instance<ITaskbarList>(CLSID_TaskbarList);
		if (_taskbarList) {
			HRESULT hr = _taskbarList->HrInit();
			if (SUCCEEDED(hr)) {
				// 修正任务栏图标
				_taskbarList->DeleteTab(GetWindowOwner(hwndSrc));
				_taskbarList->AddTab(hwndSrc);

				// 修正 Alt+Tab 切换顺序
				if (Win32Helper::GetRealForegroundWindow() == hwndSrc) {
					SetForegroundWindow(GetDesktopWindow());
					SetForegroundWindow(hwndSrc);
				}
			} else {
				Logger::Get().ComError("ITaskbarList::HrInit 失败", hr);
				_taskbarList = nullptr;
			}
		} else {
			Logger::Get().Error("创建 ITaskbarList 失败");
		}
	}

	// 再次尝试捕获
	if (_TryCreateGraphicsCaptureItem(interop)) {
		return true;
	} else {
		if (_isSrcStyleChanged) {
			// 恢复源窗口样式
			SetWindowLongPtr(hwndSrc, GWL_EXSTYLE, srcExStyle);
		}
		return false;
	}
}

bool GraphicsCaptureFrameSource::_TryCreateGraphicsCaptureItem(IGraphicsCaptureItemInterop* interop) noexcept {
	try {
		HRESULT hr = interop->CreateForWindow(
			ScalingWindow::Get().SrcTracker().Handle(),
			winrt::guid_of<winrt::GraphicsCaptureItem>(),
			winrt::put_abi(_captureItem)
		);
		if (FAILED(hr)) {
			Logger::Get().ComError("创建 GraphicsCaptureItem 失败", hr);
			return false;
		}
		_targetLifetime = std::make_shared<CaptureTargetLifetime>(GetCurrentThreadId());
		_itemClosedToken = _captureItem.Closed([lifetime = _targetLifetime](const auto&, const auto&) noexcept {
			lifetime->Close();
		});
	} catch (const winrt::hresult_error& e) {
		Logger::Get().Info(StrHelper::Concat("源窗口无法使用窗口捕获: ", StrHelper::UTF16ToUTF8(e.message())));
		return false;
	}

	return true;
}

void GraphicsCaptureFrameSource::_ApplyMinUpdateInterval() {
	if (!winrt::ApiInformation::IsPropertyPresent(
		winrt::name_of<winrt::GraphicsCaptureSession>(), L"MinUpdateInterval")) {
		Logger::Get().Info(fmt::format(
			"WGC MinUpdateInterval: session={} supported=false requested100ns={} policy={}",
			_captureSessionGeneration, _minUpdateIntervalPolicy.Requested100ns(), _minUpdateIntervalPolicy.Reason()));
		return;
	}

	if (_minUpdateIntervalPolicy.Interval100ns() == 0) {
		const char* operation = "setter";
		try {
			_captureSession.MinUpdateInterval(0ms);
			operation = "getter";
			const int64_t readback = _captureSession.MinUpdateInterval().count();
			if (readback == 0) {
				Logger::Get().Info(fmt::format(
					"WGC MinUpdateInterval: session={} requested100ns=0 applied100ns=0 readback100ns=0 fallback=false policy={}",
					_captureSessionGeneration, _minUpdateIntervalPolicy.Reason()));
				return;
			}
			Logger::Get().Warn(fmt::format(
				"WGC MinUpdateInterval zero readback mismatch: session={} readback100ns={}; falling back to 1ms for this scaling session",
				_captureSessionGeneration, readback));
		} catch (const winrt::hresult_error& e) {
			Logger::Get().Warn(fmt::format(
				"WGC MinUpdateInterval zero {} failed: session={} HRESULT=0x{:08X}; falling back to 1ms for this scaling session",
				operation, _captureSessionGeneration, uint32_t(e.code())));
		}
		_minUpdateIntervalPolicy.FallBackToCompatibility();
	}

	// Retain the explicit 1ms compatibility setter used for affected 24H2
	// systems. Setter failure still follows the existing capture-start error.
	_captureSession.MinUpdateInterval(1ms);
	try {
		const int64_t readback = _captureSession.MinUpdateInterval().count();
		if (readback != WgcMinUpdateIntervalPolicy::Compatibility100ns) {
			Logger::Get().Warn(fmt::format(
				"WGC MinUpdateInterval compatibility readback mismatch: session={} requested100ns={} readback100ns={}",
				_captureSessionGeneration, WgcMinUpdateIntervalPolicy::Compatibility100ns, readback));
		}
		Logger::Get().Info(fmt::format(
			"WGC MinUpdateInterval: session={} requested100ns={} applied100ns={} readback100ns={} fallback={} policy={}",
			_captureSessionGeneration, _minUpdateIntervalPolicy.Requested100ns(),
			WgcMinUpdateIntervalPolicy::Compatibility100ns, readback,
			_minUpdateIntervalPolicy.FellBack(), _minUpdateIntervalPolicy.Reason()));
	} catch (const winrt::hresult_error& e) {
		// Readback is diagnostic for the unchanged compatibility path. An older
		// system that accepted 1ms must not acquire a new startup failure here.
		Logger::Get().Warn(fmt::format(
			"WGC MinUpdateInterval: session={} requested100ns={} applied100ns={} readback=unverified HRESULT=0x{:08X} fallback={} policy={}",
			_captureSessionGeneration, _minUpdateIntervalPolicy.Requested100ns(),
			WgcMinUpdateIntervalPolicy::Compatibility100ns, uint32_t(e.code()),
			_minUpdateIntervalPolicy.FellBack(), _minUpdateIntervalPolicy.Reason()));
	}
}

bool GraphicsCaptureFrameSource::_StartCapture(const char* reason) noexcept {
	FrameTrace::Scope traceStart(FrameTrace::Event::WgcStart);
	if (_targetLifetime && _targetLifetime->IsClosed()) {
		_FailCapture("WGC restart rejected after target closed", RO_E_CLOSED);
		return false;
	}
	if (_captureSession) {
		return true;
	}
	_InterruptCapture(reason);
	++_captureSessionGeneration;
	_lastFrameTimestamp100ns = 0;
	if (!_recoveryTimer) {
		_FailCapture("WGC recovery timer creation", _captureErrorCode);
		return false;
	}

	try {
		// 创建帧缓冲池。帧的尺寸和 _captureItem.Size() 不同
		_frameReady = std::make_shared<FrameReadySignal>();
		_frameReady->event.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
		if (!_frameReady->event) {
			_FailCapture("WGC frame notification event creation", HRESULT_FROM_WIN32(GetLastError()));
			return false;
		}
#ifdef MP_ENABLE_FRAME_TRACE
		_frameReady->traceEnabled = FrameTrace::Enabled();
#endif
		_captureFramePool = winrt::Direct3D11CaptureFramePool::CreateFreeThreaded(
			_wrappedD3DDevice,
			ScalingWindow::Get().Options().IsHdrCaptureEnabled()
				? winrt::DirectXPixelFormat::R16G16B16A16Float
				: winrt::DirectXPixelFormat::B8G8R8A8UIntNormalized,
			4,	// 帧的缓存数量，更大的值有利于在低帧率下降低延迟
			{ (int)_frameBox.right, (int)_frameBox.bottom } // 帧的尺寸为包含源窗口的最小尺寸
		);

		// Wake the backend directly, independently of DispatcherQueue dispatch.
		// Only the backend dequeues frames and submits D3D work.
		_frameArrivedToken = _captureFramePool.FrameArrived(
			[signal = _frameReady](const auto&, const auto&) noexcept {
#ifdef MP_ENABLE_FRAME_TRACE
				if (signal->traceEnabled) {
					LARGE_INTEGER now{};
					QueryPerformanceCounter(&now);
					int64_t empty = 0;
					signal->firstNotification.compare_exchange_strong(empty, now.QuadPart);
					signal->notifications.fetch_add(1);
					signal->TraceNotification(now.QuadPart);
				}
#endif
				SetEvent(signal->event.get());
			});

		_captureSession = _captureFramePool.CreateCaptureSession(_captureItem);

		// 禁止捕获光标。从 Win10 v2004 开始支持
		if (winrt::ApiInformation::IsPropertyPresent(
			winrt::name_of<winrt::GraphicsCaptureSession>(),
			L"IsCursorCaptureEnabled"
		)) {
			_captureSession.IsCursorCaptureEnabled(false);
		}

		// 不显示黄色边框，Win32 应用中无需请求权限。从 Win11 开始支持
		if (winrt::ApiInformation::IsPropertyPresent(
			winrt::name_of<winrt::GraphicsCaptureSession>(),
			L"IsBorderRequired"
		)) {
			_captureSession.IsBorderRequired(false);
		}

		_ApplyMinUpdateInterval();

		_captureSession.StartCapture();
	} catch (const winrt::hresult_error& e) {
		_FailCapture("WGC start/restart capture", e.code());
		return false;
	}

	Logger::Get().Info(fmt::format("WGC session started: session={} sequence={} reason={}",
		_captureSessionGeneration, _captureSequence, reason));
	Logger::Get().Info("WGC delivery: free-threaded frame notification + waitable event");
	return true;
}

bool GraphicsCaptureFrameSource::_StopCapture() noexcept {
	FrameTrace::Scope traceClose(FrameTrace::Event::WgcClose);
	// Detach first and attempt both closes even if one fails. Never leave a
	// partially created session looking like a healthy, empty frame pool.
	bool success = true;
	auto close = [&](auto object, const char* operation) {
		if (!object) return;
		try {
			object.Close();
		} catch (const winrt::hresult_error& e) {
			Logger::Get().ComError(operation, e.code());
			_captureErrorCode = e.code();
			success = false;
		}
	};
	auto pool = std::exchange(_captureFramePool, nullptr);
	if (pool && _frameArrivedToken.value) {
		pool.FrameArrived(std::exchange(_frameArrivedToken, {}));
	}
	close(std::exchange(_captureSession, nullptr), "WGC close capture session");
	close(std::move(pool), "WGC close frame pool");
#ifdef MP_ENABLE_FRAME_TRACE
	if (_frameReady) _frameReady->DrainNotifications(_captureSessionGeneration, true);
#endif
	_frameReady.reset();
	return success;
}

void GraphicsCaptureFrameSource::_InterruptCapture(const char* reason) noexcept {
	if (_captureInterrupted) return;
	_captureInterrupted = true;
	++_captureSequence;
	_rejectedFrames = 0;
	_recoveryStarted = std::chrono::steady_clock::now();
	// Wake the backend even before its first frame, when StepTimer uses WaitMessage.
	_recoveryTimer = SetTimer(nullptr, 0, 250, nullptr);
	if (!_recoveryTimer) {
		const DWORD error = GetLastError();
		_captureErrorCode = HRESULT_FROM_WIN32(error ? error : ERROR_NOT_ENOUGH_MEMORY);
	}
	Logger::Get().Info(fmt::format("WGC capture interrupted: session={} sequence={} reason={}",
		_captureSessionGeneration, _captureSequence, reason));
}

void GraphicsCaptureFrameSource::_FinishRecovery() noexcept {
	_captureInterrupted = false;
	if (_recoveryTimer) KillTimer(nullptr, std::exchange(_recoveryTimer, 0));
}

FrameSourceState GraphicsCaptureFrameSource::_FailCapture(const char* operation, HRESULT hr) noexcept {
	_captureFailed = true;
	_StopCapture();
	_FinishRecovery();
	_captureErrorContext = operation;
	_captureErrorCode = hr;
	Logger::Get().ComError(operation, hr);
	return FrameSourceState::Error;
}

GraphicsCaptureFrameSource::~GraphicsCaptureFrameSource() {
	if (_targetLifetime) _targetLifetime->Retire();
	if (_captureItem && _itemClosedToken.value) _captureItem.Closed(std::exchange(_itemClosedToken, {}));
	_StopCapture();
	_FinishRecovery();

	const HWND hwndSrc = ScalingWindow::Get().SrcTracker().Handle();

	// 还原源窗口样式
	if (_isSrcStyleChanged && ScalingWindow::Get().SourceIdentityValid()) {
		const DWORD srcExStyle = GetWindowExStyle(hwndSrc);
		SetWindowLongPtr(hwndSrc, GWL_EXSTYLE, srcExStyle & ~WS_EX_APPWINDOW);
	}

	// 还原 Kirikiri 窗口
	if (_taskbarList && ScalingWindow::Get().SourceIdentityValid()) {
		_taskbarList->DeleteTab(hwndSrc);
		_taskbarList->AddTab(GetWindowOwner(hwndSrc));

		// 修正任务栏焦点窗口和 Alt+Tab 切换顺序
		if (Win32Helper::GetRealForegroundWindow() == hwndSrc) {
			SetForegroundWindow(GetDesktopWindow());
			SetForegroundWindow(hwndSrc);
		}
	}
}

}
