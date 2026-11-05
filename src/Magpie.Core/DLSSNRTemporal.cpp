#include "pch.h"
#include "DLSSNRTemporal.h"
#include "DLSSNRTemporalShader.h"
#include "DLSSNRTemporalState.h"
#include "DeviceResources.h"
#include "DirectXHelper.h"
#include "FrameTrace.h"
#include "Logger.h"
#include <array>

namespace Magpie {
struct DLSSNRTemporal::Impl {
	ID3D11Device5* device = nullptr;
	ID3D11DeviceContext4* dc = nullptr;
	FrameGuidanceExtent extent{};
	int mode = 0;
	float timeMs = DLSSNR_ANTI_FLICKER_TIME_DEFAULT_MS;
	bool hdr = false;
	float chromaStrength = 0;
	bool enforceZero = false;
	bool lastMotion = false;
	uint32_t next = 0;
	DLSSNRTemporalState state;
	DLSSNRSparseTemporalObservation observation;
	DLSSNRSparseTemporalDrawIdentity lastSparseDraw;
	bool lastSparseDrawValid = false;
	bool sparseHistory = false, baseFollowsInput = false;
	uint64_t observationRevision = 0;
	int64_t observationTimestamp100ns = 0;
	ID3D11Texture2D* inputTexture = nullptr;
	ID3D11Texture2D* outputTexture = nullptr;
	uint64_t lastInputRevision = 0;
	winrt::com_ptr<ID3D11ComputeShader> shader;
	winrt::com_ptr<ID3D11ComputeShader> reduceShader;
	std::array<winrt::com_ptr<ID3D11ShaderResourceView>, 2> low;
	std::array<winrt::com_ptr<ID3D11UnorderedAccessView>, 2> lowOut;
	winrt::com_ptr<ID3D11Buffer> constants;
	winrt::com_ptr<ID3D11SamplerState> sampler;
	std::array<winrt::com_ptr<ID3D11ShaderResourceView>, 3> inputs;
	winrt::com_ptr<ID3D11UnorderedAccessView> output;
	std::array<winrt::com_ptr<ID3D11ShaderResourceView>, 2> history, guide;
	std::array<winrt::com_ptr<ID3D11UnorderedAccessView>, 2> historyOut, guideOut;
	winrt::com_ptr<ID3D11ShaderResourceView> motion;
	winrt::com_ptr<ID3D11ShaderResourceView> zeroMotion;
	ID3D11Texture2D* motionTexture = nullptr; // Kept alive by motion's SRV.
	FrameGuidanceRegion region{};
	winrt::com_ptr<ID3D11ShaderResourceView> reuseValidity;
	uint64_t capture = 0, source = 0, epoch = 0, historyEpoch = 0;
	uint64_t outputRevision = 0;
	unsigned maxAge = 0;
};
DLSSNRTemporal::DLSSNRTemporal() noexcept = default;
DLSSNRTemporal::~DLSSNRTemporal() = default;
void DLSSNRTemporal::Reset() noexcept {
	if (_impl) {
		_impl->state.valid = false; _impl->observation = {};
		_impl->lastSparseDrawValid = false;
	}
}

void DLSSNRTemporal::SetPerformanceHistory(uint64_t capture, uint64_t source, uint64_t epoch,
	unsigned maxAge, uint64_t outputRevision, ID3D11ShaderResourceView* validity) noexcept {
	if (!_impl) return;
	auto& impl = *_impl;
	if (impl.sparseHistory) Reset();
	impl.sparseHistory = false;
	if (impl.capture == capture && (impl.source != source || impl.epoch != epoch || impl.outputRevision != outputRevision)) Reset();
	impl.capture = capture; impl.source = source; impl.epoch = epoch; impl.maxAge = maxAge;
	impl.outputRevision = outputRevision;
	impl.reuseValidity.copy_from(validity);
}

void DLSSNRTemporal::SetSparseHistory(uint64_t target, uint64_t source, uint64_t epoch,
	uint64_t observationRevision, unsigned maxSourceAge, uint64_t outputRevision,
	ID3D11ShaderResourceView* validity, int64_t observationTimestamp100ns) noexcept {
	if (!_impl) return;
	auto& impl = *_impl;
	if (!impl.sparseHistory) Reset();
	else if (impl.capture == target && (impl.source != source || impl.epoch != epoch ||
		impl.observationRevision != observationRevision || impl.outputRevision != outputRevision)) {
		// Recomposition of one target must not count the same pixels twice. Keep
		// the observation clock so a changed source cannot impersonate the same NR.
		impl.state.valid = false;
	}
	impl.sparseHistory = true;
	impl.capture = target; impl.source = source; impl.epoch = epoch;
	impl.observationRevision = observationRevision; impl.maxAge = maxSourceAge;
	impl.observationTimestamp100ns = observationTimestamp100ns;
	impl.outputRevision = outputRevision;
	impl.reuseValidity.copy_from(validity);
}

void DLSSNRTemporal::SetTimeConstant(float timeMs) noexcept {
	if (!_impl) return;
	timeMs = NormalizeDLSSNRAntiFlickerTimeMs(timeMs);
	if (_impl->timeMs == timeMs) return;
	_impl->timeMs = timeMs;
	Logger::Get().Info(fmt::format("DLSSNR anti-flicker time constant: {}ms (next capture)", timeMs));
}

void DLSSNRTemporal::ConfigureDetail(float strength, bool enforceZero) noexcept {
	if (!_impl) return;
	strength = !_impl->hdr && std::isfinite(strength) ? std::clamp(strength,0.f,1.f) : 0.f;
	if (_impl->chromaStrength != strength || _impl->enforceZero != enforceZero) Reset();
	_impl->chromaStrength = strength;
	_impl->enforceZero = !_impl->hdr && enforceZero;
}

bool DLSSNRTemporal::Initialize(DeviceResources& resources, ID3D11Texture2D* input,
	ID3D11Texture2D* base, ID3D11Texture2D* raw, ID3D11Texture2D* output,
	int mode, bool hdr, float timeMs) noexcept {
	_impl.reset();
	if (!mode) return true;
	auto impl = std::make_unique<Impl>();
	impl->device = resources.GetD3DDevice(); impl->dc = resources.GetD3DDC();
	impl->mode = mode; impl->hdr = hdr;
	impl->inputTexture = input; impl->outputTexture = output;
	impl->baseFollowsInput = input == base;
	impl->timeMs = NormalizeDLSSNRAntiFlickerTimeMs(timeMs);
	D3D11_TEXTURE2D_DESC desc{};
	output->GetDesc(&desc);
	impl->extent = {desc.Width, desc.Height};
	const std::array textures{input, base, raw};
	for (size_t i = 0; i < textures.size(); ++i) {
		D3D11_TEXTURE2D_DESC other{}; textures[i]->GetDesc(&other);
		if (other.Width != desc.Width || other.Height != desc.Height) return false;
		if (FAILED(impl->device->CreateShaderResourceView(textures[i], nullptr, impl->inputs[i].put()))) return false;
	}
	if (FAILED(impl->device->CreateUnorderedAccessView(output, nullptr, impl->output.put()))) return false;
	winrt::com_ptr<ID3DBlob> blob;
	if (!DirectXHelper::CompileComputeShader(DLSSNR_TEMPORAL_SHADER, "main", blob.put(), "DLSSNRTemporal")) return false;
	if (FAILED(impl->device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, impl->shader.put()))) return false;
	if (mode == 4) {
		blob = nullptr;
		if (!DirectXHelper::CompileComputeShader(DLSSNR_TEMPORAL_REDUCE_SHADER, "main", blob.put(), "DLSSNRTemporalReduce")) return false;
		if (FAILED(impl->device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, impl->reduceShader.put()))) return false;
	}
	D3D11_BUFFER_DESC buffer{};
	buffer.ByteWidth = 64; buffer.Usage = D3D11_USAGE_DEFAULT; buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	if (FAILED(impl->device->CreateBuffer(&buffer, nullptr, impl->constants.put()))) return false;
	D3D11_SAMPLER_DESC sampler{};
	sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	sampler.MaxLOD = D3D11_FLOAT32_MAX;
	if (FAILED(impl->device->CreateSamplerState(&sampler, impl->sampler.put()))) return false;
	D3D11_TEXTURE2D_DESC zeroDesc{};
	zeroDesc.Width = zeroDesc.Height = zeroDesc.MipLevels = zeroDesc.ArraySize = zeroDesc.SampleDesc.Count = 1;
	zeroDesc.Format = DXGI_FORMAT_R32G32_FLOAT; zeroDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	const float zeroMotion[2]{};
	D3D11_SUBRESOURCE_DATA zeroData{zeroMotion, sizeof(zeroMotion), 0};
	winrt::com_ptr<ID3D11Texture2D> zeroTexture;
	if (FAILED(impl->device->CreateTexture2D(&zeroDesc, &zeroData, zeroTexture.put())) ||
		FAILED(impl->device->CreateShaderResourceView(zeroTexture.get(), nullptr, impl->zeroMotion.put()))) return false;
	desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1; desc.SampleDesc.Quality = 0;
	desc.Usage = D3D11_USAGE_DEFAULT; desc.CPUAccessFlags = desc.MiscFlags = 0;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
	for (size_t i = 0; i < 2; ++i) {
		for (bool guide : {false, true}) {
			winrt::com_ptr<ID3D11Texture2D> texture;
			auto& srv = guide ? impl->guide[i] : impl->history[i];
			auto& uav = guide ? impl->guideOut[i] : impl->historyOut[i];
			if (FAILED(impl->device->CreateTexture2D(&desc, nullptr, texture.put())) ||
				FAILED(impl->device->CreateShaderResourceView(texture.get(), nullptr, srv.put())) ||
				FAILED(impl->device->CreateUnorderedAccessView(texture.get(), nullptr, uav.put()))) return false;
			const float zero[4]{};
			impl->dc->ClearUnorderedAccessViewFloat(uav.get(), zero);
		}
	}
	if (mode == 4) {
		desc.Width = (desc.Width+1)/2; desc.Height = (desc.Height+1)/2;
		for (size_t i = 0; i < 2; ++i) {
			winrt::com_ptr<ID3D11Texture2D> texture;
			if (FAILED(impl->device->CreateTexture2D(&desc, nullptr, texture.put())) ||
				FAILED(impl->device->CreateShaderResourceView(texture.get(), nullptr, impl->low[i].put())) ||
				FAILED(impl->device->CreateUnorderedAccessView(texture.get(), nullptr, impl->lowOut[i].put()))) return false;
		}
	}
	_impl = std::move(impl);
	Logger::Get().Info(fmt::format("DLSSNR anti-flicker: route={} history={}x{} tau={}ms lowObservations={}x{}",
		mode, _impl->extent.width, _impl->extent.height, _impl->timeMs, mode == 4 ? desc.Width : 0, mode == 4 ? desc.Height : 0));
	return true;
}

