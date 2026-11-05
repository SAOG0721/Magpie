#include <thread>
#include <optional>

// Actual production resources and queues are used throughout. The synthetic
// guidance makes a static frame fully trusted without running an OF provider.
double NativeMilliseconds(std::chrono::steady_clock::time_point started) {
	return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
}

double NativeResidentMiB(DeviceResources& resources) {
	DXGI_QUERY_VIDEO_MEMORY_INFO memory{};
	Check(resources.GetGraphicsAdapter()->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_LOCAL,&memory));
	return double(memory.CurrentUsage)/(1024.*1024.);
}

std::vector<uint64_t> NativeEvaluationCounts(const DLSSNRFilter& chain) {
	std::vector<uint64_t> result{chain._impl->evaluateCount};
	for (const auto& pass : chain._impl->laterPasses) result.push_back(pass->evaluateCount);
	return result;
}

void RequireNativeEvaluationDelta(const DLSSNRFilter& chain, const std::vector<uint64_t>& before,
	uint64_t delta) {
	const auto after = NativeEvaluationCounts(chain);
	Require(after.size() == before.size());
	for (size_t i=0; i<after.size(); ++i) Require(after[i] == before[i]+delta);
}

size_t NativeCurrentSlot(const DLSSNRFilter& chain) {
	const auto& p = *chain._performance;
	return p.pool.Latest(p.state,UINT64_MAX);
}

void RequireNativeCurrentResult(DeviceResources& resources, DLSSNRFilter& chain,
	const std::array<float,4>& color) {
	const auto& p = *chain._performance;
	const auto slot = NativeCurrentSlot(chain);
	Require(slot != DLSSNREnhancementPool::None);
	Require(p.pool[slot].sequence == p.state.sequence && p.pool[slot].epoch == p.state.epoch);
	CheckColor(resources,p.results[slot].reference.texture.get(),color.data());
	CheckPublishedDifference(resources,chain,slot);
}

void WaitNativeOutput(DeviceResources& resources) {
	winrt::com_ptr<ID3D11Fence> fence;
	Check(resources.GetD3DDevice()->CreateFence(0,D3D11_FENCE_FLAG_NONE,IID_PPV_ARGS(fence.put())));
	wil::unique_event_nothrow ready;
	Check(ready.create());
	Check(resources.GetD3DDC()->Signal(fence.get(),1));
	resources.GetD3DDC()->Flush();
	Check(fence->SetEventOnCompletion(1,ready.get()));
	Require(WaitForSingleObject(ready.get(),15000) == WAIT_OBJECT_0);
}

void CompleteNativePerformanceOutput(DeviceResources& resources, DLSSNRFilter& chain,
	NativeEffectDrawContext context) {
	const auto targetSequence = chain._performance->state.sequence;
	const auto targetEpoch = chain._performance->state.epoch;
	chain.ServiceAfterOutput();
	PollPerformance(resources,chain,[&] {
		const auto& p = *chain._performance;
		// Inference completion can precede D3D11 publication. An older readable
		// result does not establish that this capture is ready for adoption.
		const auto selected = p.pool.Latest(p.state,p.poolFence->GetCompletedValue());
		return !p.pending && selected != DLSSNREnhancementPool::None &&
			p.pool[selected].sequence == targetSequence && p.pool[selected].epoch == targetEpoch;
	});
	context.isNewCaptureFrame = false;
	Require(chain.Draw(context));
	Require(chain._performance->readySequence == targetSequence && chain._performance->readyEpoch == targetEpoch);
	WaitNativeOutput(resources);
	chain.ServiceAfterOutput();
}

unsigned RequireNativeFullBound(const DLSSNRFilter& chain) {
	unsigned owned = 1;
	if (chain._resolutionChange) {
		const auto& change = *chain._resolutionChange;
		owned += unsigned(bool(change.full)) + unsigned(bool(change.retiredFull)) + unsigned(bool(change.hotFull));
	}
	Require(owned <= 2); // active plus one initializing, retiring, or hot full chain
	return owned;
}

