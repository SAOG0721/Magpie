// Private implementation included after the conventional inference helpers.
// Every D3D11 operation stays on the render thread; the two D3D12 queues own
// independent features and working textures, not independent immediate contexts.

static VOID CALLBACK WakeDlssnrOwner(PVOID owner, BOOLEAN) {
	// The callback owns no renderer/feature pointers and performs no GPU work.
	PostThreadMessageW(static_cast<DWORD>(reinterpret_cast<uintptr_t>(owner)), WM_NULL, 0, 0);
}

bool DLSSNRFilter::HasReadyOutput() const noexcept {
	return ReadyOutputVersion() != 0;
}

uint64_t DLSSNRFilter::ReadyOutputVersion() const noexcept {
	if (_sparse) return SparseReadyOutputVersion();
	if (!_performance || !_impl || _impl->disabled) return 0;
	const auto& p = *_performance;
	const auto slot = p.pool.Latest(p.state, p.poolFence->GetCompletedValue());
	return slot != DLSSNREnhancementPool::None && p.pool[slot].version > p.consumedVersion
		? _outputVersionBase + p.pool[slot].version : 0;
}

DLSSNRPerformanceSnapshot DLSSNRFilter::GetPerformanceSnapshot() const noexcept {
	if (_sparse) return SparsePerformanceSnapshot();
	DLSSNRPerformanceSnapshot result;
	if (!_impl) return result;
	result.foregroundGpuSamples = _impl->foregroundTimer.samples;
	result.foregroundGpuMs = _impl->foregroundTimer.milliseconds;
	result.fullGpuMs = _impl->lastEvaluateGpuMs;
	result.fullGpuSamples = _impl->evaluateGpuSamples;
	result.fullSubmissions = _impl->evaluateCount;
	result.compositionGpuMs = _impl->foregroundTimer.sectionMilliseconds;
	if (_performance) {
		const auto& p = *_performance;
		result.captureSequence = p.state.sequence;
		result.fullCompleted = p.fullCompleted; result.fullLate = p.lateDiscards + p.unusedExpired;
		result.fullPublished = p.fullPublished; result.fullTurnaroundMs = p.fullTurnaroundMs;
		result.enhancementCoverage = p.enhancementCoverage;
		result.fullReadySamples = p.fullReadySamples; result.fullReadyMs = p.fullReadyMs;
		result.fullAdoptionSamples = p.fullAdoptionSamples; result.fullAdoptionMs = p.fullAdoptionMs;
		result.fullAdopted = p.adoptedResults; result.coverageSamples = p.coverageCount;
		result.fullSubmissions = p.fullSubmissions;
		result.sameSpecRecoveries = p.sameSpecRecoveries;
		result.continuousReuseOutputs = p.continuousReuseOutputs;
	}
	return result;
}

bool DLSSNRFilter::IsGpuIdle() const noexcept {
	if (InferenceSubmissionPending()) return false;
	if (_sparse) return _impl && _impl->fence12->GetCompletedValue()>=_impl->pendingInferenceValue &&
		_sparse->fence->GetCompletedValue()>=_sparse->fenceValue;
	if (!_impl) return true;
	if (_impl->fence12 && _impl->fence12->GetCompletedValue() < _impl->initializationValue) return false;
	if (_impl->fence12 && _impl->fence12->GetCompletedValue() < _impl->pendingInferenceValue) return false;
	if (_impl->retirementFence && _impl->retirementFence->GetCompletedValue() < _impl->retirementValue) return false;
	return !_performance || _performance->poolFence->GetCompletedValue() >= _performance->poolFenceValue;
}

bool DLSSNRFilter::InitializationReady() noexcept {
	if (!_impl || _impl->disabled) return false;
	auto& impl = *_impl;
	const auto completed = impl.fence12->GetCompletedValue();
	if (completed == UINT64_MAX || completed < impl.initializationValue) return false;
	if (!impl.initializationObserved) {
		impl.initializationObserved = true;
		impl.initializationWaitMs = std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now()-impl.initializationSubmittedAt).count();
		Logger::Get().Info(fmt::format(
			"DLSSNR initialization ready: size={}x{} observedGpuWaitMs={:.3f} waitMode=poll",
			impl.width, impl.height, impl.initializationWaitMs));
	}
	return true;
}

bool DLSSNRFilter::CanPrepareResolution() const noexcept {
	if (_sparse) return false;
	return IsHealthy() && (!_resolutionChange || (!_resolutionChange->pending &&
		!_resolutionChange->retiredFull));
}

bool DLSSNRFilter::PrepareResolution(DeviceResources& resources, ID3D11Texture2D* input,
	ID3D11Texture2D* output, uint32_t fullPercent) noexcept {
	if (!CanPrepareResolution() || !_ngxCore || !input || !output) return false;
	const bool cacheHit = HasHotResolution(fullPercent);
	if (!_resolutionChange) _resolutionChange = std::make_unique<ResolutionChange>();
	auto& change = *_resolutionChange;
	change.cacheHit = cacheHit;
	change.warming = false;
	change.startedAt = std::chrono::steady_clock::now();
	change.settings = _passSettings;
	fullPercent = std::clamp(fullPercent, 25u, 100u);
	for (auto& setting : change.settings) {
		setting.inputResolutionPercent = fullPercent;
	}
	auto extent = [&](uint32_t percent) {
		return FrameGuidanceExtent{
			std::max(1u, uint32_t(std::lround(double(_impl->sourceWidth)*percent/100.))),
			std::max(1u, uint32_t(std::lround(double(_impl->sourceHeight)*percent/100.)))};
	};
	change.fullChanged = extent(fullPercent) != FrameGuidanceExtent{_impl->width, _impl->height};
	change.pending = true;
	bool success = true;
	if (change.fullChanged) {
		if (cacheHit) change.full = std::move(change.hotFull);
		else {
			// Cold changes are reserved for overload recovery. The inactive slot
			// is retired here, never during a successful hot switch.
			change.hotFull.reset();
			change.full = std::make_unique<DLSSNRFilter>();
			change.full->_foregroundQueue = _foregroundQueue;
			change.full->SetHdrBoundary(_hdrBoundary);
			success = change.full->InitializeChainResources(resources, *_ngxCore, input, output,
				change.settings, true);
		}
	}
	change.preparationMs = std::chrono::duration<double, std::milli>(
		std::chrono::steady_clock::now()-change.startedAt).count();
	Logger::Get().Info(fmt::format(
		"DLSSNR resolution prepare: full={} fullRecreated={} ownerCpuMs={:.3f} gpuWait=poll success={} cacheHit={}",
		fullPercent, change.fullChanged && !cacheHit, change.preparationMs, success, cacheHit));
	if (!success) CancelPreparedResolution();
	_impl->context11->Flush();
	return success;
}

