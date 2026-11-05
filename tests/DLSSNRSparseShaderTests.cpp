// Executes the production sparse kernels on the CPU WARP rasterizer. No NGX
// session, optical-flow SDK or hardware GPU device is created.
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <string>
#include <vector>
#include <thread>
#include "../src/Magpie.Core/include/DLSSNRSparseState.h"
#include "../src/Magpie.Core/DLSSNRColorShader.h"
#include "../src/Magpie.Core/DLSSNRSparseShader.h"

using Microsoft::WRL::ComPtr;
constexpr UINT N=16, Center=8;
using Pixel=std::array<float,4>;
using Motion=std::array<float,2>;
struct Texture {
	ComPtr<ID3D11Texture2D> texture;
	ComPtr<ID3D11ShaderResourceView> srv;
	ComPtr<ID3D11UnorderedAccessView> uav;
};
struct Constants {
	uint32_t width=N,height=N,mapCount=0,confidenceBits=0;
	uint32_t inferenceMap=0,hasPast=1,finalize=0,reserved=0;
};
static_assert(sizeof(Constants)==32);
void Check(HRESULT hr) {
	if (FAILED(hr)) { std::cerr<<"D3D failure: "<<std::hex<<hr<<'\n'; std::abort(); }
}
void Near(float actual,float expected) { assert(std::isfinite(actual) && std::abs(actual-expected)<2e-5f); }
struct Harness {
	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext> dc;
	ComPtr<ID3D11Buffer> constants,counts,countReadback;
	ComPtr<ID3D11UnorderedAccessView> countUav;
	ComPtr<ID3D11SamplerState> sampler;
	std::map<std::string,ComPtr<ID3D11ComputeShader>> shaders;
	Harness() {
		Check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,
			&device,nullptr,&dc));
		D3D11_BUFFER_DESC cb{}; cb.ByteWidth=sizeof(Constants); cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
		Check(device->CreateBuffer(&cb,nullptr,&constants));
		D3D11_BUFFER_DESC raw{}; raw.ByteWidth=16; raw.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
		raw.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
		Check(device->CreateBuffer(&raw,nullptr,&counts));
		D3D11_UNORDERED_ACCESS_VIEW_DESC view{}; view.Format=DXGI_FORMAT_R32_TYPELESS;
		view.ViewDimension=D3D11_UAV_DIMENSION_BUFFER; view.Buffer.NumElements=4; view.Buffer.Flags=D3D11_BUFFER_UAV_FLAG_RAW;
		Check(device->CreateUnorderedAccessView(counts.Get(),&view,&countUav));
		raw.BindFlags=raw.MiscFlags=0; raw.Usage=D3D11_USAGE_STAGING; raw.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
		Check(device->CreateBuffer(&raw,nullptr,&countReadback));
		D3D11_SAMPLER_DESC sample{}; sample.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		sample.AddressU=sample.AddressV=sample.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP; sample.MaxLOD=D3D11_FLOAT32_MAX;
		Check(device->CreateSamplerState(&sample,&sampler));
		const std::string source=std::string(Magpie::DLSSNR_COLOR_HLSL)+std::string(Magpie::DLSSNR_SPARSE_HLSL);
		for (const char* entry:{"MapMotion","ComposeResidual","FillResidual"}) {
			ComPtr<ID3DBlob> code,error;
			const auto hr=D3DCompile(source.data(),source.size(),"Sparse production WARP",nullptr,nullptr,
				entry,"cs_5_0",D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&error);
			if (error) std::cerr.write(static_cast<const char*>(error->GetBufferPointer()),error->GetBufferSize());
			Check(hr);
			Check(device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&shaders[entry]));
		}
	}
	Texture Make(DXGI_FORMAT format,const void* data,UINT stride) {
		Texture result; D3D11_TEXTURE2D_DESC desc{};
		desc.Width=desc.Height=N; desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
		desc.Format=format; desc.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
		D3D11_SUBRESOURCE_DATA initial{data,N*stride,0};
		Check(device->CreateTexture2D(&desc,data?&initial:nullptr,&result.texture));
		Check(device->CreateShaderResourceView(result.texture.Get(),nullptr,&result.srv));
		Check(device->CreateUnorderedAccessView(result.texture.Get(),nullptr,&result.uav));
		return result;
	}
	Texture Color(float value) {
		std::vector<Pixel> data(N*N,Pixel{value,value,value,1});
		return Make(DXGI_FORMAT_R32G32B32A32_FLOAT,data.data(),sizeof(Pixel));
	}
	Texture Confidence(uint8_t value) {
		std::vector<uint8_t> data(N*N,value);
		return Make(DXGI_FORMAT_R8_UNORM,data.data(),1);
	}
	void Run(const char* entry,const Constants& values,const std::array<ID3D11ShaderResourceView*,23>& inputs,
		Texture& color,Texture& validity,Texture* motion=nullptr,Texture* confidence=nullptr) {
		dc->UpdateSubresource(constants.Get(),0,nullptr,&values,0,0);
		auto* cb=constants.Get(); auto* sample=sampler.Get();
		ID3D11UnorderedAccessView* outputs[5]{color.uav.Get(),validity.uav.Get(),countUav.Get(),
			motion?motion->uav.Get():nullptr,confidence?confidence->uav.Get():nullptr};
		dc->CSSetShader(shaders.at(entry).Get(),nullptr,0);
		dc->CSSetConstantBuffers(0,1,&cb); dc->CSSetSamplers(0,1,&sample);
		dc->CSSetShaderResources(0,23,inputs.data()); dc->CSSetUnorderedAccessViews(0,5,outputs,nullptr);
		dc->Dispatch(N/8,N/8,1);
		ID3D11ShaderResourceView* empty[23]{}; ID3D11UnorderedAccessView* emptyOutputs[5]{};
		dc->CSSetShaderResources(0,23,empty); dc->CSSetUnorderedAccessViews(0,5,emptyOutputs,nullptr);
	}
	void ClearCounts() { const UINT zero[4]{}; dc->ClearUnorderedAccessViewUint(countUav.Get(),zero); }
	std::array<uint32_t,4> Counts() {
		dc->CopyResource(countReadback.Get(),counts.Get());
		D3D11_MAPPED_SUBRESOURCE mapped{}; Check(dc->Map(countReadback.Get(),0,D3D11_MAP_READ,0,&mapped));
		std::array<uint32_t,4> result{}; std::memcpy(result.data(),mapped.pData,sizeof(result));
		dc->Unmap(countReadback.Get(),0); return result;
	}
	template<class T> std::vector<T> Read(Texture& texture) {
		D3D11_TEXTURE2D_DESC desc{}; texture.texture->GetDesc(&desc);
		desc.BindFlags=desc.MiscFlags=0; desc.Usage=D3D11_USAGE_STAGING; desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
		ComPtr<ID3D11Texture2D> staging; Check(device->CreateTexture2D(&desc,nullptr,&staging));
		dc->CopyResource(staging.Get(),texture.texture.Get());
		D3D11_MAPPED_SUBRESOURCE mapped{}; Check(dc->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped));
		std::vector<T> result(N*N);
		for (UINT y=0;y<N;++y) std::memcpy(result.data()+N*y,static_cast<const char*>(mapped.pData)+mapped.RowPitch*y,N*sizeof(T));
		dc->Unmap(staging.Get(),0); return result;
	}
};
struct Fixture {
	Harness& h;
	Texture target,reference,residual,motion,confidence,raw,rawValidity,output,validity;
	Constants settings;
	std::array<ID3D11ShaderResourceView*,23> inputs{};
	Fixture(Harness& harness):h(harness),target(h.Color(.3f)),reference(h.Color(.3f)),residual(h.Color(.04f)),
		raw(h.Color(0)),rawValidity(h.Confidence(0)),output(h.Color(0)),validity(h.Confidence(0)) {
		std::vector<Motion> zero(N*N,Motion{});
		motion=h.Make(DXGI_FORMAT_R32G32_FLOAT,zero.data(),sizeof(Motion)); confidence=h.Confidence(255);
	}
	void Compose() {
		inputs[0]=target.srv.Get(); inputs[1]=reference.srv.Get(); inputs[2]=residual.srv.Get();
		inputs[3]=motion.srv.Get(); inputs[4]=confidence.srv.Get();
		h.ClearCounts(); h.Run("ComposeResidual",settings,inputs,raw,rawValidity);
		const auto mask=h.Read<uint8_t>(rawValidity); const auto counts=h.Counts();
		unsigned contributing=0; for (auto value:mask) contributing+=value>0;
		assert(counts[0]==contributing); // observation counts match the stored R8 mask exactly.
	}
	void Fill() {
		inputs[21]=raw.srv.Get(); inputs[22]=rawValidity.srv.Get();
		h.Run("FillResidual",settings,inputs,output,validity);
	}
};
// Mirrors the production counter copy and fence: CPU inspection is permitted
// only once the existing D3D11 fence confirms completion, always with DO_NOT_WAIT.
struct AsyncContribution {
	ComPtr<ID3D11Device5> device;
	ComPtr<ID3D11DeviceContext4> dc;
	ComPtr<ID3D11Fence> fence;
	ComPtr<ID3D11Buffer> staging;
	Magpie::DLSSNRSparseObservationClock clock;
	Magpie::DLSSNRSparseObservationClock::Result result;
	uint64_t completion=0;
	AsyncContribution(Harness& h) {
		Check(h.device.As(&device)); Check(h.dc.As(&dc));
		Check(device->CreateFence(0,D3D11_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));
		D3D11_BUFFER_DESC desc{}; desc.ByteWidth=16; desc.Usage=D3D11_USAGE_STAGING;
		desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
		Check(device->CreateBuffer(&desc,nullptr,&staging));
	}
	void Begin(Harness& h,uint64_t target) {
		assert(result.Begin(1,1,1,target,++completion,1000000+static_cast<int64_t>(target-1)*166667));
		dc->CopyResource(staging.Get(),h.counts.Get()); Check(dc->Signal(fence.Get(),completion)); dc->Flush();
	}
	bool Confirm() {
		while (fence->GetCompletedValue()<completion) std::this_thread::yield();
		D3D11_MAPPED_SUBRESOURCE mapped{};
		Check(dc->Map(staging.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped));
		const bool contributes=*static_cast<const uint32_t*>(mapped.pData)!=0;
		dc->Unmap(staging.Get(),0);
		return clock.Confirm(result,1,1,1,1,contributes);
	}
};
int main() {
	Harness h;
	{
		Fixture f(h); f.Compose(); f.Fill();
		assert(h.Counts()[0]==N*N);
		Near(h.Read<Pixel>(f.output)[Center*N+Center][0],.34f);
		// Just below one as float, but exactly one after production R8 storage.
		f.reference=h.Color(.3f-.0369f); f.Compose(); f.Fill();
		assert(h.Counts()[0]==N*N && h.Read<uint8_t>(f.rawValidity)[0]==255);
		f.reference=h.Color(.3f-.038f); f.Compose(); f.Fill();
		assert(h.Counts()[0]==N*N && h.Read<uint8_t>(f.rawValidity)[0]==254);
		Near(h.Read<Pixel>(f.output)[0][0],.3f+.04f*254/255);
	}
	{
		Fixture f(h); f.confidence=h.Confidence(128); f.Compose(); f.Fill();
		assert(h.Counts()[0]==N*N);
		Near(h.Read<Pixel>(f.output)[0][0],.3f+.04f*128/255);
		assert(h.Read<uint8_t>(f.validity)[0]==128);
	}
	{
		Fixture f(h); std::vector<uint8_t> confidence(N*N,255); confidence[Center*N+Center]=0;
		f.confidence=h.Make(DXGI_FORMAT_R8_UNORM,confidence.data(),1); f.Compose(); f.Fill();
		assert(h.Counts()[0]==N*N-1);
		Near(h.Read<Pixel>(f.output)[Center*N+Center][0],.3f);
		assert(h.Read<uint8_t>(f.validity)[Center*N+Center]==0); // local current raw, no spatial repair.
		Near(h.Read<Pixel>(f.output)[0][0],.34f);
	}
	{
		Fixture f(h); f.confidence=h.Confidence(0); f.residual=h.Color(.2f);
		f.Compose(); f.Fill(); assert(h.Counts()[0]==0);
		for (auto pixel:h.Read<Pixel>(f.output)) Near(pixel[0],.3f);
		AsyncContribution asynchronous(h); asynchronous.Begin(h,1);
		// Keep target 1's zero-support readback pending at the owner while
		// target 2 already contributes, then stop capturing on target 2.
		f.target=h.Color(.35f); f.reference=h.Color(.35f); f.confidence=h.Confidence(255);
		f.Compose(); f.Fill();
		assert(!asynchronous.result.Begin(1,1,1,2,2,1166667));
		assert(!asynchronous.result.NeedsRetry(2));
		assert(!asynchronous.Confirm() && asynchronous.clock.revision==0);
		assert(asynchronous.result.NeedsRetry(2) && asynchronous.result.deferredTimestamp==1166667);
		assert(!asynchronous.result.Begin(1,1,1,1,2)); // redraw cannot retry the sampled zero target.
		// The output cache must bypass hold exactly once for the deferred
		// current target; the same staging buffer is reused after completion.
		f.Compose(); f.Fill(); asynchronous.Begin(h,2);
		assert(asynchronous.Confirm() && asynchronous.clock.revision==1);
		assert(asynchronous.clock.timestamp==1166667);
		for (auto pixel:h.Read<Pixel>(f.output)) Near(pixel[0],.55f);
		assert(!asynchronous.result.Begin(1,1,1,3,3));
		assert(asynchronous.result.source==1 && asynchronous.result.target==2);
		f.settings.hasPast=0; f.Compose(); f.Fill(); assert(h.Counts()[0]==0);
		for (auto pixel:h.Read<Pixel>(f.output)) Near(pixel[0],.35f);
	}
	{
		Fixture f(h); f.confidence=h.Confidence(0); f.Compose(); f.Fill();
		AsyncContribution zero(h); zero.Begin(h,1);
		f.Compose(); f.Fill(); assert(!zero.result.Begin(1,1,1,2,2,1166667));
		assert(!zero.Confirm() && zero.result.NeedsRetry(2));
		f.Compose(); f.Fill(); zero.Begin(h,2);
		assert(!zero.Confirm() && zero.clock.revision==0 && !zero.result.NeedsRetry(2));
		assert(!zero.result.Begin(1,1,1,2,3,1999999)); // no zero-support idle loop.
	}
	{
		Fixture f(h);
		std::vector<Motion> shifted(N*N,Motion{-2,0});
		f.motion=h.Make(DXGI_FORMAT_R32G32_FLOAT,shifted.data(),sizeof(Motion)); f.Compose(); f.Fill();
		const auto mask=h.Read<uint8_t>(f.rawValidity);
		const auto pixels=h.Read<Pixel>(f.output);
		for (UINT y=0;y<N;++y) for (UINT x=0;x<N;++x) {
			assert(mask[y*N+x]==(x<2?0:255));
			Near(pixels[y*N+x][0],x<2?.3f:.34f);
		}
		assert(h.Counts()[0]==N*(N-2));
		// Nonfinite old content or mapping cannot leak a correction into output.
		f.motion=h.Make(DXGI_FORMAT_R32G32_FLOAT,std::vector<Motion>(N*N,Motion{}).data(),sizeof(Motion));
		f.residual=h.Color(std::numeric_limits<float>::quiet_NaN()); f.Compose(); f.Fill();
		assert(h.Counts()[0]==0);
		for (auto pixel:h.Read<Pixel>(f.output)) Near(pixel[0],.3f);
		f.residual=h.Color(.04f); f.reference=h.Color(std::numeric_limits<float>::quiet_NaN()); f.Compose(); f.Fill();
		assert(h.Counts()[0]==0);
		for (auto pixel:h.Read<Pixel>(f.output)) Near(pixel[0],.3f);
		f.reference=h.Color(.3f);
		shifted.assign(N*N,Motion{std::numeric_limits<float>::infinity(),0});
		f.motion=h.Make(DXGI_FORMAT_R32G32_FLOAT,shifted.data(),sizeof(Motion)); f.Compose(); f.Fill();
		assert(h.Counts()[0]==0);
		for (auto pixel:h.Read<Pixel>(f.output)) Near(pixel[0],.3f);
	}
	{
		Fixture f(h); Constants settings; settings.mapCount=1; settings.confidenceBits=1;
		std::vector<Motion> motion(N*N,Motion{std::numeric_limits<float>::infinity(),0});
		auto step=h.Make(DXGI_FORMAT_R32G32_FLOAT,motion.data(),sizeof(Motion));
		auto mapped=h.Make(DXGI_FORMAT_R32G32_FLOAT,nullptr,sizeof(Motion)),confidence=h.Confidence(255);
		std::array<ID3D11ShaderResourceView*,23> inputs{}; inputs[5]=step.srv.Get(); inputs[13]=f.confidence.srv.Get();
		h.Run("MapMotion",settings,inputs,f.raw,f.rawValidity,&mapped,&confidence);
		for (auto value:h.Read<uint8_t>(confidence)) assert(value==0);
		for (auto value:h.Read<Motion>(mapped)) { Near(value[0],0); Near(value[1],0); }
	}
	std::cout<<"Sparse production WARP passed: current target/local raw, R8 support, async zero-to-real contribution, bounded source clock and nonfinite/bounds.\n";
}
