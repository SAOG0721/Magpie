// The asynchronous interval mode keeps one full inference in flight and one
// replaceable input request. Every displayed target is the current capture.
bool DLSSNRFilter::InitializeSparse(DeviceResources& resources, NgxD3D12Core& core,
	ID3D11Texture2D* input, ID3D11Texture2D* output,
	std::span<const DLSSNRSettings> passes) noexcept {
	if (passes.empty() || !input || !output || passes.front().experimentalHdr.enabled || !Drain()) return false;
	auto state=std::make_unique<Sparse>(); auto& p=*state;
	p.device=resources.GetD3DDevice(); p.dc=resources.GetD3DDC();
	D3D11_TEXTURE2D_DESC in{},out{}; input->GetDesc(&in); output->GetDesc(&out);
	p.width=in.Width; p.height=in.Height; p.inputFormat=in.Format; p.outputFormat=out.Format;
	p.interval=std::clamp(passes.front().sparseUpdateInterval,2u,4u);
	p.maxSourceAge=p.interval+1; p.budget.interval=p.interval;
	for (auto& f:p.captures) if (!f.Create(p.device,p.width,p.height,p.inputFormat)) return false;
	if (!p.pendingFrame.Create(p.device,p.width,p.height,p.inputFormat) || !p.outputFrame.Create(p.device,p.width,p.height,p.inputFormat)) return false;
	for (auto& r:p.results) if (!r.residual.Create(p.device,p.width,p.height,DXGI_FORMAT_R16G16B16A16_FLOAT) ||
		!r.reference.Create(p.device,p.width,p.height,p.inputFormat,false)) return false;
	for (auto* t:{&p.fullColor,&p.rawResidual})
		if (!t->Create(p.device,p.width,p.height,DXGI_FORMAT_R16G16B16A16_FLOAT)) return false;
	for (auto* t:{&p.rawValidity,&p.validity,&p.pastConfidence,&p.zeroConfidence})
		if (!t->Create(p.device,p.width,p.height,DXGI_FORMAT_R8_UNORM,true,true)) return false;
	for (auto* t:{&p.pastMotion,&p.zeroMotion})
		if (!t->Create(p.device,p.width,p.height,DXGI_FORMAT_R16G16_FLOAT,true,true)) return false;
	if (!p.lastOutput.Create(p.device,p.width,p.height,p.outputFormat)) return false;
	const float zero[4]{};
	p.dc->ClearUnorderedAccessViewFloat(p.zeroMotion.uav.get(),zero); p.dc->ClearUnorderedAccessViewFloat(p.zeroConfidence.uav.get(),zero);
	std::vector<DLSSNRSettings> inner(passes.begin(),passes.end());
	for (auto& setting:inner) setting.processingMode=DLSSNRProcessingMode::EveryFrame;
	if (!InitializeChainResources(resources,core,p.pendingFrame.input.texture.get(),p.fullColor.texture.get(),inner,false)) return false;
	for (auto& setting:_passSettings) {
		setting.processingMode=passes.front().processingMode; setting.sparseUpdateInterval=p.interval;
	}
	_settings=_passSettings.front();
	for (auto& native:p.nativeResults)
		for (auto* texture:{&native.input,&native.output})
			if (!texture->Create(p.device,_impl->width,_impl->height,DXGI_FORMAT_R8G8B8A8_UNORM,false)) return false;
	const std::string shaderSource=std::string(DLSSNR_COLOR_HLSL)+std::string(DLSSNR_SPARSE_HLSL);
	if (!CreateComputeShader(*_impl,RESIDUAL_VERTICAL_COMPOSITE_SHADER,
		"ExtractControlledResidualVertical","DLSSNRResidualVerticalExtract",_impl->residualVerticalExtractShader11) ||
		!CreateComputeShader(*_impl,shaderSource,"MapMotion","DLSSNRSparseMap",p.mapShader) ||
		!CreateComputeShader(*_impl,shaderSource,"ComposeResidual","DLSSNRSparseCompose",p.composeShader) ||
		!CreateComputeShader(*_impl,shaderSource,"FillOutput","DLSSNRSparseFillOutput",p.outputShader)) return false;
	D3D11_BUFFER_DESC cb{}; cb.ByteWidth=sizeof(Sparse::Constants); cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
	if (FAILED(p.device->CreateBuffer(&cb,nullptr,p.constants.put()))) return false;
	D3D11_BUFFER_DESC count{}; count.ByteWidth=16; count.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
	count.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
	if (FAILED(p.device->CreateBuffer(&count,nullptr,p.counts.put()))) return false;
	D3D11_UNORDERED_ACCESS_VIEW_DESC uav{}; uav.Format=DXGI_FORMAT_R32_TYPELESS; uav.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;
	uav.Buffer.NumElements=4; uav.Buffer.Flags=D3D11_BUFFER_UAV_FLAG_RAW;
	if (FAILED(p.device->CreateUnorderedAccessView(p.counts.get(),&uav,p.countUav.put()))) return false;
	count.BindFlags=count.MiscFlags=0; count.Usage=D3D11_USAGE_STAGING; count.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
	for (auto& r:p.results) if (FAILED(p.device->CreateBuffer(&count,nullptr,r.contributionReadback.put()))) return false;
	D3D11_SAMPLER_DESC sampler{}; sampler.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP; sampler.MaxLOD=D3D11_FLOAT32_MAX;
	if (FAILED(p.device->CreateSamplerState(&sampler,p.sampler.put())) ||
		FAILED(p.device->CreateFence(0,D3D11_FENCE_FLAG_NONE,IID_PPV_ARGS(p.fence.put()))) || FAILED(p.event.create()) ||
		!RegisterWaitForSingleObject(&p.registeredWait,p.event.get(),WakeDlssnrOwner,
			reinterpret_cast<PVOID>(uintptr_t(GetCurrentThreadId())),INFINITE,WT_EXECUTEDEFAULT)) return false;
	_sparse=std::move(state); _failure={};
	Logger::Get().Info(fmt::format("DLSSNR sparse initialized: mode={} full={} passes={} updateInterval={} captures={} results={}",
		int(_settings.processingMode),_settings.inputResolutionPercent,_passSettings.size(),p.interval,p.CaptureCapacity,p.ResultCapacity));
	return true;
}