bool DLSSNRFilter::CommitPreparedResolution() noexcept {
	if (!_resolutionChange || !_resolutionChange->pending || _resolutionChange->warming) return false;
	auto& change = *_resolutionChange;
	if (change.full && !change.full->InitializationReady()) return false;
	if (change.full) {
		auto& candidate = *change.full;
		// Warm features retain allocations, never old pixels or a pending job.
		candidate._impl->cache.valid = false;
		candidate._impl->resetHistory = true;
		for (auto& pass : candidate._impl->laterPasses) pass->resetHistory = true;
		if (_performance && candidate._performance) {
			auto& old = *_performance; auto& next = *candidate._performance;
			next.pool = {};
			next.pending = false; next.latestWaiting = false;
			next.postOutput = {}; next.poolRetryFence = 0;
			next.pendingSlot = DLSSNREnhancementPool::None;
			next.completionToObserve = 0;
			next.readySequence = next.readyEpoch = next.consumedVersion = 0;
			next.lastInferenceSequence = next.lastInferenceEpoch = 0;
			next.enhancementCoverage = next.zeroEnhancementFraction = -1;
			for (auto& result : next.results) {
				result.readyObserved = false; result.adopted = false;
			}
			for (auto& sample : next.coverageSamples) sample.pending = false;
			next.compositeDirty = true;
			for (auto& step : next.motion) step.valid = false;
			_outputVersionBase += old.pool.Version();
			// Preserve capture ordinals but start a new enhancement generation.
			// Old immutable jobs keep their original pool until GPU retirement.
			next.state = old.state; next.state.Invalidate();
			next.motionGeneration = old.motionGeneration;
			next.depthGeneration = old.depthGeneration;
			next.confidenceGeneration = old.confidenceGeneration;
			next.guidanceRegion = old.guidanceRegion;
		}
		std::swap(_impl, candidate._impl);
		std::swap(_submission, candidate._submission);
		std::swap(_performance, candidate._performance);
		std::swap(_settings, candidate._settings);
		std::swap(_passSettings, candidate._passSettings);
		_libraryInfo = candidate._libraryInfo;
		change.retiredFull = std::move(change.full);
		change.retiredFullCacheable = change.cacheEnabled;
	}
	for (auto& setting : _passSettings) {
		setting.inputResolutionPercent = change.settings.front().inputResolutionPercent;
	}
	_settings = _passSettings.front();
	change.preparationMs = std::chrono::duration<double, std::milli>(
		std::chrono::steady_clock::now()-change.startedAt).count();
	change.pending = false;
	return true;
}

void DLSSNRFilter::CancelPreparedResolution() noexcept {
	if (!_resolutionChange || !_resolutionChange->pending) return;
	auto& change = *_resolutionChange;
	change.retiredFull = std::move(change.full);
	change.retiredFullCacheable = false;
	change.pending = false;
	change.warming = false;
}

double DLSSNRFilter::ResolutionPreparationMs() const noexcept {
	return _resolutionChange ? _resolutionChange->preparationMs : 0;
}

bool DLSSNRFilter::PrewarmResolution(DeviceResources& resources, ID3D11Texture2D* input,
	ID3D11Texture2D* output, uint32_t fullPercent) noexcept {
	if (!PrepareResolution(resources, input, output, fullPercent)) return false;
	auto& change = *_resolutionChange;
	if (!change.full) { CancelPreparedResolution(); return false; }
	change.cacheEnabled = true;
	change.warming = true;
	return true;
}

bool DLSSNRFilter::WarmPreparedResolution(const NativeEffectDrawContext& context) noexcept {
	if (!_resolutionChange || !_resolutionChange->warming) return true;
	auto& change = *_resolutionChange;
	auto& candidate = *change.full;
	const auto start = std::chrono::steady_clock::now();
	// Initialization and the first Evaluate are paid before the first output.
	// A captured input supplies valid resources; its result is not published.
	const NativeEffectDrawContext warm{.input=context.input, .output=context.output,
		.inputMetadata=context.inputMetadata, .outputMetadata=context.outputMetadata,
		.frameId=context.frameId, .previousCaptureFrameId=context.previousCaptureFrameId,
		.inputRevision=context.inputRevision, .inputHistoryRevision=context.inputHistoryRevision,
		.inputHistoryReset=true, .isNewCaptureFrame=context.isNewCaptureFrame,
		.frameGuidance=context.zeroFrameGuidance, .zeroFrameGuidance=context.zeroFrameGuidance};
	const bool success = WaitForFence(*candidate._impl, candidate._impl->initializationValue) &&
		candidate.DrawInference(warm, false, false) && candidate.IsHealthy() && candidate.Drain();
	if (!success) {
		CancelPreparedResolution();
		change.cacheEnabled = false;
		Logger::Get().Warn("DLSSNR standby warm-up failed; active resolution retained");
		return false;
	}
	candidate._impl->cache.valid = false;
	change.hotFull = std::move(change.full);
	change.pending = change.warming = false;
	Logger::Get().Info(fmt::format("DLSSNR standby ready: full={} warmMs={:.3f} displayed=false",
		change.hotFull->_settings.inputResolutionPercent,
		std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-start).count()));
	return true;
}

