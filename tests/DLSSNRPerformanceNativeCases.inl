#include <fstream>

// Included by the actual-production native probe after its shared helpers.
// GPU gates make source replacement/expiry deterministic, not timing guesses.
struct PerformanceFixture {
	static constexpr uint32_t Width = 513, Height = 289;
	uint32_t width = Width, height = Height;
	bool bgraSource = false;
	DeviceResources& resources;
	winrt::com_ptr<ID3D11Texture2D> input, output;
	winrt::com_ptr<ID3D11UnorderedAccessView> inputUav;
	std::array<winrt::com_ptr<ID3D11Texture2D>, 3> guides;
	winrt::com_ptr<ID3D11Texture2D> coherentMotion, coherentConfidence;
	winrt::com_ptr<ID3D11UnorderedAccessView> coherentMotionUav, coherentConfidenceUav;
	FrameGuidanceView zero, coherent;
	uint64_t frame = 0;
	PerformanceFixture(DeviceResources& owner, uint32_t sourceWidth = Width, uint32_t sourceHeight = Height, bool bgra = false) :
		width(sourceWidth), height(sourceHeight), bgraSource(bgra), resources(owner) {
		input = DirectXHelper::CreateTexture2D(resources.GetD3DDevice(), bgra ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM,
			width, height, D3D11_BIND_SHADER_RESOURCE | (bgra ? 0 : D3D11_BIND_UNORDERED_ACCESS));
		output = DirectXHelper::CreateTexture2D(resources.GetD3DDevice(), DXGI_FORMAT_R8G8B8A8_UNORM,
			width, height, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
		if (!bgra) Check(resources.GetD3DDevice()->CreateUnorderedAccessView(input.get(), nullptr, inputUav.put()));
		const DXGI_FORMAT formats[]{DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R8_UNORM};
		FrameGuidanceResource* channels[]{&zero.motion, &zero.depth, &zero.confidence};
		for (size_t i=0; i<guides.size(); ++i) {
			guides[i] = DirectXHelper::CreateSharedTexture2D(resources.GetD3DDevice(), formats[i], width, height,
				D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, "NR performance probe");
			Require(bool(guides[i]));
			winrt::com_ptr<ID3D11UnorderedAccessView> uav;
			Check(resources.GetD3DDevice()->CreateUnorderedAccessView(guides[i].get(), nullptr, uav.put()));
			const float empty[4]{};
			resources.GetD3DDC()->ClearUnorderedAccessViewFloat(uav.get(), empty);
			*channels[i] = {guides[i].get(), formats[i], {.resourceGeneration=1,
				.sourceExtent={width,height}, .validRegion={0,0,width,height}, .valid=true, .isZero=true}};
		}
	}
	void Color(const std::array<float,4>& value) {
		if (bgraSource) {
			const auto byte = [&](size_t channel) { return uint32_t(std::lround(value[channel]*255)); };
			const uint32_t pixel = byte(2) | (byte(1)<<8) | (byte(0)<<16) | (byte(3)<<24);
			const std::vector<uint32_t> pixels(size_t(width)*height,pixel);
			resources.GetD3DDC()->UpdateSubresource(input.get(),0,nullptr,pixels.data(),width*4,0);
		} else resources.GetD3DDC()->ClearUnorderedAccessViewFloat(inputUav.get(), value.data());
	}
	NativeEffectDrawContext Capture(const std::array<float,4>& value) {
		Color(value); ++frame;
		for (auto* channel : {&zero.motion, &zero.depth, &zero.confidence}) {
			channel->metadata.frameId = frame;
			channel->metadata.timestamp100ns = int64_t(frame)*166667;
		}
		return {.input=input.get(), .output=output.get(), .frameId=frame,
			.previousCaptureFrameId=frame-1, .inputRevision=frame,
			.frameGuidance=zero, .zeroFrameGuidance=zero};
	}
	NativeEffectDrawContext CaptureCoherent(const std::array<float,4>& value) {
		auto context = Capture(value);
		if (!coherentMotion) {
			coherentMotion = DirectXHelper::CreateSharedTexture2D(resources.GetD3DDevice(),
				DXGI_FORMAT_R16G16_FLOAT,width,height,D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
				"NR native coherent motion");
			coherentConfidence = DirectXHelper::CreateSharedTexture2D(resources.GetD3DDevice(),
				DXGI_FORMAT_R8_UNORM,width,height,D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
				"NR native coherent confidence");
			Require(coherentMotion && coherentConfidence);
			Check(resources.GetD3DDevice()->CreateUnorderedAccessView(coherentMotion.get(),nullptr,coherentMotionUav.put()));
			Check(resources.GetD3DDevice()->CreateUnorderedAccessView(coherentConfidence.get(),nullptr,coherentConfidenceUav.put()));
		}
		const float still[4]{}, confident[4]{1,1,1,1};
		resources.GetD3DDC()->ClearUnorderedAccessViewFloat(coherentMotionUav.get(),still);
		resources.GetD3DDC()->ClearUnorderedAccessViewFloat(coherentConfidenceUav.get(),confident);
		coherent = zero;
		coherent.motion.texture = coherentMotion.get();
		coherent.confidence.texture = coherentConfidence.get();
		coherent.motion.metadata.isZero = false;
		coherent.confidence.metadata.isZero = false;
		return {.input=context.input, .output=context.output, .frameId=context.frameId,
			.previousCaptureFrameId=context.previousCaptureFrameId, .inputRevision=context.inputRevision,
			.frameGuidance=coherent, .zeroFrameGuidance=zero};
	}
	void RejectConfidenceBlock() {
		Require(bool(coherentConfidence));
		constexpr uint32_t side = 16;
		const std::array<uint8_t,side*side> rejected{};
		D3D11_BOX block{width/2,height/2,0,width/2+side,height/2+side,1};
		resources.GetD3DDC()->UpdateSubresource(coherentConfidence.get(),0,&block,rejected.data(),side,0);
	}
};

template<class Predicate>
void PollPerformance(DeviceResources& resources, DLSSNRFilter& chain, Predicate&& finished) {
	resources.GetD3DDC()->Flush();
	const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(15);
	do {
		chain.ServiceAsync();
		Require(chain.IsHealthy());
		if (finished()) return;
		Sleep(1);
	} while (std::chrono::steady_clock::now() < deadline);
	Require(false);
}

std::vector<std::array<float,4>> ReadPerformanceTexture(DeviceResources& resources, ID3D11Texture2D* texture) {
	D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
	Require(desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM || desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ||
		desc.Format == DXGI_FORMAT_R32G32B32A32_FLOAT);
	desc.BindFlags = desc.MiscFlags = 0; desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	winrt::com_ptr<ID3D11Texture2D> staging;
	Check(resources.GetD3DDevice()->CreateTexture2D(&desc, nullptr, staging.put()));
	resources.GetD3DDC()->CopyResource(staging.get(), texture);
	D3D11_MAPPED_SUBRESOURCE data{};
	Check(resources.GetD3DDC()->Map(staging.get(),0,D3D11_MAP_READ,0,&data));
	std::vector<std::array<float,4>> result(size_t(desc.Width)*desc.Height);
	for (uint32_t y=0; y<desc.Height; ++y) for (uint32_t x=0; x<desc.Width; ++x) {
		const auto* row = static_cast<const uint8_t*>(data.pData)+size_t(y)*data.RowPitch;
		for (size_t c=0; c<4; ++c) result[size_t(y)*desc.Width+x][c] = desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM ?
			row[size_t(x)*4+c]/255.f : desc.Format == DXGI_FORMAT_R32G32B32A32_FLOAT ?
			reinterpret_cast<const float*>(row)[size_t(x)*4+c] : DirectX::PackedVector::XMConvertHalfToFloat(
				reinterpret_cast<const DirectX::PackedVector::HALF*>(row)[size_t(x)*4+c]);
	}
	resources.GetD3DDC()->Unmap(staging.get(),0);
	return result;
}

void CheckPublishedDifference(DeviceResources& resources, DLSSNRFilter& chain, size_t slot) {
	auto& p = *chain._performance;
	auto& impl = *chain._impl;
	auto& last = impl.laterPasses.empty() ? impl : *impl.laterPasses.back();
	const auto base = ReadPerformanceTexture(resources, impl.sharedInput11.get());
	const auto full = ReadPerformanceTexture(resources, last.sharedOutput11.get());
	const auto enhancement = ReadPerformanceTexture(resources, p.results[slot].enhancement.texture.get());
	Require(base.size() == full.size() && full.size() == enhancement.size());
	double maximumError = 0;
	for (size_t i=0; i<base.size(); ++i) for (size_t c=0; c<3; ++c) {
		const float expected = full[i][c]-base[i][c];
		Require(std::isfinite(enhancement[i][c]));
		maximumError = std::max(maximumError, double(std::abs(enhancement[i][c]-expected)));
	}
	Require(maximumError < .002);
	std::cout << "same-source E=Rfull maximum error=" << maximumError << '\n';
}

void RunPerformanceNativeCases(DeviceResources& resources, NgxD3D12Core& core) {
	const std::array<float,4> blue{.2f,.2f,.6f,1};
	for (uint32_t interval : {2u,3u,4u}) for (int model : {0,1,2}) for (unsigned passCount : {1u,3u}) {
		PerformanceFixture fixture(resources);
		DLSSNRSettings settings;
		settings.processingMode=DLSSNRProcessingMode::Alternating;settings.modelPreset=model;
		settings.sparseUpdateInterval=interval;settings.inputResolutionPercent=67;
		settings.residualTransfer=DLSSNRResidualTransfer::PerPixel;
		settings.motionRequest=MotionVectorRequest::Amd(AmdOpticalFlowMode::Performance);
		const std::vector<DLSSNRSettings> passes(passCount,settings);
		DLSSNRFilter chain;
		Require(chain.InitializeChain(resources,core,fixture.input.get(),fixture.output.get(),passes));
		testedDevice=core.Device();
		auto& p=*chain._performance;
		Require(p.state.maxAgeFrames == interval-1 && p.motion.size() == 4);
		const auto counts=[&] {
			std::vector<uint64_t> values{chain._impl->evaluateCount};
			for (const auto& pass:chain._impl->laterPasses) values.push_back(pass->evaluateCount);
			return values;
		};
		const auto delta=[&](const std::vector<uint64_t>& before,uint64_t expected) {
			const auto after=counts();Require(after.size() == before.size());
			for (size_t i=0;i<after.size();++i) Require(after[i] == before[i]+expected);
		};
		uint64_t accepted=0;
		const auto capture=[&](bool coherent=true) {
			const auto previous=fixture.frame;
			fixture.frame+=6; // renderer IDs have gaps; accepted captures do not.
			auto context=coherent?fixture.CaptureCoherent(blue):fixture.Capture(blue);
			context.previousCaptureFrameId=previous;context.inputHistoryRevision=1;
			++accepted;
			for (auto* view:{&fixture.coherent,&fixture.zero})
				for (auto* channel:{&view->motion,&view->depth,&view->confidence})
					channel->metadata.timestamp100ns=int64_t(accepted)*166667;
			return context;
		};
		for (uint64_t ordinal=1;ordinal<=interval*3;++ordinal) {
			auto context=capture();
			const bool full=(ordinal-1)%interval == 0;
			if (!full) fixture.RejectConfidenceBlock(); // Soft rejection must not create another NR observation.
			const auto before=counts();const auto previousFull=p.lastInferenceSequence;
			Require(chain.Draw(context));delta(before,unsigned(full));
			Require(p.state.sequence == ordinal && !p.pending && !p.latestWaiting);
			const auto selected=p.pool.Latest(p.state,UINT64_MAX);
			Require(selected != DLSSNREnhancementPool::None);
			Require(p.state.sequence-p.pool[selected].sequence == (ordinal-1)%interval);
			if (full) {
				CheckPublishedDifference(resources,chain,selected);
				if (ordinal>1) {
					ID3D11ShaderResourceView* srvs[12]{};
					DLSSNRFilter::Performance::Constants values{fixture.width,fixture.height,fixture.width,fixture.height};
					Require(p.MotionInputs(previousFull,srvs,values) && values.age == interval);
					for (uint32_t step=0;step<interval;++step) Require(srvs[3+step] != nullptr);
				}
				if (ordinal>interval+1) {
					// Once the initial zero-guidance transition has completed, the
					// observation gap alone must not reset any pass's NGX history.
					int reset=1;
					Require(NVSDK_NGX_SUCCEED(chain._impl->parameters->Get(PARAM_RESET,&reset)) && reset == 0);
					for (const auto& pass:chain._impl->laterPasses) {
						reset=1;Require(NVSDK_NGX_SUCCEED(pass->parameters->Get(PARAM_RESET,&reset)) && reset == 0);
					}
				}
			}
			const auto revision=p.outputRevision,source=p.pool[selected].sequence;
			context.isNewCaptureFrame=false;
			for (unsigned redraw=0;redraw<3;++redraw) {
				chain.ServiceAsync();chain.ServiceAfterOutput();Require(chain.Draw(context));
				delta(before,unsigned(full));Require(p.state.sequence == ordinal && p.outputRevision == revision);
				Require(p.pool[selected].sequence == source);
			}
		}
		Require(p.fullSubmissions == 3 && p.sameSpecRecoveries == 0 && p.continuousReuseOutputs > 0);
		const auto beforeReset=counts();const auto oldEpoch=p.state.epoch;
		auto reset=capture();reset.inputHistoryReset=true;
		Require(chain.Draw(reset));delta(beforeReset,1);Require(p.state.epoch>oldEpoch);
		const auto beforeMissing=counts();const auto recoveries=p.sameSpecRecoveries;
		auto missing=capture(false);
		Require(chain.Draw(missing));delta(beforeMissing,1);Require(p.sameSpecRecoveries == recoveries+1);
		Require(p.readySequence == p.state.sequence && p.state.maxAgeFrames == interval-1);
		Require(chain._impl->width == uint32_t(std::lround(fixture.width*.67)));
		Require(chain._impl->laterPasses.size()+1 == passCount);
		if (interval == 4 && model == 0 && passCount == 1) {
			EffectOption option;
			option.parameters={{"processingMode",1.f},{"inputResolutionPercent",67.f},{"sparseUpdateInterval",4.f},
				{"residualTransfer",1.f},{"opticalFlowMethod",1.f},{"amdOpticalFlowMode",0.f},{"intensity",.5f}};
			const std::array<std::string,1> names{"intensity"};
			const auto beforeEdit=counts();const auto editEpoch=p.state.epoch;
			Require(chain.ApplyLiveParameters(option,names) && p.state.epoch>editEpoch);
			missing.isNewCaptureFrame=false;
			Require(chain.Draw(missing));delta(beforeEdit,1);
			Require(p.state.sequence == accepted && chain._settings.inputResolutionPercent == 67);
			PerformanceFixture resized(resources,517,291);
			Require(chain.Resize(resources,resized.input.get(),resized.output.get()));
			auto resizedContext=resized.CaptureCoherent(blue);
			Require(chain.Draw(resizedContext) && chain._performance->fullSubmissions == 1);
			Require(chain._impl->width == uint32_t(std::lround(517*.67)));
		}
		Require(chain.Drain() && DebugMessages());
		std::cout << "native Alternating passed: interval=" << interval << " model=" << model << " passes=" << passCount
			<< " captureIdGaps=true redrawCadenceStable=true softRejectNoExtraNR=true trueAgeBudget=" << interval-1
			<< " hardEpochAndMissingMap=sameSpecificationFull inferenceGuidanceSteps=" << interval << '\n';
	}
}
