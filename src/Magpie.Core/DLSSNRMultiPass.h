#pragma once
#include "DLSSNRFilter.h"
#include "DLSSNRTemporal.h"
#include "DLSSNRParameters.h"
#include "DeviceResources.h"
#include "Logger.h"
#include "DLSSNRAdaptiveResolution.h"
#include <chrono>

namespace Magpie {

// One backend owns the API boundary for all independent NR features.
class DLSSNRMultiPass final : public NativeEffectBackend {
public:
	bool Initialize(DeviceResources& resources, NgxD3D12Core& core,
		ID3D11Texture2D* input, ID3D11Texture2D* output,
		const EffectOption& option, bool hdr) noexcept {
		_failure = { DlssNrStage::Initialize, DlssNrReason::Unknown, DlssNrCodeKind::None, 0, "Initialize DLSSNR chain" };
		_processing = false;
		_resolutionChanged = false;
		_resolutionProposed = _resolutionPreparing = false;
		_startupWarmPending = false;
		_contentDelivered = _outputCaptureFrame = _deliveredCaptureFrame = 0;
		_outputContentVersion = _lastFilterContentVersion = 0;
		_forceOutputVersion = true;
		_lastPublishedAt = 0;
		_libraryInfo.clear();
		if (!Drain()) return false;
		_filter.reset();
		_temporal.reset();
		_rawOutput = {};
		_core = &core;
		_resources = &resources;
		_option = option;
		_hdr = hdr;
		_count = Count(option);
		_sparseProcessing = IsDLSSNRSparseProcessingMode(DLSSNRMode(
			[&](std::string_view name, float fallback) { return Value(name, fallback); }));
		_adaptiveEnabled = Value("resolutionControl", 1.f) == 0.f && !hdr && !_sparseProcessing;
		const auto initialPercent = ParseDLSSNRSettings(DLSSNRPassOption(option, 1), hdr).inputResolutionPercent;
		_adaptive.Reset({hdr ? 100u : _adaptiveEnabled ? uint32_t(std::lround(initialPercent/5.))*5 : initialPercent}, Now());
		// Automatic tiers use 5% steps; manual specifications retain every
		// integer percentage through initialization, drawing and status reporting.
		_visibleResolution = {hdr ? 100u : _adaptiveEnabled ? _adaptive.Active().full : initialPercent};
		const int antiFlicker = DLSSNRAntiFlickerMode([&](std::string_view name, float fallback) {
			const auto it = option.parameters.find(std::string(name));
			return it == option.parameters.end() ? fallback : it->second;
		});
		if (!IsDLSSNRPerformanceConfigurationValid([&](std::string_view name, float fallback) { return Value(name, fallback); })) {
			_failure = {DlssNrStage::Initialize, DlssNrReason::Compatibility, DlssNrCodeKind::None, 0,
				"Invalid DLSSNR performance configuration: choose compatible optical flow, residual reuse and anti-flicker"};
			return false;
		}
		if (antiFlicker) {
			D3D11_TEXTURE2D_DESC desc{};
			output->GetDesc(&desc);
			if (!hdr) desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
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
		for (int i = 1; i <= _count; ++i) {
			auto setting = ParseDLSSNRSettings(DLSSNRPassOption(option, i), hdr);
			setting.inputResolutionPercent = _visibleResolution.full;
			settings.push_back(setting);
		}
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
		if (_adaptiveEnabled) {
			const auto active = _adaptive.Active();
			const uint32_t warmFull = active.full < 100 ? active.full+5 : active.full-5;
			_startupWarmPending = _filter->PrewarmResolution(resources, input,
				antiFlicker ? _rawOutput.get() : output, warmFull);
			if (!_startupWarmPending) Logger::Get().Warn("DLSSNR adaptive startup warm unavailable; active tier retained");
		}
		_failure = {};
		return true;
	}
	void SetHdrBoundary(HdrEffectBoundaryContext context) noexcept override {
		NativeEffectBackend::SetHdrBoundary(std::move(context));
		if (_filter) _filter->SetHdrBoundary(_hdrBoundary);
	}
	FrameGuidanceRequirements GetFrameGuidanceRequirements() const noexcept override {
		FrameGuidanceRequirements result = _filter ? _filter->GetFrameGuidanceRequirements() : FrameGuidanceRequirements{};
		return result;
	}
	bool Drain() noexcept override {
		return !_filter || _filter->Drain();
	}
	bool Resize(DeviceResources& resources, ID3D11Texture2D* input,
		ID3D11Texture2D* output) noexcept override {
		const EffectOption option = _option;
		return _core && Initialize(resources, *_core, input, output, option, _hdr);
	}
	bool Draw(const NativeEffectDrawContext& context) noexcept override {
		if (!_filter) return false;
		if (_startupWarmPending) {
			_startupWarmPending = false;
			if (!_filter->WarmPreparedResolution(context))
				Logger::Get().Warn("DLSSNR adaptive startup warm failed; active tier retained");
		}
		if (_resolutionPreparing && context.isNewCaptureFrame && _filter->CommitPreparedResolution()) {
			const auto old = _adaptive.Active();
			_switchPrepareMs = _filter->ResolutionPreparationMs();
			_adaptive.Commit(_proposedResolution, Now(), _switchPrepareMs);
			_resolutionProposed = _resolutionPreparing = false;
			_resolutionChanged = true;
			if (_temporal) _temporal->Reset();
			Logger::Get().Info(fmt::format(
				"DLSSNR adaptive switch: full={}->{} targetFps={:.3f} prepareMs={:.3f} retirementPending=true",
				old.full, _adaptive.Active().full,
				_adaptive.Target(), _switchPrepareMs));
		}
		NativeEffectDrawContext chain = context;
		if (_resolutionChanged) {
			Logger::Get().Info(fmt::format("DLSSNR adaptive first capture: prepareMs={:.3f} captureGapMs={:.3f}",
				_switchPrepareMs, (double(context.frameGuidance.motion.metadata.timestamp100ns)/1e7-_switchCaptureTime)*1000));
			// Changed features start clean; an unchanged branch keeps its own
			// history. The enhancement pool uses a distinct generation after commit.
			_resolutionChanged = false;
		}
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
		auto targetContext = _filter->OutputDrawContext(chain);
		targetContext.output = context.output;
		if (_temporal) _filter->ConfigureTemporalHistory(*_temporal);
		if (_temporal && DebugEnabled()) {
			_temporal->Reset();
			const bool success = _temporal->Draw(targetContext, true);
			if (success) CommitOutputVersion();
			return success;
		}
		const bool success = !_temporal || _temporal->Draw(targetContext);
		if (!success) {
			_processing = false;
			if (_failure.operation.empty()) _failure = { DlssNrStage::Evaluate, DlssNrReason::Unknown, DlssNrCodeKind::None, 0, "Anti-flicker processing" };
		}
		if (success && _processing) {
			_failure = {};
			if (_adaptiveEnabled) _visibleResolution = _adaptive.Active();
			if (targetContext.isNewCaptureFrame) _outputCaptureFrame = targetContext.frameId;
			CommitOutputVersion();
			UpdateResolution(context, chain.output);
		}
		return success;
	}
	NativeEffectDrawContext OutputDrawContext(const NativeEffectDrawContext& context) const noexcept override {
		return _filter ? _filter->OutputDrawContext(context) : context;
	}
	uint64_t OutputContentVersion() const noexcept override {
		return _sparseProcessing ? _outputContentVersion : 0;
	}
	const std::string& LibraryInfo() const noexcept { return _libraryInfo; }
	DlssNrFailure GetDlssNrFailure() const override { return _failure; }
	bool IsDlssNrProcessing() const noexcept override { return _processing; }
	uint32_t GetDlssNrResolutionPercent() const noexcept override { return _visibleResolution.full; }
	bool HasReadyOutput() const noexcept override { return _filter && _filter->HasReadyOutput(); }
	uint64_t ReadyOutputVersion() const noexcept override { return _filter ? _filter->ReadyOutputVersion() : 0; }
	bool HasPendingPostOutputWork() const noexcept override {
		return _filter && _filter->HasPendingPostOutputWork();
	}
	void ServiceAsync() noexcept override {
		if (!_filter) return;
		_filter->ServiceAsync();
		if (!_filter->IsHealthy()) { _processing = false; _failure = _filter->GetDlssNrFailure(); }
	}
	void ServiceAfterOutput() noexcept override {
		if (!_filter) return;
		_filter->MarkOutputPublished();
		const double now = Now();
		if (_outputCaptureFrame && _outputCaptureFrame != _deliveredCaptureFrame) {
			++_contentDelivered; _deliveredCaptureFrame = _outputCaptureFrame;
			const double intervalMs = _lastPublishedAt ? (now-_lastPublishedAt)*1000 : 0;
			if (_contentDelivered <= 8 || _contentDelivered%120 == 0 || _resolutionProposed ||
				(_adaptive.Target() > 0 && intervalMs > 2000./_adaptive.Target()))
				Logger::Get().Info(fmt::format(
					"DLSSNR content publication: capture={} delivered={} intervalMs={:.3f} preparing={}",
					_outputCaptureFrame, _contentDelivered, intervalMs, _resolutionProposed));
			_lastPublishedAt = now;
		}
		if (!_resolutionProposed || _resolutionPreparing || !_filter->CanPrepareResolution()) {
			_filter->ServiceAfterOutput();
			return;
		}
		const auto active = _adaptive.Active();
		const bool upgrading = _proposedResolution.full > active.full;
		if (upgrading && !_filter->HasHotResolution(_proposedResolution.full)) {
			// Readiness can change between Draw's proposal and publication. An
			// upgrade never turns this cache miss into a cold Create on the owner.
			_adaptive.Reject(Now()); _resolutionProposed = false;
			_filter->ServiceAfterOutput();
			return;
		}
		if (!_filter->PrepareResolution(*_resources, _prepareInput, _prepareOutput,
			_proposedResolution.full)) {
			Logger::Get().Warn("DLSSNR adaptive candidate rejected; active branches retained");
			_adaptive.Reject(Now());
			_resolutionProposed = false;
			return;
		}
		// Candidate preparation and ordinary full continuation do not compete in
		// the same after-output turn. Normal progress resumes on the next turn.
		_resolutionPreparing = true;
	}
	EffectParameterApplyMode GetParameterApplyMode(std::string_view name) const noexcept override {
		if (!_failure.operation.empty() || !_filter || !_filter->IsHealthy()) return EffectParameterApplyMode::Unavailable;
		if (name == "multiPass" || name == "antiFlicker" || name == "resolutionControl") return EffectParameterApplyMode::RestartRequired;
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
		if (!IsDLSSNRPerformanceConfigurationValid([&](std::string_view name, float fallback) {
			const auto it = option.parameters.find(std::string(name));
			return it == option.parameters.end() ? fallback : it->second;
		})) return false;
		bool activeEdit = false;
		bool compositionOnly = !names.empty();
		std::vector<std::string> filterNames;
		for (const auto& name : names) {
			if (GetParameterApplyMode(name) != EffectParameterApplyMode::Live) return false;
			activeEdit |= DLSSNRParameterChangesCurrentOutput(name, _count);
			compositionOnly &= IsDLSSNRDisplayParameter(name);
			if (name != "antiFlickerTimeMs") filterNames.push_back(name);
		}
		if (_resolutionProposed && !compositionOnly) {
			_filter->CancelPreparedResolution();
			_resolutionProposed = _resolutionPreparing = false;
			_adaptive.Reject(Now());
		}
		auto activeOption = option;
		if (_adaptiveEnabled) activeOption.parameters["inputResolutionPercent"] = float(_adaptive.Active().full);
		if (!filterNames.empty() && !_filter->ApplyLiveParameters(activeOption, filterNames)) return false;
		if (activeEdit && !compositionOnly) {
			_filter->InvalidateHotResolution();
			_startupWarmPending = false;
			_adaptive.InvalidateMeasurements(Now());
		}
		_option = option;
		if (_temporal && activeEdit) _temporal->Reset();
		_forceOutputVersion |= activeEdit;
		if (_temporal) _temporal->SetTimeConstant(
			Value("antiFlickerTimeMs", DLSSNR_ANTI_FLICKER_TIME_DEFAULT_MS));
		ConfigureDetail();
		return true;
	}
private:
	void CommitOutputVersion() noexcept {
		if (!_sparseProcessing || !_filter) return;
		const auto version = _filter->OutputContentVersion();
		if (_forceOutputVersion || version != _lastFilterContentVersion) {
			++_outputContentVersion;
			_lastFilterContentVersion = version;
			_forceOutputVersion = false;
		}
	}
	static double Now() noexcept {
		return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
	}
	void UpdateResolution(const NativeEffectDrawContext& context, ID3D11Texture2D* output) noexcept {
		if (!_adaptiveEnabled || !_resources || !_core || _resolutionProposed ||
			!_filter->CanPrepareResolution() || !context.isNewCaptureFrame) return;
		const auto telemetry = _filter->GetPerformanceSnapshot();
		const double now = Now();
		const auto mode = DLSSNRMode([&](std::string_view name, float fallback) { return Value(name, fallback); });
		DLSSNRAdaptiveResolution::Resolution hot;
		const bool hotReady = _filter->GetHotResolution(hot.full);
		const auto next = _adaptive.Observe({
			.now = now,
			.captureTime = double(context.frameGuidance.motion.metadata.timestamp100ns)/1e7,
			.explicitTarget = context.targetContentFrameRate,
			.foregroundGpuMs = telemetry.foregroundGpuMs,
			.fixedGpuMs = context.guidanceGpuMs,
			.fullTurnaroundMs = telemetry.fullTurnaroundMs,
			.enhancementCoverage = telemetry.enhancementCoverage,
			.gpuSamples = telemetry.foregroundGpuSamples,
			.completed = telemetry.fullCompleted,
			.late = telemetry.fullLate,
			.published = telemetry.fullPublished,
			.contentDelivered = _contentDelivered,
			.newCapture = true,
			.reset = context.inputHistoryReset || context.frameGuidance.requiresHistoryReset,
			.asynchronous = mode == DLSSNRProcessingMode::Pipeline,
			.fixedGpuKnown = Value("opticalFlowMethod") == 0 || context.guidanceGpuSample != 0,
			.fullGpuMs = telemetry.fullGpuMs,
			.fullReadyMs = telemetry.fullReadyMs,
			.fullAdoptionMs = telemetry.fullAdoptionMs,
			.fullGpuSamples = telemetry.fullGpuSamples,
			.fullReadySamples = telemetry.fullReadySamples,
			.fullAdoptionSamples = telemetry.fullAdoptionSamples,
			.fullAdopted = telemetry.fullAdopted,
			.coverageSamples = telemetry.coverageSamples,
			.hasHotResolution = hotReady,
			.hotResolution = hot
		});
		if (next == _adaptive.Active()) return;
		Logger::Get().Info(fmt::format("DLSSNR adaptive proposal: full={}->{} reason={} hot={}",
			_adaptive.Active().full, next.full,
			DLSSNRAdaptiveResolution::ReasonName(_adaptive.LastReason()), hotReady && hot == next));
		// Default source-sized motion is shared by every NR resolution. Resource
		// preparation starts only after Renderer publishes this completed output.
		_proposedResolution = next;
		_resolutionProposed = true;
		_prepareInput = context.input; _prepareOutput = output;
		_switchCaptureTime = double(context.frameGuidance.motion.metadata.timestamp100ns)/1e7;
	}
	DlssNrFailure _failure;
	std::string _libraryInfo;
	bool _processing = false;
	bool _sparseProcessing = false, _forceOutputVersion = false;
	uint64_t _outputContentVersion = 0, _lastFilterContentVersion = 0;
	float Value(std::string_view name, float fallback = 0) const noexcept {
		const auto it = _option.parameters.find(std::string(name));
		return it == _option.parameters.end() ? fallback : it->second;
	}
	bool DetailEnabled() const noexcept {
		return !_hdr;
	}
	bool DebugEnabled() const noexcept { return DetailEnabled() && Value("residualDebugView") >= .5f; }
	void ConfigureDetail() noexcept {
		if (_temporal) _temporal->ConfigureDetail(0.f,
				DetailEnabled());
	}
	static int Count(const EffectOption& option) noexcept {
		return DLSSNRPassCount([&](std::string_view name, float fallback) {
			const auto it = option.parameters.find(std::string(name));
			return it == option.parameters.end() ? fallback : it->second;
		});
	}
	NgxD3D12Core* _core = nullptr;
	DeviceResources* _resources = nullptr;
	EffectOption _option;
	bool _hdr = false;
	int _count = 0;
	winrt::com_ptr<ID3D11Texture2D> _rawOutput;
	std::unique_ptr<DLSSNRTemporal> _temporal;
	std::unique_ptr<DLSSNRFilter> _filter;
	DLSSNRAdaptiveResolution _adaptive;
	DLSSNRAdaptiveResolution::Resolution _visibleResolution;
	bool _adaptiveEnabled = false;
	bool _resolutionChanged = false;
	bool _resolutionProposed = false, _resolutionPreparing = false;
	bool _startupWarmPending = false;
	DLSSNRAdaptiveResolution::Resolution _proposedResolution;
	ID3D11Texture2D* _prepareInput = nullptr;
	ID3D11Texture2D* _prepareOutput = nullptr;
	uint64_t _contentDelivered = 0, _outputCaptureFrame = 0, _deliveredCaptureFrame = 0;
	double _lastPublishedAt = 0;
	double _switchPrepareMs = 0, _switchCaptureTime = 0;
};

}
