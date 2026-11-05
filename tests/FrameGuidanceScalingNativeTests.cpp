// Isolated native acceptance: actual providers and service, no user settings.
#include "pch.h"
#include "ScalingOptions.h"
#include <parallel_hashmap/phmap.h>
#define private public
#include "DeviceResources.h"
#include "FrameGuidanceService.h"
#undef private
#include "FrameGuidanceService.cpp"
#include <d3d11sdklayers.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <iostream>
#include <cmath>
#include <stdexcept>

using namespace Magpie;
static winrt::com_ptr<ID3D11InfoQueue> messages11;
static winrt::com_ptr<ID3D12InfoQueue> messages12;
static bool debugFailure = false;
static void Require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
static void Check(HRESULT hr, const char* message) { Require(SUCCEEDED(hr), message); }

static bool DebugClean() {
	bool clean = true;
	for (UINT64 i = 0; i < messages11->GetNumStoredMessages(); ++i) {
		SIZE_T bytes = 0; messages11->GetMessage(i, nullptr, &bytes);
		std::vector<uint8_t> storage(bytes); auto* message = reinterpret_cast<D3D11_MESSAGE*>(storage.data());
		if (SUCCEEDED(messages11->GetMessage(i, message, &bytes)) && message->Severity <= D3D11_MESSAGE_SEVERITY_WARNING) {
			std::cout << "D3D11_GATE " << message->ID << " " << message->pDescription << std::endl; clean = false;
		}
	}
	for (UINT64 i = 0; i < messages12->GetNumStoredMessages(); ++i) {
		SIZE_T bytes = 0; messages12->GetMessage(i, nullptr, &bytes);
		std::vector<uint8_t> storage(bytes); auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
		if (SUCCEEDED(messages12->GetMessage(i, message, &bytes)) && message->Severity <= D3D12_MESSAGE_SEVERITY_WARNING) {
			std::cout << "D3D12_GATE " << message->ID << " " << message->pDescription << std::endl; clean = false;
		}
	}
	messages11->ClearStoredMessages(); messages12->ClearStoredMessages();
	debugFailure |= !clean;
	return clean;
}

static LONG CALLBACK RecordFailure(EXCEPTION_POINTERS* exception) {
	if (exception->ExceptionRecord->ExceptionCode == 0xe06d7363 && messages11 && messages12) {
		DebugClean(); Logger::Get().Flush();
	}
	return EXCEPTION_CONTINUE_SEARCH;
}

static winrt::com_ptr<ID3D11Texture2D> Color(DeviceResources& resources, FrameGuidanceExtent extent, unsigned frame) {
	std::vector<uint32_t> pixels(size_t(extent.width) * extent.height);
	for (uint32_t y = 0; y < extent.height; ++y) for (uint32_t x = 0; x < extent.width; ++x) {
		const unsigned sx = x + frame * 2, sy = y + frame;
		const unsigned a = ((sx / 13) ^ (sy / 11)) & 1;
		pixels[size_t(y) * extent.width + x] = 0xff000000u | ((a ? 180u : 45u) << 16) |
			(((sx * 3 + sy) % 190 + 25) << 8) | ((sx + sy * 3) % 190 + 25);
	}
	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = extent.width; desc.Height = extent.height; desc.MipLevels = desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	D3D11_SUBRESOURCE_DATA data{pixels.data(), extent.width * 4, 0};
	winrt::com_ptr<ID3D11Texture2D> texture;
	Check(resources.GetD3DDevice()->CreateTexture2D(&desc, &data, texture.put()), "Create color");
	return texture;
}

static double Half(uint16_t value) {
	const int sign = value & 0x8000 ? -1 : 1;
	const unsigned exponent = (value >> 10) & 31, mantissa = value & 1023;
	if (exponent == 31) return std::numeric_limits<double>::infinity();
	return sign * (exponent ? std::ldexp(1.0 + mantissa / 1024.0, int(exponent) - 15) : std::ldexp(double(mantissa), -24));
}