bool DLSSNRFilter::GetHotResolution(uint32_t& fullPercent) const noexcept {
	if (!_resolutionChange || !CanPrepareResolution()) return false;
	const auto& hot = _resolutionChange->hotFull;
	if (!hot || !hot->IsHealthy() || !hot->IsGpuIdle()) return false;
	fullPercent = hot->_settings.inputResolutionPercent;
	return true;
}

bool DLSSNRFilter::HasHotResolution(uint32_t fullPercent) const noexcept {
	uint32_t hotFull = 0;
	return GetHotResolution(hotFull) && hotFull == fullPercent;
}

void DLSSNRFilter::InvalidateHotResolution() noexcept {
	if (!_resolutionChange) return;
	// Live edits are explicit operations. An inactive allocation cannot keep
	// old inference parameters and later silently replace the edited chain.
	_resolutionChange->hotFull.reset();
	_resolutionChange->retiredFullCacheable = false;
	if (_resolutionChange->pending) CancelPreparedResolution();
}

bool DLSSNRFilter::HasPendingPostOutputWork() const noexcept {
	if (_sparse) return SparseHasPendingWork();
	if (!_performance || !_impl || _impl->disabled) return false;
	const auto& p = *_performance;
	if (_settings.processingMode != DLSSNRProcessingMode::Pipeline) return false;
	if (!p.postOutput.CanSubmit(p.state, p.latestSequence, p.latestEpoch, p.latestWaiting, p.pending)) return false;
	const auto completed = p.poolFence->GetCompletedValue();
	return completed != UINT64_MAX && (!p.poolRetryFence || completed >= p.poolRetryFence);
}

void DLSSNRFilter::MarkOutputPublished() noexcept {
	if (_sparse) { _sparse->afterOutput=true; return; }
	if (_performance) _performance->postOutput.Published(_performance->state);
}

void DLSSNRFilter::ServiceAfterOutput() noexcept {
	if (_sparse) { SparseAfterOutput(); return; }
	MarkOutputPublished();
	ServiceRetiredResolutions();
	if (!_performance || !_impl || _impl->disabled) return;
	// Renderer has enqueued publication before entering this hook. The input
	// fence recorded by DrawInference therefore orders background inference
	// after all current-output D3D11 work, including the final publication.
	ServiceAsync();
	if (_impl->disabled) return;
	auto& p = *_performance;
	if (_settings.processingMode == DLSSNRProcessingMode::Pipeline &&
		!p.pending && p.latestWaiting && !SubmitPerformanceInput(true)) {
		_impl->disabled = true;
		_failure = {DlssNrStage::Evaluate, DlssNrReason::Unknown, DlssNrCodeKind::None, 0,
			"DLSSNR post-output background submission"};
	}
}

void DLSSNRFilter::ServiceRetiredResolutions() noexcept {
	if (!_resolutionChange) return;
	auto& change = *_resolutionChange;
	// Retired jobs keep their original resources and worker. They no longer get
	// normal Draw/ServiceAsync calls, so acquire CPU completion here before the
	// existing GPU retirement/cache decision.
	if (change.retiredFull) change.retiredFull->ServiceInferenceSubmission();
	if (change.retiredFull && change.retiredFull->IsGpuIdle() && change.cacheEnabled &&
		change.retiredFullCacheable && !change.hotFull && change.retiredFull->IsHealthy()) {
		change.hotFull = std::move(change.retiredFull);
		change.retiredFullCacheable = false;
		Logger::Get().Info(fmt::format("DLSSNR standby retained: full={} allocations=reused",
			change.hotFull->_settings.inputResolutionPercent));
	}
	for (auto* retired : { &_resolutionChange->retiredFull }) {
		if (!*retired || !(*retired)->IsGpuIdle()) continue;
		const auto start = std::chrono::steady_clock::now();
		retired->reset();
		Logger::Get().Info(fmt::format("DLSSNR retired branch release: ownerMs={:.3f}",
			std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-start).count()));
	}
}

