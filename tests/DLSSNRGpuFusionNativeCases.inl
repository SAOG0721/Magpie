// The public wrapper, real GPU constant buffer and final pixels are inspected.
// Run only in the separately authorized native-GPU validation session.
ResampleConstants ReadUploadedResidualConstants(DeviceResources& resources, DLSSNRFilter::Impl& state) {
	Require(state.residualConstantsValid && state.residualConstants11);
	D3D11_BUFFER_DESC desc{}; state.residualConstants11->GetDesc(&desc);
	desc.Usage=D3D11_USAGE_STAGING; desc.BindFlags=desc.MiscFlags=0; desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
	winrt::com_ptr<ID3D11Buffer> staging;
	Check(resources.GetD3DDevice()->CreateBuffer(&desc,nullptr,staging.put()));
	resources.GetD3DDC()->CopyResource(staging.get(),state.residualConstants11.get());
	D3D11_MAPPED_SUBRESOURCE mapped{};
	Check(resources.GetD3DDC()->Map(staging.get(),0,D3D11_MAP_READ,0,&mapped));
	ResampleConstants result{}; std::memcpy(&result,mapped.pData,sizeof(result));
	resources.GetD3DDC()->Unmap(staging.get(),0);
	return result;
}

void RequireUploadedResidualControls(DeviceResources& resources, DLSSNRFilter& filter) {
	auto& state=filter._performance ? filter._performance->composition : *filter._impl;
	const auto uploaded=ReadUploadedResidualConstants(resources,state);
	const auto& expected=filter._settings;
	const std::array<std::pair<float,float>,13> values{{
		{uploaded.residualMultiplier,expected.residualMultiplier},
		{uploaded.residualSaturation,expected.residualSaturation},
		{uploaded.residualLightness,expected.residualLightness},
		{uploaded.shadowStructureMultiplier,expected.shadowStructureMultiplier},
		{uploaded.midtoneStrength,expected.residualMidtoneStrength},
		{uploaded.reflectionGlowMultiplier,expected.reflectionGlowMultiplier},
		{uploaded.hueProtection,expected.residualHueProtection},
		{uploaded.darkProtection,expected.residualDarkProtection},
		{uploaded.highlightProtection,expected.residualHighlightProtection},
		{uploaded.chromaProtection,expected.residualChromaProtection},
		{uploaded.localCompression,expected.residualLocalCompression},
		{uploaded.lowFrequencyGain,expected.residualLowFrequencyGain},
		{uploaded.detailGain,expected.residualDetailGain}
	}};
	for (const auto& [actual,value]:values) Require(actual==value);
	Require(uploaded.debugView==uint32_t(expected.residualDebugView));
	Require(!state.residualParametersDirty);
}