static void Validate(DeviceResources& resources, const FrameGuidanceConsumerViews& views,
	uint64_t frame, FrameGuidanceExtent extent, bool reset, uint64_t generation) {
	Require(views.produced.IsValidFor(frame, extent) && views.zero.IsValidFor(frame, extent), "Invalid consumer extent/frame/region");
	Require(views.produced.motionDirection == FrameGuidanceMotionDirection::CurrentToPrevious &&
		views.produced.motionUnit == FrameGuidanceMotionUnit::SourcePixels, "Wrong motion convention");
	for (const auto* channel : {&views.produced.motion, &views.produced.depth, &views.produced.confidence}) {
		Require(channel->metadata.captureSequence == frame + 1000 && channel->metadata.resourceGeneration == generation &&
			channel->metadata.timestamp100ns == int64_t(frame * 166667), "Lost capture metadata");
		Require(channel->metadata.validRegion == FrameGuidanceRegion::Full(extent), "Wrong valid region");
		if (channel->metadata.sync.fence && channel->metadata.sync.value)
			Check(resources.GetD3DDC()->Wait(channel->metadata.sync.fence, channel->metadata.sync.value), "Consumer wait");
	}
	Require(views.produced.requiresHistoryReset == reset, "Unexpected history-reset state");
	Require(views.produced.motion.metadata.isZero == reset, "Unexpected zero motion state");
	Require(views.motionAvailability == (reset ? FrameGuidanceAvailability::ResetOnly : FrameGuidanceAvailability::Valid), "Wrong provider availability");
	for (const auto* channel : {&views.produced.motion, &views.produced.confidence}) {
		D3D11_TEXTURE2D_DESC desc{}; channel->texture->GetDesc(&desc);
		Require(desc.Width == extent.width && desc.Height == extent.height, "Texture/metadata dimensions differ");
		desc.BindFlags = desc.MiscFlags = 0; desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		winrt::com_ptr<ID3D11Texture2D> staging;
		Check(resources.GetD3DDevice()->CreateTexture2D(&desc, nullptr, staging.put()), "Create staging");
		resources.GetD3DDC()->CopyResource(staging.get(), channel->texture);
		D3D11_MAPPED_SUBRESOURCE mapped{};
		Check(resources.GetD3DDC()->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped), "Read guidance");
		for (uint32_t y = 0; y < extent.height; ++y) {
			const auto* row = static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch;
			if (desc.Format == DXGI_FORMAT_R16G16_FLOAT) {
				const auto* half = reinterpret_cast<const uint16_t*>(row);
				for (uint32_t x = 0; x < extent.width * 2; ++x) {
					Require(std::isfinite(Half(half[x])), "Nonfinite displacement");
					if (reset) Require(Half(half[x]) == 0, "Nonzero reset displacement");
				}
			} else if (reset) {
				for (uint32_t x = 0; x < extent.width; ++x) Require(row[x] == 0, "Nonzero reset confidence");
			}
		}
		resources.GetD3DDC()->Unmap(staging.get(), 0);
	}
}

static bool Case(DeviceResources& resources, const std::vector<MotionVectorRequest>& requests,
	unsigned& unsupported, FrameGuidanceExtent extent = {513, 289}) {
	std::cout << "BEGIN";
	for (auto request : requests) std::cout << " method=" << int(request.method) << " quality=" << int(request.quality);
	std::cout << " source=" << extent.width << 'x' << extent.height << std::endl;
	bool supported = false, passed = false;
	{
		FrameGuidanceService service;
		FrameGuidanceRequirements requirements{.zero = true};
		for (auto request : requests) requirements.Add(request);
		requirements = requirements.Resolved();
		auto input = Color(resources, extent, 0);
		if (!service.Initialize(resources, input.get(), requirements)) {
			const auto error = service.InitializationError();
			if (error == OpticalFlowInitializationError::QualityUnsupported || error == OpticalFlowInitializationError::CapabilityUnsupported) {
				++unsupported;
				std::cout << "UNSUPPORTED method=" << int(service.InitializationFailedMethod()) << " error=" << int(error) << std::endl;
			} else {
				std::cout << "FAILED initialization method=" << int(service.InitializationFailedMethod()) << " error=" << int(error) << std::endl;
				supported = true;
			}
		} else {
			supported = true;
			try {
				const auto selected = requirements.PreferredMotion();
				Require(service._providers.size() == 1 && service._providers.front().request == selected &&
					service._providers.front().ready, "Consumers did not share one capture-space estimator");
				for (auto request : requests) {
					Require(FrameGuidanceRequirements::ResolveConsumer(request, selected) == selected,
						"Consumer retained a private estimator request");
				}
				for (uint64_t frame = 1; frame <= 12; ++frame) {
					if (frame == 3) service.ResetHistory(FrameGuidanceResetReason::SceneChange);
					if (frame == 5) extent = {515, 291};
					input = Color(resources, extent, unsigned(frame));
					const uint64_t generation = frame >= 5 ? 8 : 7;
					const bool reset = frame == 1 || frame == 3 || frame == 5;
					service.BeginFrame(frame, input.get(), requirements, frame + 1000, generation, int64_t(frame * 166667));
					Require(service.SourceExtent() == extent && service._providers.size() == 1,
						"Capture resize introduced another estimator");
					std::vector<ID3D11Texture2D*> outputs;
					for (auto request : requests) {
						auto views = service.GetConsumerViews(frame, extent, request);
						Validate(resources, views, frame, extent, reset, generation);
						outputs.push_back(views.produced.motion.texture);
						const FrameGuidanceExtent target{257, 145};
						Validate(resources, service.GetConsumerViews(frame, target, request), frame, target, reset, generation);
					}
					if (outputs.size() == 2) Require(outputs[0] == outputs[1], "Mixed consumers did not share the selected motion field");
					const auto* cached = service.View().motion.texture;
					service.BeginFrame(frame, input.get(), requirements, frame + 1000, generation, int64_t(frame * 166667));
					Require(service.View().motion.texture == cached, "Same capture regenerated its field");
					Require(DebugClean(), "GPU debug warning/error");
				}
				std::cout << "GPU_GUIDANCE sample=" << service.LastGpuSample() << " ms=" << service.LastGpuMilliseconds() << std::endl;
				Require(service.LastGpuSample() > 0 && std::isfinite(service.LastGpuMilliseconds()), "GPU timing did not produce a ready sample");
				passed = true;
			} catch (const std::exception& error) { std::cout << "FAILED " << error.what() << std::endl; }
		}
	}
	const bool clean = DebugClean() && !debugFailure;
	if (!clean) std::cout << "FAILED GPU debug gate" << std::endl;
	if (passed && clean) std::cout << "PASS" << std::endl;
	return clean && (passed || !supported);
}