void CommitNativeResolution(DLSSNRFilter& chain) {
	const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(15);
	while (!chain.CommitPreparedResolution()) {
		Require(std::chrono::steady_clock::now() < deadline);
		Sleep(1);
	}
}

void RunPostOutputPoolRetryNativeCase(DeviceResources& resources, NgxD3D12Core& core) {
	PerformanceFixture fixture(resources);
	DLSSNRSettings settings;
	settings.processingMode = DLSSNRProcessingMode::Pipeline;
	settings.inputResolutionPercent = 75;
	DLSSNRFilter chain;
	Require(chain.Initialize(resources,core,fixture.input.get(),fixture.output.get(),settings));
	testedDevice = core.Device();
	auto initial = fixture.CaptureCoherent({.2f,.2f,.6f,1});
	Require(chain.Draw(initial));WaitNativeOutput(resources);
	auto context = fixture.CaptureCoherent({.2f,.2f,.6f,1});
	Require(chain.Draw(context));WaitNativeOutput(resources);
	auto& p = *chain._performance;
	const auto initialSubmissions=p.fullSubmissions;
	Require(initialSubmissions == 1 && p.latestWaiting && !p.pending && !chain.HasPendingPostOutputWork());
	// Fence-protect every pool entry, as if queued consumers still held them.
	// The event must wake the idle owner when those GPU protections retire.
	MSG message{};
	while (PeekMessageW(&message,nullptr,WM_NULL,WM_NULL,PM_REMOVE)) {}
	winrt::com_ptr<ID3D11Fence> consumerGate;
	winrt::com_ptr<ID3D12Fence> consumerGate12;
	Check(core.Device()->CreateFence(0,D3D12_FENCE_FLAG_SHARED,IID_PPV_ARGS(consumerGate12.put())));
	wil::unique_handle consumerHandle;
	Check(core.Device()->CreateSharedHandle(consumerGate12.get(),nullptr,GENERIC_ALL,nullptr,consumerHandle.put()));
	Check(resources.GetD3DDevice()->OpenSharedFence(consumerHandle.get(),IID_PPV_ARGS(consumerGate.put())));
	Check(resources.GetD3DDC()->Wait(consumerGate.get(),1));
	const auto retired = ++p.poolFenceValue;
	Check(resources.GetD3DDC()->Signal(p.poolFence.get(),retired));
	for (size_t i=0; i<DLSSNREnhancementPool::Capacity; ++i) p.pool.ReadSubmitted(i,retired);
	chain.ServiceAfterOutput();
	Require(p.poolBusy == 1 && p.poolRetryFence == retired && !p.pending && p.latestWaiting);
	Require(!chain.HasPendingPostOutputWork());
	chain.ServiceAsync();
	Require(p.fullSubmissions == initialSubmissions && !chain.HasPendingPostOutputWork());
	Check(consumerGate12->Signal(1));
	PollPerformance(resources,chain,[&] {
		return chain.HasPendingPostOutputWork() && PeekMessageW(&message,nullptr,WM_NULL,WM_NULL,PM_REMOVE);
	});
	Require(!chain.HasReadyOutput()); // wake-up requests maintenance, not a quality redraw
	chain.ServiceAfterOutput();
	Require(p.pending && !p.latestWaiting && p.fullSubmissions == initialSubmissions+1 && p.poolRetryFence == 0);
	PollPerformance(resources,chain,[&] { return chain.HasReadyOutput(); });
	context.isNewCaptureFrame = false;
	Require(chain.Draw(context) && p.qualityUpdates == 1 && !chain.HasPendingPostOutputWork());
	Require(chain.Drain() && DebugMessages());
	std::cout << "native post-output pool retry passed: unpublishedCaptureBlocked=true idleOwnerWake=true boundedMailbox=true finalQualityUpdate=true\n";
}

