#include <atomic>

// The real backend and NGX entry are used. Only the CPU entry is gated, so a
// synchronous owner-side Evaluate would hold these Draw/ServiceAsync calls.
struct NativeSubmissionGate {
	wil::unique_event_nothrow entered, release;
	DLSSNRFilter::Impl::EvaluateFeatureFn original = nullptr;
	std::atomic<DWORD> thread = 0;
	bool fail = false;
	static inline std::atomic<NativeSubmissionGate*> active = nullptr;
	NativeSubmissionGate() { Check(entered.create()); Check(release.create()); }
	static NVSDK_NGX_Result NVSDK_CONV Evaluate(ID3D12GraphicsCommandList* list,
		const NVSDK_NGX_Handle* feature, const NVSDK_NGX_Parameter* parameters,
		PFN_NVSDK_NGX_ProgressCallback progress) {
		auto& gate = *active.load(std::memory_order_acquire);
		gate.thread.store(GetCurrentThreadId(), std::memory_order_release);
		SetEvent(gate.entered.get());
		if (WaitForSingleObject(gate.release.get(), 15000) != WAIT_OBJECT_0)
			return NVSDK_NGX_Result_FAIL_InvalidParameter;
		// The outer production call already holds NgxRuntimeGuard. Calling the
		// raw export here keeps the real Evaluate under that same SEH boundary.
		return gate.fail ? NVSDK_NGX_Result_FAIL_InvalidParameter : gate.original(list, feature, parameters, progress);
	}
};