bool DLSSNRFilter::InitializePerformance(ID3D11Texture2D* input, ID3D11Texture2D* output) noexcept {
	const auto resourcesStart = std::chrono::steady_clock::now();
	auto state = std::make_unique<Performance>();
	auto& p = *state;
	auto& c = p.composition;
	auto& full = *_impl;
	p.state.Configure(_settings.processingMode, _settings.sparseUpdateInterval);
	p.ownerThread = GetCurrentThreadId();
	if (FAILED(p.completionEvent.create()) || !RegisterWaitForSingleObject(
		&p.registeredWait, p.completionEvent.get(), WakeDlssnrOwner,
		reinterpret_cast<PVOID>(uintptr_t(p.ownerThread)), INFINITE, WT_EXECUTEINWAITTHREAD)) return false;
	c.device11 = full.device11; c.context11 = full.context11; c.shaders = full.shaders;
	c.sourceWidth = full.sourceWidth; c.sourceHeight = full.sourceHeight;
	c.width = full.width; c.height = full.height;
	c.useResolutionScaling = true; c.convertInputToRgba = full.convertInputToRgba;
	_failure.operation = "Create DLSSNR performance snapshots";
	if (!p.generated.Create(c.device11, c.width, c.height, DXGI_FORMAT_R16G16B16A16_FLOAT) ||
		!p.validity.Create(c.device11, c.width, c.height, DXGI_FORMAT_R8_UNORM) ||
		!p.pendingMotion.Create(c.device11, c.sourceWidth, c.sourceHeight, DXGI_FORMAT_R16G16_FLOAT, true) ||
		!p.pendingConfidence.Create(c.device11, c.sourceWidth, c.sourceHeight, DXGI_FORMAT_R8_UNORM, true) ||
		!p.pendingDepth.Create(c.device11, c.sourceWidth, c.sourceHeight, DXGI_FORMAT_R32_FLOAT, true) ||
		!p.zeroMotion.Create(c.device11, c.sourceWidth, c.sourceHeight, DXGI_FORMAT_R16G16_FLOAT, true) ||
		!p.zeroConfidence.Create(c.device11, c.sourceWidth, c.sourceHeight, DXGI_FORMAT_R8_UNORM, true)) return false;
	for (auto& slot : p.results) {
		if (!slot.enhancement.Create(c.device11, c.width, c.height, DXGI_FORMAT_R16G16B16A16_FLOAT) ||
			!slot.reference.Create(c.device11, c.width, c.height, DXGI_FORMAT_R8G8B8A8_UNORM)) return false;
	}
	if (FAILED(c.device11->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(p.poolFence.put())))) return false;
	D3D11_TEXTURE2D_DESC latestDesc{}; input->GetDesc(&latestDesc);
	latestDesc.Usage = D3D11_USAGE_DEFAULT; latestDesc.CPUAccessFlags = latestDesc.MiscFlags = 0;
	latestDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	if (FAILED(c.device11->CreateTexture2D(&latestDesc, nullptr, p.latestInput.put()))) return false;
	const float zero[4]{};
	c.context11->ClearUnorderedAccessViewFloat(p.zeroMotion.uav.get(), zero);
	c.context11->ClearUnorderedAccessViewFloat(p.zeroConfidence.uav.get(), zero);
	c.sharedInput11 = DirectXHelper::CreateTexture2D(c.device11, DXGI_FORMAT_R8G8B8A8_UNORM,
		c.width, c.height, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
	c.sharedOutput11 = p.generated.texture;
	D3D11_TEXTURE2D_DESC outputDesc{}; output->GetDesc(&outputDesc);
	if (!c.sharedInput11 || !CreateResolutionScalingResources(c, input, output, outputDesc)) return false;
	// Equal-size RGBA needs only its existing copies. Reuse prepared pixels
	// only when it actually removes a downsample or colour-conversion dispatch.
	if (c.convertInputToRgba || c.width != c.sourceWidth || c.height != c.sourceHeight) {
		D3D11_TEXTURE2D_DESC preparedDesc{}; c.sharedInput11->GetDesc(&preparedDesc);
		preparedDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		if (FAILED(c.device11->CreateTexture2D(&preparedDesc, nullptr, p.latestPreparedInput.put()))) return false;
	}
	for (auto& step : p.motion) {
		if (!step.motion.Create(c.device11, c.sourceWidth, c.sourceHeight, DXGI_FORMAT_R16G16_FLOAT) ||
			!step.confidence.Create(c.device11, c.sourceWidth, c.sourceHeight, DXGI_FORMAT_R8_UNORM)) return false;
	}
	D3D11_BUFFER_DESC buffer{};
	buffer.ByteWidth = sizeof(Performance::Constants); buffer.Usage = D3D11_USAGE_DEFAULT;
	buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	if (FAILED(c.device11->CreateBuffer(&buffer, nullptr, p.constants.put()))) return false;
	buffer.ByteWidth = 16; buffer.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
	buffer.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
	if (FAILED(c.device11->CreateBuffer(&buffer, nullptr, p.invalid.put()))) return false;
	D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
	uav.Format = DXGI_FORMAT_R32_TYPELESS; uav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
	uav.Buffer.NumElements = 4; uav.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
	if (FAILED(c.device11->CreateUnorderedAccessView(p.invalid.get(), &uav, p.invalidUav.put()))) return false;
	buffer.Usage = D3D11_USAGE_STAGING; buffer.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	buffer.BindFlags = buffer.MiscFlags = 0;
	if (FAILED(c.device11->CreateBuffer(&buffer, nullptr, p.invalidReadback.put()))) return false;
	for (auto& sample : p.coverageSamples) {
		D3D11_QUERY_DESC desc{D3D11_QUERY_EVENT, 0};
		if (FAILED(c.device11->CreateBuffer(&buffer, nullptr, sample.buffer.put())) ||
			FAILED(c.device11->CreateQuery(&desc, sample.done.put()))) return false;
	}
	D3D11_SAMPLER_DESC sampler{};
	sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	sampler.MaxLOD = D3D11_FLOAT32_MAX;
	if (FAILED(c.device11->CreateSamplerState(&sampler, p.sampler.put()))) return false;
	if (!CreateComputeShader(c, DLSSNR_PERFORMANCE_HLSL, "PublishEnhancement", "DLSSNRPublishEnhancement", p.publishShader) ||
		!CreateComputeShader(c, DLSSNR_PERFORMANCE_HLSL, "ComposeCurrent", "DLSSNRComposeCurrent", p.composeShader) ||
		!CreateComputeShader(c, DLSSNR_PERFORMANCE_HLSL, "CheckReuse", "DLSSNRCheckReuse", p.checkShader) ||
		!CreateComputeShader(c, DLSSNR_PERFORMANCE_HLSL, "ComposeMotion", "DLSSNRComposeMotion", p.motionShader) ||
		!CreateComputeShader(c, DLSSNR_PERFORMANCE_HLSL, "MeasureCoverage", "DLSSNRMeasureCoverage", p.coverageShader)) return false;

	Logger::Get().Info(fmt::format("DLSSNR composition resources: size={}x{} resourcesMs={:.3f}",
		c.width, c.height, std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now()-resourcesStart).count()));
	Logger::Get().Info(fmt::format(
		"DLSSNR performance target: mode={} full={}x{} passes={} updateInterval={} maxAge={} captures",
		int(_settings.processingMode), c.width, c.height, _passSettings.size(),
		p.state.updateInterval, p.state.maxAgeFrames));
	_performance = std::move(state);
	_failure = {};
	return true;
}

void DLSSNRFilter::UpdatePerformanceParameters(std::span<const std::string> names,
	bool inferenceChanged) noexcept {
	if (!_performance || std::ranges::all_of(names,
		[](const auto& name) { return IsDLSSNRDisplayParameter(name); })) return;
	auto& p = *_performance;
	if (inferenceChanged) {
		p.state.Invalidate(); p.readySequence = 0;
		p.latestWaiting = false;
	}
	p.composition.residualParametersDirty = true;
	p.compositeDirty = true;
}

