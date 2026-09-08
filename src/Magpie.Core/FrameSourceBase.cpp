#include "pch.h"
#include "FrameTrace.h"
#include "FrameSourceBase.h"
#include "BackendDescriptorStore.h"
#include "DeviceResources.h"
#include "DirectXHelper.h"
#include "Logger.h"
#include "Win32Helper.h"
#include "HdrDiagnostics.h"
#include "ScalingOptions.h"
#include "ScalingWindow.h"
#include "shaders/DuplicateFrameCS.h"
#include "Win32Helper.h"
#include <dwmapi.h>

namespace Magpie {

static constexpr uint16_t INITIAL_CHECK_COUNT = 16;
static constexpr uint16_t INITIAL_SKIP_COUNT = 1;
static constexpr uint16_t MAX_SKIP_COUNT = 16;

FrameSourceBase::FrameSourceBase() noexcept :
	_nextSkipCount(INITIAL_SKIP_COUNT), _framesLeft(INITIAL_CHECK_COUNT) {}

FrameSourceBase::~FrameSourceBase() noexcept {
	const HWND hwndSrc = ScalingWindow::Get().SrcTracker().Handle();

	// 还原窗口圆角
	if (_roundCornerDisabled) {
		_roundCornerDisabled = false;

		INT attr = DWMWCP_DEFAULT;
		HRESULT hr = DwmSetWindowAttribute(
			hwndSrc, DWMWA_WINDOW_CORNER_PREFERENCE, &attr, sizeof(attr));
		if (FAILED(hr)) {
			Logger::Get().ComError("取消禁用窗口圆角失败", hr);
		} else {
			Logger::Get().Info("已取消禁用窗口圆角");
		}
	}
}

bool FrameSourceBase::Initialize(DeviceResources& deviceResources, BackendDescriptorStore& descriptorStore) noexcept {
	_deviceResources = &deviceResources;
	_descriptorStore = &descriptorStore;

	if (!_Initialize()) {
		Logger::Get().Error("_Initialize 失败");
		return false;
	}

	_hdrEnabled = ScalingWindow::Get().Options().IsHdrCompatibilityEnabled();
	if (_hdrEnabled &&
		!_hdrProcessor.Initialize(deviceResources, descriptorStore)) {
		Logger::Get().Error("初始化 HDR 捕获处理器失败");
		return false;
	}
	if (_hdrEnabled) {
		D3D11_TEXTURE2D_DESC sourceDesc{};
		_output->GetDesc(&sourceDesc);
		if (!_hdrProcessor.Prepare(
			_output.get(), sourceDesc.Format, _GetSourceColorDescription())) {
			Logger::Get().Error("准备 HDR 捕获输出失败");
			return false;
		}
	}

	assert(_output);
	_outputSrv = descriptorStore.GetShaderResourceView(_output.get());
	if (!_outputSrv) {
		Logger::Get().Error("GetShaderResourceView 失败");
		return false;
	}

	return true;
}

FrameSourceState FrameSourceBase::Update() noexcept {
	const FrameSourceState state = _Update();
	if (state == FrameSourceState::NewFrame && _hdrEnabled) {
		if (_hdrFrameSequence != _captureSequence) {
			++_resourceGeneration;
			_hdrFrameSequence = _captureSequence;
		}
		D3D11_TEXTURE2D_DESC sourceDesc{};
		_output->GetDesc(&sourceDesc);
		ColorDescription sourceColor = _GetSourceColorDescription();
		if (!_hdrProcessor.Process(_output.get(), HdrFrameMetadata{
				.frameId = _captureSequence,
				.captureSequence = _captureSequence,
				.resourceGeneration = _resourceGeneration,
				.timestamp100ns = _captureTimestamp100ns,
				.width = sourceDesc.Width,
				.height = sourceDesc.Height,
				.sourceFormat = sourceDesc.Format,
				.color = sourceColor,
				.stage = HdrFrameStage::RawCapture,
				.valid = true
		})) {
			_hdrFrameReady = false;
			Logger::Get().Error("HDR 捕获帧处理失败");
			return FrameSourceState::Error;
		} else {
			_hdrFrameReady = true;
			if (!_hdrDiagnosticsLogged) {
				HdrDiagnostics diagnostics{};
				diagnostics.hdrOptionEnabled = true;
				diagnostics.captureMethod = Name();
				diagnostics.sourceFormat = sourceDesc.Format;
				diagnostics.sourceColorDescription = _hdrProcessor.GetFrameMetadata().color;
				diagnostics.selectedAdapterProfile = HdrAdapterProfile::DirectFP16;
				diagnostics.conversionPath = "capture->canonicalFP16";
				if (!_hdrProcessor.LastAssumption().empty()) AppendHdrAssumption(diagnostics, _hdrProcessor.LastAssumption());
				LogHdrDiagnostics(diagnostics, true);
				_hdrDiagnosticsLogged = true;
			}
		}
	}
	if (_hdrEnabled && state == FrameSourceState::NewFrame && !_hdrFrameReady) {
		return FrameSourceState::Error;
	}
	const bool newSequence = state == FrameSourceState::NewFrame &&
		_duplicateCaptureSequence != _captureSequence;
	if (newSequence) {
		_duplicateCaptureSequence = _captureSequence;
		_isCheckingForDuplicateFrame = true;
		_framesLeft = INITIAL_CHECK_COUNT;
		_nextSkipCount = INITIAL_SKIP_COUNT;
		if (_prevFrame) {
			_deviceResources->GetD3DDC()->CopyResource(_prevFrame.get(), _output.get());
		}
	}

	const ScalingOptions& options = ScalingWindow::Get().Options();
	const auto duplicateFrameDetectionMode = options.duplicateFrameDetectionMode;
	if (state != FrameSourceState::NewFrame || (newSequence && _prevFrame) || (!_forceDuplicateFrameDetection &&
		(options.Is3DGameMode() ||
			duplicateFrameDetectionMode == DuplicateFrameDetectionMode::Never))) {
		return state;
	}

	ID3D11DeviceContext4* d3dDC = _deviceResources->GetD3DDC();

	if (!_prevFrame) {
		if (_InitCheckingForDuplicateFrame()) {
			d3dDC->CopyResource(_prevFrame.get(), _output.get());
		} else {
			Logger::Get().Error("_InitCheckingForDuplicateFrame 失败");
			_prevFrame = nullptr;
			_prevFrameSrv = nullptr;
		}

		return FrameSourceState::NewFrame;
	}

	if (_forceDuplicateFrameDetection ||
		duplicateFrameDetectionMode == DuplicateFrameDetectionMode::Always) {
		// 总是检查重复帧
		if (_IsDuplicateFrame()) {
			return FrameSourceState::Waiting;
		} else {
			d3dDC->CopyResource(_prevFrame.get(), _output.get());
			return FrameSourceState::NewFrame;
		}
	}

	///////////////////////////////////////////////
	//
	// 动态检查重复帧，见 #787
	//
	///////////////////////////////////////////////

	const bool isStatisticsEnabled = options.IsStatisticsForDynamicDetectionEnabled();

	if (_isCheckingForDuplicateFrame) {
		if (--_framesLeft == 0) {
			_isCheckingForDuplicateFrame = false;
			_framesLeft = _nextSkipCount;
			if (_nextSkipCount < MAX_SKIP_COUNT) {
				// 增加下一次连续跳过检查的帧数
				++_nextSkipCount;
			}
		}

		if (_IsDuplicateFrame()) {
			_isCheckingForDuplicateFrame = true;
			_framesLeft = INITIAL_CHECK_COUNT;
			_nextSkipCount = INITIAL_SKIP_COUNT;
			return FrameSourceState::Waiting;
		} else {
			if (_isCheckingForDuplicateFrame || isStatisticsEnabled) {
				d3dDC->CopyResource(_prevFrame.get(), _output.get());
			}
			return FrameSourceState::NewFrame;
		}
	} else {
		if (--_framesLeft == 0) {
			_isCheckingForDuplicateFrame = true;
			// 第 2 次连续检查 10 帧，之后逐渐减少，从第 16 次开始只连续检查 2 帧
			_framesLeft = uint32_t((-4 * (int)_nextSkipCount + 78) / 7);
			
			if (!isStatisticsEnabled) {
				// 下一帧将检查重复帧，需要复制此帧
				d3dDC->CopyResource(_prevFrame.get(), _output.get());
			}
		}

		if (isStatisticsEnabled) {
			const bool isDuplicate = _IsDuplicateFrame();
			if (!isDuplicate) {
				d3dDC->CopyResource(_prevFrame.get(), _output.get());
			}

			std::pair<uint32_t, uint32_t> statistics = _statistics.load(std::memory_order_relaxed);
			if (isDuplicate) {
				// 预测错误
				++statistics.first;
			}
			// 总帧数
			++statistics.second;
			_statistics.store(statistics, std::memory_order_relaxed);
		}

		return FrameSourceState::NewFrame;
	}
}

ColorDescription FrameSourceBase::_GetSourceColorDescription() const noexcept {
	ColorDescription result{};
	if (!_deviceResources) return result;

	const HMONITOR monitor = MonitorFromWindow(
		ScalingWindow::Get().SrcTracker().Handle(), MONITOR_DEFAULTTONEAREST);
	if (!monitor) return result;

	IDXGIAdapter4* adapter = _deviceResources->GetGraphicsAdapter();
	if (!adapter) return result;

	for (UINT index = 0; ; ++index) {
		winrt::com_ptr<IDXGIOutput> output;
		if (FAILED(adapter->EnumOutputs(index, output.put()))) break;

		DXGI_OUTPUT_DESC outputDesc{};
		if (FAILED(output->GetDesc(&outputDesc)) || outputDesc.Monitor != monitor) continue;

		winrt::com_ptr<IDXGIOutput6> output6 = output.try_as<IDXGIOutput6>();
		if (!output6) return result;

		DXGI_OUTPUT_DESC1 desc1{};
		if (FAILED(output6->GetDesc1(&desc1))) return result;

		result.dxgiColorSpace = desc1.ColorSpace;
		result.displayHdrEnabled =
			desc1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709 ||
			desc1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
		const bool measuredPeak = desc1.MaxLuminance > 0.0f;
		result.displayPeakNits = measuredPeak ? desc1.MaxLuminance : 1000.0f;
		const float measuredSdrWhite = Win32Helper::GetMonitorSdrWhiteNits(monitor);
		// Some HDR monitors report MaxLuminance at or below the SDR white
		// level (observed: 80 nit with a 360-nit SDR white). The bounded
		// route derives its normalized peak from displayPeak/sdrWhite, so
		// a peak below the white point leaves zero HDR headroom. Keep the
		// peak at or above the measured SDR white and make the anomaly
		// visible in the log instead of degrading silently.
		const float sdrWhiteFloor = measuredSdrWhite > 0.0f ? measuredSdrWhite : 80.0f;
		if (result.displayPeakNits < sdrWhiteFloor) {
			Logger::Get().Warn(fmt::format(
				"HDR source display peak {:.1f} nit is below the SDR white "
				"{:.1f} nit; raising the peak to the SDR white level",
				result.displayPeakNits, sdrWhiteFloor));
			result.displayPeakNits = sdrWhiteFloor;
		}
		result.metadata.maxMasteringLuminanceNits = desc1.MaxLuminance;
		result.metadata.minMasteringLuminanceNits = desc1.MinLuminance;
		result.metadata.maxFrameAverageLightLevelNits = desc1.MaxFullFrameLuminance;
		// WGC scRGB stores scene-linear values with 1.0 == 80 nit. SDR content
		// rendered on an HDR desktop is raised by the monitor's SDR white-level
		// setting (for example 4.5x == 360 nit), so the source description must
		// carry that measured white point for the paired SDR bridge.
		result.referenceWhiteNits = 80.0f;
		result.sdrWhiteNits = measuredSdrWhite > 0.0f ? measuredSdrWhite : 80.0f;
		if (measuredSdrWhite <= 0.0f && result.displayHdrEnabled) {
			// Silent 80-nit fallback shifts every downstream normalization by
			// the monitor's real SDR white ratio (e.g. 4.5x); make it visible.
			Logger::Get().Warn(fmt::format(
				"HDR source SDR-white measurement failed; falling back to 80 nit. "
				"Display peak={:.1f} colorSpace={:#x}",
				result.displayPeakNits,
				static_cast<uint32_t>(desc1.ColorSpace)));
		}
		result.range = HdrColorRange::Full;
		result.isInferred = !measuredPeak;

		D3D11_TEXTURE2D_DESC capturedDesc{};
		if (_output) {
			_output->GetDesc(&capturedDesc);
		}
		if (capturedDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
			capturedDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM) {
			// GDI, DWM shared-surface, and Desktop Duplication expose the
			// selected capture as an 8-bit display-referred surface. Monitor
			// PQ metadata describes the display, not these stored code values.
			result.dxgiColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
			result.primaries = HdrColorPrimaries::Rec709;
			result.transfer = HdrTransferFunction::SRGB;
			result.range = HdrColorRange::Full;
			result.isSceneReferred = false;
			result.displayHdrEnabled = false;
			result.isInferred = false;
			return result;
		}

		switch (desc1.ColorSpace) {
		case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020:
			result.primaries = HdrColorPrimaries::Rec2020;
			result.transfer = HdrTransferFunction::PQ;
			break;
		case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709:
			result.primaries = HdrColorPrimaries::Rec709;
			result.transfer = HdrTransferFunction::Linear;
			result.range = HdrColorRange::SceneLinear;
			result.isSceneReferred = true;
			break;
		case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709:
		default:
			result.primaries = HdrColorPrimaries::Rec709;
			result.transfer = HdrTransferFunction::SRGB;
			result.range = HdrColorRange::Full;
			break;
		}
		return result;
	}

	return result;
}

bool FrameSourceBase::PrepareHdrOutputForResize() noexcept {
	if (!_hdrEnabled || !_output) return true;
	D3D11_TEXTURE2D_DESC sourceDesc{};
	_output->GetDesc(&sourceDesc);
	_hdrFrameReady = false;
	++_resourceGeneration;
	_hdrFrameSequence = 0;
	return _hdrProcessor.Prepare(_output.get(), sourceDesc.Format, _GetSourceColorDescription());
}

std::pair<uint32_t, uint32_t> FrameSourceBase::GetStatisticsForDynamicDetection() const noexcept {
	return _statistics.load(std::memory_order_relaxed);
}

void FrameSourceBase::_DisableRoundCornerInWin11() noexcept {
	if (Win32Helper::GetOSVersion().IsWin10()) {
		return;
	}

	const HWND hwndSrc = ScalingWindow::Get().SrcTracker().Handle();
	INT attr = DWMWCP_DONOTROUND;
	HRESULT hr = DwmSetWindowAttribute(
		hwndSrc, DWMWA_WINDOW_CORNER_PREFERENCE, &attr, sizeof(attr));
	if (FAILED(hr)) {
		Logger::Get().ComError("禁用窗口圆角失败", hr);
		return;
	}

	Logger::Get().Info("已禁用窗口圆角");
	_roundCornerDisabled = true;
}

bool FrameSourceBase::_GetMapToOriginDPI(HWND hWnd, double& a, double& bx, double& by) noexcept {
	// HDC 中的 HBITMAP 尺寸为窗口的原始尺寸
	// 通过 GetWindowRect 获得的尺寸为窗口的 DPI 缩放后尺寸
	// 它们的商即为窗口的 DPI 缩放
	wil::unique_hdc_window hdcWindow(wil::window_dc(
		GetDCEx(hWnd, NULL, DCX_WINDOW), hWnd));
	if (!hdcWindow) {
		Logger::Get().Win32Error("GetDCEx 失败");
		return false;
	}

	wil::unique_hdc_window hdcClient(wil::window_dc(
		GetDCEx(hWnd, NULL, 0), hWnd));
	if (!hdcClient) {
		Logger::Get().Win32Error("GetDCEx 失败");
		return false;
	}

	HGDIOBJ hBmpWindow = GetCurrentObject(hdcWindow.get(), OBJ_BITMAP);
	if (!hBmpWindow) {
		Logger::Get().Win32Error("GetCurrentObject 失败");
		return false;
	}

	if (GetObjectType(hBmpWindow) != OBJ_BITMAP) {
		Logger::Get().Error("无法获取窗口的重定向表面");
		return false;
	}

	BITMAP bmp{};
	if (!GetObject(hBmpWindow, sizeof(bmp), &bmp)) {
		Logger::Get().Win32Error("GetObject 失败");
		return false;
	}

	RECT rect;
	if (!GetWindowRect(hWnd, &rect)) {
		Logger::Get().Win32Error("GetWindowRect 失败");
		return false;
	}

	a = bmp.bmWidth / double(rect.right - rect.left);

	// 使用 DPI 缩放无法可靠计算出窗口客户区的位置
	// 这里使用窗口 HDC 和客户区 HDC 的原点坐标差值
	// GetDCOrgEx 获得的是 DC 原点的屏幕坐标

	POINT ptClient{}, ptWindow{};
	if (!GetDCOrgEx(hdcClient.get(), &ptClient)) {
		Logger::Get().Win32Error("GetDCOrgEx 失败");
		return false;
	}
	if (!GetDCOrgEx(hdcWindow.get(), &ptWindow)) {
		Logger::Get().Win32Error("GetDCOrgEx 失败");
		return false;
	}

	if (!Win32Helper::GetClientScreenRect(hWnd, rect)) {
		Logger::Get().Error("GetClientScreenRect 失败");
		return false;
	}

	// 以窗口的客户区左上角为基准
	// 该点在坐标系 1 中坐标为 (rect.left, rect.top)
	// 在坐标系 2 中坐标为 (ptClient.x - ptWindow.x, ptClient.y - ptWindow.y)
	// 由此计算出 b
	bx = ptClient.x - ptWindow.x - rect.left * a;
	by = ptClient.y - ptWindow.y - rect.top * a;

	return true;
}

bool FrameSourceBase::_InitCheckingForDuplicateFrame() {
	ID3D11Device5* d3dDevice = _deviceResources->GetD3DDevice();

	D3D11_TEXTURE2D_DESC td;
	_output->GetDesc(&td);

	_prevFrame = DirectXHelper::CreateTexture2D(
		d3dDevice, td.Format, td.Width, td.Height, D3D11_BIND_SHADER_RESOURCE);
	if (!_prevFrame) {
		return false;
	}

	HRESULT hr = d3dDevice->CreateShaderResourceView(_prevFrame.get(), nullptr, _prevFrameSrv.put());
	if (FAILED(hr)) {
		Logger::Get().ComError("CreateShaderResourceView 失败", hr);
		return false;
	}

	D3D11_BUFFER_DESC bd{
		.ByteWidth = 4,
		.Usage = D3D11_USAGE_DEFAULT,
		.BindFlags = D3D11_BIND_UNORDERED_ACCESS,
		.StructureByteStride = 4
	};
	hr = d3dDevice->CreateBuffer(&bd, nullptr, _resultBuffer.put());
	if (FAILED(hr)) {
		Logger::Get().ComError("CreateBuffer 失败", hr);
		return false;
	}

	_resultBufferUav = _descriptorStore->GetUnorderedAccessView(
		_resultBuffer.get(), 1, DXGI_FORMAT_R32_UINT);
	if (!_resultBufferUav) {
		Logger::Get().ComError("GetUnorderedAccessView 失败", hr);
		return false;
	}

	bd.Usage = D3D11_USAGE_STAGING;
	bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	bd.BindFlags = 0;
	hr = d3dDevice->CreateBuffer(&bd, nullptr, _readBackBuffer.put());
	if (FAILED(hr)) {
		Logger::Get().ComError("CreateBuffer 失败", hr);
		return false;
	}

	hr = d3dDevice->CreateComputeShader(
		DuplicateFrameCS, sizeof(DuplicateFrameCS), nullptr, _dupFrameCS.put());
	if (FAILED(hr)) {
		Logger::Get().ComError("CreateComputeShader 失败", hr);
		return false;
	}

	static constexpr std::pair<uint32_t, uint32_t> BLOCK_SIZE{ 16, 16 };
	_dispatchCount.first = (td.Width + BLOCK_SIZE.first - 1) / BLOCK_SIZE.first;
	_dispatchCount.second = (td.Height + BLOCK_SIZE.second - 1) / BLOCK_SIZE.second;
	
	return true;
}

bool FrameSourceBase::_IsDuplicateFrame() {
	FrameTrace::Scope traceDuplicate(FrameTrace::Event::DuplicateCheck);
	// 检查是否和前一帧相同
	ID3D11DeviceContext4* d3dDC = _deviceResources->GetD3DDC();

	ID3D11ShaderResourceView* srvs[]{ _outputSrv, _prevFrameSrv.get() };
	d3dDC->CSSetShaderResources(0, 2, srvs);

	ID3D11SamplerState* sam = _deviceResources->GetSampler(
		D3D11_FILTER_MIN_MAG_MIP_POINT, D3D11_TEXTURE_ADDRESS_CLAMP);
	d3dDC->CSSetSamplers(0, 1, &sam);

	// 将缓冲区置零
	static constexpr UINT ZERO[4]{};
	d3dDC->ClearUnorderedAccessViewUint(_resultBufferUav, ZERO);
	d3dDC->CSSetUnorderedAccessViews(0, 1, &_resultBufferUav, nullptr);

	d3dDC->CSSetShader(_dupFrameCS.get(), nullptr, 0);

	d3dDC->Dispatch(_dispatchCount.first, _dispatchCount.second, 1);

	// 取回结果
	d3dDC->CopyResource(_readBackBuffer.get(), _resultBuffer.get());

	uint32_t result = 1;
	D3D11_MAPPED_SUBRESOURCE ms;
	const auto readbackStart = std::chrono::steady_clock::now();
	FrameTrace::Scope traceReadback(FrameTrace::Event::DuplicateReadback);
	HRESULT hr = d3dDC->Map(_readBackBuffer.get(), 0, D3D11_MAP_READ, 0, &ms);
	if (SUCCEEDED(hr)) {
		result = *(uint32_t*)ms.pData;
		d3dDC->Unmap(_readBackBuffer.get(), 0);
	}
	traceReadback.Data(hr, result == 0);
	traceReadback.End();
	traceDuplicate.Data(result == 0, hr);
	const double readbackMs = std::chrono::duration<double, std::milli>(
		std::chrono::steady_clock::now() - readbackStart).count();
	_duplicateReadbackTotalMs += readbackMs;
	_duplicateReadbackMaxMs = std::max(_duplicateReadbackMaxMs, readbackMs);
	if (++_duplicateReadbackSamples == 120) {
		Logger::Get().Info(fmt::format(
			"Capture duplicate readback CPU wait: samples=120 avgMs={:.3f} maxMs={:.3f}",
			_duplicateReadbackTotalMs / 120, _duplicateReadbackMaxMs));
		_duplicateReadbackSamples = 0;
		_duplicateReadbackTotalMs = _duplicateReadbackMaxMs = 0;
	}
	return result == 0;
}

}