int main(int argc, char** argv) {
	try {
		winrt::init_apartment();
		Logger::Get().Initialize(spdlog::level::info, L"FrameGuidanceScalingNativeTests.log", 10000000, 1);
		winrt::com_ptr<ID3D12Debug> debug12;
		Check(D3D12GetDebugInterface(IID_PPV_ARGS(debug12.put())), "D3D12 debug layer unavailable");
		debug12->EnableDebugLayer();
		DeviceResources resources;
		Check(CreateDXGIFactory2(0, IID_PPV_ARGS(resources._dxgiFactory.put())), "Create factory");
		for (UINT i = 0;; ++i) {
			winrt::com_ptr<IDXGIAdapter1> adapter;
			Check(resources._dxgiFactory->EnumAdapters1(i, adapter.put()), "NVIDIA adapter not found");
			DXGI_ADAPTER_DESC1 desc{}; Check(adapter->GetDesc1(&desc), "Adapter description");
			if (desc.VendorId != 0x10de) continue;
			resources._graphicsAdapter = adapter.as<IDXGIAdapter4>();
			winrt::com_ptr<ID3D11Device> device; winrt::com_ptr<ID3D11DeviceContext> context;
			Check(D3D11CreateDevice(adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_DEBUG,
				nullptr, 0, D3D11_SDK_VERSION, device.put(), nullptr, context.put()), "D3D11 debug device unavailable");
			resources._d3dDevice = device.as<ID3D11Device5>(); resources._d3dDC = context.as<ID3D11DeviceContext4>();
			messages11 = device.as<ID3D11InfoQueue>();
			std::wcout << L"ADAPTER " << desc.Description << std::endl;
			break;
		}
		winrt::com_ptr<ID3D12Device> device12;
		Check(D3D12CreateDevice(resources.GetGraphicsAdapter(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(device12.put())), "D3D12 device");
		messages12 = device12.as<ID3D12InfoQueue>();
		AddVectoredExceptionHandler(1, RecordFailure);
		unsigned cases = 0, failed = 0, unsupported = 0;
		if (argc == 3 || argc == 5) {
			++cases;
			const FrameGuidanceExtent extent = argc == 5 ? FrameGuidanceExtent{uint32_t(std::atoi(argv[3])), uint32_t(std::atoi(argv[4]))} : FrameGuidanceExtent{513, 289};
			if (!Case(resources, {{static_cast<OpticalFlowMethod>(std::atoi(argv[1])),
				static_cast<uint8_t>(std::atoi(argv[2]))}}, unsupported, extent)) ++failed;
		} else if (argc == 1) {
			for (uint8_t quality : {0, 1}) {
				++cases; if (!Case(resources, {{OpticalFlowMethod::Amd, quality}}, unsupported)) ++failed;
			}
			for (uint8_t quality : {1, 2, 3, 4, 5}) {
				++cases; if (!Case(resources, {{OpticalFlowMethod::Nvidia, quality}}, unsupported)) ++failed;
			}
		}
		if (argc != 3 && argc != 5) {
			++cases;
			if (!Case(resources, {{OpticalFlowMethod::Amd, 0}, {OpticalFlowMethod::Nvidia, 2}}, unsupported)) ++failed;
		}
		Check(resources.GetD3DDevice()->GetDeviceRemovedReason(), "D3D11 device removed");
		Check(device12->GetDeviceRemovedReason(), "D3D12 device removed");
		std::cout << "SUMMARY cases=" << cases << " unsupported=" << unsupported << " failed=" << failed << std::endl;
		Logger::Get().Flush();
		return failed ? 1 : 0;
	} catch (const std::exception& error) {
		std::cout << "FATAL " << error.what() << std::endl;
		Logger::Get().Flush(); return 2;
	}
}
