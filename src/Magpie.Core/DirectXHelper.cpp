#include "pch.h"
#include "DirectXHelper.h"
#include "Logger.h"
#include "StrHelper.h"
#include <d3dcompiler.h>
#include <d3dkmthk.h>

namespace Magpie {

bool DirectXHelper::IsDisplayOnlyAdapter(IDXGIAdapter1* adapter) noexcept {
	DXGI_ADAPTER_DESC1 desc;
	if (FAILED(adapter->GetDesc1(&desc))) {
		return false;
	}

	D3DKMT_OPENADAPTERFROMLUID open{};
	open.AdapterLuid = desc.AdapterLuid;
	if (D3DKMTOpenAdapterFromLuid(&open) < 0) {
		return false;
	}

	D3DKMT_ADAPTERTYPE type{};
	D3DKMT_QUERYADAPTERINFO query{};
	query.hAdapter = open.hAdapter;
	query.Type = KMTQAITYPE_ADAPTERTYPE;
	query.pPrivateDriverData = &type;
	query.PrivateDriverDataSize = sizeof(type);
	const NTSTATUS status = D3DKMTQueryAdapterInfo(&query);

	D3DKMT_CLOSEADAPTER close{};
	close.hAdapter = open.hAdapter;
	D3DKMTCloseAdapter(&close);
	return status >= 0 && type.IndirectDisplayDevice && !type.RenderSupported;
}

bool DirectXHelper::CompileComputeShader(
	std::string_view hlsl,
	const char* entryPoint,
	ID3DBlob** blob,
	const char* sourceName,
	ID3DInclude* include,
	const std::vector<std::pair<std::string, std::string>>& macros,
	bool warningsAreErrors
) {
	winrt::com_ptr<ID3DBlob> errorMsgs = nullptr;

	UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_ALL_RESOURCES_BOUND;
	if (warningsAreErrors) {
		flags |= D3DCOMPILE_WARNINGS_ARE_ERRORS;
	}

#ifdef _DEBUG
	flags |= D3DCOMPILE_SKIP_OPTIMIZATION | D3DCOMPILE_DEBUG;
#else
	flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

	std::unique_ptr<D3D_SHADER_MACRO[]> mc(new D3D_SHADER_MACRO[macros.size() + 1]);
	for (UINT i = 0; i < macros.size(); ++i) {
		mc[i] = { macros[i].first.c_str(), macros[i].second.c_str() };
	}
	mc[macros.size()] = { nullptr,nullptr };

	HRESULT hr = D3DCompile(hlsl.data(), hlsl.size(), sourceName, mc.get(), include,
		entryPoint, "cs_5_0", flags, 0, blob, errorMsgs.put());
	if (FAILED(hr)) {
		if (errorMsgs) {
			Logger::Get().ComError(StrHelper::Concat("编译计算着色器失败: ", (const char*)errorMsgs->GetBufferPointer()), hr);
		}
		return false;
	}

	// 警告消息
	if (errorMsgs) {
		Logger::Get().Warn(StrHelper::Concat("编译计算着色器时产生警告: ", (const char*)errorMsgs->GetBufferPointer()));
	}

	return true;
}

bool DirectXHelper::IsDebugLayersAvailable() noexcept {
#ifdef _DEBUG
	static bool result = SUCCEEDED(D3D11CreateDevice(
		nullptr,
		D3D_DRIVER_TYPE_NULL,       // There is no need to create a real hardware device.
		nullptr,
		D3D11_CREATE_DEVICE_DEBUG,  // Check for the SDK layers.
		nullptr,                    // Any feature level will do.
		0,
		D3D11_SDK_VERSION,
		nullptr,                    // No need to keep the D3D device reference.
		nullptr,                    // No need to know the feature level.
		nullptr                     // No need to keep the D3D device context reference.
	));
	return result;
#else
	// Relaese 配置不使用调试层
	return false;
#endif
}

winrt::com_ptr<ID3D11Texture2D> DirectXHelper::CreateTexture2D(
	ID3D11Device* d3dDevice,
	DXGI_FORMAT format,
	UINT width,
	UINT height,
	UINT bindFlags,
	D3D11_USAGE usage,
	UINT miscFlags,
	const D3D11_SUBRESOURCE_DATA* pInitialData
) noexcept {
	const D3D11_TEXTURE2D_DESC desc{
		.Width = width,
		.Height = height,
		.MipLevels = 1,
		.ArraySize = 1,
		.Format = format,
		.SampleDesc{
			.Count = 1,
			.Quality = 0
		},
		.Usage = usage,
		.BindFlags = bindFlags,
		.MiscFlags = miscFlags
	};

	winrt::com_ptr<ID3D11Texture2D> result;
	HRESULT hr = d3dDevice->CreateTexture2D(&desc, pInitialData, result.put());
	if (FAILED(hr)) {
		Logger::Get().ComError("CreateTexture2D 失败", hr);
		return nullptr;
	}

	return result;
}

winrt::com_ptr<ID3D11Texture2D> DirectXHelper::CreateSharedTexture2D(
	ID3D11Device* d3dDevice,
	DXGI_FORMAT format,
	UINT width,
	UINT height,
	UINT bindFlags,
	std::string_view role
) noexcept {
	constexpr UINT MISC_FLAGS =
		D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
	const D3D11_TEXTURE2D_DESC desc{
		.Width = width,
		.Height = height,
		.MipLevels = 1,
		.ArraySize = 1,
		.Format = format,
		.SampleDesc{ .Count = 1, .Quality = 0 },
		.Usage = D3D11_USAGE_DEFAULT,
		.BindFlags = bindFlags,
		.MiscFlags = MISC_FLAGS
	};
	auto logFailure = [&](std::string_view operation, HRESULT hr) noexcept {
		Logger::Get().ComError(fmt::format(
			"{} shared Frame Guidance texture failed: role={} format={} size={}x{} "
			"BindFlags={:#x} MiscFlags={:#x}",
			operation, role, static_cast<uint32_t>(format), width, height,
			bindFlags, MISC_FLAGS), hr);
	};

	if (!d3dDevice || !width || !height) {
		logFailure("Validate", E_INVALIDARG);
		return nullptr;
	}
	winrt::com_ptr<ID3D11Texture2D> texture;
	HRESULT hr = d3dDevice->CreateTexture2D(&desc, nullptr, texture.put());
	if (FAILED(hr)) {
		logFailure("Create", hr);
		return nullptr;
	}

	winrt::com_ptr<IDXGIResource1> resource;
	hr = texture->QueryInterface(IID_PPV_ARGS(resource.put()));
	if (FAILED(hr)) {
		logFailure("Query IDXGIResource1 for", hr);
		return nullptr;
	}
	HANDLE rawHandle = nullptr;
	hr = resource->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &rawHandle);
	if (FAILED(hr) || !rawHandle) {
		logFailure("Create NT handle for", FAILED(hr) ? hr : E_HANDLE);
		return nullptr;
	}
	wil::unique_handle validationHandle(rawHandle);
	return texture;
}

}
