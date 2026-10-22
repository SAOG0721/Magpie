#include "pch.h"
#include "DLSSNRTemporal.h"
#include "DLSSNRTemporalShader.h"
#include "DLSSNRTemporalState.h"
#include "DeviceResources.h"
#include "DirectXHelper.h"
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
};
DLSSNRTemporal::DLSSNRTemporal() noexcept = default;
DLSSNRTemporal::~DLSSNRTemporal() = default;
void DLSSNRTemporal::Reset() noexcept {
	if (_impl) _impl->state.valid = false;
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

bool DLSSNRTemporal::Draw(const NativeEffectDrawContext& context) noexcept {
	if (!_impl) return true;
	auto& impl = *_impl;
	const auto& zero = context.zeroFrameGuidance.motion.metadata;
	const auto& guidance = context.frameGuidance;
	bool motion = context.isNewCaptureFrame && impl.mode >= 2 &&
		guidance.motion.IsValid(DXGI_FORMAT_R16G16_FLOAT, context.frameId, impl.extent) &&
		!guidance.motion.metadata.isZero &&
		guidance.motionDirection == FrameGuidanceMotionDirection::CurrentToPrevious &&
		guidance.motionUnit == FrameGuidanceMotionUnit::SourcePixels;
	const auto& meta = motion ? guidance.motion.metadata : zero;
	const bool reset = context.inputHistoryReset || context.zeroFrameGuidance.requiresHistoryReset || zero.requiresHistoryReset ||
		(motion && (guidance.requiresHistoryReset || meta.requiresHistoryReset)) ||
		(motion && !impl.state.MotionPairMatches(context.frameId, context.previousCaptureFrameId)) ||
		motion != impl.lastMotion || meta.validRegion != impl.region;
	// Duplicate draws leave both ping-pong indices and EMA time untouched.
	if (!reset && impl.lastInputRevision == context.inputRevision &&
		impl.state.Duplicate(context.frameId, context.inputHistoryRevision)) return true;
	const bool metadataValid = meta.valid && meta.frameId == context.frameId &&
		meta.sourceExtent == impl.extent && meta.validRegion.IsInside(impl.extent);
	const float weight = impl.state.Weight(context.frameId, context.inputHistoryRevision,
		meta.resourceGeneration, meta.timestamp100ns, reset || !metadataValid, impl.timeMs);
	if (motion) {
		const auto sync = meta.sync;
		if (sync.fence && sync.value && FAILED(impl.dc->Wait(sync.fence, sync.value))) return false;
		if (impl.motionTexture != guidance.motion.texture) {
			impl.motion = nullptr;
			impl.motionTexture = nullptr;
			if (FAILED(impl.device->CreateShaderResourceView(guidance.motion.texture, nullptr, impl.motion.put()))) return false;
			impl.motionTexture = guidance.motion.texture;
		}
	}
	const FrameGuidanceRegion region = metadataValid ? meta.validRegion : FrameGuidanceRegion::Full(impl.extent);
	struct Constants {
		uint32_t width, height, motion, hdr;
		float weight; uint32_t route, lowWidth, lowHeight;
		uint32_t left, top, right, bottom;
		float chromaStrength; uint32_t enforceZero, padding0, padding1;
	} constants{impl.extent.width, impl.extent.height, motion ? 1u : 0u, impl.hdr ? 1u : 0u,
		weight, static_cast<uint32_t>(impl.mode), (impl.extent.width+1)/2, (impl.extent.height+1)/2,
		region.x, region.y, region.x+region.width, region.y+region.height,
		impl.chromaStrength, impl.enforceZero ? 1u : 0u, 0, 0};
	static_assert(sizeof(constants) == 64);
	impl.dc->UpdateSubresource(impl.constants.get(), 0, nullptr, &constants, 0, 0);
	const auto next = impl.next, previous = next ^ 1u;
	ID3D11ShaderResourceView* srvs[]{impl.inputs[0].get(), impl.inputs[1].get(), impl.inputs[2].get(),
		impl.history[previous].get(), impl.guide[previous].get(), motion ? impl.motion.get() : impl.zeroMotion.get(),
		impl.mode == 4 ? impl.low[0].get() : impl.history[previous].get(),
		impl.mode == 4 ? impl.low[1].get() : impl.guide[previous].get()};
	ID3D11UnorderedAccessView* uavs[]{impl.output.get(), impl.historyOut[next].get(), impl.guideOut[next].get()};
	ID3D11Buffer* cb = impl.constants.get(); ID3D11SamplerState* sampler = impl.sampler.get();
	impl.dc->CSSetConstantBuffers(0, 1, &cb);
	if (impl.mode == 4) {
		ID3D11UnorderedAccessView* lowUavs[]{impl.lowOut[0].get(), impl.lowOut[1].get()};
		impl.dc->CSSetShader(impl.reduceShader.get(), nullptr, 0);
		impl.dc->CSSetShaderResources(0, 3, srvs);
		impl.dc->CSSetUnorderedAccessViews(0, 2, lowUavs, nullptr);
		impl.dc->Dispatch((constants.lowWidth+7)/8, (constants.lowHeight+7)/8, 1);
		ID3D11UnorderedAccessView* nullLow[2]{};
		impl.dc->CSSetUnorderedAccessViews(0, 2, nullLow, nullptr);
	}
	impl.dc->CSSetShader(impl.shader.get(), nullptr, 0);
	impl.dc->CSSetShaderResources(0, ARRAYSIZE(srvs), srvs);
	impl.dc->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);
	impl.dc->CSSetConstantBuffers(0, 1, &cb); impl.dc->CSSetSamplers(0, 1, &sampler);
	impl.dc->Dispatch((impl.extent.width+7)/8, (impl.extent.height+7)/8, 1);
	ID3D11ShaderResourceView* nullSrvs[8]{}; ID3D11UnorderedAccessView* nullUavs[3]{};
	cb = nullptr; sampler = nullptr;
	impl.dc->CSSetShaderResources(0, 8, nullSrvs); impl.dc->CSSetUnorderedAccessViews(0, 3, nullUavs, nullptr);
	impl.dc->CSSetConstantBuffers(0, 1, &cb); impl.dc->CSSetSamplers(0, 1, &sampler);
	impl.dc->CSSetShader(nullptr, nullptr, 0);
	impl.state.Commit(context.frameId, context.inputHistoryRevision, meta.resourceGeneration, meta.timestamp100ns);
	impl.lastInputRevision = context.inputRevision;
	impl.lastMotion = motion; impl.region = meta.validRegion; impl.next ^= 1u;
	return true;
}
}
