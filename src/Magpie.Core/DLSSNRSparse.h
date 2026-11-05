// Private resource owner, included inside namespace Magpie after Impl.
struct DLSSNRFilter::Sparse {
	static constexpr size_t CaptureCapacity = 12, ResultCapacity = 4;
	static constexpr unsigned ShaderResourceCount = 23;
	struct Texture {
		winrt::com_ptr<ID3D11Texture2D> texture;
		winrt::com_ptr<ID3D11ShaderResourceView> srv;
		winrt::com_ptr<ID3D11UnorderedAccessView> uav;
		bool Create(ID3D11Device5* device, uint32_t w, uint32_t h, DXGI_FORMAT format,
			bool writable = true, bool shared = false) noexcept {
			const UINT bind = D3D11_BIND_SHADER_RESOURCE | (writable ? D3D11_BIND_UNORDERED_ACCESS : 0);
			texture = shared ? DirectXHelper::CreateSharedTexture2D(device, format, w, h, bind, "DLSSNR sparse snapshot")
				: DirectXHelper::CreateTexture2D(device, format, w, h, bind);
			return texture && SUCCEEDED(device->CreateShaderResourceView(texture.get(), nullptr, srv.put())) &&
				(!writable || SUCCEEDED(device->CreateUnorderedAccessView(texture.get(), nullptr, uav.put())));
		}
	};
	struct Frame {
		Texture input, motion, depth, confidence;
		FrameGuidanceView guidance{}, zero{};
		HdrFrameMetadata inputMetadata{}, outputMetadata{};
		uint64_t sequence = 0, epoch = 0, frame = 0, previousFrame = 0;
		uint64_t inputRevision = 0, historyRevision = 0, captureFrameIndex = 0, captureReflexFrameId = 0;
		int64_t admissionTime = 0;
		bool reset = false, pairValid = false;
		bool Create(ID3D11Device5* device, uint32_t w, uint32_t h, DXGI_FORMAT format) noexcept {
			return input.Create(device,w,h,format,false) && motion.Create(device,w,h,DXGI_FORMAT_R16G16_FLOAT,true,true) &&
				depth.Create(device,w,h,DXGI_FORMAT_R32_FLOAT,true,true) && confidence.Create(device,w,h,DXGI_FORMAT_R8_UNORM,true,true);
		}
	};
	struct Result {
		Texture residual, reference;
		winrt::com_ptr<ID3D11Buffer> contributionReadback;
		uint64_t source = 0, epoch = 0, frame = 0, ready = 0, version = 0;
		uint64_t compositionRevision = 0;
		DLSSNRSparseObservationClock::Result observation;
	};
	struct NativeResult {
		Texture input, output;
		uint64_t version = 0, ready = 0;
	};
	ID3D11Device5* device = nullptr;
	ID3D11DeviceContext4* dc = nullptr;
	uint32_t width = 0, height = 0, interval = 2, maxSourceAge = 3;
	DXGI_FORMAT inputFormat = DXGI_FORMAT_UNKNOWN, outputFormat = DXGI_FORMAT_UNKNOWN;
	std::array<Frame,CaptureCapacity> captures;
	Frame pendingFrame, outputFrame;
	std::array<Result,ResultCapacity> results;
	// Only the latest ready result and its publishing successor can be selected.
	// The next inference's input fence follows the preceding publication on the
	// same D3D11 queue, so that publication is ready before another completes.
	std::array<NativeResult,2> nativeResults;
	Texture fullColor, lastOutput, rawResidual, rawValidity, validity;
	Texture pastMotion, pastConfidence, zeroMotion, zeroConfidence;
	winrt::com_ptr<ID3D11Buffer> constants, counts;
	winrt::com_ptr<ID3D11UnorderedAccessView> countUav;
	winrt::com_ptr<ID3D11SamplerState> sampler;
	winrt::com_ptr<ID3D11ComputeShader> mapShader, composeShader, outputShader;
	winrt::com_ptr<ID3D11Fence> fence;
	wil::unique_event_nothrow event;
	HANDLE registeredWait = nullptr;
	uint64_t fenceValue = 0, sequence = 0, epoch = 1, lastFrame = 0, lastInputRevision = 0, lastHistoryRevision = 0;
	int64_t lastTimestamp = 0;
	uint64_t latestSource = 0, lastInferenceSource = 0, lastInferenceFrame = 0, lastInferenceEpoch = 0;
	uint64_t pendingSource = 0, pendingEpoch = 0, pendingFrameId = 0;
	uint64_t outputVersion = 0, resultVersion = 0, consumedResultVersion = 0;
	uint64_t outputResultVersion = 0;
	uint64_t compositionRevision = 1, outputCompositionRevision = 0;
	DLSSNRSparseObservationClock observationClock;
	uint64_t fullSubmissions = 0, heldOutputs = 0, burstSubmissions = 0;
	uint64_t completed = 0, published = 0, discarded = 0, replacedRequests = 0;
	bool initialized = false, pending = false, waiting = false, urgent = false, invalidated = false;
	bool outputAdvanced = false, held = false, afterOutput = false, outputReset = false;
	bool parameterRefresh = false;
	uint64_t selectedPastSource = 0, oldestSource = 0;
	double turnaroundMs = 0, pastContribution = 0;
	std::chrono::steady_clock::time_point submittedAt{};
	DLSSNRSparseBudget budget;
	struct Constants {
		uint32_t width, height, mapCount = 0, confidenceBits = 0;
		uint32_t inferenceMap = 0, hasPast = 0, finalize = 0, reserved = 0;
	};
	static_assert(sizeof(Constants)==32);
	~Sparse() {
		if (registeredWait) UnregisterWaitEx(registeredWait, INVALID_HANDLE_VALUE);
	}
	Frame* Find(uint64_t origin) noexcept {
		if (!origin) return nullptr;
		auto& frame=captures[origin%CaptureCapacity];
		return frame.sequence==origin ? &frame : nullptr;
	}
	void Dispatch(ID3D11ComputeShader* shader, const Constants& values,
		ID3D11ShaderResourceView* const* srvs, ID3D11UnorderedAccessView* const* uavs) noexcept {
		auto* cb=constants.get(); auto* sample=sampler.get();
		dc->UpdateSubresource(cb,0,nullptr,&values,0,0);
		dc->CSSetShader(shader,nullptr,0); dc->CSSetConstantBuffers(0,1,&cb); dc->CSSetSamplers(0,1,&sample);
		dc->CSSetShaderResources(0,ShaderResourceCount,srvs); dc->CSSetUnorderedAccessViews(0,5,uavs,nullptr);
		dc->Dispatch((width+7)/8,(height+7)/8,1);
		ID3D11ShaderResourceView* empty[ShaderResourceCount]{}; ID3D11UnorderedAccessView* out[5]{};
		dc->CSSetShaderResources(0,ShaderResourceCount,empty); dc->CSSetUnorderedAccessViews(0,5,out,nullptr); dc->CSSetShader(nullptr,nullptr,0);
	}
	void Rebind(Frame& f) noexcept {
		f.guidance.motion.texture=f.motion.texture.get(); f.guidance.depth.texture=f.depth.texture.get();
		f.guidance.confidence.texture=f.confidence.texture.get();
		f.guidance.reverseMotion={}; f.guidance.reverseConfidence={}; f.guidance.motionPair={};
		f.zero.depth.texture=f.depth.texture.get(); f.zero.motion.texture=zeroMotion.texture.get(); f.zero.confidence.texture=zeroConfidence.texture.get();
		f.zero.reverseMotion={}; f.zero.reverseConfidence={}; f.zero.motionPair={};
		for (auto* view:{&f.guidance,&f.zero}) for (auto* channel:{&view->motion,&view->depth,&view->confidence}) channel->metadata.sync={};
	}
	void CopyFrame(Frame& to, const Frame& from, bool copyMotion=true) noexcept {
		for (auto pair:{std::pair{&to.input,&from.input},std::pair{&to.depth,&from.depth}})
			dc->CopyResource(pair.first->texture.get(),pair.second->texture.get());
		// Inference immediately replaces motion/confidence with the composed
		// mapping (or zeros); display snapshots retain the original pair.
		if (copyMotion) for (auto pair:{std::pair{&to.motion,&from.motion},std::pair{&to.confidence,&from.confidence}})
			dc->CopyResource(pair.first->texture.get(),pair.second->texture.get());
		to.guidance=from.guidance; to.zero=from.zero; to.inputMetadata=from.inputMetadata; to.outputMetadata=from.outputMetadata;
		to.sequence=from.sequence; to.epoch=from.epoch; to.frame=from.frame; to.previousFrame=from.previousFrame;
		to.inputRevision=from.inputRevision; to.historyRevision=from.historyRevision;
		to.captureFrameIndex=from.captureFrameIndex; to.captureReflexFrameId=from.captureReflexFrameId;
		to.admissionTime=from.admissionTime; to.reset=from.reset; to.pairValid=from.pairValid;
		Rebind(to);
	}
	NativeEffectDrawContext Context(const Frame& f, ID3D11Texture2D* output, bool advanced) const noexcept {
		return {.input=f.input.texture.get(),.output=output,.inputMetadata=f.inputMetadata,.outputMetadata=f.outputMetadata,
			.frameId=f.frame,.previousCaptureFrameId=f.previousFrame,.inputRevision=f.inputRevision,
			.inputHistoryRevision=f.historyRevision,.inputHistoryReset=f.reset,.isNewCaptureFrame=advanced,
			.captureFrameIndex=f.captureFrameIndex,.captureAdmissionTime100ns=f.admissionTime,
			.captureReflexFrameId=f.captureReflexFrameId,.frameGuidance=f.guidance,.zeroFrameGuidance=f.zero};
	}
	bool Map(uint64_t from, uint64_t to, Texture& motion, Texture& confidence, bool inference=false) noexcept {
		Constants values{width,height}; values.inferenceMap=inference;
		ID3D11ShaderResourceView* srvs[ShaderResourceCount]{};
		const uint64_t distance=from>=to?from-to:UINT64_MAX;
		auto* first=Find(from); auto* last=Find(to);
		bool valid=distance<=8 && first && last && first->epoch==last->epoch;
		for (uint32_t i=0;valid && i<distance;++i) {
			auto* step=Find(from-i); auto* previous=Find(from-i-1);
			valid=step && previous && step->epoch==first->epoch && previous->epoch==first->epoch && step->pairValid && step->previousFrame==previous->frame;
			if (!valid) break;
			srvs[5+i]=step->motion.srv.get(); srvs[13+i]=step->confidence.srv.get();
			if (!step->guidance.confidence.metadata.isZero) values.confidenceBits|=1u<<i;
		}
		if (!valid) {
			const float zero[4]{}; dc->ClearUnorderedAccessViewFloat(motion.uav.get(),zero); dc->ClearUnorderedAccessViewFloat(confidence.uav.get(),zero); return false;
		}
		values.mapCount=uint32_t(distance);
		ID3D11UnorderedAccessView* uavs[5]{nullptr,nullptr,nullptr,motion.uav.get(),confidence.uav.get()};
		Dispatch(mapShader.get(),values,srvs,uavs); return true;
	}
};