void RunGpuFusionLiveNativeCases(DeviceResources& resources, NgxD3D12Core& core) {
	const std::array<float,4> blue{.2f,.3f,.6f,1},green{.3f,.6f,.2f,1},white{1,1,1,1};
	for (int mode=0;mode<4;++mode) {
		PerformanceFixture fixture(resources);
		EffectOption option;
		option.parameters={{"processingMode",float(mode)},{"modelPreset",0.f},{"resolutionControl",1.f},
			{"inputResolutionPercent",67.f},{"sparseUpdateInterval",2.f},{"residualTransfer",1.f},
			{"opticalFlowMethod",1.f},{"amdOpticalFlowMode",0.f},{"antiFlicker",4.f}};
		DLSSNRMultiPass backend;
		Require(backend.Initialize(resources,core,fixture.input.get(),fixture.output.get(),option,false));
		testedDevice=core.Device();
		Require(backend._temporal && backend._rawOutput);
		auto& filter=*backend._filter;
		auto settle=[&](NativeEffectDrawContext context,bool expectLiveEdit=false) {
			Require(backend.Draw(context));
			if (expectLiveEdit) RequireUploadedResidualControls(resources,filter);
			backend.ServiceAfterOutput();
			Require(backend.Drain());
			backend.ServiceAsync();
			WaitNativeOutput(resources);
			backend.ServiceAsync();
			context.isNewCaptureFrame=false;
			Require(backend.Draw(context));
			WaitNativeOutput(resources);
			backend.ServiceAsync();
			RequireUploadedResidualControls(resources,filter);
			Require(DebugMessages());
		};
		auto current=fixture.CaptureCoherent(blue);
		settle(current);
		current.isNewCaptureFrame=false;
		// All fourteen controls change in one live transaction. Inspect the
		// uploaded GPU buffer rather than accepting the parsed settings alone.
		option.parameters.insert_or_assign("residualMultiplier",1.6f);
		option.parameters.insert_or_assign("residualSaturation",.45f);
		option.parameters.insert_or_assign("residualLightness",1.35f);
		option.parameters.insert_or_assign("shadowStructureMultiplier",.65f);
		option.parameters.insert_or_assign("residualMidtoneStrength",1.2f);
		option.parameters.insert_or_assign("reflectionGlowMultiplier",1.7f);
		option.parameters.insert_or_assign("residualHueProtection",.4f);
		option.parameters.insert_or_assign("residualDarkProtection",.3f);
		option.parameters.insert_or_assign("residualHighlightProtection",.6f);
		option.parameters.insert_or_assign("residualChromaProtection",.25f);
		option.parameters.insert_or_assign("residualLocalCompression",.5f);
		option.parameters.insert_or_assign("residualLowFrequencyGain",.4f);
		option.parameters.insert_or_assign("residualDetailGain",1.8f);
		option.parameters.insert_or_assign("residualDebugView",2.f);
		std::vector<std::string> names;
		for (const auto name:DLSSNR_RESIDUAL_PARAMETERS) names.emplace_back(name);
		Require(backend.ApplyLiveParameters(option,names));
		settle(current,true);
		// Protection debug with no protection is exactly white. Anti-flicker
		// must bypass its old image while this diagnostic view is selected.
		option.parameters["residualDebugView"]=5.f;
		option.parameters["residualDarkProtection"]=0.f;
		option.parameters["residualHighlightProtection"]=0.f;
		names={"residualDebugView","residualDarkProtection","residualHighlightProtection"};
		Require(backend.ApplyLiveParameters(option,names));
		// The first visible draw is the live-edit contract. Do not submit or
		// drain a replacement inference before checking its actual pixels.
		Require(backend.Draw(current));
		WaitNativeOutput(resources);
		std::cout << "native first-draw protection debug: mode=" << mode << std::endl;
		CheckColor(resources,fixture.output.get(),white.data());
		settle(current,true);
		CheckColor(resources,fixture.output.get(),white.data());
		// No frequency band contributes after this edit. Returning to normal
		// temporal processing must not retain the earlier enhancement/debug view.
		option.parameters["residualDebugView"]=0.f;
		option.parameters["residualLowFrequencyGain"]=0.f;
		option.parameters["residualDetailGain"]=0.f;
		names={"residualDebugView","residualLowFrequencyGain","residualDetailGain"};
		Require(backend.ApplyLiveParameters(option,names));
		Require(backend.Draw(current));
		CheckColor(resources,fixture.output.get(),blue.data());
		settle(current,true);
		CheckColor(resources,fixture.output.get(),blue.data());
		settle(fixture.CaptureCoherent(green));
		CheckColor(resources,fixture.output.get(),green.data());
		Require(backend.Drain() && DebugMessages());
		std::cout << "native live detail passed: mode=" << mode
			<< " antiFlicker=4 uploadedControls=14 nonNeutral=true debugBypassWhite=true zeroBandsRaw=true nextCaptureNoResidual=true\n";
	}
}

