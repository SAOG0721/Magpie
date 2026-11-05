#include <Windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <cassert>
#include <cmath>
#include <iostream>
#include <vector>
#include "OpticalFlowAdaptShaderUnderTest.h"

using Microsoft::WRL::ComPtr;
static ComPtr<ID3D11Device> device;
static ComPtr<ID3D11DeviceContext> context;

static void Check(HRESULT hr) { if (FAILED(hr)) { std::cerr << "HRESULT=" << std::hex << hr << '\n'; std::abort(); } }
static ComPtr<ID3D11Texture2D> Texture(unsigned w, unsigned h, DXGI_FORMAT format,
	const void* data = nullptr, unsigned pitch = 0) {
	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = w; desc.Height = h; desc.MipLevels = desc.ArraySize = 1;
	desc.Format = format; desc.SampleDesc.Count = 1;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
	D3D11_SUBRESOURCE_DATA initial{data, pitch, 0};
	ComPtr<ID3D11Texture2D> result;
	Check(device->CreateTexture2D(&desc, data ? &initial : nullptr, &result));
	return result;
}
static ComPtr<ID3D11ComputeShader> Shader(const char* text, const char* entry) {
	ComPtr<ID3DBlob> blob, errors;
	const auto hr = D3DCompile(text, strlen(text), "production-flow-shader", nullptr,
		nullptr, entry, "cs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &blob, &errors);
	if (errors) std::cerr << static_cast<const char*>(errors->GetBufferPointer());
	Check(hr);
	ComPtr<ID3D11ComputeShader> result;
	Check(device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &result));
	return result;
}
static void Dispatch(ID3D11ComputeShader* shader, unsigned w, unsigned h,
	const std::vector<ID3D11Texture2D*>& inputs, const std::vector<ID3D11Texture2D*>& outputs,
	const void* values, unsigned bytes) {
	std::vector<ComPtr<ID3D11ShaderResourceView>> inputViews(inputs.size());
	std::vector<ComPtr<ID3D11UnorderedAccessView>> outputViews(outputs.size());
	std::vector<ID3D11ShaderResourceView*> srvs(inputs.size());
	std::vector<ID3D11UnorderedAccessView*> uavs(outputs.size());
	for (size_t i = 0; i < inputs.size(); ++i) { Check(device->CreateShaderResourceView(inputs[i], nullptr, &inputViews[i])); srvs[i] = inputViews[i].Get(); }
	for (size_t i = 0; i < outputs.size(); ++i) { Check(device->CreateUnorderedAccessView(outputs[i], nullptr, &outputViews[i])); uavs[i] = outputViews[i].Get(); }
	D3D11_BUFFER_DESC desc{}; desc.ByteWidth = bytes; desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	D3D11_SUBRESOURCE_DATA data{values, 0, 0};
	ComPtr<ID3D11Buffer> buffer; Check(device->CreateBuffer(&desc, &data, &buffer));
	auto* cb = buffer.Get();
	context->CSSetShader(shader, nullptr, 0);
	context->CSSetShaderResources(0, unsigned(srvs.size()), srvs.data());
	context->CSSetUnorderedAccessViews(0, unsigned(uavs.size()), uavs.data(), nullptr);
	context->CSSetConstantBuffers(0, 1, &cb);
	context->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
	context->ClearState();
}
static std::vector<float> Read(ID3D11Texture2D* texture, unsigned channels) {
	D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
	desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	desc.BindFlags = desc.MiscFlags = 0;
	ComPtr<ID3D11Texture2D> staging; Check(device->CreateTexture2D(&desc, nullptr, &staging));
	context->CopyResource(staging.Get(), texture);
	D3D11_MAPPED_SUBRESOURCE mapped{}; Check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
	std::vector<float> result(desc.Width * desc.Height * channels);
	for (unsigned y = 0; y < desc.Height; ++y)
		memcpy(result.data() + y * desc.Width * channels,
			static_cast<const char*>(mapped.pData) + y * mapped.RowPitch, desc.Width * channels * sizeof(float));
	context->Unmap(staging.Get(), 0);
	return result;
}
int main() {
	Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
		D3D11_SDK_VERSION, &device, nullptr, &context));
	auto adapt = Shader(OPTICAL_FLOW_ADAPT_UNDER_TEST, "Adapt");
	unsigned cases = 0;
	for (const auto size : {std::array<unsigned,2>{17,11}, {32,24}, {1,1}}) {
		const auto [sw, sh] = size;
		for (const auto target : {std::array<unsigned,2>{1,1}, {sw,sh},
			{sw/2+1,sh/3+1}, {sw*2+1,sh*2+1}}) {
			const auto [tw, th] = target;
			// Consumer adaptation changes the motion pixel units independently in
			// x/y, while preserving scalar depth and confidence.
			std::vector<float> motion(sw * sh * 2), scalar(sw * sh, 0.65f);
			for (size_t i = 0; i < motion.size(); i += 2) { motion[i] = 2; motion[i+1] = -3; }
			auto m = Texture(sw, sh, DXGI_FORMAT_R32G32_FLOAT, motion.data(), sw * 8);
			auto d = Texture(sw, sh, DXGI_FORMAT_R32_FLOAT, scalar.data(), sw * 4);
			auto q = Texture(sw, sh, DXGI_FORMAT_R32_FLOAT, scalar.data(), sw * 4);
			auto om = Texture(tw, th, DXGI_FORMAT_R32G32_FLOAT);
			auto od = Texture(tw, th, DXGI_FORMAT_R32_FLOAT);
			auto oq = Texture(tw, th, DXGI_FORMAT_R32_FLOAT);
			struct Params { unsigned sw, sh, tw, th; float x, y, p0, p1; } params{sw, sh, tw, th, float(tw)/sw, float(th)/sh, 0, 0};
			Dispatch(adapt.Get(), tw, th, {m.Get(), d.Get(), q.Get()}, {om.Get(), od.Get(), oq.Get()}, &params, sizeof(params));
			const auto adapted = Read(om.Get(), 2), depth = Read(od.Get(), 1), confidence = Read(oq.Get(), 1);
			for (size_t i = 0; i < confidence.size(); ++i) {
				assert(std::abs(adapted[i*2] - 2.f*tw/sw) < 0.0001f);
				assert(std::abs(adapted[i*2+1] + 3.f*th/sh) < 0.0001f);
				assert(std::abs(depth[i] - 0.65f) < 0.0001f);
				assert(std::abs(confidence[i] - 0.65f) < 0.0001f);
			}
			++cases;
		}
	}
	std::cout << cases << " production consumer-adapter WARP cases: odd/minimum dimensions, motion units, depth and confidence passed.\n";
}