void RunPipelineNativeCases(DeviceResources& resources, NgxD3D12Core& core) {
	const std::array<float,4> blue{.2f,.2f,.6f,1}, green{.2f,.6f,.2f,1};
	for (int model : {0,1,2}) for (unsigned passCount : {1u,3u}) {
		PerformanceFixture fixture(resources);
		DLSSNRSettings settings;
		settings.modelPreset = model;
		settings.processingMode = DLSSNRProcessingMode::Pipeline;
		settings.residualTransfer = DLSSNRResidualTransfer::PerPixel;
		settings.motionRequest = MotionVectorRequest::Amd(AmdOpticalFlowMode::Performance);
		settings.inputResolutionPercent = 75;
		const std::vector<DLSSNRSettings> passes(passCount,settings);
		DLSSNRFilter chain;
		Require(chain.InitializeChain(resources,core,fixture.input.get(),fixture.output.get(),passes));
		testedDevice = core.Device();
		auto& p = *chain._performance;
		auto first = fixture.CaptureCoherent(blue);
		const auto initialCounts = NativeEvaluationCounts(chain);
		Require(chain.Draw(first));
		RequireNativeEvaluationDelta(chain,initialCounts,1);
		Require(p.fullSubmissions == 1 && p.pipelineFallbacks == 1 && !p.pending && !p.latestWaiting);
		RequireNativeCurrentResult(resources,chain,blue);
		const auto firstOutput = ReadPerformanceTexture(resources,fixture.output.get());
		first.isNewCaptureFrame = false;
		const auto firstRevision = p.outputRevision;
		Require(chain.Draw(first));
		RequireNativeEvaluationDelta(chain,initialCounts,1);
		Require(p.outputRevision == firstRevision);

		// A fully trusted age-one source is the foreground result. The next full
		// capture is queued only once, after output, and never evaluates lite.
		auto trusted = fixture.CaptureCoherent(blue);
		const auto trustedCounts = NativeEvaluationCounts(chain);
		Require(chain.Draw(trusted));
		RequireNativeEvaluationDelta(chain,trustedCounts,0);
		Require(!p.pending && p.latestWaiting && p.latestSequence == p.state.sequence);
		const auto previous = NativeCurrentSlot(chain);
		Require(previous != DLSSNREnhancementPool::None && p.state.sequence-p.pool[previous].sequence == 1);
		const auto trustedOutput = ReadPerformanceTexture(resources,fixture.output.get());
		Require(trustedOutput == firstOutput);
		chain.ServiceAsync();
		RequireNativeEvaluationDelta(chain,trustedCounts,0);
		chain.ServiceAfterOutput();
		Require(p.pending && !p.latestWaiting && p.fullSubmissions == 2);
		chain.ServiceAfterOutput();
		Require(p.fullSubmissions == 2);
		CompleteNativePerformanceOutput(resources,chain,trusted);
		RequireNativeEvaluationDelta(chain,trustedCounts,1);
		RequireNativeCurrentResult(resources,chain,blue);

		// A missed post-output opportunity keeps its true source through age3.
		// At age4, without any pending result, rebuild exactly once at the active
		// resolution/pass specification and clear the replaceable mailbox.
		const auto historySequence = p.readySequence;
		const auto reuseCounts = NativeEvaluationCounts(chain);
		const auto reuseFallbacks = p.pipelineFallbacks;
		for (uint64_t age : {1u,2u,3u}) {
			auto skipped = fixture.CaptureCoherent(blue);
			Require(chain.Draw(skipped) && p.latestWaiting && !p.pending);
			Require(p.readySequence == historySequence && p.state.sequence-historySequence == age);
			RequireNativeEvaluationDelta(chain,reuseCounts,0);
			Require(p.pipelineFallbacks == reuseFallbacks);
		}
		const auto expiredCounts = NativeEvaluationCounts(chain);
		const auto fallbacksBeforeAge = p.pipelineFallbacks;
		auto expired = fixture.CaptureCoherent(blue);
		Require(chain.Draw(expired));
		RequireNativeEvaluationDelta(chain,expiredCounts,1);
		Require(p.state.maxAgeFrames == 3 && p.pipelineFallbacks == fallbacksBeforeAge+1);
		Require(!p.pending && !p.latestWaiting && p.lastInferenceSequence == p.state.sequence);
		RequireNativeCurrentResult(resources,chain,blue);
		chain.ServiceAfterOutput();
		RequireNativeEvaluationDelta(chain,expiredCounts,1);

		// One rejected block uses the same protected continuous shader as mode1.
		// It neither evaluates full in front nor suppresses the post-output job.
		auto rejected = fixture.CaptureCoherent(blue);
		fixture.RejectConfidenceBlock();
		const auto rejectCounts = NativeEvaluationCounts(chain);
		const auto rejectSubmissions = p.fullSubmissions;
		const auto rejectFallbacks = p.pipelineFallbacks;
		Require(chain.Draw(rejected));
		RequireNativeEvaluationDelta(chain,rejectCounts,0);
		Require(p.fullSubmissions == rejectSubmissions && p.pipelineFallbacks == rejectFallbacks);
		Require(!p.pending && p.latestWaiting);
		chain.ServiceAfterOutput();
		Require(p.pending && !p.latestWaiting && p.fullSubmissions == rejectSubmissions+1);
		chain.ServiceAfterOutput();
		Require(p.fullSubmissions == rejectSubmissions+1);
		CompleteNativePerformanceOutput(resources,chain,rejected);
		RequireNativeEvaluationDelta(chain,rejectCounts,1);
		RequireNativeCurrentResult(resources,chain,blue);

		auto reset = fixture.CaptureCoherent(blue);
		reset.inputHistoryReset = true;
		const auto resetCounts = NativeEvaluationCounts(chain);
		const auto oldEpoch = p.state.epoch;
		Require(chain.Draw(reset));
		Require(p.state.epoch > oldEpoch && !p.pending && !p.latestWaiting);
		RequireNativeEvaluationDelta(chain,resetCounts,1);
		RequireNativeCurrentResult(resources,chain,blue);
		chain.ServiceAfterOutput();
		RequireNativeEvaluationDelta(chain,resetCounts,1);

		// Missing motion is a complete fallback as well, with the same feature
		// extent and pass count as the active chain.
		auto missing = fixture.Capture(blue);
		const auto missingCounts = NativeEvaluationCounts(chain);
		Require(chain.Draw(missing));
		RequireNativeEvaluationDelta(chain,missingCounts,1);
		RequireNativeCurrentResult(resources,chain,blue);

		// Keep the real feature queue gated across two target captures. Soft
		// rejection must return while it is still gated, preserve one immutable
		// job plus the latest mailbox, and adopt that genuine result at age2/3.
		for (uint32_t completionAge : {2u,3u}) {
			auto anchor = fixture.CaptureCoherent(blue);
			anchor.inputHistoryReset = true;
			Require(chain.Draw(anchor));
			RequireNativeCurrentResult(resources,chain,blue);
			auto inFlight = fixture.CaptureCoherent(blue);
			const auto beforeJob = NativeEvaluationCounts(chain);
			const auto beforeFallbacks = p.pipelineFallbacks;
			const auto beforeSubmissions = p.fullSubmissions;
			const auto beforeDiscards = p.lateDiscards;
			Require(chain.Draw(inFlight));
			RequireNativeEvaluationDelta(chain,beforeJob,0);
			winrt::com_ptr<ID3D12Fence> gate;
			Check(core.Device()->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(gate.put())));
			Check(chain._impl->queue12->Wait(gate.get(),1));
			chain.ServiceAfterOutput();
			Require(p.pending && !p.latestWaiting && p.fullSubmissions == beforeSubmissions+1);
			// CPU submission is independent of the GPU gate. Acquire its counters
			// before asserting them, while the command queue stays blocked.
			PollPerformance(resources,chain,[&] { return !chain.InferenceSubmissionPending(); });
			Require(gate->GetCompletedValue() == 0 && p.pending);
			const auto jobSequence = p.pendingSequence, jobEpoch = p.pendingEpoch;
			wil::unique_event_nothrow drawsFinished;
			Check(drawsFinished.create());
			bool timedOut = false;
			std::jthread release([&] {
				const auto status = WaitForSingleObject(drawsFinished.get(),2000);
				timedOut = status != WAIT_OBJECT_0;
				Check(gate->Signal(1));
			});
			std::optional<NativeEffectDrawContext> target;
			for (uint32_t age : {1u,2u}) {
				target.emplace(fixture.CaptureCoherent(age == 1 ? green : blue));
				fixture.RejectConfidenceBlock();
				Require(chain.Draw(*target));
				Require(p.pending && gate->GetCompletedValue() == 0);
				Require(p.pendingSequence == jobSequence && p.pendingEpoch == jobEpoch);
				Require(p.state.sequence-jobSequence == age);
				Require(p.readySequence == p.state.sequence-age-1);
				Require(p.latestWaiting && p.latestSequence == p.state.sequence);
				Require(p.pipelineFallbacks == beforeFallbacks && p.fullSubmissions == beforeSubmissions+1);
				RequireNativeEvaluationDelta(chain,beforeJob,1);
				chain.ServiceAfterOutput();
				Require(p.fullSubmissions == beforeSubmissions+1);
			}
			Check(SetEvent(drawsFinished.get()) ? S_OK : HRESULT_FROM_WIN32(GetLastError()));
			release.join();
			Require(!timedOut);
			if (completionAge == 3) {
				target.emplace(fixture.CaptureCoherent(green));
				fixture.RejectConfidenceBlock();
				Require(chain.Draw(*target));
			} else {
				PollPerformance(resources,chain,[&] { return !p.pending && chain.HasReadyOutput(); });
				target->isNewCaptureFrame = false;
				Require(chain.Draw(*target));
			}
			const auto adopted = NativeCurrentSlot(chain);
			Require(adopted != DLSSNREnhancementPool::None && !p.pending);
			Require(p.readySequence == jobSequence && p.readyEpoch == jobEpoch);
			Require(p.pool[adopted].sequence == jobSequence && p.state.sequence-jobSequence == completionAge);
			Require(p.lateDiscards == beforeDiscards && p.pipelineFallbacks == beforeFallbacks);
			Require(p.fullSubmissions == beforeSubmissions+1);
			RequireNativeEvaluationDelta(chain,beforeJob,1);
			CheckColor(resources,p.results[adopted].reference.texture.get(),blue.data());
			CheckPublishedDifference(resources,chain,adopted);
			if (completionAge == 3) CheckColor(resources,fixture.output.get(),green.data());
			// This target has not been evaluated yet. The single post-output job
			// must refresh it once even though its foreground used late history.
			chain.ServiceAfterOutput();
			Require(p.fullSubmissions == beforeSubmissions+2);
			chain.ServiceAfterOutput();
			Require(p.fullSubmissions == beforeSubmissions+2);
			CompleteNativePerformanceOutput(resources,chain,*target);
			RequireNativeEvaluationDelta(chain,beforeJob,2);
			RequireNativeCurrentResult(resources,chain,completionAge == 3 ? green : blue);
		}
		Require(chain.Drain() && DebugMessages());
		std::cout << "native Pipeline passed: model=" << model << " passes=" << passCount
			<< " protectedReuseNoForegroundFull=true noLiteEvaluate=true postOutputOnly=true maxAge=3"
			<< " lateAge2/3Adoption=true trueSourceOrdinal=true age4/reset/missingMotion=sameResolutionFull"
			<< " pendingSoftRejectionNonblocking=true noDoubleCompute=true\n";
	}
}

