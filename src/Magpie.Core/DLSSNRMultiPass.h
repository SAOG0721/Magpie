#pragma once
#include "DLSSNRFilter.h"
#include "DLSSNRTemporal.h"
#include "DLSSNRParameters.h"
#include "DeviceResources.h"
#include "Logger.h"

namespace Magpie {

// One backend owns the API boundary for all independent NR features.
class DLSSNRMultiPass final : public NativeEffectBackend {
public:
	bool Initialize(DeviceResources& resources, NgxD3D12Core& core,
		ID3D11Texture2D* input, ID3D11Texture2D* output,
		const EffectOption& option, bool hdr) noexcept {
		_failure = { DlssNrStage::Initialize, DlssNrReason::Unknown, DlssNrCodeKind::None, 0, "Initialize DLSSNR chain" };
		_processing = false;
		_libraryInfo.clear();
		if (!Drain()) return false;
		_filter.reset();
		_temporal.reset();
		_rawOutput = {};
		_core = &core;
		_context = resources.GetD3DDC();
		_option = option;
		_hdr = hdr;
		_count = Count(option);
		const int antiFlicker = DLSSNRAntiFlickerMode([&](std::string_view name, float fallback) {
			const auto it = option.parameters.find(std::string(name));
			return it == option.parameters.end() ? fallback : it->second;
		});
		if (antiFlicker) {
			D3D11_TEXTURE2D_DESC desc{};
			output->GetDesc(&desc);
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
			desc.CPUAccessFlags = desc.MiscFlags = 0;
			const HRESULT hr = resources.GetD3DDevice()->CreateTexture2D(&desc, nullptr, _rawOutput.put());
			if (FAILED(hr)) {
				_failure = DlssNrApiFailure(DlssNrStage::Initialize, DlssNrCodeKind::HResult, static_cast<uint32_t>(hr), "Create anti-flicker output");
				Logger::Get().ComError("Create DLSSNR anti-flicker raw output failed", hr);
				return false;
			}
		}
		std::vector<DLSSNRSettings> settings;
		for (int i = 1; i <= _count; ++i)
			settings.push_back(ParseDLSSNRSettings(DLSSNRPassOption(option, i), hdr));
		auto filter = std::make_unique<DLSSNRFilter>();
		if (!filter->InitializeChain(resources, core, input,
			antiFlicker ? _rawOutput.get() : output, settings)) {
			_failure = filter->GetDlssNrFailure();
			_libraryInfo = filter->LibraryInfo();
			return false;
		}
		_libraryInfo = filter->LibraryInfo();
		filter->SetHdrBoundary(_hdrBoundary);
		_filter = std::move(filter);
		if (antiFlicker) {
			_failure.operation = "Initialize DLSSNR anti-flicker resources";
			_temporal = std::make_unique<DLSSNRTemporal>();
			if (!_temporal->Initialize(resources, input, input,
				_rawOutput.get(), output, antiFlicker, hdr,
				NormalizeDLSSNRAntiFlickerTimeMs(Value("antiFlickerTimeMs", DLSSNR_ANTI_FLICKER_TIME_DEFAULT_MS)))) return false;
		}
		ConfigureDetail();
		_failure = {};
		return true;
	}
	void SetHdrBoundary(HdrEffectBoundaryContext context) noexcept override {
		NativeEffectBackend::SetHdrBoundary(std::move(context));
		if (_filter) _filter->SetHdrBoundary(_hdrBoundary);
	}
	FrameGuidanceRequirements GetFrameGuidanceRequirements() const noexcept override {
		FrameGuidanceRequirements result = _filter ? _filter->GetFrameGuidanceRequirements() : FrameGuidanceRequirements{};
		const auto it = _option.parameters.find("antiFlicker");
		if (it != _option.parameters.end() && it->second >= 2 && it->second <= 4 &&
			it->second == std::floor(it->second) && !result.HasMotion())
			result.Add(MotionVectorRequest::Amd(AmdOpticalFlowMode::Quality));
		return result;
	}
	bool Drain() noexcept override { return !_filter || _filter->Drain(); }
	bool Resize(DeviceResources& resources, ID3D11Texture2D* input,
		ID3D11Texture2D* output) noexcept override {
		const EffectOption option = _option;
		return _core && Initialize(resources, *_core, input, output, option, _hdr);
	}
	bool Draw(const NativeEffectDrawContext& context) noexcept override {
		if (!_filter) return false;
		NativeEffectDrawContext chain = context;
		chain.output = _temporal ? _rawOutput.get() : context.output;
		if (!_filter->Draw(chain)) {
			_processing = false;
			if (_failure.operation.empty()) _failure = _filter->GetDlssNrFailure();
			if (_temporal) _temporal->Reset();
			return false;
		}
		_processing = _filter->IsHealthy();
		if (!_filter->IsHealthy()) {
			if (_failure.operation.empty()) _failure = _filter->GetDlssNrFailure();
			if (_temporal) _temporal->Reset();
			if (_count > 1) return false;
		}
		if (_temporal && DebugEnabled()) {
			_temporal->Reset();
			_context->CopyResource(context.output, _rawOutput.get());
			return true;
		}
		const bool success = !_temporal || _temporal->Draw(context);
		if (!success) {
			_processing = false;
			if (_failure.operation.empty()) _failure = { DlssNrStage::Evaluate, DlssNrReason::Unknown, DlssNrCodeKind::None, 0, "Anti-flicker processing" };
		}
		if (success && _processing) _failure = {};
		return success;
	}
	const std::string& LibraryInfo() const noexcept { return _libraryInfo; }
	DlssNrFailure GetDlssNrFailure() const override { return _failure; }
	bool IsDlssNrProcessing() const noexcept override { return _processing; }
	EffectParameterApplyMode GetParameterApplyMode(std::string_view name) const noexcept override {
		if (!_failure.operation.empty() || !_filter || !_filter->IsHealthy()) return EffectParameterApplyMode::Unavailable;
		if (name == "multiPass" || name == "antiFlicker") return EffectParameterApplyMode::RestartRequired;
		if (name == "antiFlickerTimeMs") return EffectParameterApplyMode::Live;
		if (DLSSNRParameterPass(name) > _count) return EffectParameterApplyMode::Live;
		return _filter->GetParameterApplyMode(DLSSNRBaseParameter(name));
	}
	EffectParameterRestartReason GetParameterRestartReason(std::string_view name) const noexcept override {
		if (name == "antiFlicker") return EffectParameterRestartReason::FrameGuidance;
		if (name == "multiPass" || !_filter) return EffectParameterRestartReason::ResourceRecreation;
		return _filter->GetParameterRestartReason(DLSSNRBaseParameter(name));
	}
	bool ApplyLiveParameters(const EffectOption& option,
		std::span<const std::string> names) noexcept override {
		if (!_filter || Count(option) != _count) return false;
		bool activeEdit = false;
		std::vector<std::string> filterNames;
		for (const auto& name : names) {
			if (GetParameterApplyMode(name) != EffectParameterApplyMode::Live) return false;
			activeEdit |= DLSSNRParameterChangesCurrentOutput(name, _count);
			if (name != "antiFlickerTimeMs") filterNames.push_back(name);
		}
		if (!filterNames.empty() && !_filter->ApplyLiveParameters(option, filterNames)) return false;
		_option = option;
		if (_temporal && activeEdit) _temporal->Reset();
		if (_temporal) _temporal->SetTimeConstant(
			Value("antiFlickerTimeMs", DLSSNR_ANTI_FLICKER_TIME_DEFAULT_MS));
		ConfigureDetail();
		return true;
	}
private:
	DlssNrFailure _failure;
	std::string _libraryInfo;
	bool _processing = false;
	float Value(std::string_view name, float fallback = 0) const noexcept {
		const auto it = _option.parameters.find(std::string(name));
		return it == _option.parameters.end() ? fallback : it->second;
	}
	bool DetailEnabled() const noexcept {
		return !_hdr && Value("enableInputResolutionScaling") >= .5f;
	}
	bool DebugEnabled() const noexcept { return DetailEnabled() && Value("residualDebugView") >= .5f; }
	void ConfigureDetail() noexcept {
		if (_temporal) _temporal->ConfigureDetail(0.f,
				!_hdr && Value("enableInputResolutionScaling") >= .5f);
	}
	ID3D11DeviceContext* _context = nullptr;
	static int Count(const EffectOption& option) noexcept {
		return DLSSNRPassCount([&](std::string_view name, float fallback) {
			const auto it = option.parameters.find(std::string(name));
			return it == option.parameters.end() ? fallback : it->second;
		});
	}
	NgxD3D12Core* _core = nullptr;
	EffectOption _option;
	bool _hdr = false;
	int _count = 0;
	winrt::com_ptr<ID3D11Texture2D> _rawOutput;
	std::unique_ptr<DLSSNRTemporal> _temporal;
	std::unique_ptr<DLSSNRFilter> _filter;
};

}