bool DLSSNRFilter::DrawPerformance(const NativeEffectDrawContext& context) noexcept {
	auto& p = *_performance;
	auto& full = *_impl;
	auto& c = p.composition;
	auto* dc = c.context11;
	const bool pipeline = _settings.processingMode == DLSSNRProcessingMode::Pipeline;
	const bool alternating = _settings.processingMode == DLSSNRProcessingMode::Alternating;
	const bool continuous = alternating || pipeline;
	const FrameGuidanceExtent extent{c.sourceWidth, c.sourceHeight};
	const auto guidance = SelectGuidance(context, _settings, extent);
	auto fail = [&](std::string_view operation) {
		full.disabled = true;
		if (_failure.operation.empty()) _failure = {DlssNrStage::Evaluate, DlssNrReason::Unknown,
			DlssNrCodeKind::None, 0, std::string(operation)};
		return false;
	};
	if (full.disabled || !guidance.IsValidFor(context.frameId, extent))
		return fail("DLSSNR performance feature/guidance unavailable");
	const uint64_t previousFrame = p.state.frame;
	const bool changed = !p.state.initialized || p.state.frame != context.frameId || p.state.revision != context.inputRevision;
	const bool resourcesChanged = p.motionGeneration != guidance.motion.metadata.resourceGeneration ||
		p.depthGeneration != guidance.depth.metadata.resourceGeneration ||
		p.confidenceGeneration != guidance.confidence.metadata.resourceGeneration ||
		p.guidanceRegion != guidance.motion.metadata.validRegion;
	p.state.Begin(context.frameId, context.inputRevision, context.inputHistoryRevision,
		guidance.motion.metadata.timestamp100ns, context.isNewCaptureFrame,
		resourcesChanged || (changed && (context.inputHistoryReset || guidance.requiresHistoryReset)));
	p.unusedExpired += p.pool.ExpireUnconsumed(p.state);
	p.motionGeneration = guidance.motion.metadata.resourceGeneration;
	p.depthGeneration = guidance.depth.metadata.resourceGeneration;
	p.confidenceGeneration = guidance.confidence.metadata.resourceGeneration;
	p.guidanceRegion = guidance.motion.metadata.validRegion;
	if (p.state.reset) {
		++p.historyResetFrames;
		p.readySequence = 0;
		for (auto& step : p.motion) step.valid = false;
	}
	if (p.state.newCapture || p.state.reset) {
		auto& step = p.motion[p.state.sequence%p.motion.size()];
		step.sequence = p.state.sequence;
		step.valid = !p.state.reset && previousFrame == context.previousCaptureFrameId &&
			!guidance.motion.metadata.isZero && !guidance.requiresHistoryReset &&
			guidance.motion.metadata.validRegion == FrameGuidanceRegion::Full(extent);
		step.confidenceAvailable = !guidance.confidence.metadata.isZero;
		if (step.valid) {
			for (const auto* channel : {&guidance.motion, &guidance.confidence}) {
				const auto& sync = channel->metadata.sync;
				if (sync.fence && sync.value && FAILED(dc->Wait(sync.fence, sync.value))) return fail("DLSSNR motion snapshot wait");
			}
			dc->CopyResource(step.motion.texture.get(), guidance.motion.texture);
			if (step.confidenceAvailable) dc->CopyResource(step.confidence.texture.get(), guidance.confidence.texture);
		}
	}
	if (!changed && !p.state.reset && !p.compositeDirty && !HasReadyOutput()) return true;
	if (!PrepareInput(c, context.input)) return fail("DLSSNR current input preparation");
	Performance::Constants values{c.width, c.height, c.sourceWidth, c.sourceHeight};
	values.processingMode = uint32_t(_settings.processingMode);

	ServiceAsync();
	if (full.disabled) return false;

	// One replaceable input mailbox, separate from the immutable in-flight job.
	auto snapshotInput = [&](bool force = false) {
		if (!force && ((p.lastInferenceSequence == p.state.sequence && p.lastInferenceEpoch == p.state.epoch) ||
			(p.latestWaiting && p.latestSequence == p.state.sequence && p.latestEpoch == p.state.epoch))) return;
		if (p.latestWaiting) ++p.replacedRequests;
		dc->CopyResource(p.latestInput.get(), context.input);
		// Both textures belong to the same capture/epoch/revision below. The
		// replaceable mailbox never aliases the full branch's in-flight input.
		if (p.latestPreparedInput) dc->CopyResource(p.latestPreparedInput.get(), c.sharedInput11.get());
		p.latestZero = context.zeroFrameGuidance;
		p.latestDepth.copy_from(context.zeroFrameGuidance.depth.texture);
		p.latestDepthFence.copy_from(context.zeroFrameGuidance.depth.metadata.sync.fence);
		p.latestInputMetadata = context.inputMetadata; p.latestOutputMetadata = context.outputMetadata;
		p.latestSequence = p.state.sequence; p.latestEpoch = p.state.epoch;
		p.latestFrame = context.frameId; p.latestPreviousFrame = context.previousCaptureFrameId;
		p.latestRevision = context.inputRevision; p.latestHistoryRevision = context.inputHistoryRevision;
		p.latestIsNewCapture = context.isNewCaptureFrame; p.latestWaiting = true;
	};
	if (pipeline || p.state.NeedsFullUpdate(p.lastInferenceSequence, p.lastInferenceEpoch)) snapshotInput();
	if (!pipeline && p.latestWaiting && !SubmitPerformanceInput(false)) return fail("DLSSNR alternating inference");
	ID3D11ShaderResourceView* srvs[12]{};
	// Async consumers only acquire fully published results. Alternating inference
	// explicitly enqueued its D3D12 wait; this same D3D11 queue can read its
	// ordered publication without an additional CPU fence wait. Pipeline only
	// publishes completed inference and orders that publication on this queue.
	size_t selected = DLSSNREnhancementPool::None;
	auto selectHistory = [&]() {
		std::fill(std::begin(srvs), std::end(srvs), nullptr);
		srvs[0] = c.sharedInputSrv11.get();
		selected = p.pool.Latest(p.state, UINT64_MAX);
		values.hasHistory = selected != DLSSNREnhancementPool::None;
		values.age = 0;
		values.reproject = _settings.residualTransfer == DLSSNRResidualTransfer::PerPixel;
		if (!values.hasHistory) return;
		const auto& slot = p.pool[selected];
		p.readySequence = slot.sequence; p.readyEpoch = slot.epoch;
		srvs[1] = p.results[selected].enhancement.srv.get(); srvs[2] = p.results[selected].reference.srv.get();
		values.age = uint32_t(p.state.sequence-p.readySequence);
		if (values.reproject && !p.MotionInputs(p.readySequence, srvs, values)) {
			values.hasHistory = 0;
			if (p.state.newCapture) ++p.missingMotionFrames;
		}
	};
	selectHistory();
	if (p.state.newCapture && p.readySequence && p.readyEpoch == p.state.epoch &&
		p.state.sequence > p.readySequence + p.state.maxAgeFrames) ++p.expiredHistoryFrames;
	if (!values.hasHistory && p.state.newCapture) ++p.noHistoryFrames;
	// Soft pixel rejection stays local and never adds complete-chain work.
	// Missing history, epoch/age expiry or an unavailable motion chain rebuilds
	// the current input at the same specification and anchors a fresh cycle.
	if ((pipeline || alternating) && !values.hasHistory) {
		// A full feature owns immutable in-flight input/output. A hard history
		// boundary may finish it before reusing the feature; an age2/3 completion
		// can restore usable history without another foreground evaluation.
		if (p.pending) {
			if (!ServiceInferenceSubmission(true)) return fail("DLSSNR pipeline CPU submission wait");
			if (!WaitForFence(full, full.pendingInferenceValue)) return fail("DLSSNR pipeline in-flight wait");
			ServiceAsync();
			if (full.disabled) return false;
			selectHistory();
		}
		if (!values.hasHistory) {
			snapshotInput(true);
			if (!SubmitPerformanceInput(false) || p.latestWaiting)
				return fail("DLSSNR pipeline same-resolution foreground inference");
			if (pipeline) ++p.pipelineFallbacks;
			else ++p.sameSpecRecoveries;
			selectHistory();
			if (!values.hasHistory || values.age != 0)
				return fail("DLSSNR pipeline invalid current full output");
			if (alternating) {
				// Preserve Alternating's existing hard-recovery validation. Pipeline
				// handles pixel confidence entirely in the composition shader.
				const UINT zero[4]{}; dc->ClearUnorderedAccessViewUint(p.invalidUav.get(), zero);
				ID3D11UnorderedAccessView* checkUavs[4]{nullptr, nullptr, p.invalidUav.get()};
				p.Dispatch(p.checkShader.get(), values, srvs, checkUavs);
				dc->CopyResource(p.invalidReadback.get(), p.invalid.get());
				D3D11_MAPPED_SUBRESOURCE mapped{};
				if (FAILED(dc->Map(p.invalidReadback.get(), 0, D3D11_MAP_READ, 0, &mapped)))
					return fail("DLSSNR alternating current coverage readback");
				const bool incomplete = *static_cast<const uint32_t*>(mapped.pData) != 0;
				dc->Unmap(p.invalidReadback.get(), 0);
				if (incomplete) return fail("DLSSNR alternating invalid current full output");
			}
		}
	}
	ID3D11UnorderedAccessView* uavs[4]{p.generated.uav.get(), p.validity.uav.get()};
	full.foregroundTimer.BeginSection(dc);
	p.Dispatch(p.composeShader.get(), values, srvs, uavs);
	if (p.state.newCapture && p.state.sequence % 8 == 0) {
		for (auto& sample : p.coverageSamples) if (!sample.pending) {
			const UINT zero[4]{}; dc->ClearUnorderedAccessViewUint(p.invalidUav.get(), zero);
			ID3D11UnorderedAccessView* measureUavs[4]{nullptr, nullptr, p.invalidUav.get()};
			p.Dispatch(p.coverageShader.get(), values, srvs, measureUavs, 64);
			dc->CopyResource(sample.buffer.get(), p.invalid.get()); dc->End(sample.done.get());
			sample.pending = true; break;
		}
	}
	if (!CompositeResidual(c, context.output, p.generated.srv.get(), _settings)) return fail("DLSSNR current residual composition");
	full.foregroundTimer.EndSection(dc);
	if (continuous && values.hasHistory && values.age > 0) ++p.continuousReuseOutputs;
	++p.outputRevision;
	c.residualParametersDirty = false; p.compositeDirty = false;
	if (selected != DLSSNREnhancementPool::None) {
		const uint64_t readDone = ++p.poolFenceValue;
		if (FAILED(dc->Signal(p.poolFence.get(), readDone))) return fail("DLSSNR consumer fence");
		auto& result = p.results[selected];
		if (values.hasHistory && !result.adopted) {
			result.adopted = true; ++p.adoptedResults; ++p.fullAdoptionSamples;
			p.fullAdoptionMs = std::chrono::duration<double, std::milli>(
				std::chrono::steady_clock::now()-result.submittedAt).count();
		}
		p.pool.ReadSubmitted(selected, readDone);
		if (!context.isNewCaptureFrame && p.pool[selected].version > p.consumedVersion) ++p.qualityUpdates;
		p.consumedVersion = p.pool[selected].version;
	}
	if (p.state.newCapture && (p.state.sequence <= 8 || p.state.sequence%120 == 0)) {
		FrameTrace::NativeNrScope trace(FrameTrace::NativeNrStage::PerformanceLog, context.frameId);
		Logger::Get().Info(fmt::format("DLSSNR performance: capture={} source={} epoch={} age={} maxAge={} updateInterval={} full={} discardedLate={} discardedEpoch={} noHistoryFrames={} missingMotionFrames={} expiredHistoryFrames={} historyResetFrames={} pending={} completed={} published={} qualityUpdates={} poolBusy={} replacedRequests={} foregroundGpuMs={:.3f} turnaroundMs={:.3f} meanEnhancementWeight={:.4f} zeroEnhancementFraction={:.4f} adoptedResults={} unusedExpired={} fullGpuMs={:.3f} compositionGpuMs={:.3f} mode={} pipelineFallbacks={} readyMs={:.3f} adoptionMs={:.3f} sameSpecRecoveries={} continuousReuseOutputs={}",
			p.state.sequence, p.readySequence, p.state.epoch, values.hasHistory ? int(values.age) : -1,
			p.state.maxAgeFrames, p.state.updateInterval, p.fullSubmissions,
			p.lateDiscards, p.epochDiscards, p.noHistoryFrames, p.missingMotionFrames,
			p.expiredHistoryFrames, p.historyResetFrames, p.pending, p.fullCompleted, p.fullPublished,
			p.qualityUpdates, p.poolBusy, p.replacedRequests, full.foregroundTimer.milliseconds,
			p.fullTurnaroundMs, p.enhancementCoverage, p.zeroEnhancementFraction,
			p.adoptedResults, p.unusedExpired, full.lastEvaluateGpuMs, full.foregroundTimer.sectionMilliseconds,
			int(_settings.processingMode), p.pipelineFallbacks, p.fullReadyMs, p.fullAdoptionMs,
			p.sameSpecRecoveries, p.continuousReuseOutputs));
	}
	return true;
}