bool DLSSNRFilter::SubmitSparseInput(bool bootstrap) noexcept {
	if (!_sparse || !_impl) return false;
	auto& p=*_sparse;
	if (p.pending || !p.waiting) return true;
	if (!bootstrap && !p.parameterRefresh && !p.budget.CanSubmit(p.latestSource,p.urgent)) return true;
	auto* source=p.Find(p.latestSource);
	if (!source) {
		p.waiting=false; ++p.discarded;
		return true;
	}
	p.CopyFrame(p.pendingFrame,*source,false);
	const bool sameEpoch=p.lastInferenceEpoch==source->epoch && p.lastInferenceSource<source->sequence;
	const bool mapped=sameEpoch && p.Map(source->sequence,p.lastInferenceSource,p.pendingFrame.motion,p.pendingFrame.confidence,true);
	p.pendingFrame.guidance.motion.texture=p.pendingFrame.motion.texture.get();
	p.pendingFrame.guidance.confidence.texture=p.pendingFrame.confidence.texture.get();
	for (auto* ch:{&p.pendingFrame.guidance.motion,&p.pendingFrame.guidance.confidence}) {
		ch->metadata.sync={}; ch->metadata.isZero=!mapped; ch->metadata.requiresHistoryReset=false;
	}
	p.pendingFrame.guidance.requiresHistoryReset=false;
	if (!mapped) {
		const float zero[4]{};
		p.dc->ClearUnorderedAccessViewFloat(p.pendingFrame.motion.uav.get(),zero);
		p.dc->ClearUnorderedAccessViewFloat(p.pendingFrame.confidence.uav.get(),zero);
		p.pendingFrame.reset=true;
	} else {
		p.pendingFrame.previousFrame=p.lastInferenceFrame;
	}
	auto context=p.Context(p.pendingFrame,p.fullColor.texture.get(),true);
	p.submittedAt=std::chrono::steady_clock::now();
	if (!DrawInference(context,true,false) || !IsHealthy()) return false;
	const bool burst=p.budget.Submit(source->sequence,p.urgent);
	if (p.parameterRefresh) p.budget.RecordRefresh(source->sequence);
	p.parameterRefresh=false;
	if (burst) ++p.burstSubmissions;
	p.pending=true; p.pendingSource=source->sequence; p.pendingEpoch=source->epoch; p.pendingFrameId=source->frame;
	p.lastInferenceSource=source->sequence; p.lastInferenceFrame=source->frame; p.lastInferenceEpoch=source->epoch;
	p.waiting=false; p.urgent=false; ++p.fullSubmissions;
	return true;
}

