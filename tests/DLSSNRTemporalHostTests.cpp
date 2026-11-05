// Compile the real Temporal host and HLSL; only the D3D device is supplied by
// the fixture. No NGX, optical-flow provider, or physical GPU is required.
#include "pch.h"
#include "ScalingOptions.h"
#include <parallel_hashmap/phmap.h>
#define private public
#include "DeviceResources.h"
#include "DLSSNRTemporal.h"
#undef private
#include "DLSSNRTemporal.cpp"
#include <d3d11sdklayers.h>
#include <array>
#include <iostream>
#include <stdexcept>

using namespace Magpie;
using Pixel = std::array<float,4>;
constexpr uint32_t WIDTH=16, HEIGHT=16;
constexpr int64_t STEP=166667;
static unsigned failures=0, checks=0;

static void Check(HRESULT hr) {
	if (FAILED(hr)) throw std::runtime_error("D3D operation failed: "+std::to_string(hr));
}
static void Expect(bool condition,const std::string& label) {
	++checks;
	if (!condition) { ++failures; std::cerr<<"FAIL: "<<label<<'\n'; }
}
static void Near(float actual,float expected,const std::string& label,float tolerance=.0003f) {
	const bool pass=std::isfinite(actual)&&std::abs(actual-expected)<=tolerance;
	Expect(pass,label);
	if (!pass) std::cerr<<"  actual="<<actual<<" expected="<<expected<<'\n';
}

struct Fixture {
	DeviceResources resources;
	winrt::com_ptr<ID3D11Texture2D> input,raw,output,validity,motion,staging;
	winrt::com_ptr<ID3D11ShaderResourceView> validitySrv;
	winrt::com_ptr<ID3D11InfoQueue> debug;
	FrameGuidanceView zero,guide;
	DLSSNRTemporal temporal;
	uint64_t source=0,epoch=1,revision=0;
	unsigned maxAge=0;
	int route;
	bool useMotion=false,sparse=false;
	float validityValue=1;
	Fixture(int nextRoute,unsigned age=0,bool nextSparse=false,bool nextMotion=false):
		maxAge(age),route(nextRoute),useMotion(nextMotion),sparse(nextSparse) {
		winrt::com_ptr<ID3D11Device> device;
		winrt::com_ptr<ID3D11DeviceContext> dc;
		auto hr=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,D3D11_CREATE_DEVICE_DEBUG,
			nullptr,0,D3D11_SDK_VERSION,device.put(),nullptr,dc.put());
		if (hr==DXGI_ERROR_SDK_COMPONENT_MISSING) hr=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,
			nullptr,0,nullptr,0,D3D11_SDK_VERSION,device.put(),nullptr,dc.put());
		Check(hr);
		resources._d3dDevice=device.as<ID3D11Device5>();
		resources._d3dDC=dc.as<ID3D11DeviceContext4>();
		(void)device->QueryInterface(IID_PPV_ARGS(debug.put()));
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width=WIDTH; desc.Height=HEIGHT; desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
		desc.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;
		desc.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
		for (auto* texture:{&input,&raw,&output}) Check(device->CreateTexture2D(&desc,nullptr,texture->put()));
		desc.Usage=D3D11_USAGE_STAGING; desc.BindFlags=0; desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
		Check(device->CreateTexture2D(&desc,nullptr,staging.put()));
		desc.Usage=D3D11_USAGE_DEFAULT; desc.CPUAccessFlags=0; desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
		desc.Format=DXGI_FORMAT_R32_FLOAT;
		Check(device->CreateTexture2D(&desc,nullptr,validity.put()));
		Check(device->CreateShaderResourceView(validity.get(),nullptr,validitySrv.put()));
		desc.Format=DXGI_FORMAT_R16G16_FLOAT;
		Check(device->CreateTexture2D(&desc,nullptr,motion.put()));
		std::vector<uint32_t> vectors(WIDTH*HEIGHT,0);
		dc->UpdateSubresource(motion.get(),0,nullptr,vectors.data(),WIDTH*sizeof(uint32_t),0);
		Set(input.get(),{.4f,.4f,.4f,1}); Set(raw.get(),{.5f,.5f,.5f,1}); SetValidity(1);
		if (!temporal.Initialize(resources,input.get(),input.get(),raw.get(),output.get(),route,false,80))
			throw std::runtime_error("Production Temporal initialization failed");
	}
	void Set(ID3D11Texture2D* texture,Pixel value) {
		std::vector<Pixel> pixels(WIDTH*HEIGHT,value);
		resources.GetD3DDC()->UpdateSubresource(texture,0,nullptr,pixels.data(),WIDTH*sizeof(Pixel),0);
	}
	void SetValidity(float value) {
		validityValue=value;
		std::vector<float> values(WIDTH*HEIGHT,value);
		resources.GetD3DDC()->UpdateSubresource(validity.get(),0,nullptr,values.data(),WIDTH*sizeof(float),0);
	}
	Pixel Read() {
		auto* dc=resources.GetD3DDC(); dc->CopyResource(staging.get(),output.get());
		D3D11_MAPPED_SUBRESOURCE mapped{}; Check(dc->Map(staging.get(),0,D3D11_MAP_READ,0,&mapped));
		const auto* row=reinterpret_cast<const Pixel*>(static_cast<const char*>(mapped.pData)+8*mapped.RowPitch);
		const Pixel result=row[8]; dc->Unmap(staging.get(),0); return result;
	}
	Pixel Draw(uint64_t frame,uint64_t nextSource,Pixel color,uint64_t nextRevision=0,
		bool hardReset=false,bool metadataValid=true,bool presentValidity=true,bool bypass=false,
		int64_t timestamp=0,bool newCapture=true) {
		source=nextSource; revision=nextRevision; Set(raw.get(),color);
		FrameGuidanceMetadata meta;
		meta.frameId=frame; meta.resourceGeneration=1; meta.timestamp100ns=timestamp?timestamp:int64_t(frame)*STEP;
		meta.sourceExtent={WIDTH,HEIGHT}; meta.validRegion=FrameGuidanceRegion::Full(meta.sourceExtent);
		meta.valid=metadataValid; meta.isZero=true;
		zero.motion.metadata=meta;
		guide.motion.metadata=meta; guide.motion.metadata.isZero=false;
		guide.motion.texture=useMotion?motion.get():nullptr; guide.motion.format=DXGI_FORMAT_R16G16_FLOAT;
		if (sparse) temporal.SetSparseHistory(frame,source,epoch,revision,maxAge,revision,
			presentValidity?validitySrv.get():nullptr,int64_t(source)*STEP);
		else temporal.SetPerformanceHistory(frame,source,epoch,maxAge,revision,presentValidity?validitySrv.get():nullptr);
		NativeEffectDrawContext context{.input=input.get(),.output=output.get(),.frameId=frame,
			.previousCaptureFrameId=frame-1,.inputRevision=frame,.inputHistoryRevision=1,
			.inputHistoryReset=hardReset,.isNewCaptureFrame=newCapture,
			.captureAdmissionTime100ns=meta.timestamp100ns,.frameGuidance=guide,.zeroFrameGuidance=zero};
		if (!temporal.Draw(context,bypass)) throw std::runtime_error("Production Temporal Draw failed");
		return Read();
	}
	void CheckDebug() {
		if (!debug) return;
		for (UINT64 i=0;i<debug->GetNumStoredMessages();++i) {
			SIZE_T bytes=0; Check(debug->GetMessage(i,nullptr,&bytes)); std::vector<char> data(bytes);
			auto* message=reinterpret_cast<D3D11_MESSAGE*>(data.data()); Check(debug->GetMessage(i,message,&bytes));
			if (message->Severity<=D3D11_MESSAGE_SEVERITY_WARNING) {
				Expect(false,"D3D11 debug layer"); std::cerr<<message->pDescription<<'\n';
			}
		}
		debug->ClearStoredMessages();
	}
};