void RunSubmissionWorkerNativeCases(DeviceResources& resources, NgxD3D12Core& core) {
	for (const auto mode : {DLSSNRProcessingMode::Pipeline, DLSSNRProcessingMode::SparseEconomy})
	for (const unsigned passCount : {1u, 3u}) for (const bool fail : {false, true}) {
		PerformanceFixture fixture(resources, 129, 73);
		DLSSNRSettings settings;
		settings.processingMode = mode;
		settings.inputResolutionPercent = 75;
		settings.sparseUpdateInterval = 2;
		settings.residualTransfer = DLSSNRResidualTransfer::PerPixel;
		settings.motionRequest = MotionVectorRequest::Amd(AmdOpticalFlowMode::Performance);
		DLSSNRFilter chain;
		const std::vector<DLSSNRSettings> passes(passCount, settings);
		Require(chain.InitializeChain(resources, core, fixture.input.get(), fixture.output.get(), passes));
		testedDevice = core.Device();
		const std::array<float, 4> color{.4f, .3f, .6f, 1.f};
		Require(chain.Draw(fixture.CaptureCoherent(color)));
		const auto initialCounts = NativeEvaluationCounts(chain);
		Require(chain._impl->useSignedSnippet);
		NativeSubmissionGate gate;
		gate.fail = fail;
		gate.original = chain._impl->snippetEvaluateFeature;
		NativeSubmissionGate::active.store(&gate, std::memory_order_release);
		chain._impl->snippetEvaluateFeature = NativeSubmissionGate::Evaluate;
		std::optional<NativeEffectDrawContext> context;
		// Sparse pays the same accepted-capture interval as the product; Pipeline
		// can submit the capture immediately after the initial same-spec result.
		for (unsigned i = 0; i < (mode == DLSSNRProcessingMode::Pipeline ? 1u : 2u); ++i) {
			context.emplace(fixture.CaptureCoherent(color));
			Require(chain.Draw(*context));
			chain.ServiceAfterOutput();
		}
		Require(WaitForSingleObject(gate.entered.get(), 5000) == WAIT_OBJECT_0);
		Require(gate.thread.load(std::memory_order_acquire) != GetCurrentThreadId());
		Require(chain.InferenceSubmissionPending());
		const auto frozenFrame = chain._submission->job.frame;
		const auto frozenInput = chain._impl->sharedInput11.get();
		const auto started = std::chrono::steady_clock::now();
		for (unsigned i = 0; i < 20; ++i) {
			chain.ServiceAsync();
			context->isNewCaptureFrame = false;
			Require(chain.Draw(*context));
		}
		Require(NativeMilliseconds(started) < 1000);
		RequireNativeEvaluationDelta(chain, initialCounts, 0);
		// New captures replace only the owner mailbox. The worker's input,
		// settings and source identity stay frozen until the CPU call returns.
		for (unsigned i = 0; i < 2; ++i) {
			context.emplace(fixture.CaptureCoherent(color));
			Require(chain.Draw(*context));
			chain.ServiceAfterOutput();
		}
		Require(chain._submission->job.frame == frozenFrame && chain._impl->sharedInput11.get() == frozenInput);
		Require(chain.InferenceSubmissionPending());
		Require(chain._sparse ? chain._sparse->waiting : chain._performance->latestWaiting);
		RequireNativeEvaluationDelta(chain, initialCounts, 0);
		EffectOption option;
		option.parameters = {{"processingMode", float(mode)}, {"inputResolutionPercent", 75.f},
			{"sparseUpdateInterval", 2.f}, {"residualTransfer", 1.f}, {"opticalFlowMethod", 1.f},
			{"amdOpticalFlowMode", 0.f}, {"residualMultiplier", .5f}};
		const std::array<std::string, 1> residualNames{"residualMultiplier"};
		const auto editStarted = std::chrono::steady_clock::now();
		Require(chain.ApplyLiveParameters(option, residualNames));
		Require(NativeMilliseconds(editStarted) < 1000 && chain.InferenceSubmissionPending());
		Require(chain._settings.residualMultiplier == .5f && chain._impl->residualParametersDirty);
		Require(chain._submission->job.settings[0].residualMultiplier == 1.f);
		option.parameters["residualShowAdvanced"] = 1.f;
		const std::array<std::string, 1> displayNames{"residualShowAdvanced"};
		const auto displayStarted = std::chrono::steady_clock::now();
		Require(chain.ApplyLiveParameters(option, displayNames));
		Require(NativeMilliseconds(displayStarted) < 1000 && chain.InferenceSubmissionPending());
		// Stop owns the only allowed blocking boundary in this phase. A helper
		// only opens the event; it never calls D3D11 or changes feature state.
		wil::unique_event_nothrow drainFinished;
		Check(drainFinished.create());
		bool stoppedBeforeRelease = false;
		std::jthread release([&] {
			stoppedBeforeRelease = WaitForSingleObject(drainFinished.get(), 30) == WAIT_OBJECT_0;
			SetEvent(gate.release.get());
		});
		const bool drained = chain.Drain();
		SetEvent(drainFinished.get());
		release.join();
		Require(!stoppedBeforeRelease && !chain.InferenceSubmissionPending());
		Require(drained != fail);
		Require(chain._settings.residualMultiplier == .5f);
		chain._impl->snippetEvaluateFeature = gate.original;
		NativeSubmissionGate::active.store(nullptr, std::memory_order_release);
		if (fail) {
			Require(!chain.IsHealthy());
			Require(chain.GetDlssNrFailure().operation.find("EvaluateFeature") != std::string::npos);
			Require(chain._impl->evaluateCount == initialCounts[0]+1 && chain._impl->evaluateFailureCount == 1);
		} else {
			Require(chain.IsHealthy());
			RequireNativeEvaluationDelta(chain, initialCounts, 1);
			// CPU retirement cannot consume a residual edit that has not yet
			// reached the owner's real composition operation.
			if (!chain._sparse) Require(chain._impl->residualParametersDirty);
			if (passCount == 1) {
				// A real inference edit has a different contract: first reclaim
				// the feature, then advance its parameter/history generation.
				chain.ServiceAsync();
				ResetEvent(gate.entered.get()); ResetEvent(gate.release.get());
				NativeSubmissionGate::active.store(&gate, std::memory_order_release);
				chain._impl->snippetEvaluateFeature = NativeSubmissionGate::Evaluate;
				chain.ServiceAfterOutput();
				Require(WaitForSingleObject(gate.entered.get(), 5000) == WAIT_OBJECT_0);
				const auto parameterRevision = chain._impl->evaluateParameterRevision;
				const auto parameterEpoch = chain._performance ? chain._performance->state.epoch : 0;
				option.parameters["intensity"] = .75f;
				const std::array<std::string, 1> inferenceNames{"intensity"};
				ResetEvent(drainFinished.get());
				stoppedBeforeRelease = false;
				std::jthread releaseEdit([&] {
					stoppedBeforeRelease = WaitForSingleObject(drainFinished.get(), 30) == WAIT_OBJECT_0;
					SetEvent(gate.release.get());
				});
				Require(chain.ApplyLiveParameters(option, inferenceNames));
				SetEvent(drainFinished.get());
				releaseEdit.join();
				Require(!stoppedBeforeRelease && !chain.InferenceSubmissionPending());
				Require(chain._settings.intensity == .75f && chain._impl->resetHistory);
				Require(chain._impl->evaluateParameterRevision == parameterRevision+1);
				Require(chain._sparse ? chain._sparse->invalidated : chain._performance->state.epoch > parameterEpoch);
				chain._impl->snippetEvaluateFeature = gate.original;
				NativeSubmissionGate::active.store(nullptr, std::memory_order_release);
				Require(chain.Drain());
			}
		}
		Require(DebugMessages());
		std::cout << "native CPU submission gate passed: mode=" << int(mode) << " passes=" << passCount
			<< " failure=" << fail << " ownerResponsive=true immutableJob=true boundedMailbox=true"
			<< " residualEditNonblocking=true stopJoined=true\n";
	}
}
