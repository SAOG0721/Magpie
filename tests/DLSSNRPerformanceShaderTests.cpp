// Runs production kernels on WARP without NGX. Actual queue overlap and NR
// latency still require the separate hardware acceptance session.
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <map>
#include <string>
#include <source_location>
#include <vector>
#include "DLSSNRPerformanceShader.h"

using Microsoft::WRL::ComPtr;
constexpr UINT N = 8;
using Pixel = std::array<float, 4>;
void Check(HRESULT hr, std::source_location source=std::source_location::current()) { if (FAILED(hr)) { std::cerr << std::hex << hr << " at " << source.function_name() << ":" << std::dec << source.line() << '\n'; std::abort(); } }
struct Texture {
	ComPtr<ID3D11Texture2D> texture;
	ComPtr<ID3D11ShaderResourceView> srv;
	ComPtr<ID3D11UnorderedAccessView> uav;
};
struct Constants {
	uint32_t width=N, height=N, sourceWidth=N, sourceHeight=N;
	uint32_t age=0, reproject=0, hasHistory=1, processingMode=1;
	uint32_t confidenceBits=0, reserved[3]{};
};
static_assert(sizeof(Constants)==48);
struct Harness {
	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext> dc;
	ComPtr<ID3D11Buffer> constants;
	ComPtr<ID3D11Buffer> invalid, invalidReadback;
	ComPtr<ID3D11UnorderedAccessView> invalidUav;
	ComPtr<ID3D11SamplerState> sampler;
	std::map<std::string, ComPtr<ID3D11ComputeShader>> shaders;
	Harness() {
		Check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,
			D3D11_SDK_VERSION,&device,nullptr,&dc));
		D3D11_BUFFER_DESC cb{}; cb.ByteWidth=sizeof(Constants); cb.Usage=D3D11_USAGE_DEFAULT;
		cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
		Check(device->CreateBuffer(&cb,nullptr,&constants));
		D3D11_BUFFER_DESC raw{}; raw.ByteWidth=sizeof(uint32_t)*4; raw.Usage=D3D11_USAGE_DEFAULT;
		raw.BindFlags=D3D11_BIND_UNORDERED_ACCESS; raw.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
		Check(device->CreateBuffer(&raw,nullptr,&invalid));
		D3D11_UNORDERED_ACCESS_VIEW_DESC view{}; view.Format=DXGI_FORMAT_R32_TYPELESS;
		view.ViewDimension=D3D11_UAV_DIMENSION_BUFFER; view.Buffer.NumElements=4;
		view.Buffer.Flags=D3D11_BUFFER_UAV_FLAG_RAW;
		Check(device->CreateUnorderedAccessView(invalid.Get(),&view,&invalidUav));
		raw.Usage=D3D11_USAGE_STAGING; raw.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
		raw.BindFlags=raw.MiscFlags=0;
		Check(device->CreateBuffer(&raw,nullptr,&invalidReadback));
		D3D11_SAMPLER_DESC sd{}; sd.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP; sd.MaxLOD=D3D11_FLOAT32_MAX;
		Check(device->CreateSamplerState(&sd,&sampler));
		for (const char* entry : {"PublishEnhancement","CheckReuse","ComposeCurrent","ComposeMotion","MeasureCoverage"}) {
			ComPtr<ID3DBlob> code,error;
			const auto source=Magpie::DLSSNR_PERFORMANCE_HLSL;
			const auto hr=D3DCompile(source.data(),source.size(),"production",nullptr,nullptr,entry,
				"cs_5_0",D3DCOMPILE_ENABLE_STRICTNESS,0,&code,&error);
			if (error) std::cerr << static_cast<const char*>(error->GetBufferPointer());
			Check(hr);
			Check(device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&shaders[entry]));
		}
	}
	Texture Make(DXGI_FORMAT format, const void* data, UINT stride) {
		Texture t; D3D11_TEXTURE2D_DESC d{};
		d.Width=d.Height=N; d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
		d.Format=format; d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
		D3D11_SUBRESOURCE_DATA init{data,N*stride,0};
		Check(device->CreateTexture2D(&d,data?&init:nullptr,&t.texture));
		Check(device->CreateShaderResourceView(t.texture.Get(),nullptr,&t.srv));
		Check(device->CreateUnorderedAccessView(t.texture.Get(),nullptr,&t.uav));
		return t;
	}
	Texture Color(float value) {
		std::vector<Pixel> data(N*N,Pixel{value,value,value,1});
		return Make(DXGI_FORMAT_R32G32B32A32_FLOAT,data.data(),sizeof(Pixel));
	}
	void Run(const char* entry, Constants values, const std::array<ID3D11ShaderResourceView*,12>& inputs,
		Texture& output, Texture& validity, Texture* motion = nullptr) {
		dc->UpdateSubresource(constants.Get(),0,nullptr,&values,0,0);
		auto* cb=constants.Get(); auto* sample=sampler.Get();
		ID3D11UnorderedAccessView* uavs[4]{output.uav.Get(),validity.uav.Get(),invalidUav.Get(),
			motion ? motion->uav.Get() : nullptr};
		dc->CSSetShader(shaders.at(entry).Get(),nullptr,0);
		dc->CSSetConstantBuffers(0,1,&cb); dc->CSSetSamplers(0,1,&sample);
		dc->CSSetShaderResources(0,UINT(inputs.size()),inputs.data());
		dc->CSSetUnorderedAccessViews(0,4,uavs,nullptr); dc->Dispatch(1,1,1);
		ID3D11ShaderResourceView* empty[12]{}; ID3D11UnorderedAccessView* emptyUav[4]{};
		dc->CSSetShaderResources(0,12,empty); dc->CSSetUnorderedAccessViews(0,4,emptyUav,nullptr);
	}
	bool NeedsRecovery(Constants values, const std::array<ID3D11ShaderResourceView*,12>& inputs,
		Texture& output, Texture& validity) {
		const UINT zero[4]{}; dc->ClearUnorderedAccessViewUint(invalidUav.Get(),zero);
		Run("CheckReuse",values,inputs,output,validity);
		dc->CopyResource(invalidReadback.Get(),invalid.Get());
		D3D11_MAPPED_SUBRESOURCE mapped{};
		Check(dc->Map(invalidReadback.Get(),0,D3D11_MAP_READ,0,&mapped));
		const bool needed=*static_cast<const uint32_t*>(mapped.pData)!=0;
		dc->Unmap(invalidReadback.Get(),0); return needed;
	}
	std::array<uint32_t,4> Coverage(Constants values, const std::array<ID3D11ShaderResourceView*,12>& inputs,
		Texture& output, Texture& validity) {
		const UINT zero[4]{}; dc->ClearUnorderedAccessViewUint(invalidUav.Get(),zero);
		Run("MeasureCoverage",values,inputs,output,validity);
		dc->CopyResource(invalidReadback.Get(),invalid.Get());
		D3D11_MAPPED_SUBRESOURCE mapped{};
		Check(dc->Map(invalidReadback.Get(),0,D3D11_MAP_READ,0,&mapped));
		std::array<uint32_t,4> result{}; std::memcpy(result.data(),mapped.pData,sizeof(result));
		dc->Unmap(invalidReadback.Get(),0); return result;
	}
	Pixel Read(Texture& t, UINT x=4, UINT y=4) {
		D3D11_TEXTURE2D_DESC d{}; t.texture->GetDesc(&d);
		d.Usage=D3D11_USAGE_STAGING; d.CPUAccessFlags=D3D11_CPU_ACCESS_READ; d.BindFlags=0;
		ComPtr<ID3D11Texture2D> staging; Check(device->CreateTexture2D(&d,nullptr,&staging));
		dc->CopyResource(staging.Get(),t.texture.Get());
		D3D11_MAPPED_SUBRESOURCE mapped{}; Check(dc->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped));
		const size_t stride=d.Format==DXGI_FORMAT_R32_FLOAT ? sizeof(float) :
			d.Format==DXGI_FORMAT_R32G32_FLOAT ? sizeof(float)*2 : sizeof(Pixel);
		Pixel result{}; std::memcpy(result.data(),static_cast<const char*>(mapped.pData)+y*mapped.RowPitch+x*stride,stride);
		dc->Unmap(staging.Get(),0); return result;
	}
};
int main() {
	Harness h;
	auto original=h.Color(.2f),full=h.Color(.6f),enhancement=h.Color(0),output=h.Color(0);
	auto wrongReference=h.Color(.9f);
	auto validity=h.Make(DXGI_FORMAT_R32_FLOAT,nullptr,sizeof(float));
	std::array<ID3D11ShaderResourceView*,12> inputs{};
	inputs[0]=original.srv.Get();inputs[11]=full.srv.Get();
	Constants p;
	h.Run("PublishEnhancement",p,inputs,enhancement,validity);
	assert(std::abs(h.Read(enhancement)[0]-.4f)<1e-5f);
	inputs[1]=enhancement.srv.Get();inputs[2]=original.srv.Get();
	for (uint32_t mode : {1u,2u}) {
		p.processingMode=mode;
		h.Run("ComposeCurrent",p,inputs,output,validity);
		assert(std::abs(h.Read(output)[0]-.6f)<1e-5f && !h.NeedsRecovery(p,inputs,output,validity));
		p.hasHistory=0;
		h.Run("ComposeCurrent",p,inputs,output,validity);
		assert(std::abs(h.Read(output)[0]-.2f)<1e-5f);
		p.hasHistory=1;
	}
	std::vector<std::array<float,2>> stationary(N*N,{0,0}),translated(N*N,{-1,0});
	auto zeroMotion=h.Make(DXGI_FORMAT_R32G32_FLOAT,stationary.data(),sizeof(stationary[0]));
	auto motion=h.Make(DXGI_FORMAT_R32G32_FLOAT,translated.data(),sizeof(translated[0]));
	auto composed=h.Make(DXGI_FORMAT_R32G32_FLOAT,nullptr,sizeof(stationary[0]));
	for (unsigned i=0;i<4;++i) inputs[3+i]=zeroMotion.srv.Get();
	// Conservative provider confidence remains motion evidence rather than NR strength.
	for (float confidence : {.65f,.5f}) {
		std::vector<float> data(N*N,confidence);
		auto field=h.Make(DXGI_FORMAT_R32_FLOAT,data.data(),sizeof(float));
		for (unsigned i=0;i<4;++i) inputs[7+i]=field.srv.Get();
		p.reproject=1;p.confidenceBits=15;
		for (uint32_t mode : {1u,2u}) for (uint32_t age : {0u,1u,2u,3u}) {
			p.processingMode=mode;p.age=age;
			h.Run("ComposeCurrent",p,inputs,output,validity);
			assert(std::abs(h.Read(output)[0]-.6f)<1e-5f && !h.NeedsRecovery(p,inputs,output,validity));
		}
		// K4's next full observation is four source transitions away. The fourth
		// field is used for NGX guidance, while residual reuse remains at most age3.
		p.age=4;
		h.Run("ComposeMotion",p,inputs,output,validity,&composed);
		assert(std::abs(h.Read(validity)[0]-std::pow(confidence,4.f))<1e-5f);
		assert(std::abs(h.Read(composed)[0])<1e-5f);
	}
	// The per-confidence fixtures have retired; no stale SRV pointer may be
	// rebound even when the next shader configuration disables confidence.
	for (unsigned i=0;i<4;++i) inputs[7+i]=nullptr;
	// Every older motion field is sampled at the already displaced coordinate.
	{
		std::vector<Pixel> residuals(N*N,Pixel{0,0,0,0});
		std::vector<std::array<float,2>> older(N*N,{0,0});
		for (UINT y=0;y<N;++y) { residuals[y*N+2]={.4f,.4f,.4f,0};older[y*N+4]={-2,0}; }
		auto history=h.Make(DXGI_FORMAT_R32G32B32A32_FLOAT,residuals.data(),sizeof(Pixel));
		auto previous=h.Make(DXGI_FORMAT_R32G32_FLOAT,older.data(),sizeof(older[0]));
		inputs[1]=history.srv.Get();inputs[3]=motion.srv.Get();inputs[4]=previous.srv.Get();
		p=Constants{};p.reproject=1;p.age=2;
		h.Run("ComposeCurrent",p,inputs,output,validity);
		assert(std::abs(h.Read(output,5,4)[0]-.6f)<1e-5f);
		p.reproject=0;
		h.Run("ComposeCurrent",p,inputs,output,validity);
		assert(std::abs(h.Read(output,5,4)[0]-.2f)<1e-5f);
	}
	// A translated feature follows three accepted captures and slight lighting
	// changes, retaining immutable full NR source pixels at the fourth target.
	{
		std::vector<Pixel> referencePixels(N*N),fullPixels(N*N);
		std::vector<float> confidences(N*N,.65f);
		for (UINT y=0;y<N;++y) for (UINT x=0;x<N;++x) {
			const float color=.1f+.04f*x,residual=.1f+.02f*x;
			referencePixels[y*N+x]={color,color,color,1};
			fullPixels[y*N+x]={color+residual,color+residual,color+residual,1};
		}
		auto reference=h.Make(DXGI_FORMAT_R32G32B32A32_FLOAT,referencePixels.data(),sizeof(Pixel));
		auto result=h.Make(DXGI_FORMAT_R32G32B32A32_FLOAT,fullPixels.data(),sizeof(Pixel));
		auto field=h.Make(DXGI_FORMAT_R32_FLOAT,confidences.data(),sizeof(float));
		auto residual=h.Color(0);
		std::array<ID3D11ShaderResourceView*,12> moving{};
		moving[0]=moving[2]=reference.srv.Get();moving[11]=result.srv.Get();
		for (unsigned i=0;i<4;++i) { moving[3+i]=motion.srv.Get();moving[7+i]=field.srv.Get(); }
		Constants settings;settings.reproject=1;settings.confidenceBits=15;
		h.Run("PublishEnhancement",settings,moving,residual,validity);
		moving[1]=residual.srv.Get();
		for (uint32_t mode : {1u,2u}) for (uint32_t age : {0u,1u,2u,3u}) {
			std::vector<Pixel> currentPixels(N*N);
			for (UINT y=0;y<N;++y) for (UINT x=0;x<N;++x) {
				const float color=.1f+.04f*(x>=age ? x-age : 0)+.01f*age;
				currentPixels[y*N+x]={color,color,color,1};
			}
			auto current=h.Make(DXGI_FORMAT_R32G32B32A32_FLOAT,currentPixels.data(),sizeof(Pixel));
			moving[0]=current.srv.Get();settings.processingMode=mode;settings.age=age;
			h.Run("ComposeCurrent",settings,moving,output,validity);
			assert(std::abs(h.Read(output,4+age,4)[0]-(.44f+.01f*age))<1e-5f);
			assert(std::abs(h.Read(validity,4+age,4)[0]-1)<1e-5f);
			if (age) assert(std::abs(h.Read(output,0,4)[0]-currentPixels[4*N][0])<1e-5f);
		}
	}
	// Weak finite evidence retains only bounded low-frequency correction. Soft
	// rejection remains local in both modes and never changes the NR cadence.
	for (uint32_t mode : {1u,2u}) {
		std::vector<Pixel> pixels(N*N);
		std::vector<float> weak(N*N,.05f);
		for (UINT y=0;y<N;++y) for (UINT x=0;x<N;++x) {
			const float value=.04f+((x+y)%2 ? -.02f : .02f);pixels[y*N+x]={value,value,value,0};
		}
		auto residual=h.Make(DXGI_FORMAT_R32G32B32A32_FLOAT,pixels.data(),sizeof(Pixel));
		auto confidence=h.Make(DXGI_FORMAT_R32_FLOAT,weak.data(),sizeof(float));
		inputs={};inputs[0]=inputs[2]=original.srv.Get();inputs[1]=residual.srv.Get();
		for (unsigned i=0;i<4;++i) { inputs[3+i]=zeroMotion.srv.Get();inputs[7+i]=confidence.srv.Get(); }
		p=Constants{};p.processingMode=mode;p.age=1;p.reproject=1;p.confidenceBits=15;
		h.Run("ComposeCurrent",p,inputs,output,validity);
		assert(std::abs(h.Read(output)[0]-.22f)<1e-5f && h.Read(validity)[0] == 0);
		assert(h.NeedsRecovery(p,inputs,output,validity));
		auto current=h.Color(.21f);inputs[0]=current.srv.Get();
		h.Run("ComposeCurrent",p,inputs,output,validity);
		assert(std::abs(h.Read(output)[0]-.23f)<1e-5f);
		inputs[0]=original.srv.Get();
		for (float value : {.3f,.65f}) {
			std::fill(weak.begin(),weak.end(),value);
			auto partial=h.Make(DXGI_FORMAT_R32_FLOAT,weak.data(),sizeof(float));inputs[7]=partial.srv.Get();
			h.Run("ComposeCurrent",p,inputs,output,validity);
			assert(std::abs(h.Read(output)[0]-(value==.3f ? .24f : .26f))<1e-5f);
		}
		inputs[7]=confidence.srv.Get();
		for (UINT y=0;y<N;++y) for (UINT x=0;x<N;++x) {
			const float value=(x+y)%2 ? -.02f : .02f;pixels[y*N+x]={value,value,value,0};
		}
		auto high=h.Make(DXGI_FORMAT_R32G32B32A32_FLOAT,pixels.data(),sizeof(Pixel));inputs[1]=high.srv.Get();
		h.Run("ComposeCurrent",p,inputs,output,validity);assert(std::abs(h.Read(output)[0]-.2f)<1e-5f);
		auto oversized=h.Color(10);inputs[1]=oversized.srv.Get();
		h.Run("ComposeCurrent",p,inputs,output,validity);assert(std::abs(h.Read(output)[0]-.24f)<1e-5f);
		inputs[1]=residual.srv.Get();inputs[2]=wrongReference.srv.Get();
		h.Run("ComposeCurrent",p,inputs,output,validity);assert(std::abs(h.Read(output)[0]-.2f)<1e-5f);
		inputs[2]=original.srv.Get();p.age=0;
		h.Run("ComposeCurrent",p,inputs,output,validity);assert(std::abs(h.Read(output)[0]-.26f)<1e-5f);
		auto invalid=h.Color(std::numeric_limits<float>::quiet_NaN());inputs[1]=invalid.srv.Get();
		for (uint32_t age : {0u,1u,3u}) {
			p.age=age;h.Run("ComposeCurrent",p,inputs,output,validity);
			assert(std::abs(h.Read(output)[0]-.2f)<1e-5f && h.NeedsRecovery(p,inputs,output,validity));
		}
		inputs[1]=residual.srv.Get();p.age=1;
		for (float value : {-100.f,std::numeric_limits<float>::quiet_NaN()}) {
			std::vector<std::array<float,2>> invalidMotion(N*N,{value,0});
			auto field=h.Make(DXGI_FORMAT_R32G32_FLOAT,invalidMotion.data(),sizeof(invalidMotion[0]));inputs[3]=field.srv.Get();
			h.Run("ComposeCurrent",p,inputs,output,validity);assert(std::abs(h.Read(output)[0]-.2f)<1e-5f);
		}
		inputs[3]=zeroMotion.srv.Get();
		const auto counts=h.Coverage(p,inputs,output,validity);
		assert((counts==std::array<uint32_t,4>{1,1,0,0}));
	}
	std::cout << "Production kernels: Alternating/Pipeline protected continuous reuse, age3 transfer, four-step NGX guidance and hard rejection passed.\n";
}