void DLSSNRFilter::ServiceSparse() noexcept {
	if (!_sparse || !_impl || _impl->disabled) return;
	if (!ServiceInferenceSubmission() || InferenceSubmissionPending()) return;
	auto& p=*_sparse; ResetEvent(p.event.get());
	const auto done=_impl->fence12->GetCompletedValue();
	const auto contributionDone=p.fence->GetCompletedValue();
	if (done==UINT64_MAX || contributionDone==UINT64_MAX) { _impl->disabled=true; return; }
	for (auto& slot:_impl->commandSlots) if (slot.completionValue && slot.completionValue<=done) CollectGpuTiming(*_impl,slot);
	for (auto& result:p.results) {
		auto& sample=result.observation;
		if (!sample.ready) continue;
		if (sample.epoch!=p.epoch || sample.source!=result.source || sample.version!=result.version || sample.epoch!=result.epoch) {
			sample.Cancel(); continue;
		}
		if (sample.ready>contributionDone) continue;
		D3D11_MAPPED_SUBRESOURCE mapped{};
		const auto hr=p.dc->Map(result.contributionReadback.get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped);
		if (hr==DXGI_ERROR_WAS_STILL_DRAWING) continue;
		if (FAILED(hr)) { _impl->disabled=true; return; }
		const bool contributes=*static_cast<const uint32_t*>(mapped.pData)!=0;
		p.dc->Unmap(result.contributionReadback.get(),0);
		if (p.observationClock.Confirm(sample,result.source,result.epoch,result.version,p.epoch,contributes)) {
			// Only metadata advances here. Capture/target identity and the NR
			// source age remain the captures that produced the displayed pixels.
			++p.outputVersion;
		}
	}
	if (!p.pending || done<_impl->pendingInferenceValue) return;
	++p.completed; p.pending=false;
	p.turnaroundMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-p.submittedAt).count();
	if (FAILED(p.dc->Wait(_impl->fence11.get(),_impl->pendingInferenceValue))) { _impl->disabled=true; return; }
	// Obsolete inference cannot be selected for the current target. Drop it
	// before reconstructing and publishing its full-resolution residual.
	if (p.pendingEpoch!=p.epoch || p.pendingSource>p.sequence || p.sequence-p.pendingSource>p.maxSourceAge) {
		++p.discarded; return;
	}
	Impl& last=_impl->laterPasses.empty()?*_impl:*_impl->laterPasses.back();
	// One in-flight inference cannot overtake the preceding publication: its
	// input-ready fence was enqueued after that publication's D3D11 copies.
	// Keep its predecessor until ready; no third raw history can be selected.
	if (p.resultVersion && p.nativeResults[p.resultVersion%p.nativeResults.size()].ready>contributionDone) {
		_impl->disabled=true;
		_failure={DlssNrStage::Evaluate,DlssNrReason::Unknown,DlssNrCodeKind::None,0,"DLSSNR sparse native snapshot publication order"};
		return;
	}
	auto& result=p.results[(++p.resultVersion)%Sparse::ResultCapacity];
	auto& native=p.nativeResults[p.resultVersion%p.nativeResults.size()];
	p.dc->CopyResource(native.input.texture.get(),_impl->sharedInput11.get());
	p.dc->CopyResource(native.output.texture.get(),last.sharedOutput11.get());
	p.dc->CopyResource(result.reference.texture.get(),p.pendingFrame.input.texture.get());
	if (!CompositeResidual(*_impl,p.fullColor.texture.get(),native.output.srv.get(),_settings,result.residual.uav.get(),
		native.input.srv.get(),result.reference.srv.get())) {
		_impl->disabled=true; return;
	}
	result.source=p.pendingSource; result.epoch=p.pendingEpoch; result.frame=p.pendingFrameId; result.version=p.resultVersion;
	result.compositionRevision=p.compositionRevision;
	// Slot reuse discards any old asynchronous sample before this identity can
	// arm a new copy into its staging buffer. Immediate-context order fences it.
	result.observation={};
	result.ready=++p.fenceValue;
	native.version=result.version; native.ready=result.ready;
	if (FAILED(p.dc->Signal(p.fence.get(),result.ready)) || FAILED(p.fence->SetEventOnCompletion(result.ready,p.event.get()))) _impl->disabled=true;
	p.dc->Flush(); ++p.published;
}