static Pixel Gray(float value) { return {value,value,value,1}; }

static void FreshSource(int route,unsigned age,unsigned interval,bool motion) {
	Fixture tested(route,age,false,motion),reference(route,0,false,motion);
	for (uint64_t frame=1;frame<=25;++frame) {
		const uint64_t source=1+(frame-1)/interval*interval;
		const float raw=.5f+((source-1)/interval%2?.02f:0);
		const auto actual=tested.Draw(frame,source,Gray(raw));
		const auto expected=reference.Draw(frame,source,Gray(raw));
		Near(actual[0],expected[0],"fresh source retains EMA route="+std::to_string(route)+
			" age="+std::to_string(age)+" frame="+std::to_string(frame));
		if (frame==1+interval) std::cout<<"fresh-source route="<<route<<" age="<<age<<" frame="<<frame
			<<" output="<<actual[0]<<" continuous="<<expected[0]<<" raw="<<raw<<'\n';
	}
	tested.CheckDebug(); reference.CheckDebug();
}

static void LegacyBoundaries(int route) {
	const std::string label=" route="+std::to_string(route);
	{
		Fixture h(route,1); h.Draw(1,1,Gray(.5f));
		const auto boundary=h.Draw(2,1,Gray(.52f));
		Expect(boundary[0]<.51f,"source age exactly maxAge remains supported"+label);
		Near(h.Draw(3,1,Gray(.52f))[0],.52f,"repeated source expires at maxAge+1"+label);
		Near(h.Draw(4,1,Gray(.54f))[0],.54f,"repeated source never refreshes age"+label);
		std::cout<<"stale-source route="<<route<<" target=4 source=1 output="<<h.Read()[0]<<" raw=.54\n";
		h.CheckDebug();
	}
	for (uint64_t source:{0ull,3ull}) {
		Fixture h(route,1); h.Draw(1,1,Gray(.5f));
		Near(h.Draw(2,source,Gray(.52f))[0],.52f,"missing/future source resets"+label);
		h.CheckDebug();
	}
	{
		Fixture h(route,1); h.Draw(1,1,Gray(.5f));
		const auto before=h.Draw(2,2,Gray(.52f));
		const auto next=h.temporal._impl->next;
		const auto timestamp=h.temporal._impl->state.timestamp;
		const auto again=h.Draw(2,2,Gray(.52f));
		Near(again[0],before[0],"duplicate Draw preserves filtered pixels"+label,0);
		Expect(h.temporal._impl->next==next&&h.temporal._impl->state.timestamp==timestamp,
			"duplicate Draw preserves ping-pong and EMA clock"+label);
		h.temporal.SetTimeConstant(5);
		Near(h.Draw(2,2,Gray(.52f))[0],before[0],"tau edit waits for next capture"+label,0);
		h.epoch=2;
		Near(h.Draw(3,3,Gray(.54f))[0],.54f,"epoch change resets"+label);
		h.CheckDebug();
	}
	{
		Fixture fast(route,1),slow(route,1);
		fast.Draw(1,1,Gray(.5f)); slow.Draw(1,1,Gray(.5f));
		fast.Draw(2,1,Gray(.5f)); slow.Draw(2,1,Gray(.5f));
		fast.temporal.SetTimeConstant(5); slow.temporal.SetTimeConstant(200);
		const float shortTau=fast.Draw(3,3,Gray(.52f))[0],longTau=slow.Draw(3,3,Gray(.52f))[0];
		Expect(shortTau>longTau+.01f,"live tau edit affects next fresh observation"+label);
		fast.CheckDebug(); slow.CheckDebug();
	}
	for (int reset:{0,1,2,3,4}) {
		Fixture h(route,1); h.Draw(1,1,Gray(.5f));
		if (reset==3) h.temporal.Reset();
		const auto result=h.Draw(2,2,Gray(.52f),0,reset==0,reset!=1,true,reset==2,
			reset==4?3000000:0);
		Near(result[0],.52f,"hard/metadata/bypass/manual/idle reset="+std::to_string(reset)+label);
		h.CheckDebug();
	}
	for (float validity:{0.f,.5f,.998f}) {
		Fixture h(route,1); h.Draw(1,1,Gray(.5f)); h.SetValidity(validity);
		Near(h.Draw(2,2,Gray(.52f))[0],.52f,"partial validity retains .999 guard"+label);
		h.CheckDebug();
	}
	{
		Fixture h(route,1); h.Draw(1,1,Gray(.5f));
		const auto result=h.Draw(2,2,Gray(.52f),0,false,true,false);
		Expect(result[0]<.51f,"optional absent reuse mask preserves legacy path"+label);
		h.temporal.ConfigureDetail(0,true);
		Near(h.Draw(3,3,Gray(.4f))[0],.4f,"detail zero-current contract survives"+label);
		h.CheckDebug();
	}
}