bool DLSSNRFilter::PublishPerformanceResult() noexcept {
	auto& p = *_performance; auto& full = *_impl;
	auto& c = p.composition; auto* dc = c.context11;
	if (!p.state.Usable(p.pendingSequence, p.pendingEpoch)) {
		if (p.pendingEpoch != p.state.epoch) ++p.epochDiscards; else ++p.lateDiscards;
		p.pool.Cancel(p.pendingSlot); p.pendingSlot = DLSSNREnhancementPool::None;
		return true;
	}
	auto& result = p.results[p.pendingSlot];
	Impl& last = full.laterPasses.empty() ? full : *full.laterPasses.back();
	ID3D11ShaderResourceView* srvs[12]{};
	srvs[0] = full.sharedInputSrv11.get(); srvs[11] = last.sharedOutputSrv11.get();
	ID3D11UnorderedAccessView* uavs[4]{result.enhancement.uav.get()};
	Performance::Constants values{c.width, c.height, c.sourceWidth, c.sourceHeight};
	p.Dispatch(p.publishShader.get(), values, srvs, uavs);
	dc->CopyResource(result.reference.texture.get(), full.sharedInput11.get());
	const auto ready = ++p.poolFenceValue;
	if (FAILED(dc->Signal(p.poolFence.get(), ready))) return false;
	p.pool.Publish(p.pendingSlot, p.pendingSequence, p.pendingEpoch, ready);
	p.pendingSlot = DLSSNREnhancementPool::None; ++p.fullPublished;
	// Publication is asynchronous too. Wake the owner only when its payload is
	// actually readable; a full-inference fence alone is not a Ready result.
	if (FAILED(p.poolFence->SetEventOnCompletion(ready, p.completionEvent.get()))) return false;
	dc->Flush();
	return true;
}

