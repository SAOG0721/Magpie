#define NOMINMAX
#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <DirectXPackedVector.h>
#include <array>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>
#include <vector>
#include <fstream>
#include <string>
#include "DLSSNRTemporalShader.h"
#include "DLSSNRTemporalState.h"
using Microsoft::WRL::ComPtr;
using namespace Magpie;
constexpr unsigned W = 16, H = 16;
using Pixel = std::array<float,4>;
using Image = std::vector<Pixel>;
void Check(HRESULT hr) { if (FAILED(hr)) { std::cerr << "HRESULT " << std::hex << hr << '\n'; std::abort(); } }
struct Texture {
	ComPtr<ID3D11Texture2D> texture;
	ComPtr<ID3D11ShaderResourceView> srv;
	ComPtr<ID3D11UnorderedAccessView> uav;
};
struct Harness {
	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext> dc;
	ComPtr<ID3D11InfoQueue> debug;
	ComPtr<ID3D11ComputeShader> shader;
	ComPtr<ID3D11ComputeShader> reduceShader;
	ComPtr<ID3D11SamplerState> sampler;
	ComPtr<ID3D11Buffer> cb;
	std::array<Texture,6> inputs;
	std::array<Texture,3> outputs;
	std::array<Texture,2> low;
	unsigned width, height;
	struct Constants {
		unsigned w = W, h = H, motion = 0, hdr = 0;
		float weight = .8f; unsigned route = 1, lowWidth = (W+1)/2, lowHeight = (H+1)/2;
		unsigned left = 0, top = 0, right = W, bottom = H;
		float chromaStrength = 0; unsigned enforceZero = 0, padding0 = 0, padding1 = 0;
	} constants;
	Harness(unsigned w=W, unsigned h=H) : width(w), height(h) {
		auto create = D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,D3D11_CREATE_DEVICE_DEBUG,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&dc);
		if (create == DXGI_ERROR_SDK_COMPONENT_MISSING)
			create = D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&dc);
		Check(create);
		device.As(&debug);
		ComPtr<ID3DBlob> blob, errors;
		auto hr = D3DCompile(DLSSNR_TEMPORAL_SHADER.data(),DLSSNR_TEMPORAL_SHADER.size(),"DLSSNRTemporal",nullptr,nullptr,
			"main","cs_5_0",D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_WARNINGS_ARE_ERRORS|D3DCOMPILE_ALL_RESOURCES_BOUND|D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&blob,&errors);
		if (errors) std::cerr << static_cast<const char*>(errors->GetBufferPointer());
		Check(hr);
		Check(device->CreateComputeShader(blob->GetBufferPointer(),blob->GetBufferSize(),nullptr,&shader));
		blob.Reset(); errors.Reset();
		hr = D3DCompile(DLSSNR_TEMPORAL_REDUCE_SHADER.data(),DLSSNR_TEMPORAL_REDUCE_SHADER.size(),"DLSSNRTemporalReduce",nullptr,nullptr,
			"main","cs_5_0",D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_WARNINGS_ARE_ERRORS|D3DCOMPILE_ALL_RESOURCES_BOUND|D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&blob,&errors);
		if (errors) std::cerr << static_cast<const char*>(errors->GetBufferPointer());
		Check(hr);
		Check(device->CreateComputeShader(blob->GetBufferPointer(),blob->GetBufferSize(),nullptr,&reduceShader));
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width=width; desc.Height=height; desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
		desc.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;
		desc.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
		for (auto* list : {&inputs}) for (auto& t : *list) Create(t,desc);
		Create(outputs[0],desc);
		desc.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;
		Create(outputs[1],desc); Create(outputs[2],desc);
		desc.Width=(width+1)/2; desc.Height=(height+1)/2;
		for (auto& t : low) Create(t,desc);
		D3D11_BUFFER_DESC buffer{}; buffer.ByteWidth=sizeof(constants); buffer.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
		Check(device->CreateBuffer(&buffer,nullptr,&cb));
		D3D11_SAMPLER_DESC s{}; s.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		s.AddressU=s.AddressV=s.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP; s.MaxLOD=D3D11_FLOAT32_MAX;
		Check(device->CreateSamplerState(&s,&sampler));
	}
	void Create(Texture& t,const D3D11_TEXTURE2D_DESC& desc) {
		Check(device->CreateTexture2D(&desc,nullptr,&t.texture));
		Check(device->CreateShaderResourceView(t.texture.Get(),nullptr,&t.srv));
		Check(device->CreateUnorderedAccessView(t.texture.Get(),nullptr,&t.uav));
	}
	void CheckDebug() {
		if (!debug) return;
		for (UINT64 i=0;i<debug->GetNumStoredMessages();++i) {
			SIZE_T bytes=0; Check(debug->GetMessage(i,nullptr,&bytes));
			std::vector<char> storage(bytes);
			auto* message=reinterpret_cast<D3D11_MESSAGE*>(storage.data());
			Check(debug->GetMessage(i,message,&bytes));
			if (message->Severity <= D3D11_MESSAGE_SEVERITY_WARNING) {
				std::cerr<<message->pDescription<<'\n'; std::abort();
			}
		}
		debug->ClearStoredMessages();
	}
	void Set(unsigned i,const Image& image) { assert(image.size()==width*height); dc->UpdateSubresource(inputs[i].texture.Get(),0,nullptr,image.data(),width*sizeof(Pixel),0); }
	void Constant(unsigned i,Pixel p) { Set(i,Image(width*height,p)); }
	void Default() {
		constants={};
		constants.w=constants.right=width; constants.h=constants.bottom=height;
		constants.lowWidth=(width+1)/2; constants.lowHeight=(height+1)/2;
		Constant(0,{.4f,.4f,.4f,1}); Constant(1,{.4f,.4f,.4f,1}); Constant(2,{.5f,.5f,.5f,.7f});
		Constant(3,{.3f,.3f,.3f,1}); Constant(4,{.4f,.4f,.4f,1}); Constant(5,{0,0,0,0});
	}
	Image Run(unsigned result = 0) {
		dc->UpdateSubresource(cb.Get(),0,nullptr,&constants,0,0);
		ID3D11ShaderResourceView* srvs[8]; for (unsigned i=0;i<6;++i) srvs[i]=inputs[i].srv.Get();
		srvs[6]=low[0].srv.Get(); srvs[7]=low[1].srv.Get();
		ID3D11UnorderedAccessView* uavs[3]; for (unsigned i=0;i<3;++i) uavs[i]=outputs[i].uav.Get();
		auto* buffer=cb.Get(); auto* s=sampler.Get();
		dc->CSSetConstantBuffers(0,1,&buffer);
		if (constants.route==4) {
			ID3D11UnorderedAccessView* lowUavs[]{low[0].uav.Get(),low[1].uav.Get()};
			dc->CSSetShader(reduceShader.Get(),nullptr,0); dc->CSSetShaderResources(0,3,srvs);
			dc->CSSetUnorderedAccessViews(0,2,lowUavs,nullptr);
			dc->Dispatch((constants.lowWidth+7)/8,(constants.lowHeight+7)/8,1);
			ID3D11UnorderedAccessView* nu[2]{}; dc->CSSetUnorderedAccessViews(0,2,nu,nullptr);
		}
		dc->CSSetShader(shader.Get(),nullptr,0); dc->CSSetShaderResources(0,8,srvs);
		dc->CSSetUnorderedAccessViews(0,3,uavs,nullptr); dc->CSSetConstantBuffers(0,1,&buffer); dc->CSSetSamplers(0,1,&s);
		dc->Dispatch((width+7)/8,(height+7)/8,1);
		ID3D11ShaderResourceView* ns[8]{}; ID3D11UnorderedAccessView* nu[3]{};
		dc->CSSetShaderResources(0,8,ns); dc->CSSetUnorderedAccessViews(0,3,nu,nullptr);
		auto* texture=result < 3 ? outputs[result].texture.Get() : low[result-3].texture.Get();
		D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
		desc.Usage=D3D11_USAGE_STAGING; desc.BindFlags=0; desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
		ComPtr<ID3D11Texture2D> staging; Check(device->CreateTexture2D(&desc,nullptr,&staging));
		dc->CopyResource(staging.Get(),texture);
		D3D11_MAPPED_SUBRESOURCE mapped{}; Check(dc->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped));
		Image image(desc.Width*desc.Height);
		for (unsigned y=0;y<desc.Height;++y) {
			const auto* row=static_cast<const char*>(mapped.pData)+y*mapped.RowPitch;
			if (desc.Format==DXGI_FORMAT_R16G16B16A16_FLOAT) {
				const auto* half=reinterpret_cast<const DirectX::PackedVector::HALF*>(row);
				for (unsigned x=0;x<desc.Width;++x) for (unsigned c=0;c<4;++c)
					image[y*desc.Width+x][c]=DirectX::PackedVector::XMConvertHalfToFloat(half[x*4+c]);
			} else memcpy(image.data()+desc.Width*y,row,desc.Width*sizeof(Pixel));
		}
		dc->Unmap(staging.Get(),0); return image;
	}
};
void Near(float a,float b,float tolerance=1e-4f) { if (!std::isfinite(a) || std::abs(a-b)>tolerance) { std::cerr<<"Expected "<<b<<", got "<<a<<'\n'; std::abort(); } }
int main(int argc, char** argv) {
	DLSSNRTemporalState state;
	assert(state.Weight(1,0,0,1000000,false)==0);
	state.Commit(1,4,3,1000000);
	assert(state.Duplicate(1,4) && !state.Duplicate(1,5));
	assert(state.Weight(1,4,3,1166667,false)==0);
	assert(state.Weight(2,5,3,1166667,false)==0);
	assert(state.Weight(2,4,4,1166667,false)==0);
	assert(state.Weight(2,4,3,1000000,false)==0);
	assert(state.Weight(2,4,3,4000000,false)==0);
	assert(state.Weight(2,4,3,1166667,true)==0);
	Near(state.Weight(2,4,3,1166667,false),std::exp(-1.f/60/.08f));
	Near(static_cast<float>(std::pow(state.Weight(2,4,3,1166667,false),60)),
		static_cast<float>(std::pow(state.Weight(2,4,3,1333333,false),30)),1e-7f);
	for (float time : {5.f, 80.f, 200.f}) {
		// Equal elapsed time must decay equally at 30/60/120 capture FPS.
		for (int fps : {30, 60, 120}) {
			DLSSNRTemporalState clock;
			clock.Commit(1, 0, 1, 0);
			double weight = 1;
			for (int i = 1; i <= fps; ++i) {
				const int64_t timestamp = std::llround(double(i) * 10000000 / fps);
				weight *= clock.Weight(i + 1, 0, 1, timestamp, false, time);
				clock.Commit(i + 1, 0, 1, timestamp);
			}
			Near(float(weight), float(std::exp(-1000.0 / time)), 1e-7f);
		}
		assert(state.Weight(1,4,3,1166667,false,time) == 0);
		assert(state.Weight(2,4,3,4000000,false,time) == 0);
		assert(state.Weight(2,4,4,1166667,false,time) == 0);
	}
	assert(state.Weight(2,4,3,1166667,false,5) < state.Weight(2,4,3,1166667,false,80));
	assert(state.Weight(2,4,3,1166667,false,80) < state.Weight(2,4,3,1166667,false,200));
	Near(state.Weight(2,4,3,1166667,false,std::numeric_limits<float>::quiet_NaN()),
		state.Weight(2,4,3,1166667,false,80));
	Harness h;
	const unsigned center=8*W+8;
	h.Default(); auto out=h.Run(); Near(out[center][0],.66f); Near(out[center][3],.7f);
	// Stable negative residual and on/off disappearance remain signed and decay.
	h.Constant(3,{-.2f,-.2f,-.2f,1}); h.Constant(2,{.4f,.4f,.4f,.7f});
	Near(h.Run()[center][0],.24f);
	h.constants.enforceZero=1;
	Near(h.Run()[center][0],.4f); Near(h.Run(1)[center][3],0);
	h.constants.enforceZero=0;
	// Additional chroma history retains luma and follows the existing rejection.
	h.Default(); h.Constant(2,{.42f,.38f,.4f,.7f}); h.Constant(3,{-.02f,.02f,0,1});
	auto baseChroma=h.Run(); h.constants.chromaStrength=1; auto stableChroma=h.Run();
	assert(std::abs(baseChroma[center][0]-stableChroma[center][0])>1e-4f);
	h.Constant(0,{.8f,.8f,.8f,1}); auto rejectedChroma=h.Run();
	Near(rejectedChroma[center][0],.42f); Near(rejectedChroma[center][1],.38f);
	// Current input changes: reject old correction immediately.
	h.Default(); h.Constant(2,{.4f,.4f,.4f,.7f}); h.Constant(3,{-.2f,-.2f,-.2f,1});
	h.Constant(0,{.7f,.7f,.7f,1}); Near(h.Run()[center][0],.4f);
	// Reset ignores invalid old values, and nonfinite current values do not seed history.
	h.Default(); h.constants.weight=0;
	const float nan=std::numeric_limits<float>::quiet_NaN();
	h.Constant(3,{nan,nan,nan,1}); Near(h.Run()[center][0],.5f);
	h.Constant(2,{nan,nan,nan,1}); Near(h.Run()[center][0],.4f); Near(h.Run(1)[center][3],0);
	h.Default(); h.Constant(3,{nan,nan,nan,1}); Near(h.Run()[center][0],.5f);
	// Isolated correct correction must survive a 3x3 neighborhood box.
	h.Default(); Image residual(W*H,{0,0,0,1}), raw(W*H,{.4f,.4f,.4f,1});
	residual[center]={.4f,-.3f,.2f,1}; raw[center]={.8f,.1f,.6f,1};
	h.Set(3,residual); h.Set(2,raw); out=h.Run(); Near(out[center][0],.8f); Near(out[center][1],.1f);
	// Known backward subpixel displacement; B follows the history coordinate.
	h.Default(); h.constants.motion=1; h.Constant(5,{-.5f,0,0,0});
	for (unsigned y=0;y<H;++y) for (unsigned x=0;x<W;++x) residual[y*W+x]={float(x)*.01f,0,0,1};
	h.Set(3,residual); Near(h.Run()[center][0],.4f+.02f+.8f*.075f);
	// Invalid footprint and nonfinite optical flow reject before sampling.
	h.Constant(5,{100,0,0,0}); Near(h.Run()[center][0],.5f);
	h.Constant(5,{nan,0,0,0}); Near(h.Run()[center][0],.5f);
	h.Constant(5,{0,0,0,0}); h.constants.left=8; Near(h.Run()[center][0],.5f);
	// Wrong motion over textured input is rejected by color, even with finite vectors.
	h.Default(); h.constants.motion=1;
	Image guide(W*H), input(W*H);
	for (unsigned y=0;y<H;++y) for (unsigned x=0;x<W;++x) {
		guide[y*W+x]={float(x)*.04f,0,0,1}; input[y*W+x]={float(x-1)*.04f,0,0,1};
	}
	h.Set(0,input); h.Set(4,guide); h.Constant(5,{-1,0,0,0}); Near(h.Run()[center][0],.66f);
	h.Constant(5,{2,0,0,0}); Near(h.Run()[center][0],.5f);
	// HDR values are not clamped to SDR [0,1].
	h.Default(); h.constants.hdr=1; h.constants.weight=0;
	h.Constant(2,{2,-.1f,.5f,1}); out=h.Run(); Near(out[center][0],2); Near(out[center][1],-.1f);
	// A sustained alternating residual loses temporal variation without losing
	// its mean correction. Exercise the real shader's recurrent history output.
	h.Default(); h.Constant(3,{.15f,.15f,.15f,1});
	float sum=0, sumSquares=0;
	for (int frame=0;frame<80;++frame) {
		const float residualValue=.15f + (frame%2 ? .1f : -.1f);
		h.Constant(2,{.4f+residualValue,.4f+residualValue,.4f+residualValue,1});
		const Image history=h.Run(1); h.Set(3,history);
		if (frame>=40) { sum+=history[center][0]; sumSquares+=history[center][0]*history[center][0]; }
	}
	Near(sum/40,.15f,.00075f); // <=0.5% mean error after recurrent FP16 storage.
	assert(sumSquares/40-(sum/40)*(sum/40) < .0002f); // Raw variance is .01.
	// F: disappearance changes support, not conditional amplitude. No second
	// averaging of zero residuals is allowed to attenuate the correction twice.
	h.Default(); h.constants.route=3;
	h.Constant(3,{.2f,-.2f,.1f,2}); h.Constant(2,{.4f,.4f,.4f,1});
	const float release=std::pow(.8f,.08f/.18f);
	out=h.Run(); Near(out[center][0],.4f+.2f*release); Near(out[center][1],.4f-.2f*release);
	Image history=h.Run(1); Near(history[center][0],.2f,.0001f); Near(history[center][3],1+release,.001f);
	// Attack from a previously empty valid history is continuous; sustained
	// signed correction reaches full amplitude instead of being permanently gated.
	h.Constant(3,{0,0,0,1}); h.Constant(2,{.6f,.2f,.5f,1});
	out=h.Run(); Near(out[center][0],.4f+.2f*(1-std::pow(.8f,.08f/.06f)));
	for (int frame=0;frame<60;++frame) h.Set(3,h.Run(1));
	out=h.Run(); Near(out[center][0],.6f,.001f); Near(out[center][1],.2f,.001f);
	// An isolated correct point is also retained after persistence settles.
	h.Default(); h.constants.route=3;
	residual=Image(W*H,{0,0,0,1}); raw=Image(W*H,{.4f,.4f,.4f,1});
	residual[center]={.4f,-.3f,.2f,2}; raw[center]={.8f,.1f,.6f,1};
	h.Set(3,residual); h.Set(2,raw); out=h.Run(); Near(out[center][0],.8f); Near(out[center][1],.1f);
	// Opposite direction starts a new episode rather than carrying old sign.
	h.Default(); h.constants.route=3; h.Constant(3,{.2f,.2f,.2f,2}); h.Constant(2,{.2f,.2f,.2f,1});
	assert(h.Run()[center][0]<.4f);
	// Recurrent on/off signal remains bounded, reduces variance, and keeps
	// conditional amplitude .2 throughout (support intentionally has hysteresis).
	h.Default(); h.constants.route=3; h.Constant(3,{.2f,.2f,.2f,2});
	sum=0; sumSquares=0;
	for (int frame=0;frame<80;++frame) {
		h.Constant(2,frame%2 ? Pixel{.6f,.6f,.6f,1} : Pixel{.4f,.4f,.4f,1});
		out=h.Run(); history=h.Run(1); h.Set(3,history);
		Near(history[center][0],.2f,.0005f);
		if (frame>=40) { const float v=out[center][0]-.4f; sum+=v; sumSquares+=v*v; }
	}
	assert(sum/40>.1f && sum/40<.2f);
	assert(sumSquares/40-(sum/40)*(sum/40)<.0005f);
	// Continuous absence has finite release, with no immortal correction.
	h.Constant(2,{.4f,.4f,.4f,1});
	for (int frame=0;frame<100;++frame) h.Set(3,h.Run(1));
	Near(h.Run()[center][0],.4f,.001f);
	// F and G retain rejection/reset semantics, flow coordinates and signed HDR.
	for (unsigned route : {3u,4u}) {
		h.Default(); h.constants.route=route;
		h.Constant(3,{.3f,.3f,.3f,route==3 ? 2.f : 1.f});
		h.Constant(0,{.7f,.7f,.7f,1}); Near(h.Run()[center][0],.5f);
		h.Default(); h.constants.route=route; h.constants.weight=0;
		h.Constant(3,{nan,nan,nan,nan}); Near(h.Run()[center][0],.5f);
		h.Constant(2,{nan,nan,nan,1}); Near(h.Run()[center][0],.4f); Near(h.Run(1)[center][3],0);
		h.Default(); h.constants.route=route; h.constants.motion=1;
		h.Constant(5,{nan,0,0,0}); Near(h.Run()[center][0],.5f);
		h.Constant(5,{100,0,0,0}); Near(h.Run()[center][0],.5f);
		h.Default(); h.constants.route=route; h.constants.hdr=1; h.constants.weight=0;
		h.Constant(2,{2,-.1f,.5f,1}); out=h.Run(); Near(out[center][0],2); Near(out[center][1],-.1f);
		h.Default(); h.constants.route=route; h.constants.motion=1;
		for (unsigned y=0;y<H;++y) for (unsigned x=0;x<W;++x)
			residual[y*W+x]={float(x)*.01f,0,0,route==3 ? 2.f : 1.f};
		h.Set(3,residual); h.Constant(5,{-1,0,0,0});
		Near(h.Run()[center][0],.4f+.02f+.8f*.07f);
	}
	// G really executes reduction: a signed 2x2 pattern averages to +.1/- .1.
	h.Default(); h.constants.route=4; h.constants.weight=0;
	for (unsigned y=0;y<H;++y) for (unsigned x=0;x<W;++x) {
		const float v=(x%2 ? .2f : 0.f);
		raw[y*W+x]={.4f+v,.4f-v,.4f,1};
	}
	h.Set(2,raw); auto reduced=h.Run(3);
	for (const auto& p : reduced) { Near(p[0],.1f,.0001f); Near(p[1],-.1f,.0001f); Near(p[3],1); }
	// Exact current high-frequency complement preserves all pixels on reset.
	out=h.Run(); for (unsigned i=0;i<W*H;++i) for (unsigned c=0;c<4;++c) Near(out[i][c],raw[i][c]);
	h.Set(3,h.Run(1)); h.constants.weight=.8f;
	out=h.Run(); for (unsigned i=0;i<W*H;++i) for (unsigned c=0;c<3;++c) Near(out[i][c],raw[i][c],.0002f);
	// A bright isolated positive/negative correction is not lost to reduction.
	h.Default(); h.constants.route=4; h.constants.weight=0;
	raw=Image(W*H,{.4f,.4f,.4f,1}); raw[center]={.8f,.1f,.6f,1}; h.Set(2,raw);
	h.Set(3,h.Run(1)); h.constants.weight=.8f; out=h.Run();
	Near(out[center][0],.8f,.0002f); Near(out[center][1],.1f,.0002f);
	// Input edge with no compatible low sample falls back to the exact raw image
	// and marks history invalid, including partial valid-domain cells.
	h.Default(); h.constants.route=4;
	input=Image(W*H,{.4f,.4f,.4f,1}); input[center]={.9f,.9f,.9f,1}; h.Set(0,input);
	Near(h.Run()[center][0],.5f); Near(h.Run(1)[center][3],0);
	h.Default(); h.constants.route=4; h.constants.left=9;
	Near(h.Run()[center][0],.5f); Near(h.Run(1)[center][3],0);
	// An invalid neighbor cannot poison a valid pixel's reduction/reconstruction.
	h.Default(); h.constants.route=4; raw=Image(W*H,{.5f,.5f,.5f,1}); raw[center+1]={nan,nan,nan,1}; h.Set(2,raw);
	out=h.Run(); for (const auto& p : out) for (float v : p) assert(std::isfinite(v));
	Near(h.Run(3)[4*((W+1)/2)+4][3],0);
	// Bilinear guide agreement alone cannot approve two individually mismatched
	// history taps: G rejects this subpixel foreground/background mixture.
	h.Default(); h.constants.route=4; h.constants.motion=1; h.Constant(5,{-.5f,0,0,0});
	for (unsigned y=0;y<H;++y) for (unsigned x=0;x<W;++x) guide[y*W+x]={x%2 ? .8f : .4f,.4f,.4f,1};
	h.Set(4,guide); h.Constant(0,{.6f,.4f,.4f,1}); Near(h.Run()[center][0],.5f);
	// G reduces low-frequency temporal variance and preserves the mean while
	// retaining current high-frequency checkerboard detail.
	h.Default(); h.constants.route=4; h.Constant(3,{.15f,.15f,.15f,1}); sum=0; sumSquares=0;
	for (int frame=0;frame<80;++frame) {
		for (unsigned y=0;y<H;++y) for (unsigned x=0;x<W;++x) {
			const float v=.15f+(frame%2 ? .1f : -.1f)+(x%2 ? .025f : -.025f);
			raw[y*W+x]={.4f+v,.4f+v,.4f+v,1};
		}
		h.Set(2,raw); out=h.Run(); h.Set(3,h.Run(1));
		Near(out[center+1][0]-out[center][0],.05f,.0002f);
		if (frame>=40) { const float v=out[center][0]-.4f+.025f; sum+=v; sumSquares+=v*v; }
	}
	Near(sum/40,.15f,.001f); assert(sumSquares/40-(sum/40)*(sum/40)<.0002f);
	// Odd/non-dispatch-aligned and 1x1 extents include their last texel without
	// reading beyond resources; DC and reset identity hold everywhere.
	for (auto size : {std::array{17u,13u},std::array{1u,1u}}) {
		Harness odd(size[0],size[1]); odd.Default(); odd.constants.route=4; odd.constants.weight=0;
		for (const auto& p : odd.Run(3)) Near(p[0],.1f,.0001f);
		for (const auto& p : odd.Run()) { Near(p[0],.5f); Near(p[3],.7f); }
		odd.CheckDebug();
	}
	h.CheckDebug();
	std::cout << "D3D11 debug layer: " << (h.debug ? "no resource/API warnings" : "not installed; validation skipped") << '\n';

	if (argc > 1) {
		// Fixed aligned history shows the extra chroma control on L/S axes.
		h.Default();
		Image axis(W*H), axisRaw(W*H), axisOld(W*H), axisGuide(W*H);
		for (unsigned y=0;y<H;++y) for (unsigned x=0;x<W;++x) {
			float light=float(y)/(H-1), sat=float(x)/(W-1);
			float chroma=(1-std::abs(2*light-1))*sat, base=light-chroma*.5f;
			const unsigned i=y*W+x;
			axis[i]={base,base+chroma/3,base+chroma,1}; axisGuide[i]=axis[i];
			axisRaw[i]={std::clamp(axis[i][0]+.03f,0.f,1.f),std::clamp(axis[i][1]-.01f,0.f,1.f),std::clamp(axis[i][2]-.02f,0.f,1.f),1};
			axisOld[i]={-.02f,.01f,.01f,1};
		}
		h.Set(0,axis); h.Set(1,axis); h.Set(2,axisRaw); h.Set(3,axisOld); h.Set(4,axisGuide);
		auto baseline=h.Run();
		std::ofstream csv(std::string(argv[1])+"/temporal-response.csv");
		csv<<"strength,lightness,saturation,r,g,b,delta\n";
		for (float strength : {0.f,.5f,1.f}) {
			h.constants.chromaStrength=strength; auto result=h.Run();
			for (unsigned y=0;y<H;++y) for (unsigned x=0;x<W;++x) {
				const unsigned i=y*W+x; float difference=0;
				for (int c=0;c<3;++c) difference=std::max(difference,std::abs(result[i][c]-baseline[i][c]));
				csv<<strength<<','<<float(y)/(H-1)<<','<<float(x)/(W-1)<<','<<result[i][0]<<','<<result[i][1]<<','<<result[i][2]<<','<<difference<<'\n';
			}
		}
	}
	std::cout<<"A/B/F/G production HLSL WARP tests passed: signed EMA, conditional persistence, attack/release, motion/reset/rejection, half-resolution reduction, guided reconstruction, tap validation, detail, odd extents and recurrent variance.\n";
}
