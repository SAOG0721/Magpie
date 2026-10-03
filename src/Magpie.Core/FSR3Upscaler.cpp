#include "pch.h"
#include "FSR3Upscaler.h"
#include "DeviceResources.h"
#include "Logger.h"
#include "NativeOutputValidation.h"

#ifdef MP_ENABLE_FSR3_ZEROMV
#include <d3d12.h>
#include <ffx_api.h>
#include <dx12/ffx_api_dx12.h>
#include <ffx_upscale.h>

namespace Magpie {

struct FSR3Upscaler::Impl {
	~Impl();

	ID3D11Device5* device11 = nullptr;
	ID3D11DeviceContext4* context11 = nullptr;
	winrt::com_ptr<ID3D12Device> device12;
	winrt::com_ptr<ID3D12CommandQueue> queue12;
	winrt::com_ptr<ID3D12CommandAllocator> allocator12;
	winrt::com_ptr<ID3D12GraphicsCommandList> commandList12;
	winrt::com_ptr<ID3D11Texture2D> sharedInput11;
	winrt::com_ptr<ID3D11Texture2D> sharedOutput11;
	winrt::com_ptr<ID3D11Texture2D> sharedMotion11;
	winrt::com_ptr<ID3D12Resource> sharedInput12;
	winrt::com_ptr<ID3D12Resource> sharedOutput12;
	winrt::com_ptr<ID3D12Resource> sharedMotion12;
	winrt::com_ptr<ID3D12Resource> zeroMotion12;
	winrt::com_ptr<ID3D12Resource> flatDepth12;
	winrt::com_ptr<ID3D12Resource> exposure12;
	winrt::com_ptr<ID3D12Resource> reactive12;
	winrt::com_ptr<ID3D12Resource> transparency12;
	winrt::com_ptr<ID3D12DescriptorHeap> descriptorHeap12;
	winrt::com_ptr<ID3D11Fence> fence11;
	winrt::com_ptr<ID3D12Fence> fence12;
	HMODULE loaderModule = nullptr;
	HMODULE providerModule = nullptr;
	decltype(&ffxCreateContext) createContext = nullptr;
	decltype(&ffxDestroyContext) destroyContext = nullptr;
	decltype(&ffxDispatch) dispatch = nullptr;
	decltype(&ffxQuery) query = nullptr;
	ffxContext context = nullptr;
	ffxCreateContextDescUpscale createDesc{};
	ffxCreateBackendDX12Desc backendDesc{};
	ffxCreateContextDescUpscaleVersion apiVersion{};
	ffxOverrideVersion overrideVersion{};
	uint64_t dispatchCount = 0;
	int64_t previousTimestamp100ns = 0;
	uint64_t selectedProvider = 0;
	std::string selectedProviderName;
	uint64_t fenceValue = 0;
	uint64_t lastSubmittedValue = 0;
	uint32_t inputWidth = 0;
	uint32_t inputHeight = 0;
	uint32_t outputWidth = 0;
	uint32_t outputHeight = 0;
	bool enableOpticalFlow = false;
	FsrHdrProtocol hdrProtocol{};
	bool useFsr4 = false;
	bool resetHistory = true;
	FrameGuidanceFrameId lastGuidanceResetFrameId = std::numeric_limits<FrameGuidanceFrameId>::max();
};

static bool WaitForFence(FSR3Upscaler::Impl& impl, uint64_t value) noexcept {
	if (!value || impl.fence12->GetCompletedValue() >= value) return true;
	wil::unique_event_nothrow event;
	HRESULT hr = event.create();
	if (FAILED(hr)) return false;
	hr = impl.fence12->SetEventOnCompletion(value, event.get());
	if (FAILED(hr)) return false;
	WaitForSingleObject(event.get(), INFINITE);
	return true;
}

static bool WaitForQueue(FSR3Upscaler::Impl& impl) noexcept {
	const uint64_t value = ++impl.fenceValue;
	HRESULT hr = impl.queue12->Signal(impl.fence12.get(), value);
	if (FAILED(hr)) return false;
	return WaitForFence(impl, value);
}

FSR3Upscaler::Impl::~Impl() {
	if (queue12 && fence12) WaitForQueue(*this);
	if (context && destroyContext) destroyContext(&context, nullptr);
	if (loaderModule) FreeLibrary(loaderModule);
	if (providerModule) FreeLibrary(providerModule);
}

static bool IsAddressInExecutableSection(
	const uint8_t* base,
	const IMAGE_NT_HEADERS* nt,
	const void* address
) noexcept {
	const uintptr_t value = reinterpret_cast<uintptr_t>(address);
	for (const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
		section != IMAGE_FIRST_SECTION(nt) + nt->FileHeader.NumberOfSections; ++section) {
		if (!(section->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
		const uintptr_t begin = reinterpret_cast<uintptr_t>(base) + section->VirtualAddress;
		const uintptr_t end = begin + std::max(section->Misc.VirtualSize, section->SizeOfRawData);
		if (value >= begin && value < end) return true;
	}
	return false;
}

// FSR 4.1.1 contains a dedicated INT8 provider but hides it when IsSupported
// rejects the real adapter. Patch only that provider's virtual support check in
// this process. This avoids changing the system adapter identity or other apps.
static bool ForceFsr4Int8ProviderSupport(HMODULE module) noexcept {
	if (!module) return false;
	uint8_t* base = reinterpret_cast<uint8_t*>(module);
	const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
	if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
	const IMAGE_NT_HEADERS* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

	constexpr char typeName[] = ".?AVffxProvider_FSR4_Int8@@";
	const uint8_t* typeNameAddress = nullptr;
	const IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(nt);
	for (uint16_t i = 0; i < nt->FileHeader.NumberOfSections && !typeNameAddress; ++i) {
		const IMAGE_SECTION_HEADER& section = sections[i];
		if (!(section.Characteristics & IMAGE_SCN_MEM_READ)) continue;
		const uint8_t* begin = base + section.VirtualAddress;
		const size_t size = std::max(section.Misc.VirtualSize, section.SizeOfRawData);
		if (size < sizeof(typeName)) continue;
		for (size_t offset = 0; offset + sizeof(typeName) <= size; ++offset) {
			if (memcmp(begin + offset, typeName, sizeof(typeName)) == 0) {
				typeNameAddress = begin + offset;
				break;
			}
		}
	}
	if (!typeNameAddress || typeNameAddress < base + 2 * sizeof(void*)) return false;

	// MSVC x64 TypeDescriptor stores two pointers immediately before its name.
	const uint8_t* typeDescriptor = typeNameAddress - 2 * sizeof(void*);
	const uint32_t typeDescriptorRva = static_cast<uint32_t>(typeDescriptor - base);
	struct CompleteObjectLocator {
		uint32_t signature;
		uint32_t offset;
		uint32_t cdOffset;
		uint32_t typeDescriptorRva;
		uint32_t classDescriptorRva;
		uint32_t selfRva;
	};

	for (uint16_t i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
		const IMAGE_SECTION_HEADER& colSection = sections[i];
		if (!(colSection.Characteristics & IMAGE_SCN_MEM_READ)) continue;
		uint8_t* colBegin = base + colSection.VirtualAddress;
		const size_t colSize = std::max(colSection.Misc.VirtualSize, colSection.SizeOfRawData);
		for (size_t colOffset = 0; colOffset + sizeof(CompleteObjectLocator) <= colSize;
			colOffset += alignof(uint32_t)) {
			const auto* col = reinterpret_cast<const CompleteObjectLocator*>(colBegin + colOffset);
			const uint32_t colRva = static_cast<uint32_t>(colBegin + colOffset - base);
			if (col->signature != 1 || col->typeDescriptorRva != typeDescriptorRva ||
				col->selfRva != colRva) continue;

			const uintptr_t colAddress = reinterpret_cast<uintptr_t>(col);
			for (uint16_t j = 0; j < nt->FileHeader.NumberOfSections; ++j) {
				const IMAGE_SECTION_HEADER& tableSection = sections[j];
				if (!(tableSection.Characteristics & IMAGE_SCN_MEM_READ)) continue;
				uint8_t* tableBegin = base + tableSection.VirtualAddress;
				const size_t tableSize = std::max(tableSection.Misc.VirtualSize, tableSection.SizeOfRawData);
				for (size_t tableOffset = 0; tableOffset + 4 * sizeof(void*) <= tableSize;
					tableOffset += alignof(void*)) {
					const auto* locatorPointer = reinterpret_cast<const uintptr_t*>(tableBegin + tableOffset);
					if (*locatorPointer != colAddress) continue;
					void** vtable = reinterpret_cast<void**>(tableBegin + tableOffset + sizeof(void*));
					// MSVC order: deleting destructor, CanProvide, IsSupported.
					uint8_t* supportFunction = reinterpret_cast<uint8_t*>(vtable[2]);
					if (!IsAddressInExecutableSection(base, nt, supportFunction)) continue;

					DWORD oldProtect = 0;
					if (!VirtualProtect(supportFunction, 6, PAGE_EXECUTE_READWRITE, &oldProtect)) return false;
					const uint8_t returnTrue[]{ 0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3 };
					memcpy(supportFunction, returnTrue, sizeof(returnTrue));
					FlushInstructionCache(GetCurrentProcess(), supportFunction, sizeof(returnTrue));
					DWORD ignored = 0;
					VirtualProtect(supportFunction, 6, oldProtect, &ignored);
					Logger::Get().Info("Enabled process-local FSR 4.1.1 INT8 provider support override");
					return true;
				}
			}
		}
	}
	return false;
}

static bool CreateSharedTexture(
	FSR3Upscaler::Impl& impl,
	const D3D11_TEXTURE2D_DESC& sourceDesc,
	winrt::com_ptr<ID3D11Texture2D>& texture11,
	winrt::com_ptr<ID3D12Resource>& texture12
) noexcept {
	D3D11_TEXTURE2D_DESC desc = sourceDesc;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.CPUAccessFlags = 0;
	desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
	HRESULT hr = impl.device11->CreateTexture2D(&desc, nullptr, texture11.put());
	if (FAILED(hr)) {
		Logger::Get().ComError("Create FSR3 shared D3D11 texture failed", hr);
		return false;
	}
	winrt::com_ptr<IDXGIResource1> dxgiResource;
	hr = texture11->QueryInterface(IID_PPV_ARGS(dxgiResource.put()));
	if (FAILED(hr)) return false;
	HANDLE rawHandle = nullptr;
	hr = dxgiResource->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &rawHandle);
	if (FAILED(hr)) return false;
	wil::unique_handle handle(rawHandle);
	hr = impl.device12->OpenSharedHandle(handle.get(), IID_PPV_ARGS(texture12.put()));
	if (FAILED(hr)) {
		Logger::Get().ComError("Open FSR3 shared texture in D3D12 failed", hr);
		return false;
	}
	return true;
}

static bool CreateAuxTexture(
	FSR3Upscaler::Impl& impl,
	DXGI_FORMAT format,
	uint32_t width,
	uint32_t height,
	winrt::com_ptr<ID3D12Resource>& resource,
	D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle,
	D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle,
	const float clearValue[4],
	D3D12_RESOURCE_BARRIER& barrier
) noexcept {
	D3D12_RESOURCE_DESC desc{};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.Width = width;
	desc.Height = height;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.Format = format;
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	D3D12_HEAP_PROPERTIES heap{};
	heap.Type = D3D12_HEAP_TYPE_DEFAULT;
	HRESULT hr = impl.device12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(resource.put()));
	if (FAILED(hr)) {
		Logger::Get().ComError("Create FSR3 virtual input texture failed", hr);
		return false;
	}
	D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
	uav.Format = format;
	uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
	impl.device12->CreateUnorderedAccessView(resource.get(), nullptr, &uav, cpuHandle);
	impl.commandList12->ClearUnorderedAccessViewFloat(
		gpuHandle, cpuHandle, resource.get(), clearValue, 0, nullptr);
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = resource.get();
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	return true;
}

FSR3Upscaler::FSR3Upscaler() = default;

FSR3Upscaler::~FSR3Upscaler() = default;

bool FSR3Upscaler::Initialize(
	DeviceResources& resources,
	ID3D11Texture2D* input,
	ID3D11Texture2D* output,
	MotionVectorRequest motionRequest,
	bool useFsr4
) noexcept {
	_motionRequest = motionRequest;
	const bool enableOpticalFlow = motionRequest.method != OpticalFlowMethod::None;
	_useFsr4 = useFsr4;
	_impl.reset();
	auto impl = std::make_unique<Impl>();
	impl->device11 = resources.GetD3DDevice();
	impl->context11 = resources.GetD3DDC();
	impl->enableOpticalFlow = enableOpticalFlow;
	impl->hdrProtocol = _hdrProtocol;
	impl->useFsr4 = useFsr4;
	const char* upscalerName = useFsr4 ? "FSR 4.1.1" : "FSR 3.1.5";

	D3D11_TEXTURE2D_DESC inputDesc{};
	D3D11_TEXTURE2D_DESC outputDesc{};
	input->GetDesc(&inputDesc);
	output->GetDesc(&outputDesc);
	if (inputDesc.Width > outputDesc.Width || inputDesc.Height > outputDesc.Height) {
		Logger::Get().Error("FSR3 experimental backend only supports upscaling");
		return false;
	}
	if ((float)outputDesc.Width / inputDesc.Width > 3.0f ||
		(float)outputDesc.Height / inputDesc.Height > 3.0f) {
		Logger::Get().Error("FSR3 experimental backend supports up to a 3x scale");
		return false;
	}
	impl->inputWidth = inputDesc.Width;
	impl->inputHeight = inputDesc.Height;
	impl->outputWidth = outputDesc.Width;
	impl->outputHeight = outputDesc.Height;

	HRESULT hr = D3D12CreateDevice(resources.GetGraphicsAdapter(), D3D_FEATURE_LEVEL_11_0,
		IID_PPV_ARGS(impl->device12.put()));
	if (FAILED(hr)) {
		Logger::Get().ComError("Create FSR3 D3D12 device failed", hr);
		return false;
	}
	D3D12_COMMAND_QUEUE_DESC queueDesc{};
	queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	hr = impl->device12->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(impl->queue12.put()));
	if (SUCCEEDED(hr)) hr = impl->device12->CreateCommandAllocator(
		D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(impl->allocator12.put()));
	if (SUCCEEDED(hr)) hr = impl->device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
		impl->allocator12.get(), nullptr, IID_PPV_ARGS(impl->commandList12.put()));
	if (FAILED(hr)) {
		Logger::Get().ComError("Create FSR3 D3D12 command objects failed", hr);
		return false;
	}

	if (!CreateSharedTexture(*impl, inputDesc, impl->sharedInput11, impl->sharedInput12) ||
		!CreateSharedTexture(*impl, outputDesc, impl->sharedOutput11, impl->sharedOutput12)) {
		return false;
	}
	if (enableOpticalFlow) {
		D3D11_TEXTURE2D_DESC motionDesc{};
		motionDesc.Width = inputDesc.Width;
		motionDesc.Height = inputDesc.Height;
		motionDesc.MipLevels = 1;
		motionDesc.ArraySize = 1;
		motionDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
		motionDesc.SampleDesc.Count = 1;
		motionDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		if (!CreateSharedTexture(*impl, motionDesc, impl->sharedMotion11, impl->sharedMotion12)) return false;
	}

	D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
	heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	heapDesc.NumDescriptors = 5;
	heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	hr = impl->device12->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(impl->descriptorHeap12.put()));
	if (FAILED(hr)) return false;
	ID3D12DescriptorHeap* heaps[]{ impl->descriptorHeap12.get() };
	impl->commandList12->SetDescriptorHeaps(1, heaps);
	const UINT stride = impl->device12->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	D3D12_CPU_DESCRIPTOR_HANDLE cpu = impl->descriptorHeap12->GetCPUDescriptorHandleForHeapStart();
	D3D12_GPU_DESCRIPTOR_HANDLE gpu = impl->descriptorHeap12->GetGPUDescriptorHandleForHeapStart();
	D3D12_RESOURCE_BARRIER auxBarriers[5]{};
	UINT auxCount = 0;
	const float zero[4]{};
	const float one[4]{ impl->hdrProtocol.exposure, impl->hdrProtocol.exposure,
		impl->hdrProtocol.exposure, impl->hdrProtocol.exposure };
	const float reactive02[4]{ 0.2f, 0.2f, 0.2f, 0.2f };
	const float reactiveFsr4OpticalFlow[4]{ 0.8f, 0.8f, 0.8f, 0.8f };
	const float reactive08[4]{ 0.8f, 0.8f, 0.8f, 0.8f };
	const float reactiveFsr4ZeroMv[4]{ 1.0f, 1.0f, 1.0f, 1.0f };
	auto addAux = [&](DXGI_FORMAT format, uint32_t width, uint32_t height,
		winrt::com_ptr<ID3D12Resource>& texture, const float value[4]) -> bool {
		if (!CreateAuxTexture(*impl, format, width, height, texture, cpu, gpu, value,
			auxBarriers[auxCount])) return false;
		++auxCount;
		cpu.ptr += stride;
		gpu.ptr += stride;
		return true;
	};
	if (!addAux(DXGI_FORMAT_R32_FLOAT, inputDesc.Width, inputDesc.Height, impl->flatDepth12, zero) ||
		!addAux(DXGI_FORMAT_R32_FLOAT, 1, 1, impl->exposure12, one) ||
		!addAux(DXGI_FORMAT_R8_UNORM, inputDesc.Width, inputDesc.Height, impl->reactive12,
			useFsr4 ? (enableOpticalFlow ? reactiveFsr4OpticalFlow : reactiveFsr4ZeroMv) :
				(enableOpticalFlow ? reactive02 : reactive08)) ||
		!addAux(DXGI_FORMAT_R8_UNORM, inputDesc.Width, inputDesc.Height, impl->transparency12, zero)) {
		return false;
	}
	if (!enableOpticalFlow && !addAux(DXGI_FORMAT_R16G16_FLOAT, inputDesc.Width, inputDesc.Height,
		impl->zeroMotion12, zero)) return false;
	impl->commandList12->ResourceBarrier(auxCount, auxBarriers);
	hr = impl->commandList12->Close();
	if (FAILED(hr)) return false;
	ID3D12CommandList* lists[]{ impl->commandList12.get() };
	impl->queue12->ExecuteCommandLists(1, lists);

	hr = impl->device11->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(impl->fence11.put()));
	if (FAILED(hr)) return false;
	HANDLE rawFence = nullptr;
	hr = impl->fence11->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &rawFence);
	if (FAILED(hr)) return false;
	wil::unique_handle fenceHandle(rawFence);
	hr = impl->device12->OpenSharedHandle(fenceHandle.get(), IID_PPV_ARGS(impl->fence12.put()));
	if (FAILED(hr) || !WaitForQueue(*impl)) return false;

	if (useFsr4) {
		impl->providerModule = LoadLibraryW(L"amd_fidelityfx_upscaler_dx12.dll");
		if (!impl->providerModule) {
			Logger::Get().Win32Error("Load FSR 4.1.1 provider DLL failed");
			return false;
		}
		if (!ForceFsr4Int8ProviderSupport(impl->providerModule)) {
			Logger::Get().Error("Locate FSR 4.1.1 INT8 provider support check failed");
			return false;
		}
	}

	impl->loaderModule = LoadLibraryW(L"amd_fidelityfx_loader_dx12.dll");
	if (!impl->loaderModule) {
		Logger::Get().Win32Error("Load amd_fidelityfx_loader_dx12.dll failed");
		return false;
	}
	impl->createContext = reinterpret_cast<decltype(impl->createContext)>(
		GetProcAddress(impl->loaderModule, "ffxCreateContext"));
	impl->destroyContext = reinterpret_cast<decltype(impl->destroyContext)>(
		GetProcAddress(impl->loaderModule, "ffxDestroyContext"));
	impl->dispatch = reinterpret_cast<decltype(impl->dispatch)>(
		GetProcAddress(impl->loaderModule, "ffxDispatch"));
	impl->query = reinterpret_cast<decltype(impl->query)>(
		GetProcAddress(impl->loaderModule, "ffxQuery"));
	if (!impl->createContext || !impl->destroyContext || !impl->dispatch || !impl->query) {
		Logger::Get().Error("AMD FidelityFX loader exports are incomplete");
		return false;
	}

	ffxQueryDescGetVersions versionQuery{};
	versionQuery.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
	versionQuery.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
	versionQuery.device = impl->device12.get();
	uint64_t versionCount = 0;
	versionQuery.outputCount = &versionCount;
	ffxReturnCode_t rc = impl->query(nullptr, &versionQuery.header);
	if (rc != FFX_API_RETURN_OK || !versionCount) {
		Logger::Get().Error(fmt::format("Query FSR upscaler versions failed ({})", (uint32_t)rc));
		return false;
	}
	std::vector<uint64_t> versionIds(versionCount);
	std::vector<const char*> versionNames(versionCount);
	versionQuery.versionIds = versionIds.data();
	versionQuery.versionNames = versionNames.data();
	rc = impl->query(nullptr, &versionQuery.header);
	if (rc != FFX_API_RETURN_OK) return false;
	uint64_t selectedVersionId = 0;
	const char* requestedVersion = useFsr4 ? "4.1.1" : "3.1.5";
	std::string availableVersions;
	for (uint64_t i = 0; i < versionCount; ++i) {
		const char* name = versionNames[i] ? versionNames[i] : "unknown";
		if (!availableVersions.empty()) availableVersions += ", ";
		availableVersions += name;
		if (strstr(name, requestedVersion)) {
			selectedVersionId = versionIds[i];
			impl->selectedProviderName = name;
		}
	}
	if (!selectedVersionId) {
		Logger::Get().Error(fmt::format("{} provider not found; available: {}", upscalerName, availableVersions));
		return false;
	}

	impl->selectedProvider = selectedVersionId;
	Logger::Get().Info(fmt::format("FSR provider selected: id={} name={} requested={} available={}",
		selectedVersionId, impl->selectedProviderName, requestedVersion, availableVersions));
	impl->createDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
	impl->createDesc.flags =
		(impl->hdrProtocol.depthInverted ? FFX_UPSCALE_ENABLE_DEPTH_INVERTED : 0) |
		(impl->hdrProtocol.depthInfinite ? FFX_UPSCALE_ENABLE_DEPTH_INFINITE : 0);
	if (!impl->hdrProtocol.hdrColorInput) {
		impl->createDesc.flags |= FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;
	}
	impl->createDesc.maxRenderSize = { inputDesc.Width, inputDesc.Height };
	impl->createDesc.maxUpscaleSize = { outputDesc.Width, outputDesc.Height };
	impl->backendDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
	impl->backendDesc.device = impl->device12.get();
	impl->apiVersion.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
	impl->apiVersion.version = FFX_UPSCALER_VERSION;
	impl->overrideVersion.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
	impl->overrideVersion.versionId = selectedVersionId;
	impl->createDesc.header.pNext = &impl->backendDesc.header;
	impl->backendDesc.header.pNext = &impl->apiVersion.header;
	impl->apiVersion.header.pNext = &impl->overrideVersion.header;
	rc = impl->createContext(&impl->context, &impl->createDesc.header, nullptr);
	if (rc != FFX_API_RETURN_OK || !impl->context) {
		const char* hint = useFsr4
			? "; FSR4 INT8 may still be rejected by GPU capability detection"
			: "";
		Logger::Get().Error(fmt::format("Create {} context failed ({}){}",
			upscalerName, (uint32_t)rc, hint));
		return false;
	}
	Logger::Get().Info(fmt::format(
		"{} D3D11/D3D12 backend initialized (opticalFlow={}, virtual auxiliary inputs): {}x{} -> {}x{}",
		upscalerName, enableOpticalFlow,
		inputDesc.Width, inputDesc.Height, outputDesc.Width, outputDesc.Height));
	_impl = std::move(impl);
	return true;
}