bool DLSSNRFilter::SubmitPerformanceInput(bool background) noexcept {
	auto& p = *_performance; auto& full = *_impl; auto& c = p.composition; auto* dc = c.context11;
	if (!p.latestWaiting || p.pending) return true;
	if (p.latestEpoch != p.state.epoch || p.latestSequence != p.state.sequence) { p.latestWaiting = false; return true; }
	const auto completed = p.poolFence->GetCompletedValue();
	if (completed == UINT64_MAX) return false;
	auto slot = p.pool.Reserve(completed);
	if (slot == DLSSNREnhancementPool::None) {
		++p.poolBusy;
		if (background) {
			// An idle source has no next capture to retry this mailbox. Arm the
			// earliest retiring slot and wake the owner; polling only authorizes
			// another visit to ServiceAfterOutput once that GPU fence is complete.
			if (!p.poolRetryFence || completed >= p.poolRetryFence) {
				uint64_t retry = UINT64_MAX;
				for (size_t i = 0; i < DLSSNREnhancementPool::Capacity; ++i) {
					const auto& candidate = p.pool[i];
					if (!candidate.writing) retry = std::min(retry, std::max(candidate.readyFence, candidate.readFence));
				}
				if (retry == UINT64_MAX || retry <= completed || FAILED(p.poolFence->SetEventOnCompletion(
					retry, p.completionEvent.get()))) return false;
				p.poolRetryFence = retry;
				dc->Flush();
			}
			return true;
		}
		// Foreground fallback must produce NR. Retire already submitted GPU
		// readers before reusing bounded pool storage instead of passing raw input.
		wil::unique_event_nothrow available;
		if (FAILED(available.create()) || FAILED(p.poolFence->SetEventOnCompletion(
			p.poolFenceValue, available.get()))) return false;
		dc->Flush();
		if (WaitForSingleObject(available.get(), INFINITE) != WAIT_OBJECT_0) return false;
		const auto retired = p.poolFence->GetCompletedValue();
		if (retired == UINT64_MAX) return false;
		slot = p.pool.Reserve(retired);
		if (slot == DLSSNREnhancementPool::None) return false;
	}
	p.poolRetryFence = 0;
	p.pendingSlot = slot;
	const auto depthSync = p.latestZero.depth.metadata.sync;
	if (depthSync.fence && depthSync.value && FAILED(dc->Wait(depthSync.fence, depthSync.value))) return false;
	dc->CopyResource(p.pendingDepth.texture.get(), p.latestDepth.get());
	FrameGuidanceView privateZero = p.latestZero;
	privateZero.depth.texture = p.pendingDepth.texture.get();
	privateZero.motion.texture = p.zeroMotion.texture.get();
	privateZero.confidence.texture = p.zeroConfidence.texture.get();
	for (auto* channel : {&privateZero.depth, &privateZero.motion, &privateZero.confidence}) channel->metadata.sync = {};
	FrameGuidanceView frozen = privateZero;
	bool reset = p.lastInferenceEpoch != p.state.epoch;
	ID3D11ShaderResourceView* srvs[12]{};
	Performance::Constants values{c.sourceWidth, c.sourceHeight, c.sourceWidth, c.sourceHeight};
	values.reproject = 1;
	const bool motionValid = p.latestIsNewCapture && !reset && _settings.motionRequest.method != OpticalFlowMethod::None &&
		p.MotionInputs(p.lastInferenceSequence, srvs, values);
	if (motionValid) {
		ID3D11UnorderedAccessView* uavs[4]{nullptr, p.pendingConfidence.uav.get(), nullptr, p.pendingMotion.uav.get()};
		p.Dispatch(p.motionShader.get(), values, srvs, uavs);
		frozen.motion.texture = p.pendingMotion.texture.get(); frozen.confidence.texture = p.pendingConfidence.texture.get();
		for (auto* channel : {&frozen.motion, &frozen.confidence}) {
			channel->metadata.isZero = false; channel->metadata.sync = {};
			channel->metadata.validRegion = FrameGuidanceRegion::Full({c.sourceWidth, c.sourceHeight});
			channel->metadata.requiresHistoryReset = false;
		}
	} else if (_settings.motionRequest.method != OpticalFlowMethod::None ||
		p.lastInferenceSequence+1 != p.state.sequence) reset = true;
	const NativeEffectDrawContext context{.input=p.latestInput.get(),
		.output=p.generated.texture.get(),
		.inputMetadata=p.latestInputMetadata, .outputMetadata=p.latestOutputMetadata,
		.frameId=p.latestFrame, .previousCaptureFrameId=p.latestPreviousFrame,
		.inputRevision=p.latestRevision, .inputHistoryRevision=p.latestHistoryRevision,
		.inputHistoryReset=reset, .isNewCaptureFrame=p.latestIsNewCapture,
		.frameGuidance=frozen, .zeroFrameGuidance=privateZero};
	p.submittedAt = std::chrono::steady_clock::now();
	auto& result = p.results[slot];
	result.submittedAt = p.submittedAt; result.readyObserved = result.adopted = false;
	if (!DrawInference(context, background, false, p.latestPreparedInput.get()) || !IsHealthy()) return false;
	p.completionToObserve = full.pendingInferenceValue;
	p.pendingSequence = p.latestSequence; p.pendingEpoch = p.latestEpoch;
	p.lastInferenceSequence = p.latestSequence; p.lastInferenceEpoch = p.latestEpoch;
	p.pending = background; p.latestWaiting = false; ++p.fullSubmissions;
	if (!background && !PublishPerformanceResult()) return false;
	return true;
}