void RunSparseLiveNativeCases(DeviceResources& resources, NgxD3D12Core& core) {
	const std::array<float,4> blue{.2f,.3f,.6f,1},white{1,1,1,1};
	for (uint32_t interval:{2u,3u,4u}) for (unsigned passCount:{1u,3u})
	for (uint32_t percent:{75u,100u}) for (int model:{0,1,2}) {
		PerformanceFixture fixture(resources,129,73,model==2);
		std::vector<uint32_t> texture(size_t(fixture.width)*fixture.height);
		for (uint32_t y=0;y<fixture.height;++y) for (uint32_t x=0;x<fixture.width;++x) {
			const uint32_t noise=(x*71+y*113+x*y*13)%53;
			const uint32_t r=65+(x/9%2)*60+noise,g=75+(y/7%2)*55+noise,b=85+noise;
			texture[size_t(y)*fixture.width+x]=fixture.bgraSource ? b|(g<<8)|(r<<16)|0xff000000u : r|(g<<8)|(b<<16)|0xff000000u;
		}
		auto capture=[&]() {
			auto context=fixture.CaptureCoherent(blue);
			resources.GetD3DDC()->UpdateSubresource(fixture.input.get(),0,nullptr,texture.data(),fixture.width*4,0);
			return context;
		};
		EffectOption option;
		option.parameters={{"processingMode",3.f},{"modelPreset",float(model)},{"resolutionControl",1.f},
			{"inputResolutionPercent",float(percent)},{"sparseUpdateInterval",float(interval)},{"residualTransfer",1.f},
			{"opticalFlowMethod",1.f},{"amdOpticalFlowMode",0.f},{"antiFlicker",4.f},{"multiPass",float(passCount)}};
		DLSSNRMultiPass backend;
		Require(backend.Initialize(resources,core,fixture.input.get(),fixture.output.get(),option,false));
		testedDevice=core.Device();
		auto& filter=*backend._filter;
		auto& p=*filter._sparse;
		Require(p.nativeResults.size()==2);
		std::optional<NativeEffectDrawContext> current(capture());
		Require(backend.Draw(*current));
		WaitNativeOutput(resources);
		const auto enhanced=ReadPerformanceTexture(resources,fixture.output.get());
		current->isNewCaptureFrame=false;
		const auto epoch=p.epoch,source=p.oldestSource;
		const auto submissions=p.fullSubmissions,budgetSource=p.budget.lastSource,budgetAdmission=p.budget.lastAdmission;
		const auto initialCounts=NativeEvaluationCounts(filter);
		const std::array<std::string,1> multiplier{"residualMultiplier"};
		auto edit=[&](float value) {
			option.parameters["residualMultiplier"]=value;
			Require(backend.ApplyLiveParameters(option,multiplier));
			Require(backend.Draw(*current)); // no ServiceAfterOutput/Drain before these checks.
			Require(p.epoch==epoch && p.fullSubmissions==submissions && p.oldestSource==source && !p.invalidated && !p.parameterRefresh);
			Require(p.budget.lastSource==budgetSource && p.budget.lastAdmission==budgetAdmission);
			RequireNativeEvaluationDelta(filter,initialCounts,0);
			RequireUploadedResidualControls(resources,filter);
			return ReadPerformanceTexture(resources,fixture.output.get());
		};
		const auto raw=edit(0);
		size_t changed=0;
		for (size_t i=0;i<raw.size();++i) for (size_t channel=0;channel<3;++channel)
			changed+=std::abs(raw[i][channel]-enhanced[i][channel])>1.f/255;
		Require(changed>0); // actual textured NR correction, not just parsed controls.
		for (unsigned drag=0;drag<4;++drag) {
			const auto restored=edit(1);
			Require(restored==enhanced); // exact original FP16/UNORM arithmetic survives rebuild.
			edit(0);
		}
		edit(1);
		const auto revision=p.compositionRevision;
		Require(backend.ApplyLiveParameters(option,multiplier));
		Require(p.compositionRevision==revision && !p.invalidated);
		Require(backend.Draw(*current));
		// Freeze the next real GPU job. An edit must read the paired snapshots,
		// while its shared native output is still owned by the in-flight feature.
		winrt::com_ptr<ID3D12Fence> gate;
		Check(core.Device()->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(gate.put())));
		Check(filter._impl->queue12->Wait(gate.get(),1));
		for (unsigned i=0;i<interval;++i) {
			current.emplace(capture());
			Require(backend.Draw(*current));
			backend.ServiceAfterOutput();
		}
		Require(p.pending && p.fullSubmissions==submissions+1);
		current->isNewCaptureFrame=false;
		const auto pendingSubmissions=p.fullSubmissions,pendingSource=p.pendingSource,pendingEpoch=p.pendingEpoch;
		const std::array<std::string,3> debug{"residualDebugView","residualDarkProtection","residualHighlightProtection"};
		option.parameters["residualDebugView"]=5;
		option.parameters["residualDarkProtection"]=0;
		option.parameters["residualHighlightProtection"]=0;
		for (unsigned drag=0;drag<4;++drag) {
			option.parameters["residualLightness"] = drag%2 ? .5f : 1.5f;
			const std::array<std::string,4> names{"residualDebugView","residualDarkProtection","residualHighlightProtection","residualLightness"};
			const auto& selected=p.results[p.outputResultVersion%p.results.size()];
			const auto ready=selected.ready;
			Require(backend.ApplyLiveParameters(option,names));
			Require(backend.Draw(*current));
			CheckColor(resources,fixture.output.get(),white.data());
			Require(p.epoch==epoch && p.pending && p.fullSubmissions==pendingSubmissions && p.pendingSource==pendingSource && p.pendingEpoch==pendingEpoch);
			Require(selected.ready==ready && selected.compositionRevision==p.compositionRevision);
			Require(p.oldestSource==source && p.outputCompositionRevision==p.compositionRevision);
		}
		Check(gate->Signal(1));
		Require(backend.Drain()); backend.ServiceAsync(); WaitNativeOutput(resources);
		Require(backend.Draw(*current));
		CheckColor(resources,fixture.output.get(),white.data());
		// Repeated publication crosses both the four-result ring and the two-pair
		// ring. No third historical raw result may become selected or reappear.
		for (unsigned publication=0;publication<5;++publication) {
			for (unsigned i=0;i<interval;++i) {
				current.emplace(capture()); Require(backend.Draw(*current)); backend.ServiceAfterOutput();
			}
			Require(backend.Drain()); backend.ServiceAsync(); WaitNativeOutput(resources);
			current->isNewCaptureFrame=false;
			Require(backend.Draw(*current));
			const auto full=p.fullSubmissions;
			option.parameters["residualDebugView"]=publication%2 ? 5.f : 2.f;
			Require(backend.ApplyLiveParameters(option,debug));
			Require(backend.Draw(*current));
			Require(p.epoch==epoch && p.fullSubmissions==full);
			const auto& result=p.results[p.outputResultVersion%p.results.size()];
			const auto& native=p.nativeResults[result.version%p.nativeResults.size()];
			Require(native.version==result.version && result.compositionRevision==p.compositionRevision);
			Require(p.resultVersion-result.version<2);
			if (publication%2) CheckColor(resources,fixture.output.get(),white.data());
		}
		Require(backend.Drain() && DebugMessages());
		std::cout << "native sparse first-draw live passed: interval=" << interval << " passes=" << passCount
			<< " percent=" << percent << " model=" << model << " bgra=" << fixture.bgraSource
			<< " changedChannels=" << changed << " pausedDragExact=true pendingDragImmediate=true"
			<< " extraNr=0 readyIdentityStable=true nativePairs=2 ringReuse=true\n";
	}
}