bool DLSSNRTemporal::Draw(const NativeEffectDrawContext& context, bool bypassHistory) noexcept {
	if (!_impl) return true;
	FrameTrace::NativeNrScope traceDraw(FrameTrace::NativeNrStage::TemporalDraw, context.frameId);
	auto& impl = *_impl;
	const auto& zero = context.zeroFrameGuidance.motion.metadata;
	const auto& guidance = context.frameGuidance;
	const DLSSNRSparseTemporalDrawIdentity sparseDraw{
		context.frameId,context.inputRevision,context.inputHistoryRevision,
		impl.capture,impl.source,impl.epoch,impl.observationRevision,impl.outputRevision,
		zero.resourceGeneration,guidance.motion.metadata.resourceGeneration,
		reinterpret_cast<uintptr_t>(context.input),reinterpret_cast<uintptr_t>(context.output),
		(context.inputHistoryReset ? 1u : 0u) |
		(context.zeroFrameGuidance.requiresHistoryReset ? 2u : 0u) |
		(zero.requiresHistoryReset ? 4u : 0u) |
		(guidance.requiresHistoryReset ? 8u : 0u) |
		(guidance.motion.metadata.requiresHistoryReset ? 16u : 0u)};
	if (impl.sparseHistory && !bypassHistory && impl.state.valid && impl.lastSparseDrawValid &&
		sparseDraw.ReusesPixelsFrom(impl.lastSparseDraw)) {
		impl.lastSparseDraw = sparseDraw;
		return true;
	}
	if (context.input && context.input != impl.inputTexture) {
		D3D11_TEXTURE2D_DESC desc{}; context.input->GetDesc(&desc);
		if (desc.Width != impl.extent.width || desc.Height != impl.extent.height) return false;
		winrt::com_ptr<ID3D11ShaderResourceView> input;
		{
			FrameTrace::NativeNrScope trace(FrameTrace::NativeNrStage::TemporalInputView, context.frameId);
			const HRESULT hr = impl.device->CreateShaderResourceView(context.input,nullptr,input.put());
			trace.Result(hr);
			if (FAILED(hr)) return false;
		}
		impl.inputs[0] = input;
		if (impl.baseFollowsInput) impl.inputs[1] = input;
		impl.inputTexture = context.input;
	} else if (!context.input && impl.sparseHistory) return false;
	if (context.output && context.output != impl.outputTexture) {
		D3D11_TEXTURE2D_DESC desc{}; context.output->GetDesc(&desc);
		if (desc.Width != impl.extent.width || desc.Height != impl.extent.height) return false;
		winrt::com_ptr<ID3D11UnorderedAccessView> output;
		{
			FrameTrace::NativeNrScope trace(FrameTrace::NativeNrStage::TemporalOutputView, context.frameId);
			const HRESULT hr = impl.device->CreateUnorderedAccessView(context.output,nullptr,output.put());
			trace.Result(hr);
			if (FAILED(hr)) return false;
		}
		impl.output = std::move(output); impl.outputTexture = context.output;
	}
	bool motion = context.isNewCaptureFrame && impl.mode >= 2 &&
		guidance.motion.IsValid(DXGI_FORMAT_R16G16_FLOAT, context.frameId, impl.extent) &&
		!guidance.motion.metadata.isZero &&
		guidance.motionDirection == FrameGuidanceMotionDirection::CurrentToPrevious &&
		guidance.motionUnit == FrameGuidanceMotionUnit::SourcePixels;
	const auto& meta = motion ? guidance.motion.metadata : zero;
	const int64_t timestamp = impl.sparseHistory && context.captureAdmissionTime100ns
		? context.captureAdmissionTime100ns : meta.timestamp100ns;
	// Source age follows the current mapping. The observation timestamp is the
	// original contributing target, even if asynchronous confirmation is later.
	const bool sourceAccepted = !impl.sparseHistory || impl.observation.Accepts(
		impl.capture,impl.source,impl.epoch,impl.observationRevision,impl.maxAge);
	const bool observationExpired = impl.sparseHistory && (impl.observationTimestamp100ns <= 0 ||
		timestamp < impl.observationTimestamp100ns || uint64_t(timestamp)-uint64_t(impl.observationTimestamp100ns) >
		uint64_t(DLSSNRSparseTemporalObservation::MAX_IDLE_100NS));
	const bool discardHistory = bypassHistory || (impl.sparseHistory && (!sourceAccepted || observationExpired));
	const bool epochChanged = impl.state.valid && impl.historyEpoch != impl.epoch;
	// Fresh NR renews support for the accumulated correction. Check the real
	// source of this output, so transporting that same source cannot renew it.
	const bool historyExpired = epochChanged || (impl.sparseHistory ?
		(!sourceAccepted || observationExpired || impl.observation.Expired(timestamp)) :
		(impl.maxAge && (!impl.source || impl.source > impl.capture || impl.capture-impl.source > impl.maxAge)));
	const bool reset = bypassHistory || historyExpired || context.inputHistoryReset || context.zeroFrameGuidance.requiresHistoryReset || zero.requiresHistoryReset ||
		(motion && (guidance.requiresHistoryReset || meta.requiresHistoryReset)) ||
		(motion && !impl.state.MotionPairMatches(context.frameId, context.previousCaptureFrameId)) ||
		motion != impl.lastMotion || meta.validRegion != impl.region;
	// Duplicate draws leave both ping-pong indices and EMA time untouched.
	if (!reset && impl.lastInputRevision == context.inputRevision &&
		impl.state.Duplicate(context.frameId, context.inputHistoryRevision)) return true;
	const bool metadataValid = meta.valid && meta.frameId == context.frameId &&
		meta.sourceExtent == impl.extent && meta.validRegion.IsInside(impl.extent);
	const float transportWeight = impl.state.Weight(context.frameId, context.inputHistoryRevision,
		meta.resourceGeneration, timestamp, reset || !metadataValid, impl.timeMs);
	const float weight = impl.sparseHistory ? impl.observation.Weight(impl.epoch,
		impl.observationRevision,timestamp,transportWeight <= 0,impl.timeMs,impl.observationTimestamp100ns) : transportWeight;
	if (motion) {
		const auto sync = meta.sync;
		if (sync.fence && sync.value) {
			FrameTrace::NativeNrScope trace(FrameTrace::NativeNrStage::TemporalMotionWait, context.frameId);
			const HRESULT hr = impl.dc->Wait(sync.fence, sync.value);
			trace.Result(hr);
			if (FAILED(hr)) return false;
		}
		if (impl.motionTexture != guidance.motion.texture) {
			impl.motion = nullptr;
			impl.motionTexture = nullptr;
			{
				FrameTrace::NativeNrScope trace(FrameTrace::NativeNrStage::TemporalMotionView, context.frameId);
				const HRESULT hr = impl.device->CreateShaderResourceView(guidance.motion.texture, nullptr, impl.motion.put());
				trace.Result(hr);
				if (FAILED(hr)) return false;
			}
			impl.motionTexture = guidance.motion.texture;
		}
	}
	const FrameGuidanceRegion region = metadataValid ? meta.validRegion : FrameGuidanceRegion::Full(impl.extent);
	struct Constants {
		uint32_t width, height, motion, hdr;
		float weight; uint32_t route, lowWidth, lowHeight;
		uint32_t left, top, right, bottom;
		float chromaStrength; uint32_t enforceZero, useReuseValidity, bypassHistory;
	} constants{impl.extent.width, impl.extent.height, motion ? 1u : 0u, impl.hdr ? 1u : 0u,
		weight, static_cast<uint32_t>(impl.mode), (impl.extent.width+1)/2, (impl.extent.height+1)/2,
		region.x, region.y, region.x+region.width, region.y+region.height,
		impl.chromaStrength, impl.enforceZero ? 1u : 0u,
		(impl.reuseValidity ? 1u : 0u) | (impl.sparseHistory ? 2u : 0u), discardHistory ? 1u : 0u};
	static_assert(sizeof(constants) == 64);
	{
		FrameTrace::NativeNrScope trace(FrameTrace::NativeNrStage::TemporalConstants, context.frameId);
		impl.dc->UpdateSubresource(impl.constants.get(), 0, nullptr, &constants, 0, 0);
	}
	const auto next = impl.next, previous = next ^ 1u;
	ID3D11ShaderResourceView* srvs[]{impl.inputs[0].get(), impl.inputs[1].get(), impl.inputs[2].get(),
		impl.history[previous].get(), impl.guide[previous].get(), motion ? impl.motion.get() : impl.zeroMotion.get(),
		impl.mode == 4 ? impl.low[0].get() : impl.history[previous].get(),
		impl.mode == 4 ? impl.low[1].get() : impl.guide[previous].get(), impl.reuseValidity.get()};
	ID3D11UnorderedAccessView* uavs[]{impl.output.get(), impl.historyOut[next].get(), impl.guideOut[next].get()};
	ID3D11Buffer* cb = impl.constants.get(); ID3D11SamplerState* sampler = impl.sampler.get();
	{
		FrameTrace::NativeNrScope trace(FrameTrace::NativeNrStage::TemporalConstantBind, context.frameId);
		impl.dc->CSSetConstantBuffers(0, 1, &cb);
	}
	if (impl.mode == 4 && !discardHistory) {
		ID3D11UnorderedAccessView* lowUavs[]{impl.lowOut[0].get(), impl.lowOut[1].get()};
		{
			FrameTrace::NativeNrScope trace(FrameTrace::NativeNrStage::TemporalReduceBind, context.frameId);
			impl.dc->CSSetShader(impl.reduceShader.get(), nullptr, 0);
			impl.dc->CSSetShaderResources(0, 3, srvs);
			impl.dc->CSSetUnorderedAccessViews(0, 2, lowUavs, nullptr);
		}
		{
			FrameTrace::NativeNrScope trace(FrameTrace::NativeNrStage::TemporalReduceDispatch, context.frameId);
			impl.dc->Dispatch((constants.lowWidth+7)/8, (constants.lowHeight+7)/8, 1);
		}
		ID3D11UnorderedAccessView* nullLow[2]{};
		{
			FrameTrace::NativeNrScope trace(FrameTrace::NativeNrStage::TemporalReduceUnbind, context.frameId);
			impl.dc->CSSetUnorderedAccessViews(0, 2, nullLow, nullptr);
		}
	}
	{
		FrameTrace::NativeNrScope trace(FrameTrace::NativeNrStage::TemporalMainBind, context.frameId);
		impl.dc->CSSetShader(impl.shader.get(), nullptr, 0);
		impl.dc->CSSetShaderResources(0, ARRAYSIZE(srvs), srvs);
		impl.dc->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);
		impl.dc->CSSetConstantBuffers(0, 1, &cb); impl.dc->CSSetSamplers(0, 1, &sampler);
	}
	{
		FrameTrace::NativeNrScope trace(FrameTrace::NativeNrStage::TemporalMainDispatch, context.frameId);
		impl.dc->Dispatch((impl.extent.width+7)/8, (impl.extent.height+7)/8, 1);
	}
	ID3D11ShaderResourceView* nullSrvs[9]{}; ID3D11UnorderedAccessView* nullUavs[3]{};
	cb = nullptr; sampler = nullptr;
	{
		FrameTrace::NativeNrScope trace(FrameTrace::NativeNrStage::TemporalUnbind, context.frameId);
		impl.dc->CSSetShaderResources(0, 9, nullSrvs); impl.dc->CSSetUnorderedAccessViews(0, 3, nullUavs, nullptr);
		impl.dc->CSSetConstantBuffers(0, 1, &cb); impl.dc->CSSetSamplers(0, 1, &sampler);
		impl.dc->CSSetShader(nullptr, nullptr, 0);
	}
	impl.state.Commit(context.frameId, context.inputHistoryRevision, meta.resourceGeneration, timestamp);
	if (impl.sparseHistory) {
		if (sourceAccepted && metadataValid) impl.observation.Observe(
			impl.source,impl.epoch,impl.observationRevision,impl.observationTimestamp100ns);
	}
	impl.historyEpoch = impl.epoch;
	impl.lastInputRevision = context.inputRevision;
	impl.lastMotion = motion; impl.region = meta.validRegion; impl.next ^= 1u;
	impl.lastSparseDraw = sparseDraw;
	impl.lastSparseDrawValid = impl.sparseHistory && !bypassHistory;
	return true;
}
}
