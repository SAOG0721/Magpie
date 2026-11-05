// Native test only: expose ownership/counters without changing the product API.
#include "pch.h"
#include "ScalingOptions.h"
#include <parallel_hashmap/phmap.h>
#define private public
#include "DeviceResources.h"
#include "DLSSNRFilter.h"
#undef private
#include "DLSSNRFilter.cpp"
#include "FrameGuidanceD3D12Interop.cpp"
#include <d3d11sdklayers.h>
#include <d3d12sdklayers.h>
#include <iostream>
#include <cassert>
#include <source_location>
#include <DirectXPackedVector.h>
#include "DLSSNRColorReference.h"
#include "DLSSNRAdaptiveResolution.h"
#include "DLSSNRTemporal.h"
#define private public
#include "DLSSNRMultiPass.h"
#undef private

using namespace Magpie;
ID3D12Device* testedDevice = nullptr;
bool DebugMessages() {
	if (!testedDevice) return true;
	bool clean = true;
	winrt::com_ptr<ID3D12InfoQueue> info;
	if (SUCCEEDED(testedDevice->QueryInterface(IID_PPV_ARGS(info.put())))) {
		for (UINT64 i=0; i<info->GetNumStoredMessages(); ++i) {
			SIZE_T bytes=0; info->GetMessage(i,nullptr,&bytes);
			std::vector<uint8_t> data(bytes);
			auto* message=reinterpret_cast<D3D12_MESSAGE*>(data.data());
			if (SUCCEEDED(info->GetMessage(i,message,&bytes)) && message->Severity <= D3D12_MESSAGE_SEVERITY_WARNING) {
				std::cerr << message->pDescription << '\n';
				clean = false;
			}
		}
		info->ClearStoredMessages();
	}
	const auto removed = testedDevice->GetDeviceRemovedReason();
	if (FAILED(removed)) std::cerr << "Device removed reason: " << std::hex << removed << '\n';
	return clean && SUCCEEDED(removed);
}
void Check(HRESULT hr) { if (FAILED(hr)) { std::cerr << std::hex << hr << '\n'; std::abort(); } }
void Require(bool value, const std::source_location where = std::source_location::current()) {
	if (!value) {
		std::cerr << "Requirement failed at " << where.file_name() << ':' << where.line() << '\n';
		DebugMessages(); Logger::Get().Flush(); std::abort();
	}
}
NVSDK_NGX_Result NVSDK_CONV FailEvaluation(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*,
	const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback) {
	return NVSDK_NGX_Result_FAIL_InvalidParameter;
}

void CheckColor(DeviceResources& resources, ID3D11Texture2D* texture, const float* expected,
	const std::source_location where = std::source_location::current()) {
	D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
	const uint32_t width=desc.Width, height=desc.Height;
	desc.Usage=D3D11_USAGE_STAGING; desc.BindFlags=0; desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ; desc.MiscFlags=0;
	winrt::com_ptr<ID3D11Texture2D> staging;
	Check(resources.GetD3DDevice()->CreateTexture2D(&desc,nullptr,staging.put()));
	resources.GetD3DDC()->CopyResource(staging.get(),texture);
	D3D11_MAPPED_SUBRESOURCE map{}; Check(resources.GetD3DDC()->Map(staging.get(),0,D3D11_MAP_READ,0,&map));
	for (uint32_t y=0; y<height; ++y) for (uint32_t x=0; x<width; ++x) for (uint32_t c=0; c<4; ++c) {
		const int value=static_cast<const uint8_t*>(map.pData)[y*map.RowPitch+x*4+c];
		if (std::abs(value-static_cast<int>(std::lround(expected[c]*255))) > (c == 3 ? 0 : 1))
			std::cerr << "Pixel mismatch from line " << where.line() << ": xy=" << x << ',' << y
				<< " channel=" << c << " actual=" << value << " expected=" << expected[c]*255 << '\n';
		Require(std::abs(value-static_cast<int>(std::lround(expected[c]*255))) <= (c == 3 ? 0 : 1));
	}
	resources.GetD3DDC()->Unmap(staging.get(),0);
}

