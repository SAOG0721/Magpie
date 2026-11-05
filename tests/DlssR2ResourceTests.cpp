// Software D3D11/D3D12 only. No window, capture, vendor DLL or NGX evaluation.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <winrt/base.h>
#include <wil/resource.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <optional>
#include <thread>
#include <vector>
#include "NativeBackendTiming.h"
#include "include/DlssNrStatus.h"

using winrt::com_ptr;
void Check(bool condition, const char* label) {
	if (!condition) { std::fprintf(stderr, "FAIL: %s\n", label); std::exit(1); }
}
void Hr(HRESULT hr) { if (FAILED(hr)) { std::fprintf(stderr, "HRESULT %08lx\n", hr); std::exit(1); } }
struct Logger {
	static Logger& Get() { static Logger log; return log; }
	void ComError(const char* message, HRESULT hr) { std::fprintf(stderr, "%s (%08lx)\n", message, hr); }
};

namespace Nr {
using Magpie::DlssNrFailure;
using Magpie::DlssNrStage;
using Magpie::DlssNrCodeKind;
using Magpie::DlssNrApiFailure;
struct DLSSNRSettings {
	float residualMultiplier = 1, residualSaturation = 1, residualLightness = 1;
	float shadowStructureMultiplier = 1, reflectionGlowMultiplier = 1;
	float residualMidtoneStrength = 1;
	int residualDebugView = 0;
	float residualHueProtection = 0, residualDarkProtection = 0, residualHighlightProtection = 0, residualLocalCompression = 0;
	float residualLowFrequencyGain = 1, residualDetailGain = 1;
	float residualChromaProtection = 0;
};
struct DLSSNRFilter { struct Impl {
	DlssNrFailure failure;
	ID3D11Device* device11 = nullptr;
	ID3D11DeviceContext* context11 = nullptr;
	uint32_t width = 0, height = 0, sourceWidth = 0, sourceHeight = 0;
	bool useResolutionScaling = true;
	bool finalizeOutput = true;
	bool residualConstantsValid = false, residualParametersDirty = true;
	com_ptr<ID3D11Texture2D> compositeOutput11;
	com_ptr<ID3D11UnorderedAccessView> compositeOutputUav11, controlledResidualUav11, resampleIntermediateUav11;
	com_ptr<ID3D11ShaderResourceView> inputSrv11, sharedInputSrv11, sharedOutputSrv11, controlledResidualSrv11, resampleIntermediateSrv11;
	com_ptr<ID3D11ComputeShader> residualPrepareShader11, residualHorizontalShader11, residualVerticalCompositeShader11;
	com_ptr<ID3D11Buffer> residualConstants11;
}; };
}
namespace Fg {
struct DLSSFrameGenerator { struct Impl {
	com_ptr<ID3D12Device> device12;
	com_ptr<ID3D12Fence> fence12;
	wil::unique_event_nothrow fenceEvent;
	std::array<com_ptr<ID3D12Resource>, 4> interpolationDisable12, interpolationDisableReadback12;
	std::array<const uint8_t*, 4> interpolationDisableMapped{};
	std::array<uint32_t, 4> diagnosticInterpolationReadbackFailure{}, diagnosticInterpolationEnabled{}, diagnosticInterpolationDisabled{};
	~Impl() {
		const D3D12_RANGE empty{};
		for (size_t i = 0; i < 4; ++i) if (interpolationDisableMapped[i]) interpolationDisableReadback12[i]->Unmap(0, &empty);
	}
}; };
}
#include "dlss_r2_production.h"