static void SparseBoundaries(int route) {
	const std::string label=" route="+std::to_string(route);
	Fixture h(route,1,true);
	h.Draw(1,1,Gray(.5f),1);
	const float held=h.Draw(2,1,Gray(.52f),1)[0];
	Near(held,.5f,"Sparse repeated source transports without new NR observation"+label);
	const float fresh=h.Draw(3,3,Gray(.52f),2)[0];
	const float expected=.4f+.12f+(.1f-.12f)*float(std::exp(-double(2*STEP)*1e-7/.08));
	Near(fresh,expected,"Sparse fresh NR uses observation elapsed time"+label);
	Near(h.Draw(4,3,Gray(.54f),2)[0],fresh,"Sparse repeated source leaves observation time unchanged"+label);
	Near(h.Draw(5,3,Gray(.56f),2)[0],.56f,"Sparse old source expires"+label);
	h.CheckDebug();
	{
		Fixture exact(route,0,true); exact.Draw(1,1,Gray(.5f),1);
		Near(exact.Draw(2,1,Gray(.52f),1)[0],.52f,"Sparse maxAge0 requires current source"+label);
		exact.CheckDebug();
	}
	{
		Fixture idle(route,1,true); idle.Draw(1,1,Gray(.5f),1);
		Near(idle.Draw(2,1,Gray(.52f),1,false,true,true,false,3000000)[0],.52f,
			"Sparse observation idle deadline remains bounded"+label);
		idle.CheckDebug();
	}
}

int main() {
	try {
		winrt::init_apartment(); Logger::Get().Initialize(spdlog::level::warn,L"DLSSNRTemporalHostTests.log",1000000,1);
		for (int route:{1,2,3,4}) {
			// Actual Alternating/Pipeline cadence and max-age values, across all four
			// anti-flicker algorithms. The reference has only the source-age gate off.
			FreshSource(route,1,2,false); FreshSource(route,3,4,false);
			if (route>=2) FreshSource(route,1,2,true);
			LegacyBoundaries(route); SparseBoundaries(route);
		}
		std::cout<<"Temporal host WARP: "<<checks<<" checks, "<<failures<<" failures\n";
		Logger::Get().Flush(); return failures?1:0;
	} catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 2; }
}