bool DLSSNRFilter::DrainSparse() noexcept {
	if (!_sparse) return true;
	const bool submissionHealthy=ServiceInferenceSubmission(true);
	auto& p=*_sparse;
	if (_impl && !WaitForQueue(*_impl)) return false;
	ServiceSparse();
	const auto value=++p.fenceValue;
	wil::unique_event_nothrow retired;
	if (FAILED(retired.create()) || FAILED(p.dc->Signal(p.fence.get(),value)) || FAILED(p.fence->SetEventOnCompletion(value,retired.get()))) return false;
	p.dc->Flush();
	return WaitForSingleObject(retired.get(),30000)==WAIT_OBJECT_0 && p.fence->GetCompletedValue()>=value && submissionHealthy;
}

bool DLSSNRFilter::SparseHasPendingWork() const noexcept {
	if (!_sparse || !_impl || _impl->disabled) return false;
	const auto& p=*_sparse;
	return p.afterOutput && p.waiting && !p.pending && (p.parameterRefresh || p.budget.CanSubmit(p.latestSource,p.urgent));
}

void DLSSNRFilter::SparseAfterOutput() noexcept {
	if (!_sparse) return;
	_sparse->afterOutput=true; ServiceSparse();
	if (SparseHasPendingWork() && !SubmitSparseInput()) {
		_impl->disabled=true; _failure={DlssNrStage::Evaluate,DlssNrReason::Unknown,DlssNrCodeKind::None,0,"DLSSNR sparse after-output submission"};
	}
}

uint64_t DLSSNRFilter::SparseReadyOutputVersion() const noexcept {
	if (!_sparse || !_impl || _impl->disabled) return 0;
	const auto& p=*_sparse;
	const auto ready=p.fence->GetCompletedValue();
	const auto& targetFrame=p.captures[p.sequence%Sparse::CaptureCapacity];
	for (const auto& result:p.results) {
		if (result.version==p.outputResultVersion && result.epoch==p.epoch &&
			result.observation.NeedsRetry(p.outputFrame.sequence)) return _outputVersionBase+p.outputVersion+1;
		if (result.observation.ready && result.observation.ready<=ready && result.epoch==p.epoch)
			return _outputVersionBase+p.outputVersion+1;
		if (result.epoch==targetFrame.epoch && result.version>p.consumedResultVersion && result.ready<=ready && result.source<=p.sequence)
			return _outputVersionBase+p.outputVersion+1;
	}
	return 0;
}

NativeEffectDrawContext DLSSNRFilter::OutputDrawContext(const NativeEffectDrawContext& context) const noexcept {
	if (!_sparse || !_sparse->outputFrame.sequence) return context;
	auto result=_sparse->Context(_sparse->outputFrame,context.output,_sparse->outputAdvanced);
	result.inputHistoryReset=_sparse->outputReset; result.targetContentFrameRate=context.targetContentFrameRate;
	return result;
}

uint64_t DLSSNRFilter::OutputContentVersion() const noexcept { return _sparse?_sparse->outputVersion:0; }

DLSSNRSparseOutputInfo DLSSNRFilter::GetSparseOutputInfo() const noexcept {
	if (!_sparse) return {};
	const auto& p=*_sparse;
	return {.active=true,.held=p.held,.captureSequence=p.sequence,.targetSequence=p.outputFrame.sequence,
		.oldestSourceSequence=p.oldestSource,.epoch=p.outputFrame.epoch,.pastSourceFrameId=p.selectedPastSource,
		.observationRevision=p.observationClock.observedEpoch==p.outputFrame.epoch?p.observationClock.revision:0,.outputRevision=p.outputVersion,
		.observationTimestamp100ns=p.observationClock.observedEpoch==p.outputFrame.epoch?p.observationClock.timestamp:0,
		.maxSourceAge=p.maxSourceAge,.validity=p.validity.srv.get(),.pastContributionFraction=p.pastContribution};
}