com_ptr<ID3D11Device> device;
com_ptr<ID3D11DeviceContext> context;
com_ptr<ID3D11Texture2D> Texture(UINT w, UINT h, DXGI_FORMAT format, UINT bind, const void* data = nullptr, UINT pitch = 0) {
	D3D11_TEXTURE2D_DESC d{};
	d.Width = w; d.Height = h; d.Format = format; d.MipLevels = d.ArraySize = d.SampleDesc.Count = 1;
	d.BindFlags = bind;
	D3D11_SUBRESOURCE_DATA init{data, pitch, 0};
	com_ptr<ID3D11Texture2D> result;
	Hr(device->CreateTexture2D(&d, data ? &init : nullptr, result.put()));
	return result;
}
com_ptr<ID3D11ShaderResourceView> Srv(ID3D11Texture2D* texture) {
	com_ptr<ID3D11ShaderResourceView> result; Hr(device->CreateShaderResourceView(texture, nullptr, result.put())); return result;
}
com_ptr<ID3D11UnorderedAccessView> Uav(ID3D11Texture2D* texture) {
	com_ptr<ID3D11UnorderedAccessView> result; Hr(device->CreateUnorderedAccessView(texture, nullptr, result.put())); return result;
}
com_ptr<ID3D11ComputeShader> Shader(const char* source, const char* entry) {
	com_ptr<ID3DBlob> code, error;
	HRESULT hr = D3DCompile(source, std::strlen(source), nullptr, nullptr, nullptr, entry, "cs_5_0",
		D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_WARNINGS_ARE_ERRORS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, code.put(), error.put());
	if (error) std::fprintf(stderr, "%s\n", static_cast<char*>(error->GetBufferPointer()));
	Hr(hr);
	com_ptr<ID3D11ComputeShader> shader; Hr(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, shader.put())); return shader;
}
std::vector<uint8_t> Read(ID3D11Texture2D* texture, UINT bytesPerPixel = 4) {
	D3D11_TEXTURE2D_DESC d{}; texture->GetDesc(&d); d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
	com_ptr<ID3D11Texture2D> staging; Hr(device->CreateTexture2D(&d, nullptr, staging.put()));
	context->CopyResource(staging.get(), texture);
	D3D11_MAPPED_SUBRESOURCE mapped{}; Hr(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped));
	std::vector<uint8_t> data(size_t(d.Width) * d.Height * bytesPerPixel);
	for (UINT y = 0; y < d.Height; ++y) std::memcpy(data.data() + size_t(y) * d.Width * bytesPerPixel,
		static_cast<uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch, size_t(d.Width) * bytesPerPixel);
	context->Unmap(staging.get(), 0); return data;
}
constexpr UINT Views = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