bool FSR3Upscaler::Resize(DeviceResources& resources, ID3D11Texture2D* input,
	ID3D11Texture2D* output) noexcept {
	return Initialize(resources, input, output, _motionRequest, _useFsr4);
}

bool FSR3Upscaler::Draw(const NativeEffectDrawContext& drawContext) noexcept {
	ID3D11Texture2D* input = drawContext.input;
	ID3D11Texture2D* output = drawContext.output;
	if (!_impl || !_impl->context) return false;
	Impl& impl = *_impl;
	const auto dispatchStart = std::chrono::steady_clock::now();
	const bool guidanceReset = drawContext.frameGuidance.requiresHistoryReset &&
		impl.lastGuidanceResetFrameId != drawContext.frameId;
	impl.resetHistory |= guidanceReset || drawContext.inputHistoryReset || !drawContext.isNewCaptureFrame;
	if (!WaitForFence(impl, impl.lastSubmittedValue)) return false;
	impl.context11->CopyResource(impl.sharedInput11.get(), input);
	if (impl.enableOpticalFlow) {
		if (!drawContext.frameGuidance.IsValidFor(drawContext.frameId, { impl.inputWidth, impl.inputHeight })) return false;
		const auto sync = drawContext.frameGuidance.motion.metadata.sync;
		if (sync.fence && sync.value && FAILED(impl.context11->Wait(sync.fence, sync.value))) return false;
		impl.context11->CopyResource(impl.sharedMotion11.get(), drawContext.frameGuidance.motion.texture);
	}
	const uint64_t inputReady = ++impl.fenceValue;
	HRESULT hr = impl.context11->Signal(impl.fence11.get(), inputReady);
	if (FAILED(hr)) return false;
	impl.context11->Flush();
	hr = impl.queue12->Wait(impl.fence12.get(), inputReady);
	if (FAILED(hr)) return false;

	hr = impl.allocator12->Reset();
	if (SUCCEEDED(hr)) hr = impl.commandList12->Reset(impl.allocator12.get(), nullptr);
	if (FAILED(hr)) return false;
	D3D12_RESOURCE_BARRIER barriers[3]{};
	barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barriers[0].Transition = { impl.sharedInput12.get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
		D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE };
	barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barriers[1].Transition = { impl.sharedOutput12.get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
		D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS };
	const UINT barrierCount = impl.enableOpticalFlow ? 3 : 2;
	if (impl.enableOpticalFlow) {
		barriers[2].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barriers[2].Transition = { impl.sharedMotion12.get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
			D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE };
	}
	impl.commandList12->ResourceBarrier(barrierCount, barriers);

	ffxDispatchDescUpscale desc{};
	desc.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
	desc.commandList = impl.commandList12.get();
	desc.color = ffxApiGetResourceDX12(impl.sharedInput12.get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
	desc.depth = ffxApiGetResourceDX12(impl.flatDepth12.get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
	desc.motionVectors = ffxApiGetResourceDX12(
		impl.enableOpticalFlow ? impl.sharedMotion12.get() : impl.zeroMotion12.get(),
		FFX_API_RESOURCE_STATE_COMPUTE_READ);
	desc.exposure = ffxApiGetResourceDX12(impl.exposure12.get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
	desc.reactive = ffxApiGetResourceDX12(impl.reactive12.get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
	desc.transparencyAndComposition = ffxApiGetResourceDX12(
		impl.transparency12.get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
	desc.output = ffxApiGetResourceDX12(impl.sharedOutput12.get(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
	desc.jitterOffset = { 0.0f, 0.0f };
	desc.motionVectorScale = { 1.0f, 1.0f };
	desc.renderSize = { impl.inputWidth, impl.inputHeight };
	desc.upscaleSize = { impl.outputWidth, impl.outputHeight };
	desc.enableSharpening = true;
	desc.sharpness = 0.2f;
	const auto timestamp = drawContext.frameGuidance.motion.metadata.timestamp100ns;
	desc.frameTimeDelta = impl.previousTimestamp100ns && timestamp > impl.previousTimestamp100ns
		? std::clamp(float(timestamp - impl.previousTimestamp100ns) / 10000.0f, 1.0f, 1000.0f) : 16.6667f;
	if (drawContext.isNewCaptureFrame && timestamp > 0) impl.previousTimestamp100ns = timestamp;
	desc.preExposure = impl.hdrProtocol.preExposure;
	desc.reset = impl.resetHistory;
	desc.cameraNear = 1.0f;
	desc.cameraFar = FLT_MAX;
	desc.cameraFovAngleVertical = 1.04719755f;
	desc.viewSpaceToMetersFactor = 1.0f;
	desc.flags = 0;
	if (!impl.hdrProtocol.hdrColorInput) {
		desc.flags = FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;
	}
	const ffxReturnCode_t rc = impl.dispatch(&impl.context, &desc.header);
	if (rc != FFX_API_RETURN_OK) {
		Logger::Get().Error(fmt::format("Dispatch {} failed ({})",
			impl.useFsr4 ? "FSR 4.1.1" : "FSR 3.1.5", (uint32_t)rc));
		return false;
	}
	for (UINT i = 0; i < barrierCount; ++i) {
		std::swap(barriers[i].Transition.StateBefore, barriers[i].Transition.StateAfter);
	}
	impl.commandList12->ResourceBarrier(barrierCount, barriers);
	hr = impl.commandList12->Close();
	if (FAILED(hr)) return false;
	ID3D12CommandList* lists[]{ impl.commandList12.get() };
	impl.queue12->ExecuteCommandLists(1, lists);
	const uint64_t outputReady = ++impl.fenceValue;
	hr = impl.queue12->Signal(impl.fence12.get(), outputReady);
	impl.lastSubmittedValue = outputReady;
	if (SUCCEEDED(hr)) hr = impl.context11->Wait(impl.fence11.get(), outputReady);
	if (FAILED(hr)) return false;
	impl.context11->CopyResource(output, impl.sharedOutput11.get());
	if (++impl.dispatchCount <= 3 || impl.dispatchCount % 60 == 0) {
		if (!WaitForFence(impl, outputReady)) return false;
		Logger::Get().Info(fmt::format("FSR dispatch completed: provider={} name={} frame={} input={}x{} output={}x{} rc={} synchronized_wall_ms={:.3f} depth=zero jitter=zero motion={}",
			impl.selectedProvider, impl.selectedProviderName, drawContext.frameId,
			impl.inputWidth, impl.inputHeight, impl.outputWidth, impl.outputHeight, uint32_t(rc),
			std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - dispatchStart).count(),
			impl.enableOpticalFlow ? "estimated" : "zero"));
	}
	if (impl.dispatchCount == 2 && !ValidateNativeOutput(impl.device11, impl.context11, input, output,
		impl.useFsr4 ? "FSR4" : "FSR3")) return false;
	impl.resetHistory = false;
	if (guidanceReset) impl.lastGuidanceResetFrameId = drawContext.frameId;
	return true;
}

}

#else

namespace Magpie {
struct FSR3Upscaler::Impl {};
FSR3Upscaler::FSR3Upscaler() = default;
FSR3Upscaler::~FSR3Upscaler() = default;
bool FSR3Upscaler::Initialize(DeviceResources&, ID3D11Texture2D*, ID3D11Texture2D*, MotionVectorRequest, bool) noexcept {
	Logger::Get().Error("FSR3 support is not enabled in this build");
	return false;
}
bool FSR3Upscaler::Resize(DeviceResources&, ID3D11Texture2D*, ID3D11Texture2D*) noexcept { return false; }
bool FSR3Upscaler::Draw(const NativeEffectDrawContext&) noexcept { return false; }
}

#endif