std::array<float,4> OriginalLightness(const float* original, const float* candidate) {
	const auto base = ColorReference::ToLab({original[0],original[1],original[2]});
	auto controlled = ColorReference::ToLab({candidate[0],candidate[1],candidate[2]});
	controlled[0] = base[0];
	const auto rgb = ColorReference::FromLab(controlled);
	for (const double channel : rgb) Require(channel >= 0 && channel <= 1); // In-gamut reference fixture.
	return {float(rgb[0]),float(rgb[1]),float(rgb[2]),original[3]};
}

#include "DLSSNRPerformanceNativeCases.inl"
#include "DLSSNRBeta3NativeCases.inl"
#include "DLSSNRGpuFusionNativeCases.inl"
#include "DLSSNRSubmissionWorkerNativeCases.inl"

int main(int argc, char** argv) {
	bool performanceOnly=false,inferenceOnly=false,debugLayer=true;
	bool alternatingOnly=false,pipelineOnly=false,hotCacheOnly=false,postOutputOnly=false,gpuFusionLiveOnly=false,submissionWorkerOnly=false;
	bool sparsePairedInputOnly=false;
	for (int i=1;i<argc;++i) {
		const std::string_view argument=argv[i];
		performanceOnly |= argument == "--performance-only" || argument == "--beta3-only";
		alternatingOnly |= argument == "--alternating-only";
		pipelineOnly |= argument == "--pipeline-only";
		hotCacheOnly |= argument == "--hot-cache-only";
		postOutputOnly |= argument == "--post-output-only";
		gpuFusionLiveOnly |= argument == "--gpu-fusion-live-only";
		submissionWorkerOnly |= argument == "--submission-worker-only";
		sparsePairedInputOnly |= argument == "--sparse-paired-input-only";
		inferenceOnly |= argument == "--inference-only";
		if (argument == "--no-debug") debugLayer=false;
	}
	performanceOnly |= alternatingOnly || pipelineOnly || hotCacheOnly || postOutputOnly || gpuFusionLiveOnly || submissionWorkerOnly || sparsePairedInputOnly;
	winrt::init_apartment();
	Logger::Get().Initialize(spdlog::level::info, L"DLSSNRPipelineTests.log", 10000000, 1);
	winrt::com_ptr<ID3D12Debug> debug12;
	if (debugLayer && SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(debug12.put())))) debug12->EnableDebugLayer();
	DeviceResources resources;
	Check(CreateDXGIFactory2(0, IID_PPV_ARGS(resources._dxgiFactory.put())));
	for (UINT i = 0;; ++i) {
		winrt::com_ptr<IDXGIAdapter1> adapter;
		Require(SUCCEEDED(resources._dxgiFactory->EnumAdapters1(i, adapter.put())));
		DXGI_ADAPTER_DESC1 desc{}; Check(adapter->GetDesc1(&desc));
		if (desc.VendorId != 0x10de) continue;
		resources._graphicsAdapter = adapter.as<IDXGIAdapter4>();
		winrt::com_ptr<ID3D11Device> device;
		winrt::com_ptr<ID3D11DeviceContext> context;
		auto hr = D3D11CreateDevice(adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
			debugLayer ? D3D11_CREATE_DEVICE_DEBUG : 0, nullptr, 0, D3D11_SDK_VERSION, device.put(), nullptr, context.put());
		if (hr == DXGI_ERROR_SDK_COMPONENT_MISSING) hr = D3D11CreateDevice(adapter.get(),
			D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, device.put(), nullptr, context.put());
		Check(hr);
		resources._d3dDevice = device.as<ID3D11Device5>();
		resources._d3dDC = context.as<ID3D11DeviceContext4>();
		std::wcout << L"Adapter: " << desc.Description << L'\n';
		break;
	}
	NgxD3D12Core core;
	if (sparsePairedInputOnly) RunSparsePairedInputNativeCases(resources,core);
	else if (gpuFusionLiveOnly) {
		RunGpuFusionLiveNativeCases(resources,core);
		RunSparseLiveNativeCases(resources,core);
		RunSparsePairedInputNativeCases(resources,core);
	}
	else if (submissionWorkerOnly) RunSubmissionWorkerNativeCases(resources,core);
	else if (alternatingOnly) RunPerformanceNativeCases(resources,core);
	else if (pipelineOnly) RunPipelineNativeCases(resources,core);
	else if (hotCacheOnly) RunHotResolutionNativeCases(resources,core);
	else if (postOutputOnly) RunPostOutputPoolRetryNativeCase(resources,core);
	else if (!inferenceOnly) {
		RunPerformanceNativeCases(resources,core);
		RunPipelineNativeCases(resources,core);
		RunHotResolutionNativeCases(resources,core);
		RunPostOutputPoolRetryNativeCase(resources,core);
		RunSubmissionWorkerNativeCases(resources,core);
	}
	if (!performanceOnly) {
	for (int inputCase : {0, 1, 2, 3, 4}) for (uint32_t percent : {25u, 67u, 100u}) {
		const bool hdr = inputCase == 4, bgra = inputCase == 2 || inputCase == 3;
		const bool scaling = inputCase == 1 || inputCase == 3 || hdr;
		if (hdr && percent != 100) continue;
		constexpr uint32_t w = 513, h = 289;
		const auto format = hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
		auto input = DirectXHelper::CreateTexture2D(resources.GetD3DDevice(), bgra ? DXGI_FORMAT_B8G8R8A8_UNORM : format, w, h,
			D3D11_BIND_SHADER_RESOURCE | (bgra ? 0 : D3D11_BIND_UNORDERED_ACCESS));
		auto output = DirectXHelper::CreateTexture2D(resources.GetD3DDevice(), format, w, h,
			D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
		winrt::com_ptr<ID3D11UnorderedAccessView> inputUav;
		const float color[4]{.4f, .3f, .6f, .43f};
		if (bgra) {
			std::vector<uint32_t> pixels(w*h, 153u | (76u<<8) | (102u<<16) | (110u<<24));
			resources.GetD3DDC()->UpdateSubresource(input.get(),0,nullptr,pixels.data(),w*4,0);
		} else {
			Check(resources.GetD3DDevice()->CreateUnorderedAccessView(input.get(), nullptr, inputUav.put()));
			resources.GetD3DDC()->ClearUnorderedAccessViewFloat(inputUav.get(), color);
		}
		FrameGuidanceView zero;
		std::array<winrt::com_ptr<ID3D11Texture2D>, 3> guides;
		const DXGI_FORMAT formats[]{DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R8_UNORM};
		FrameGuidanceResource* channels[]{&zero.motion, &zero.depth, &zero.confidence};
		for (size_t i = 0; i < guides.size(); ++i) {
			guides[i] = DirectXHelper::CreateSharedTexture2D(resources.GetD3DDevice(), formats[i], w, h,
				D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, "DLSSNR native test");
			Require(static_cast<bool>(guides[i]));
			winrt::com_ptr<ID3D11UnorderedAccessView> uav;
			Check(resources.GetD3DDevice()->CreateUnorderedAccessView(guides[i].get(), nullptr, uav.put()));
			const float value[4]{i == 1 ? 1.f : 0.f, 0, 0, 0}; // exercise constant ONE depth
			resources.GetD3DDC()->ClearUnorderedAccessViewFloat(uav.get(), value);
			*channels[i] = {guides[i].get(), formats[i], {.resourceGeneration=1,
				.sourceExtent={w,h}, .validRegion={0,0,w,h}, .valid=true, .isZero=true}};
		}
		for (int count : {1, 2, 3, 2, 1}) {
			EffectOption option;
			option.parameters["enableInputResolutionScaling"] = scaling ? 1.f : 0.f;
			option.parameters["inputResolutionPercent"] = static_cast<float>(percent);
			std::vector<DLSSNRSettings> settings(count, ParseDLSSNRSettings(option, hdr));
			DLSSNRFilter chain;
			Require(chain.InitializeChain(resources, core, input.get(), output.get(), settings));
			testedDevice = core.Device();
			auto& impl = *chain._impl;
			Require(impl.laterPasses.size() == size_t(count - 1));
			auto pass = [&](size_t i) -> DLSSNRFilter::Impl& { return i ? *impl.laterPasses[i-1] : impl; };
			for (int i=0; i<count; ++i) {
				Require(settings[i].enableInputResolutionScaling);
				Require(pass(i).modelPreset == settings[i].modelPreset);
			}
			Require(impl.useResolutionScaling == !hdr);
			for (uint64_t frame = 1; frame <= 6; ++frame) {
				for (auto* channel : channels) { channel->metadata.frameId=frame; channel->metadata.timestamp100ns=static_cast<int64_t>(frame)*166667; }
				NativeEffectDrawContext context{.input=input.get(), .output=output.get(), .frameId=frame,
					.inputRevision=frame,
					.frameGuidance=zero, .zeroFrameGuidance=zero};
				const auto fenceBefore = impl.fenceValue;
				Require(chain.Draw(context) && chain.IsHealthy());
				Require(impl.fenceValue == fenceBefore + 2); // exactly one input/output handshake
				for (int i=0; i<count; ++i) Require(pass(i).evaluateCount == frame);
				context.isNewCaptureFrame = false;
				Require(chain.Draw(context));
				Require(impl.fenceValue == fenceBefore + 2); // entire duplicate chain reused
				if (!hdr && frame == 6) {
					option.parameters["residualSaturation"] = .5f;
					std::vector<std::string> names{"residualSaturation"};
					Require(chain.ApplyLiveParameters(option,names) && chain.Draw(context));
					Require(impl.fenceValue == fenceBefore + 2);
					for (int i=0; i<count; ++i) Require(pass(i).evaluateCount == frame);
					// All new controls share the cached total NR result on real hardware.
					for (auto key : {"residualHueProtection","residualDarkProtection","residualHighlightProtection",
						"residualChromaProtection",
						"residualLocalCompression","residualLowFrequencyGain","residualDetailGain","residualDebugView"}) {
						const std::string_view name(key);
						option.parameters[key] = name == "residualDebugView" ? 5.f : .5f;
						names={key};
						Require(chain.ApplyLiveParameters(option,names) && chain.Draw(context));
						for (int i=0;i<count;++i) Require(pass(i).evaluateCount == frame);
					}
					option.parameters["residualDebugView"] = 0;
					option.parameters["residualMultiplier"] = 0;
					names={"residualDebugView","residualMultiplier"};
					Require(chain.ApplyLiveParameters(option,names) && chain.Draw(context));
					const float sourceColor[4]{.4f,.3f,.6f,.43f};
					CheckColor(resources,output.get(),sourceColor);
					if (count > 1) {
						option.parameters["pass2_intensity"] = .8f; names={"pass2_intensity"};
						Require(chain.ApplyLiveParameters(option,names) && chain.Draw(context));
						Require(pass(0).evaluateCount == frame);
						for (int i=1; i<count; ++i) Require(pass(i).evaluateCount == frame + 1);
					}
				}
			}
			// Same capture, different upstream output: all NR inputs are dirty.
			// Capture ID stays real; a following identical redraw must reuse it.
			NativeEffectDrawContext revised{.input=input.get(), .output=output.get(), .frameId=6,
				.inputRevision=100, .inputHistoryRevision=1, .inputHistoryReset=true,
				.isNewCaptureFrame=false, .frameGuidance=zero, .zeroFrameGuidance=zero};
			std::array<uint64_t,3> beforeRevised{};
			for (int i=0; i<count; ++i) beforeRevised[i]=pass(i).evaluateCount;
			Require(chain.Draw(revised));
			for (int i=0; i<count; ++i) Require(pass(i).evaluateCount==beforeRevised[i]+1);
			revised.inputHistoryReset=false;
			Require(chain.Draw(revised));
			for (int i=0; i<count; ++i) Require(pass(i).evaluateCount==beforeRevised[i]+1);
			Require(chain.Drain());
			Require(DebugMessages());
			D3D11_TEXTURE2D_DESC desc{}; output->GetDesc(&desc);
			desc.Usage=D3D11_USAGE_STAGING; desc.BindFlags=0; desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ; desc.MiscFlags=0;
			winrt::com_ptr<ID3D11Texture2D> staging;
			Check(resources.GetD3DDevice()->CreateTexture2D(&desc,nullptr,staging.put()));
			resources.GetD3DDC()->CopyResource(staging.get(),output.get());
			D3D11_MAPPED_SUBRESOURCE map{}; Check(resources.GetD3DDC()->Map(staging.get(),0,D3D11_MAP_READ,0,&map));
			if (!hdr) for (uint32_t y=0; y<h; ++y) for (uint32_t x=0; x<w; ++x)
				Require(static_cast<const uint8_t*>(map.pData)[y*map.RowPitch+x*4+3] == 110);
			resources.GetD3DDC()->Unmap(staging.get(),0);
			// A normal SDK failure can occur after a successful recorded prefix.
			// Submit/retire that work, invalidate the whole chain, and never expose it.
			pass(static_cast<size_t>(count-1)).snippetEvaluateFeature=&FailEvaluation;
			for (auto* channel : channels) { channel->metadata.frameId=7; channel->metadata.timestamp100ns=7*166667; }
			NativeEffectDrawContext failed{.input=input.get(), .output=output.get(), .frameId=7,
				.frameGuidance=zero, .zeroFrameGuidance=zero};
			Require(!chain.Draw(failed) && !chain.IsHealthy() && !impl.cache.valid);
			Require(chain.Drain());
			Require(DebugMessages());
			if (count==1) Require(chain.Draw(failed)); // original single-pass fallback
			else Require(!chain.Draw(failed));
			std::cout << "native chain passed: hdr=" << hdr << " bgra=" << bgra << " scaling=" << scaling << " percent=" << percent << " count=" << count << '\n';
		}
	}
	// Execute the production resampling/reconstruction helpers without NGX at
	// tiny and odd sizes, including signed and clipped residuals and alpha.
	for (const auto extent : {FrameGuidanceExtent{1,1}, FrameGuidanceExtent{17,11}, FrameGuidanceExtent{513,289}}) {
		for (uint32_t percent : {25u, 67u, 100u}) {
			DLSSNRFilter::Impl fixture;
			fixture.device11=resources.GetD3DDevice(); fixture.context11=resources.GetD3DDC(); fixture.device12.copy_from(core.Device());
			fixture.sourceWidth=extent.width; fixture.sourceHeight=extent.height;
			fixture.width=std::max(1u,static_cast<uint32_t>(std::lround(double(extent.width)*percent/100)));
			fixture.height=std::max(1u,static_cast<uint32_t>(std::lround(double(extent.height)*percent/100)));
			fixture.useResolutionScaling=true;
			auto input=DirectXHelper::CreateTexture2D(fixture.device11,DXGI_FORMAT_R8G8B8A8_UNORM,extent.width,extent.height,
				D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS);
			auto output=DirectXHelper::CreateTexture2D(fixture.device11,DXGI_FORMAT_R8G8B8A8_UNORM,extent.width,extent.height,
				D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS);
			D3D11_TEXTURE2D_DESC desc{}; output->GetDesc(&desc);
			auto shared=desc; shared.Width=fixture.width; shared.Height=fixture.height;
			Require(CreateSharedTexture(fixture,shared,true,fixture.sharedInput11,fixture.sharedInput12));
			Require(CreateSharedTexture(fixture,shared,true,fixture.sharedOutput11,fixture.sharedOutput12));
			Require(CreateResolutionScalingResources(fixture,input.get(),output.get(),desc));
			winrt::com_ptr<ID3D11UnorderedAccessView> inputUav, denoisedUav;
			Check(fixture.device11->CreateUnorderedAccessView(input.get(),nullptr,inputUav.put()));
			Check(fixture.device11->CreateUnorderedAccessView(fixture.sharedOutput11.get(),nullptr,denoisedUav.put()));
			const float original[4]{.4f,.3f,.6f,.43f};
			fixture.context11->ClearUnorderedAccessViewFloat(inputUav.get(),original);
			Require(PrepareInput(fixture,input.get()));
			fixture.context11->CopyResource(fixture.sharedOutput11.get(),fixture.sharedInput11.get());
			DLSSNRSettings controls;
			Require(CompositeResidual(fixture,output.get(),fixture.sharedOutputSrv11.get(),controls));
			CheckColor(resources,output.get(),original);
			const float negative[4]{.2f,.1f,.4f,.43f};
			fixture.context11->ClearUnorderedAccessViewFloat(denoisedUav.get(),negative);
			Require(CompositeResidual(fixture,output.get(),fixture.sharedOutputSrv11.get(),controls));
			CheckColor(resources,output.get(),negative);
			controls.shadowStructureMultiplier=0; fixture.residualParametersDirty=true;
			Require(CompositeResidual(fixture,output.get(),fixture.sharedOutputSrv11.get(),controls));
			// Directional gains now control lightness alone; chroma correction stays.
			// Compare against independent double-precision color coordinates.
			const auto noDarkening = OriginalLightness(original,negative);
			CheckColor(resources,output.get(),noDarkening.data());
			const float positive[4]{.6f,.5f,.8f,.43f};
			fixture.context11->ClearUnorderedAccessViewFloat(denoisedUav.get(),positive);
			controls.shadowStructureMultiplier=1; controls.reflectionGlowMultiplier=0;
			Require(CompositeResidual(fixture,output.get(),fixture.sharedOutputSrv11.get(),controls));
			const auto noBrightening = OriginalLightness(original,positive);
			CheckColor(resources,output.get(),noBrightening.data());
			controls.reflectionGlowMultiplier=1; controls.residualMultiplier=2;
			Require(CompositeResidual(fixture,output.get(),fixture.sharedOutputSrv11.get(),controls));
			const float clipped[4]{.8f,.7f,1.f,.43f};
			CheckColor(resources,output.get(),clipped);
		}
	}
	}
	winrt::com_ptr<ID3D11InfoQueue> info11;
	if (SUCCEEDED(resources.GetD3DDevice()->QueryInterface(IID_PPV_ARGS(info11.put())))) {
		for (UINT64 i=0; i<info11->GetNumStoredMessages(); ++i) {
			SIZE_T bytes=0; info11->GetMessage(i,nullptr,&bytes);
			std::vector<uint8_t> data(bytes); auto* msg=reinterpret_cast<D3D11_MESSAGE*>(data.data());
			Check(info11->GetMessage(i,msg,&bytes));
			if (msg->Severity<=D3D11_MESSAGE_SEVERITY_WARNING) { std::cerr<<msg->pDescription<<'\n'; Require(false); }
		}
	}
	Logger::Get().Flush();
	std::cout << (alternatingOnly ? "Real NGX Alternating intervals2/3/4, true age, redraw cadence and hard rebuild passed.\n" :
		pipelineOnly ? "Real NGX Pipeline trusted reuse, complete fallback, age/reset and in-flight lifecycle passed.\n" :
		hotCacheOnly ? "Real NGX prewarm, hot switch/return, bounded cache and parameter invalidation passed.\n" :
		postOutputOnly ? "Real NGX Pipeline post-output pool retry, idle-owner wake-up and final quality update passed.\n" :
		gpuFusionLiveOnly ? "Real NGX all-mode live detail, GPU constants, debug bypass and zero residual with anti-flicker passed.\n" :
		sparsePairedInputOnly ? "Real NGX Sparse distinct-input native-pair ownership, normal live controls and exact target-plus-residual pixels passed.\n" :
		inferenceOnly ? "Real NGX chain, partial reuse, boundary fences, SDR/HDR/restart and tiny/odd reconstruction passed.\n" :
		performanceOnly ? "Real NGX four-mode performance, Pipeline and hot-resolution regressions passed.\n" :
		"Real NGX chain, SDR/HDR/restart, reconstruction and four-mode performance regressions passed.\n");
	std::cout << "D3D11/D3D12 validation clean; debugLayerRequested=" << debugLayer << '\n';
}