DLSSNRPerformanceSnapshot DLSSNRFilter::SparsePerformanceSnapshot() const noexcept {
	DLSSNRPerformanceSnapshot s;
	if (!_sparse || !_impl) return s;
	const auto& p=*_sparse;
	s.captureSequence=p.sequence; s.fullCompleted=p.completed; s.fullLate=p.discarded; s.fullPublished=p.published;
	s.foregroundGpuSamples=_impl->foregroundTimer.samples; s.foregroundGpuMs=_impl->foregroundTimer.milliseconds;
	s.fullGpuMs=_impl->lastEvaluateGpuMs; s.fullGpuSamples=_impl->evaluateGpuSamples;
	s.fullTurnaroundMs=p.turnaroundMs;
	s.sparseFullSubmissions=p.fullSubmissions; s.sparseHeldOutputs=p.heldOutputs;
	s.fullSubmissions=p.fullSubmissions; s.sparseBurstSubmissions=p.burstSubmissions;
	s.sparsePastContributionFraction=p.pastContribution;
	return s;
}

void DLSSNRFilter::UpdateSparseParameters(bool inferenceChanged, bool residualChanged) noexcept {
	if (!_sparse) return;
	auto& p=*_sparse;
	p.invalidated|=inferenceChanged;
	if (!residualChanged) return;
	++p.compositionRevision;
	for (auto& result:p.results) {
		// An old-parameter readback cannot confirm new-parameter contribution.
		// Already observed NR sources keep their one observation; an unobserved
		// source may sample the recomposed pixels of this same target again.
		if (result.observation.observed) result.observation.Cancel();
		else result.observation={};
	}
}