void TestResidual(UINT w, UINT h, UINT rw, UINT rh) {
	std::vector<uint32_t> original(w*h), reduced(rw*rh), filtered(rw*rh);
	for (size_t i = 0; i < original.size(); ++i) original[i] = uint32_t(i*2654435761u + 0x09253580u);
	for (size_t i = 0; i < reduced.size(); ++i) { reduced[i] = uint32_t(i*747796405u + 0x45203875u); filtered[i] = reduced[i] ^ 0x365347u; }
	auto input = Texture(w, h, DXGI_FORMAT_R8G8B8A8_UNORM, Views, original.data(), w*4);
	auto low = Texture(rw, rh, DXGI_FORMAT_R8G8B8A8_UNORM, Views, reduced.data(), rw*4);
	auto nr = Texture(rw, rh, DXGI_FORMAT_R8G8B8A8_UNORM, Views, filtered.data(), rw*4);
	auto direct = Texture(w, h, DXGI_FORMAT_R8G8B8A8_UNORM, Views);
	auto copied = Texture(w, h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE);
	auto controlled = Texture(rw, rh, DXGI_FORMAT_R16G16B16A16_FLOAT, Views);
	auto horizontal = Texture(w, rh, DXGI_FORMAT_R16G16B16A16_FLOAT, Views);
	Nr::DLSSNRFilter::Impl impl;
	impl.device11 = device.get(); impl.context11 = context.get(); impl.sourceWidth=w; impl.sourceHeight=h; impl.width=rw; impl.height=rh;
	impl.inputSrv11=Srv(input.get()); impl.sharedInputSrv11=Srv(low.get()); impl.sharedOutputSrv11=Srv(nr.get());
	impl.controlledResidualSrv11=Srv(controlled.get()); impl.controlledResidualUav11=Uav(controlled.get());
	impl.resampleIntermediateSrv11=Srv(horizontal.get()); impl.resampleIntermediateUav11=Uav(horizontal.get());
	impl.residualPrepareShader11=Shader(Nr::RESIDUAL_PREPARE_SHADER.c_str(), "PrepareResidual");
	impl.residualHorizontalShader11=Shader(Nr::RESIDUAL_HORIZONTAL_HLSL, "UpsampleResidualHorizontal");
	impl.residualVerticalCompositeShader11=Shader(Nr::RESIDUAL_VERTICAL_COMPOSITE_SHADER.c_str(), "CompositeResidualVertical");
	D3D11_BUFFER_DESC cb{}; cb.ByteWidth=sizeof(Nr::ResampleConstants); cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
	Hr(device->CreateBuffer(&cb, nullptr, impl.residualConstants11.put()));
	const std::array<Nr::DLSSNRSettings, 5> settings{{{}, {0,1,1,1,1}, {2,0,0.5f,0,2}, {0.4f,2,1.8f,2,0}, {1,1,1,0.2f,1.8f}}};
	for (const auto& s : settings) {
		D3D11_TEXTURE2D_DESC d{}; copied->GetDesc(&d);
		impl.compositeOutput11=nullptr; impl.compositeOutputUav11=nullptr;
		Check(Nr::CreateCompositeOutput(impl,input.get(),copied.get(),d), "fallback target creation");
		Check(impl.compositeOutput11.get()!=copied.get(), "no-UAV fallback owns intermediate");
		Check(Nr::CompositeResidual(impl,copied.get(),impl.sharedOutputSrv11.get(),s), "fallback composite");
		auto expected=Read(copied.get());
		impl.compositeOutput11=nullptr; impl.compositeOutputUav11=nullptr;
		direct->GetDesc(&d);
		Check(Nr::CreateCompositeOutput(impl,input.get(),direct.get(),d), "direct target creation");
		Check(impl.compositeOutput11.get()==direct.get(), "direct target avoids intermediate allocation");
		Check(Nr::CompositeResidual(impl,direct.get(),impl.sharedOutputSrv11.get(),s), "direct composite");
		Check(Read(direct.get())==expected, "direct output bit-matches copied output");
		Check(Nr::CompositeResidual(impl,copied.get(),impl.sharedOutputSrv11.get(),s), "changed output still copied");
		Check(Read(copied.get())==expected, "changed output correctness");
	}
	impl.compositeOutput11=nullptr; impl.compositeOutputUav11=nullptr;
	D3D11_TEXTURE2D_DESC d{}; input->GetDesc(&d);
	Check(Nr::CreateCompositeOutput(impl,input.get(),input.get(),d), "aliased input/output fallback");
	Check(impl.compositeOutput11.get()!=input.get(), "original input not overwritten by composite");
}