void DLSSNRFilter::ServiceAsync() noexcept {
	if (_sparse) { ServiceSparse(); return; }
	if (!_performance || !_impl || _impl->disabled) return;
	if (!ServiceInferenceSubmission() || InferenceSubmissionPending()) return;
	auto& p = *_performance; auto& full = *_impl;
	ResetEvent(p.completionEvent.get());
	const auto inferenceCompleted = full.fence12->GetCompletedValue();
	if (inferenceCompleted != UINT64_MAX)
		for (auto& slot : full.commandSlots) if (slot.completionValue && slot.completionValue <= inferenceCompleted)
			CollectGpuTiming(full, slot);
	if (p.completionToObserve && full.fence12->GetCompletedValue() >= p.completionToObserve &&
		full.fence12->GetCompletedValue() != UINT64_MAX) {
		++p.fullCompleted; p.completionToObserve = 0;
		p.fullTurnaroundMs = std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now()-p.submittedAt).count();
	}
	for (auto& sample : p.coverageSamples) if (sample.pending && full.context11->GetData(
		sample.done.get(), nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK) {
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (SUCCEEDED(full.context11->Map(sample.buffer.get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped))) {
			const auto* counts = static_cast<const uint32_t*>(mapped.pData);
			if (counts[0]) {
				p.enhancementCoverage = double(counts[2])/(1024.0*counts[0]);
				p.zeroEnhancementFraction = double(counts[1])/counts[0];
				++p.coverageCount;
			}
			full.context11->Unmap(sample.buffer.get(), 0); sample.pending = false;
		}
	}
	bool success = true;
	if (p.pending) {
		const auto completed = full.fence12->GetCompletedValue();
		if (completed == UINT64_MAX) success = false;
		else if (completed >= full.pendingInferenceValue) {
			success = SUCCEEDED(full.context11->Wait(full.fence11.get(), full.pendingInferenceValue)) && PublishPerformanceResult();
			p.pending = false;
		}
	}
	const auto publishedCompleted = p.poolFence->GetCompletedValue();
	if (publishedCompleted == UINT64_MAX) success = false;
	else for (size_t i = 0; i < DLSSNREnhancementPool::Capacity; ++i) {
		const auto& slot = p.pool[i];
		auto& result = p.results[i];
		if (!slot.writing && slot.version && slot.readyFence <= publishedCompleted && !result.readyObserved) {
			result.readyObserved = true; ++p.fullReadySamples;
			p.fullReadyMs = std::chrono::duration<double, std::milli>(
				std::chrono::steady_clock::now()-result.submittedAt).count();
		}
	}
	if (!success) {
		full.disabled = true;
		_failure = {DlssNrStage::Evaluate, DlssNrReason::Unknown, DlssNrCodeKind::None, 0, "DLSSNR asynchronous progress"};
	}
}

void DLSSNRFilter::ConfigureTemporalHistory(DLSSNRTemporal& temporal) const noexcept {
	if (_sparse) {
		const auto info=GetSparseOutputInfo();
		temporal.SetSparseHistory(info.targetSequence,info.oldestSourceSequence,info.epoch,info.observationRevision,
			info.maxSourceAge,info.outputRevision,info.validity,info.observationTimestamp100ns);
		return;
	}
	if (!_performance) { temporal.SetPerformanceHistory(0, 0, 0, 0, 0, nullptr); return; }
	const auto& p = *_performance;
	const uint64_t origin = p.state.Usable(p.readySequence, p.readyEpoch)
		? p.readySequence : p.state.sequence;
	temporal.SetPerformanceHistory(p.state.sequence, origin, p.state.epoch,
		p.state.maxAgeFrames, p.outputRevision, p.validity.srv.get());
}