bool DLSSNRFilter::DrawSparse(const NativeEffectDrawContext& context) noexcept {
	auto& p=*_sparse; auto* dc=p.dc;
	ServiceSparse(); if (!IsHealthy()) return false;
	p.afterOutput=false; p.outputAdvanced=false; p.outputReset=false;
	const bool newCapture=!p.initialized || (context.isNewCaptureFrame && context.frameId!=p.lastFrame);
	const bool contentChanged=!p.initialized || context.frameId!=p.lastFrame || context.inputRevision!=p.lastInputRevision;
	const int64_t timestamp=context.zeroFrameGuidance.motion.metadata.timestamp100ns;
	const bool sameFrameEdit=p.initialized && context.frameId==p.lastFrame && context.inputRevision!=p.lastInputRevision;
	const bool reset=!p.initialized || p.invalidated || sameFrameEdit || (contentChanged && (context.inputHistoryReset || context.frameGuidance.requiresHistoryReset)) ||
		context.inputHistoryRevision!=p.lastHistoryRevision || context.frameId<p.lastFrame ||
		(timestamp>0 && p.lastTimestamp>0 && (timestamp<p.lastTimestamp || timestamp-p.lastTimestamp>5'000'000));
	const bool parameterRefresh=p.invalidated || sameFrameEdit;
	if (newCapture) ++p.sequence;
	if (reset && p.initialized) {
		++p.epoch;
		for (auto& result:p.results) result.observation.Cancel();
	}
	p.budget.Advance(p.sequence);
	if (contentChanged || reset) {
		auto& f=p.captures[p.sequence%Sparse::CaptureCapacity];
		const auto selected=SelectGuidance(context,_settings,{p.width,p.height});
		if (!selected.IsValidFor(context.frameId,{p.width,p.height})) return false;
		f.guidance=selected; f.zero=context.zeroFrameGuidance;
		f.sequence=p.sequence; f.epoch=p.epoch; f.frame=context.frameId; f.previousFrame=context.previousCaptureFrameId;
		f.inputRevision=context.inputRevision; f.historyRevision=p.epoch; f.inputMetadata=context.inputMetadata; f.outputMetadata=context.outputMetadata;
		f.captureFrameIndex=context.captureFrameIndex; f.captureReflexFrameId=context.captureReflexFrameId;
		f.admissionTime=context.captureAdmissionTime100ns; f.reset=reset;
		auto* previous=p.Find(p.sequence-1);
		f.pairValid=previous && previous->epoch==f.epoch && previous->frame==f.previousFrame && !reset &&
			!selected.requiresHistoryReset && !selected.motion.metadata.isZero &&
			selected.motion.metadata.validRegion==FrameGuidanceRegion::Full({p.width,p.height});
		dc->CopyResource(f.input.texture.get(),context.input);
		const FrameGuidanceResource* sources[]{&selected.motion,&selected.depth,&selected.confidence};
		Sparse::Texture* destinations[]{&f.motion,&f.depth,&f.confidence};
		for (size_t i=0;i<3;++i) {
			const auto& sync=sources[i]->metadata.sync;
			if (sync.fence && sync.value && FAILED(dc->Wait(sync.fence,sync.value))) return false;
			dc->CopyResource(destinations[i]->texture.get(),sources[i]->texture);
		}
		p.Rebind(f);
	}
	p.lastFrame=context.frameId; p.lastInputRevision=context.inputRevision; p.lastHistoryRevision=context.inputHistoryRevision;
	p.lastTimestamp=timestamp; p.initialized=true; p.invalidated=false;
	if (parameterRefresh) p.parameterRefresh=true;
	auto request=[&](bool urgent) {
		auto* frame=p.Find(p.sequence);
		if (!frame) return;
		if (p.lastInferenceSource==p.sequence && p.lastInferenceEpoch==frame->epoch && !p.parameterRefresh) return;
		if (p.waiting && p.latestSource!=p.sequence) ++p.replacedRequests;
		p.latestSource=p.sequence; p.waiting=true; p.urgent=p.urgent || urgent;
	};
	if (parameterRefresh) request(true);
	if (!p.outputFrame.sequence) {
		request(false);
		if (!SubmitSparseInput(true) || !ServiceInferenceSubmission(true) || !WaitForFence(*_impl,_impl->pendingInferenceValue)) return false;
		ServiceSparse(); if (!IsHealthy()) return false;
		wil::unique_event_nothrow initialReady;
		if (FAILED(initialReady.create()) || FAILED(p.fence->SetEventOnCompletion(p.fenceValue,initialReady.get()))) return false;
		dc->Flush(); if (WaitForSingleObject(initialReady.get(),30000)!=WAIT_OBJECT_0) return false;
	}
	if (newCapture && (!p.budget.lastSource || p.sequence-p.budget.lastAdmission>=p.interval)) request(false);
	if (p.parameterRefresh) request(true);
	auto* target=p.Find(p.sequence);
	if (!target) return false;
	Sparse::Result* past=nullptr;
	const auto ready=p.fence->GetCompletedValue();
	for (auto& r:p.results) {
		if (!r.version || r.epoch!=target->epoch || r.ready>ready || r.source>p.sequence) continue;
		p.consumedResultVersion=std::max(p.consumedResultVersion,r.version);
		if (p.sequence-r.source<=p.maxSourceAge && (!past || r.source>past->source ||
			(r.source==past->source && r.version>past->version))) past=&r;
	}
	auto hold=[&]() {
		dc->CopyResource(context.output,p.lastOutput.texture.get()); p.held=true; ++p.heldOutputs;
		return true;
	};
	const bool sameTarget=target->sequence==p.outputFrame.sequence && target->epoch==p.outputFrame.epoch;
	const uint64_t resultVersion=past?past->version:0;
	const bool observationRetry=past && past->observation.NeedsRetry(target->sequence);
	const bool compositionChanged=p.outputCompositionRevision!=p.compositionRevision;
	if (sameTarget && !parameterRefresh && !compositionChanged && resultVersion==p.outputResultVersion && !observationRetry) return hold();
	if (past && past->compositionRevision!=p.compositionRevision) {
		auto& native=p.nativeResults[past->version%p.nativeResults.size()];
		if (native.version!=past->version) {
			_impl->disabled=true;
			_failure={DlssNrStage::Evaluate,DlssNrReason::Unknown,DlssNrCodeKind::None,0,"DLSSNR sparse native snapshot identity"};
			return false;
		}
		if (!CompositeResidual(*_impl,p.fullColor.texture.get(),native.output.srv.get(),_settings,past->residual.uav.get(),
			native.input.srv.get(),past->reference.srv.get())) return false;
		past->compositionRevision=p.compositionRevision;
		// Publication readiness belongs to the immutable paired snapshots. This
		// rebuild and its consumers share the immediate queue; replacing ready
		// with a new fence would wrongly hide it until a later Draw.
	}
	if (past && !p.Map(p.sequence,past->source,p.pastMotion,p.pastConfidence)) past=nullptr;
	Sparse::Constants values{p.width,p.height}; values.hasPast=past!=nullptr;
	ID3D11ShaderResourceView* srvs[Sparse::ShaderResourceCount]{};
	srvs[0]=target->input.srv.get();
	if (past) { srvs[1]=past->reference.srv.get(); srvs[2]=past->residual.srv.get(); srvs[3]=p.pastMotion.srv.get(); srvs[4]=p.pastConfidence.srv.get(); }
	const UINT clear[4]{}; dc->ClearUnorderedAccessViewUint(p.countUav.get(),clear);
	ID3D11UnorderedAccessView* composeUavs[5]{p.rawResidual.uav.get(),p.rawValidity.uav.get(),p.countUav.get()};
	p.Dispatch(p.composeShader.get(),values,srvs,composeUavs);
	srvs[21]=p.rawResidual.srv.get(); srvs[22]=p.rawValidity.srv.get();
	values.finalize=p.outputFormat!=DXGI_FORMAT_R16G16B16A16_FLOAT;
	ID3D11UnorderedAccessView* outputUavs[5]{p.lastOutput.uav.get(),p.validity.uav.get()};
	p.Dispatch(p.outputShader.get(),values,srvs,outputUavs);
	dc->CopyResource(context.output,p.lastOutput.texture.get());
	const uint64_t previousOutput=p.outputFrame.sequence;
	const uint64_t previousEpoch=p.outputFrame.epoch;
	const uint64_t previousFrameId=p.outputFrame.frame;
	p.CopyFrame(p.outputFrame,*target);
	p.outputAdvanced=target->sequence>previousOutput || target->epoch!=previousEpoch;
	p.outputReset=target->reset || target->epoch!=previousEpoch;
	if (previousOutput && p.outputAdvanced && target->previousFrame!=previousFrameId) {
		if (previousEpoch==target->epoch && p.Map(target->sequence,previousOutput,p.outputFrame.motion,p.outputFrame.confidence)) {
			p.outputFrame.previousFrame=previousFrameId;
			p.outputFrame.guidance.motion.metadata.isZero=false; p.outputFrame.guidance.confidence.metadata.isZero=false;
			p.outputFrame.guidance.motion.metadata.requiresHistoryReset=false; p.outputFrame.guidance.confidence.metadata.requiresHistoryReset=false;
			p.outputFrame.guidance.requiresHistoryReset=false;
		} else {
			p.outputReset=true; p.outputFrame.guidance=p.outputFrame.zero;
		}
	}
	p.held=false; ++p.outputVersion; p.outputResultVersion=resultVersion;
	p.outputCompositionRevision=p.compositionRevision;
	p.oldestSource=past?past->source:0; p.selectedPastSource=past?past->frame:0;
	// As before, economy reports selected mapping coverage; per-pixel trust is
	// carried by validity. Only confirmed pixel contribution advances NR time.
	p.pastContribution=past?1.:0;
	const int64_t contributionTimestamp=observationRetry && past?past->observation.deferredTimestamp:
		target->admissionTime?target->admissionTime:target->zero.motion.metadata.timestamp100ns;
	if (past && past->observation.Begin(past->source,past->epoch,past->version,target->sequence,p.fenceValue+1,contributionTimestamp)) {
		dc->CopyResource(past->contributionReadback.get(),p.counts.get());
		const auto completion=++p.fenceValue;
		if (FAILED(dc->Signal(p.fence.get(),completion)) || FAILED(p.fence->SetEventOnCompletion(completion,p.event.get()))) return false;
		dc->Flush();
	}
	if (newCapture && (p.sequence<=8 || p.sequence%120==0)) Logger::Get().Info(fmt::format(
		"DLSSNR sparse: mode={} capture={} target={} epoch={} source={} updateInterval={} full={} burst={} hold={} observations={}",
		int(_settings.processingMode),p.sequence,p.outputFrame.sequence,p.outputFrame.epoch,p.selectedPastSource,
		p.interval,p.fullSubmissions,p.burstSubmissions,p.heldOutputs,p.observationClock.revision));
	return true;
}