void RunHotResolutionNativeCases(DeviceResources& resources, NgxD3D12Core& core) {
	const std::array<float,4> blue{.2f,.2f,.6f,1}, marker{.6f,.6f,.2f,1};
	std::ofstream costs("DLSSNRHotResolutionCosts.csv"), json("DLSSNRHotResolutionCosts.json");
	costs << "mode,model,passes,coldCreateMs,prewarmCreateMs,firstWarmMs,hotPrepareMs,hotCommitMs,hotFirstOutputMs,returnPrepareMs,returnCommitMs,returnFirstOutputMs,activeResidentMiB,prewarmResidentMiB,warmedResidentMiB,switchedResidentMiB,returnedResidentMiB,ownedFullPeak\n";
	json << "{\"schemaVersion\":1,\"sourceWidth\":513,\"sourceHeight\":289,\"cases\":[\n";
	bool comma = false;
	for (auto mode : {DLSSNRProcessingMode::Pipeline})
	for (int model : {0,1,2}) for (unsigned passCount : {1u,3u}) {
		PerformanceFixture fixture(resources);
		DLSSNRSettings settings;
		settings.modelPreset = model; settings.processingMode = mode;
		settings.inputResolutionPercent = 75;
		settings.residualTransfer = DLSSNRResidualTransfer::PerPixel;
		settings.motionRequest = MotionVectorRequest::Amd(AmdOpticalFlowMode::Performance);
		const std::vector<DLSSNRSettings> passes(passCount,settings);
		DLSSNRFilter chain;
		auto started = std::chrono::steady_clock::now();
		Require(chain.InitializeChain(resources,core,fixture.input.get(),fixture.output.get(),passes));
		const double createMs = NativeMilliseconds(started), activeMiB = NativeResidentMiB(resources);
		testedDevice = core.Device();
		auto* original = chain._impl.get();
		started = std::chrono::steady_clock::now();
		Require(chain.PrewarmResolution(resources,fixture.input.get(),fixture.output.get(),70));
		const double prewarmMs = NativeMilliseconds(started), prewarmMiB = NativeResidentMiB(resources);
		auto& change = *chain._resolutionChange;
		auto* warmed = change.full->_impl.get();
		Require(change.warming && change.pending && !change.hotFull && chain._impl.get() == original);
		Require(!chain.HasHotResolution(70) && !chain.CommitPreparedResolution());
		unsigned peakOwned = RequireNativeFullBound(chain);
		winrt::com_ptr<ID3D11UnorderedAccessView> outputUav;
		Check(resources.GetD3DDevice()->CreateUnorderedAccessView(fixture.output.get(),nullptr,outputUav.put()));
		resources.GetD3DDC()->ClearUnorderedAccessViewFloat(outputUav.get(),marker.data());
		auto first = fixture.CaptureCoherent(blue);
		started = std::chrono::steady_clock::now();
		Require(chain.WarmPreparedResolution(first));
		const double warmMs = NativeMilliseconds(started), warmedMiB = NativeResidentMiB(resources);
		Require(chain.HasHotResolution(70) && chain.CanPrepareResolution());
		Require(!change.pending && !change.warming && change.hotFull->_impl.get() == warmed);
		Require(change.hotFull->_impl->evaluateCount == 1 && !change.hotFull->_impl->cache.valid);
		for (const auto& pass : change.hotFull->_impl->laterPasses) Require(pass->evaluateCount == 1);
		CheckColor(resources,fixture.output.get(),marker.data()); // warming never presents its pixels
		Require(chain._impl->evaluateCount == 0 && chain.Draw(first));
		CompleteNativePerformanceOutput(resources,chain,first);

		// Commit while the former active full chain is fenced. NR's session
		// orders subsequent GPU evaluations after that old work completes.
		auto current = fixture.CaptureCoherent(blue);
		Require(chain.Draw(current));
		winrt::com_ptr<ID3D12Fence> gate;
		Check(core.Device()->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(gate.put())));
		Check(original->queue12->Wait(gate.get(),1));
		chain.ServiceAfterOutput();
		Require(chain._performance->pending);
		const auto oldEpoch = chain._performance->state.epoch;
		started = std::chrono::steady_clock::now();
		Require(chain.PrepareResolution(resources,fixture.input.get(),fixture.output.get(),70));
		const double prepareMs = NativeMilliseconds(started);
		Require(change.cacheHit && change.full->_impl.get() == warmed && !change.hotFull);
		started = std::chrono::steady_clock::now();
		CommitNativeResolution(chain);
		const double commitMs = NativeMilliseconds(started);
		peakOwned = std::max(peakOwned,RequireNativeFullBound(chain));
		Require(chain._impl.get() == warmed);
		Require(change.retiredFull->_impl.get() == original && !chain.HasHotResolution(75));
		Require(!chain.CanPrepareResolution() && !chain.HasReadyOutput());
		Require(chain._performance->state.epoch > oldEpoch && NativeCurrentSlot(chain) == DLSSNREnhancementPool::None);
		Require(!change.retiredFull->IsGpuIdle());
		Check(gate->Signal(1)); // release old work before waiting for the ordered current evaluation
		const auto warmedCounts = NativeEvaluationCounts(chain);
		current.isNewCaptureFrame = false;
		started = std::chrono::steady_clock::now();
		Require(chain.Draw(current));
		CompleteNativePerformanceOutput(resources,chain,current);
		const double firstOutputMs = NativeMilliseconds(started), switchedMiB = NativeResidentMiB(resources);
		RequireNativeEvaluationDelta(chain,warmedCounts,1);
		RequireNativeCurrentResult(resources,chain,blue);
		Require(chain.Drain());
		chain.ServiceAfterOutput();
		Require(chain.HasHotResolution(75) && change.hotFull->_impl.get() == original);
		Require(!change.retiredFull && chain.CanPrepareResolution());
		peakOwned = std::max(peakOwned,RequireNativeFullBound(chain));

		// Return to the exact original feature. Its former pending/pool/cache
		// state cannot become current simply because the capture ID is the same.
		started = std::chrono::steady_clock::now();
		Require(chain.PrepareResolution(resources,fixture.input.get(),fixture.output.get(),75));
		const double returnPrepareMs = NativeMilliseconds(started);
		Require(change.cacheHit && change.full->_impl.get() == original);
		started = std::chrono::steady_clock::now();
		CommitNativeResolution(chain);
		const double returnCommitMs = NativeMilliseconds(started);
		Require(chain._impl.get() == original);
		Require(!chain.HasReadyOutput() && !chain._performance->pending && !chain._performance->latestWaiting);
		Require(NativeCurrentSlot(chain) == DLSSNREnhancementPool::None && !chain._impl->cache.valid);
		const auto returnCounts = NativeEvaluationCounts(chain);
		started = std::chrono::steady_clock::now();
		Require(chain.Draw(current));
		CompleteNativePerformanceOutput(resources,chain,current);
		const double returnOutputMs = NativeMilliseconds(started), returnedMiB = NativeResidentMiB(resources);
		RequireNativeEvaluationDelta(chain,returnCounts,1);
		RequireNativeCurrentResult(resources,chain,blue);
		Require(chain.HasHotResolution(70));
		peakOwned = std::max(peakOwned,RequireNativeFullBound(chain));

		// Both inference and composition edits invalidate cached settings.
		EffectOption option;
		option.parameters = {{"processingMode",float(mode)},{"modelPreset",float(model)},
			{"inputResolutionPercent",75.f},{"residualTransfer",1.f},{"opticalFlowMethod",1.f},
			{"amdOpticalFlowMode",0.f},{"intensity",1.f},{"performanceShowAdvanced",1.f}};
		const std::array<std::string,1> inferenceEdit{"intensity"}, compositionEdit{"residualMultiplier"};
		const std::array<std::string,1> displayEdit{"performanceShowAdvanced"};
		auto* retainedHot = change.hotFull.get();
		Require(chain.ApplyLiveParameters(option,displayEdit));
		Require(chain.HasHotResolution(70) && change.hotFull.get() == retainedHot);
		option.parameters["intensity"] = .5f;
		Require(chain.ApplyLiveParameters(option,inferenceEdit));
		Require(!chain.HasHotResolution(70) && !change.hotFull && !change.retiredFullCacheable);
		Require(chain.PrewarmResolution(resources,fixture.input.get(),fixture.output.get(),70));
		Require(chain.WarmPreparedResolution(current) && chain.HasHotResolution(70));
		option.parameters["residualMultiplier"] = .5f;
		Require(chain.ApplyLiveParameters(option,compositionEdit));
		Require(!chain.HasHotResolution(70) && !change.hotFull && !change.retiredFullCacheable);
		if (mode == DLSSNRProcessingMode::Pipeline && model == 0 && passCount == 1) {
			// A direct-filter caller may edit after ordinary Prepare, before
			// Commit. The old candidate must never undo the accepted live edit.
			Require(chain.PrepareResolution(resources,fixture.input.get(),fixture.output.get(),70));
			Require(change.pending && change.full);
			option.parameters["intensity"] = .75f;
			Require(chain.ApplyLiveParameters(option,inferenceEdit));
			Require(!change.pending && !chain.CommitPreparedResolution());
			Require(chain._impl.get() == original && chain._settings.intensity == .75f);
			Require(chain._settings.residualMultiplier == .5f && chain._settings.inputResolutionPercent == 75);
			Require(chain.Drain());
			chain.ServiceAfterOutput();
			Require(chain.CanPrepareResolution() && !change.hotFull && !change.retiredFull);
		}
		Require(chain.Drain() && DebugMessages());
		costs << int(mode) << ',' << model << ',' << passCount << ',' << createMs << ',' << prewarmMs << ',' << warmMs
			<< ',' << prepareMs << ',' << commitMs << ',' << firstOutputMs << ',' << returnPrepareMs << ',' << returnCommitMs
			<< ',' << returnOutputMs << ',' << activeMiB << ',' << prewarmMiB << ',' << warmedMiB << ',' << switchedMiB
			<< ',' << returnedMiB << ',' << peakOwned << '\n';
		costs.flush();
		if (comma) json << ",\n";
		comma = true;
		json << "{\"mode\":" << int(mode) << ",\"model\":" << model << ",\"passes\":" << passCount
			<< ",\"coldCreateMs\":" << createMs << ",\"prewarmCreateMs\":" << prewarmMs << ",\"firstWarmMs\":" << warmMs
			<< ",\"hotPrepareMs\":" << prepareMs << ",\"hotCommitMs\":" << commitMs << ",\"hotFirstOutputMs\":" << firstOutputMs
			<< ",\"returnPrepareMs\":" << returnPrepareMs << ",\"returnCommitMs\":" << returnCommitMs
			<< ",\"returnFirstOutputMs\":" << returnOutputMs << ",\"activeResidentMiB\":" << activeMiB
			<< ",\"prewarmResidentMiB\":" << prewarmMiB << ",\"warmedResidentMiB\":" << warmedMiB
			<< ",\"switchedResidentMiB\":" << switchedMiB << ",\"returnedResidentMiB\":" << returnedMiB
			<< ",\"ownedFullPeak\":" << peakOwned << '}';
		json.flush();
		std::cout << "native hot-cache passed: mode=" << int(mode) << " model=" << model << " passes=" << passCount
			<< " startupWarm=true featureReuse=true sameCaptureFresh=true retirementFence=true parameterInvalidation=true"
			<< " ownedFullPeak=" << peakOwned << " hotPrepareMs=" << prepareMs << " hotCommitMs=" << commitMs
			<< " returnPrepareMs=" << returnPrepareMs << " returnCommitMs=" << returnCommitMs << '\n';
	}
	json << "\n]}\n";
}
