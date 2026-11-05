#include "pch.h"
#include "ReflexController.h"
#include "FrameTrace.h"
#include "Logger.h"

#ifdef MP_ENABLE_DLSS_FRAME_GENERATION
#include <d3d12.h>
#include <nvapi.h>
#include <nvapi_interface.h>
#endif

namespace Magpie {

#ifdef MP_ENABLE_DLSS_FRAME_GENERATION
namespace {

class NvReflexDriver final : public ReflexDriver {
public:
	~NvReflexDriver() override {
		if (_initialized) _unload();
	}
	bool Initialize(ID3D11Device* device) noexcept {
		_device.copy_from(device);
		const auto dxgiDevice = _device.try_as<IDXGIDevice>();
		winrt::com_ptr<IDXGIAdapter> adapter;
		DXGI_ADAPTER_DESC adapterDesc{};
		if (!dxgiDevice || FAILED(dxgiDevice->GetAdapter(adapter.put())) ||
			FAILED(adapter->GetDesc(&adapterDesc))) {
			ReportFailure("query presentation adapter", NVAPI_INVALID_ARGUMENT);
			return false;
		}
		_adapterLuid = adapterDesc.AdapterLuid;
		// The driver is supplied by Windows/NVIDIA, never by an effect directory.
		_module.reset(LoadLibraryExW(L"nvapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32));
		if (!_module) {
			ReportFailure("load system nvapi64.dll", static_cast<int>(GetLastError()));
			return false;
		}
		_query = reinterpret_cast<QueryInterface>(GetProcAddress(_module.get(), "nvapi_QueryInterface"));
		if (!_query || !_Resolve(_initialize, "NvAPI_Initialize") ||
			!_Resolve(_unload, "NvAPI_Unload") ||
			!_Resolve(_setSleepMode, "NvAPI_D3D_SetSleepMode") ||
			!_Resolve(_getSleepStatus, "NvAPI_D3D_GetSleepStatus") ||
			!_Resolve(_sleep, "NvAPI_D3D_Sleep") ||
			!_Resolve(_marker, "NvAPI_D3D_SetLatencyMarker") ||
			!_Resolve(_async11, "NvAPI_D3D11_SetAsyncFrameMarker") ||
			!_Resolve(_async12, "NvAPI_D3D12_SetAsyncFrameMarker") ||
			!_Resolve(_outOfBand, "NvAPI_D3D12_NotifyOutOfBandCommandQueue")) {
			ReportFailure("resolve native Reflex interfaces (D3D11 async markers require R565+)", NVAPI_NO_IMPLEMENTATION);
			return false;
		}
		const NvAPI_Status status = _initialize();
		_initialized = status == NVAPI_OK;
		if (!_initialized) ReportFailure("NvAPI_Initialize", status);
		return _initialized;
	}
	ReflexConfigurationResult Configure(ReflexSettings settings) noexcept override {
		NV_SET_SLEEP_MODE_PARAMS options{};
		options.version = NV_SET_SLEEP_MODE_PARAMS_VER;
		options.bLowLatencyMode = settings.lowLatency;
		options.bLowLatencyBoost = settings.lowLatency && settings.boost;
		options.minimumIntervalUs = settings.minimumIntervalUs;
		// Keep marker-driven optimization off. Optional frontend out-of-band
		// tracking can be unavailable independently of Sleep/capture/present.
		ReflexConfigurationResult result;
		result.setStatus = _setSleepMode(_device.get(), &options);
		NV_GET_SLEEP_STATUS_PARAMS state{};
		state.version = NV_GET_SLEEP_STATUS_PARAMS_VER;
		if (result.setStatus == NVAPI_OK) {
			result.queried = true;
			result.queryStatus = _getSleepStatus(_device.get(), &state);
			result.lowLatency = result.queryStatus == NVAPI_OK && state.bLowLatencyMode;
		}
		Logger::Get().Info(fmt::format(
			"Reflex configuration: requestedOn={} Boost={} minimumIntervalUs={} "
			"SetSleepMode={} queried={} GetSleepStatus={} actualOn={}; capture-to-present scope",
			settings.lowLatency, settings.boost, settings.minimumIntervalUs, result.setStatus,
			result.queried, result.queryStatus, result.lowLatency));
		return result;
	}
	ReflexSleepStatus QueryStatus() noexcept override {
		NV_GET_SLEEP_STATUS_PARAMS state{};
		state.version = NV_GET_SLEEP_STATUS_PARAMS_VER;
		const int status = _getSleepStatus(_device.get(), &state);
		const bool enabled = status == NVAPI_OK && state.bLowLatencyMode;
		Logger::Get().Info(fmt::format("Reflex warm status: GetSleepStatus={} actualOn={}; read-only observation",
			status, enabled));
		return { status, enabled };
	}
	int Sleep() noexcept override {
		FrameTrace::Scope traceSleep(FrameTrace::Event::ReflexSleep);
		const int status = _sleep(_device.get());
		traceSleep.Data(status);
		return status;
	}
	int Marker(ReflexMarker marker, uint64_t frameId) noexcept override {
		NV_LATENCY_MARKER_PARAMS params{};
		params.version = NV_LATENCY_MARKER_PARAMS_VER;
		params.frameID = frameId;
		switch (marker) {
		case ReflexMarker::SimulationStart: params.markerType = SIMULATION_START; break;
		case ReflexMarker::SimulationEnd: params.markerType = SIMULATION_END; break;
		case ReflexMarker::RenderStart: params.markerType = RENDERSUBMIT_START; break;
		case ReflexMarker::RenderEnd: params.markerType = RENDERSUBMIT_END; break;
		}
		return _marker(_device.get(), &params);
	}
	int RegisterGenerationQueue(ID3D12CommandQueue* queue) noexcept override {
		winrt::com_ptr<ID3D12Device> device;
		if (!queue || FAILED(queue->GetDevice(IID_PPV_ARGS(device.put())))) return NVAPI_INVALID_ARGUMENT;
		const LUID luid = device->GetAdapterLuid();
		if (luid.HighPart != _adapterLuid.HighPart || luid.LowPart != _adapterLuid.LowPart)
			return NVAPI_INVALID_COMBINATION;
		return _outOfBand(queue, OUT_OF_BAND_RENDER);
	}
	int Generation(ID3D12CommandQueue* queue, uint64_t frameId,
		uint64_t presentId, bool start) noexcept override {
		auto params = _AsyncParams(frameId, presentId,
			start ? OUT_OF_BAND_RENDERSUBMIT_START : OUT_OF_BAND_RENDERSUBMIT_END);
		return _async12(queue, &params);
	}
	int FrontendRender(uint64_t frameId, uint64_t presentId, bool start) noexcept override {
		auto params = _AsyncParams(frameId, presentId,
			start ? OUT_OF_BAND_RENDERSUBMIT_START : OUT_OF_BAND_RENDERSUBMIT_END);
		return _async11(_device.get(), &params);
	}
	int Present(uint64_t frameId, uint64_t presentId, bool generated, bool start) noexcept override {
		auto params = _AsyncParams(frameId, presentId, generated
			? (start ? OUT_OF_BAND_PRESENT_START : OUT_OF_BAND_PRESENT_END)
			: (start ? PRESENT_START : PRESENT_END));
		return _async11(_device.get(), &params);
	}
	void ReportFailure(const char* operation, int status) noexcept override {
		Logger::Get().Warn(fmt::format("Reflex call failed: operation={} status={}", operation, status));
	}

private:
	static NV_ASYNC_FRAME_MARKER_PARAMS _AsyncParams(uint64_t frameId,
		uint64_t presentId, NV_LATENCY_MARKER_TYPE marker) noexcept {
		NV_ASYNC_FRAME_MARKER_PARAMS params{};
		params.version = NV_ASYNC_FRAME_MARKER_PARAMS_VER;
		params.frameID = frameId;
		params.presentFrameID = presentId;
		params.markerType = marker;
		// vendorInternal and all reserved fields remain zero.
		return params;
	}
	template<class T> bool _Resolve(T& function, const char* name) noexcept {
		for (const auto& entry : nvapi_interface_table) {
			if (std::strcmp(entry.func, name) == 0) {
				function = reinterpret_cast<T>(_query(entry.id));
				return function != nullptr;
			}
		}
		return false;
	}
	using QueryInterface = void* (__cdecl*)(unsigned int);
	wil::unique_hmodule _module;
	winrt::com_ptr<ID3D11Device> _device;
	LUID _adapterLuid{};
	QueryInterface _query = nullptr;
	decltype(&NvAPI_Initialize) _initialize = nullptr;
	decltype(&NvAPI_Unload) _unload = nullptr;
	decltype(&NvAPI_D3D_SetSleepMode) _setSleepMode = nullptr;
	decltype(&NvAPI_D3D_GetSleepStatus) _getSleepStatus = nullptr;
	decltype(&NvAPI_D3D_Sleep) _sleep = nullptr;
	decltype(&NvAPI_D3D_SetLatencyMarker) _marker = nullptr;
	decltype(&NvAPI_D3D11_SetAsyncFrameMarker) _async11 = nullptr;
	decltype(&NvAPI_D3D12_SetAsyncFrameMarker) _async12 = nullptr;
	decltype(&NvAPI_D3D12_NotifyOutOfBandCommandQueue) _outOfBand = nullptr;
	bool _initialized = false;
};

}
#endif

std::unique_ptr<ReflexDriver> CreateNvReflexDriver(ID3D11Device* presentDevice) noexcept {
#ifdef MP_ENABLE_DLSS_FRAME_GENERATION
	if (!presentDevice) return nullptr;
	auto driver = std::make_unique<NvReflexDriver>();
	if (!driver->Initialize(presentDevice)) return nullptr;
	return driver;
#else
	(void)presentDevice;
	return nullptr;
#endif
}

}