void RunSparsePairedInputNativeCases(DeviceResources& resources, NgxD3D12Core& core) {
	// Power-of-two full extents make zero-motion sampling land exactly on texel
	// centers. With confidence one and <.035 colour changes, the reuse contract
	// is simply FP16(current target + previously measured controlled residual).
	// This reference does not implement the NR or nonlinear residual controls.
	constexpr uint32_t width=128,height=64,interval=3;
	const std::array<float,4> clear{.2f,.3f,.6f,1};
	for (uint32_t percent:{75u,100u}) for (unsigned passCount:{1u,3u}) {
		PerformanceFixture fixture(resources,width,height);
		std::array<std::vector<uint32_t>,3> packed;
		std::array<std::vector<std::array<float,4>>,3> pixels;
		for (unsigned variant=0;variant<packed.size();++variant) {
			packed[variant].resize(size_t(width)*height);
			pixels[variant].resize(packed[variant].size());
			for (uint32_t y=0;y<height;++y) for (uint32_t x=0;x<width;++x) {
				const uint32_t noise=(x*71+y*113+x*y*13)%53;
				uint32_t r=65+(x/9%2)*60+noise,g=75+(y/7%2)*55+noise,b=85+noise;
				if (variant==1) { r+=3+(x+y)%3; g+=2+(x*3+y)%4; b+=4+(x+y*2)%3; }
				if (variant==2) { r+=6+(x+y)%2; g+=5+(x*3+y)%3; b+=6+(x+y*2)%3; }
				const size_t i=size_t(y)*width+x;
				packed[variant][i]=r|(g<<8)|(b<<16)|0xff000000u;
				pixels[variant][i]={r/255.f,g/255.f,b/255.f,1};
			}
		}
		auto capture=[&](unsigned variant) {
			auto context=fixture.CaptureCoherent(clear);
			resources.GetD3DDC()->UpdateSubresource(fixture.input.get(),0,nullptr,packed[variant].data(),width*4,0);
			return context;
		};
		EffectOption option;
		option.parameters={{"processingMode",3.f},{"modelPreset",0.f},{"resolutionControl",1.f},
			{"inputResolutionPercent",float(percent)},{"sparseUpdateInterval",float(interval)},{"residualTransfer",1.f},
			{"opticalFlowMethod",1.f},{"amdOpticalFlowMode",0.f},{"antiFlicker",4.f},{"multiPass",float(passCount)},
			{"residualMultiplier",.7f}};
		DLSSNRMultiPass backend;
		Require(backend.Initialize(resources,core,fixture.input.get(),fixture.output.get(),option,false));
		testedDevice=core.Device();
		auto& filter=*backend._filter;
		auto& p=*filter._sparse;
		Require(p.outputFormat==DXGI_FORMAT_R16G16B16A16_FLOAT);
		std::optional<NativeEffectDrawContext> current(capture(0));
		Require(backend.Draw(*current));
		current->isNewCaptureFrame=false;
		auto& resultA=p.results[p.outputResultVersion%p.results.size()];
		auto& nativeA=p.nativeResults[resultA.version%p.nativeResults.size()];
		const auto sourceA=resultA.source,versionA=resultA.version,epoch=p.epoch,readyA=resultA.ready;
		const auto nativeInputA=ReadPerformanceTexture(resources,nativeA.input.texture.get());
		const auto nativeOutputA=ReadPerformanceTexture(resources,nativeA.output.texture.get());
		Require(nativeA.input.texture.get()!=filter._impl->sharedInput11.get());
		auto& last=filter._impl->laterPasses.empty()?*filter._impl:*filter._impl->laterPasses.back();
		Require(nativeA.output.texture.get()!=last.sharedOutput11.get());
		const auto residualA07=ReadPerformanceTexture(resources,resultA.residual.texture.get());
		DLSSNRFilter::Sparse::Texture referenceA07,referenceA135,referenceOutput,decodedTarget;
		for (auto* texture:{&referenceA07,&referenceA135})
			Require(texture->Create(resources.GetD3DDevice(),width,height,DXGI_FORMAT_R16G16B16A16_FLOAT,false));
		Require(referenceOutput.Create(resources.GetD3DDevice(),width,height,DXGI_FORMAT_R16G16B16A16_FLOAT));
		Require(decodedTarget.Create(resources.GetD3DDevice(),width,height,DXGI_FORMAT_R32G32B32A32_FLOAT));
		resources.GetD3DDC()->CopyResource(referenceA07.texture.get(),resultA.residual.texture.get());
		// Decode the real R8 target on the GPU, then add the already measured A
		// correction. The independent reference has no reprojection, controls,
		// filtering or reuse logic. Its second output exposes exact operands and
		// raw validity to diagnose CPU /255 versus hardware decode differences.
		constexpr std::string_view referenceSource=R"hlsl(
Texture2D<float4> CurrentTarget : register(t0);
Texture2D<float4> MeasuredCorrection : register(t1);
Texture2D<float> RawValidity : register(t2);
RWTexture2D<float4> Expected : register(u0);
RWTexture2D<float4> Decoded : register(u1);
[numthreads(8,8,1)] void AddMeasured(uint3 id : SV_DispatchThreadID) {
    float3 target=CurrentTarget.Load(int3(id.xy,0)).rgb;
    Expected[id.xy]=float4(target+MeasuredCorrection.Load(int3(id.xy,0)).rgb,1);
    Decoded[id.xy]=float4(target,RawValidity.Load(int3(id.xy,0)));
}
)hlsl";
		winrt::com_ptr<ID3D11ComputeShader> referenceShader;
		Require(CreateComputeShader(*filter._impl,referenceSource,"AddMeasured","SparsePairIndependentAdd",referenceShader));
		const std::array<std::string,1> names{"residualMultiplier"};
		auto edit=[&](float multiplier) {
			option.parameters["residualMultiplier"]=multiplier;
			Require(backend.ApplyLiveParameters(option,names));
			Require(backend.Draw(*current));
			Require(p.epoch==epoch && p.outputResultVersion==versionA && p.oldestSource==sourceA);
			Require(resultA.ready==readyA && nativeA.version==versionA && nativeA.ready==readyA);
			return ReadPerformanceTexture(resources,resultA.residual.texture.get());
		};
		const auto residualA135=edit(1.35f);
		Require(residualA07!=residualA135);
		resources.GetD3DDC()->CopyResource(referenceA135.texture.get(),resultA.residual.texture.get());
		Require(edit(.7f)==residualA07); // Same-source controls restore the stored FP16 residual exactly.
		winrt::com_ptr<ID3D12Fence> gate;
		Check(core.Device()->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(gate.put())));
		Check(filter._impl->queue12->Wait(gate.get(),1));
		for (unsigned i=0;i<interval;++i) {
			current.emplace(capture(1)); // B is distinct from A at every pixel.
			Require(backend.Draw(*current)); backend.ServiceAfterOutput();
		}
		Require(p.pending && p.fullSubmissions==2 && p.pendingSource!=sourceA);
		const auto pendingSource=p.pendingSource,pendingEpoch=p.pendingEpoch;
		Require(ReadPerformanceTexture(resources,p.pendingFrame.input.texture.get())==pixels[1]);
		current->isNewCaptureFrame=false;
		auto requirePairA=[&]() {
			Require(ReadPerformanceTexture(resources,nativeA.input.texture.get())==nativeInputA);
			Require(ReadPerformanceTexture(resources,nativeA.output.texture.get())==nativeOutputA);
			Require(nativeA.version==versionA && nativeA.ready==readyA);
		};
		auto requireTargetPlusA=[&](unsigned variant,const auto& residual,ID3D11ShaderResourceView* measuredCorrection) {
			Require(ReadPerformanceTexture(resources,p.outputFrame.input.texture.get())==pixels[variant]);
			auto* dc=resources.GetD3DDC();
			ID3D11ShaderResourceView* inputs[]{p.outputFrame.input.srv.get(),measuredCorrection,p.rawValidity.srv.get()};
			ID3D11UnorderedAccessView* outputs[]{referenceOutput.uav.get(),decodedTarget.uav.get()};
			dc->CSSetShader(referenceShader.get(),nullptr,0);
			dc->CSSetShaderResources(0,3,inputs); dc->CSSetUnorderedAccessViews(0,2,outputs,nullptr);
			dc->Dispatch(width/8,height/8,1);
			ID3D11ShaderResourceView* noInputs[3]{}; ID3D11UnorderedAccessView* noOutputs[2]{};
			dc->CSSetShaderResources(0,3,noInputs); dc->CSSetUnorderedAccessViews(0,2,noOutputs,nullptr); dc->CSSetShader(nullptr,nullptr,0);
			const auto actual=ReadPerformanceTexture(resources,backend._rawOutput.get());
			const auto expectedGpu=ReadPerformanceTexture(resources,referenceOutput.texture.get());
			const auto decoded=ReadPerformanceTexture(resources,decodedTarget.texture.get());
			size_t cpuHalfDifferences=0,cpuDecodeDifferences=0;
			for (size_t i=0;i<actual.size();++i) {
				for (size_t channel=0;channel<3;++channel) {
					const float sum=pixels[variant][i][channel]+residual[i][channel];
					const float expectedCpu=DirectX::PackedVector::XMConvertHalfToFloat(
						DirectX::PackedVector::XMConvertFloatToHalf(sum));
					cpuDecodeDifferences+=decoded[i][channel]!=pixels[variant][i][channel];
					if (actual[i][channel]!=expectedCpu && cpuHalfDifferences++==0)
						std::cerr << "Sparse CPU-reference diagnostic: xy=" << i%width << ',' << i/width << " channel=" << channel
							<< std::hexfloat << " targetCpu=" << pixels[variant][i][channel] << " targetGpu=" << decoded[i][channel]
							<< " residualA=" << residual[i][channel] << " actual=" << actual[i][channel] << " cpuHalf=" << expectedCpu
							<< " gpuReference=" << expectedGpu[i][channel] << " rawValidity=" << decoded[i][3] << std::defaultfloat << '\n';
					Require(actual[i][channel]==expectedGpu[i][channel]);
				}
				Require(actual[i][3]==1 && decoded[i][3]==1);
			}
			if (cpuHalfDifferences) std::cout << "Sparse CPU-reference difference: cpuHalfChannels=" << cpuHalfDifferences
				<< " cpuDecodeChannels=" << cpuDecodeDifferences << " gpuReferenceExact=true\n";
			Require(p.pending && p.fullSubmissions==2 && p.pendingSource==pendingSource && p.pendingEpoch==pendingEpoch);
			requirePairA();
			return actual;
		};
		Require(edit(.7f)==residualA07);
		const auto outputB07=requireTargetPlusA(1,residualA07,referenceA07.srv.get());
		const auto displayedB07=ReadPerformanceTexture(resources,fixture.output.get());
		Require(edit(1.35f)==residualA135);
		const auto outputB135=requireTargetPlusA(1,residualA135,referenceA135.srv.get());
		Require(outputB07!=outputB135);
		Require(edit(.7f)==residualA07);
		Require(requireTargetPlusA(1,residualA07,referenceA07.srv.get())==outputB07);
		Require(ReadPerformanceTexture(resources,fixture.output.get())==displayedB07);
		// Move the current target again while B remains in flight. C still has
		// unit trust against A, so its own RGB, rather than A or B's, is the base.
		current.emplace(capture(2));
		Require(backend.Draw(*current));
		current->isNewCaptureFrame=false;
		Require(edit(1.35f)==residualA135);
		Require(requireTargetPlusA(2,residualA135,referenceA135.srv.get())!=outputB135);
		Require(edit(.7f)==residualA07);
		requireTargetPlusA(2,residualA07,referenceA07.srv.get());
		Check(gate->Signal(1));
		Require(backend.Drain()); backend.ServiceAsync(); WaitNativeOutput(resources);
		// B has now actually overwritten the worker's shared resources. The
		// protected old pair must still contain A, while the new pair contains B.
		requirePairA();
		const auto& resultB=p.results[p.resultVersion%p.results.size()];
		const auto& nativeB=p.nativeResults[resultB.version%p.nativeResults.size()];
		Require(resultB.source==pendingSource && resultB.epoch==epoch && resultB.version!=versionA);
		Require(ReadPerformanceTexture(resources,resultB.reference.texture.get())==pixels[1]);
		Require(ReadPerformanceTexture(resources,nativeB.input.texture.get())!=nativeInputA);
		Require(ReadPerformanceTexture(resources,nativeB.output.texture.get())!=nativeOutputA);
		Require(backend.Drain() && DebugMessages());
		std::cout << "native sparse distinct-input pair passed: percent=" << percent << " passes=" << passCount
			<< " sourceA=" << sourceA << " pendingB=" << pendingSource << " targetC=" << p.sequence
			<< " normalControls=true pairAUnchanged=true controlRoundTripExact=true"
			<< " targetBPlusAExact=true targetCPlusAExact=true postProcessRoundTripExact=true extraNr=0\n";
	}
}