void TestFgReadback() {
	Fg::DLSSFrameGenerator::Impl impl;
	com_ptr<IDXGIFactory4> factory; Hr(CreateDXGIFactory1(IID_PPV_ARGS(factory.put())));
	com_ptr<IDXGIAdapter> warp; Hr(factory->EnumWarpAdapter(IID_PPV_ARGS(warp.put())));
	Hr(D3D12CreateDevice(warp.get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(impl.device12.put())));
	Hr(impl.device12->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(impl.fence12.put()))); Hr(impl.fenceEvent.create());
	const HANDLE event=impl.fenceEvent.get();
	for(UINT i=1;i<4;++i) Check(Fg::CreateInterpolationDisableResources(impl,i),"persistent readback creation");
	com_ptr<ID3D12CommandQueue> queue; D3D12_COMMAND_QUEUE_DESC q{}; Hr(impl.device12->CreateCommandQueue(&q,IID_PPV_ARGS(queue.put())));
	com_ptr<ID3D12CommandAllocator> allocator; Hr(impl.device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(allocator.put())));
	com_ptr<ID3D12GraphicsCommandList> list; Hr(impl.device12->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.get(),nullptr,IID_PPV_ARGS(list.put()))); Hr(list->Close());
	D3D12_HEAP_PROPERTIES hp{}; hp.Type=D3D12_HEAP_TYPE_UPLOAD; D3D12_RESOURCE_DESC bd{}; bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width=4; bd.Height=1; bd.DepthOrArraySize=bd.MipLevels=bd.SampleDesc.Count=1; bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	com_ptr<ID3D12Resource> upload; Hr(impl.device12->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(upload.put())));
	for(UINT frame=1;frame<=96;++frame) {
		UINT slot=1+frame%3; uint32_t flag=(frame&1)?0xffffff00u:0x00000001u;
		void* ptr=nullptr; D3D12_RANGE empty{}; Hr(upload->Map(0,&empty,&ptr)); std::memcpy(ptr,&flag,4); upload->Unmap(0,nullptr);
		Hr(allocator->Reset()); Hr(list->Reset(allocator.get(),nullptr));
		D3D12_RESOURCE_BARRIER b{}; b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition={impl.interpolationDisable12[slot].get(),0,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_DEST};
		list->ResourceBarrier(1,&b); list->CopyBufferRegion(b.Transition.pResource,0,upload.get(),0,4);
		b.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_DEST; b.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE; list->ResourceBarrier(1,&b);
		list->CopyBufferRegion(impl.interpolationDisableReadback12[slot].get(),0,b.Transition.pResource,0,4);
		b.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_SOURCE; b.Transition.StateAfter=D3D12_RESOURCE_STATE_UNORDERED_ACCESS; list->ResourceBarrier(1,&b); Hr(list->Close());
		ID3D12CommandList* lists[]{list.get()}; queue->ExecuteCommandLists(1,lists); Hr(queue->Signal(impl.fence12.get(),frame));
		Check(Fg::WaitForFence(impl,frame),"GPU readback fence completed"); auto disabled=Fg::ReadInterpolationDisabled(impl,slot);
		Check(disabled.has_value() && *disabled==((flag&255)!=0),"persistent mapping reads current first byte only"); Check(impl.fenceEvent.get()==event,"wait event reused");
	}
	Check(!Fg::ReadInterpolationDisabled(impl,0).has_value(),"missing mapping remains a failure");
	std::thread staleWake([event] { Sleep(10); SetEvent(event); });
	Check(!Fg::WaitForFence(impl,97),"event wake alone does not authorize unfinished GPU output");
	staleWake.join(); Hr(impl.fence12->Signal(97));
	Check(Fg::WaitForFence(impl,97),"completed fence remains usable after stale wake");
}

int main() {
	Hr(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,device.put(),nullptr,context.put()));
	Shader(Nr::COLOR_DOWNSAMPLE_HLSL,"DownsampleColorVertical"); Shader(Nr::COLOR_DOWNSAMPLE_HLSL,"DownsampleColorHorizontal"); Shader(Nr::COLOR_CONVERT_HLSL,"ConvertToRgba");
	TestResidual(17,11,17,11); TestResidual(17,11,13,8); TestResidual(17,11,9,6); TestResidual(1,1,1,1); TestResidual(1,13,1,7);
	TestFgReadback();
	if constexpr (Magpie::NativeBackendTiming::Enabled) { Check(Magpie::NativeBackendTiming::Now().time_since_epoch().count()>0,"timing enabled in validation build"); }
	else { Check(Magpie::NativeBackendTiming::Now()==Magpie::NativeBackendTiming::Clock::time_point{} && Magpie::NativeBackendTiming::ElapsedMilliseconds({})==0,"disabled timing is a no-op"); }
	std::puts("PASS: NR copied/direct outputs (25 cases), alias fallback, FG persistent readback (96 submissions), timing option");
}
