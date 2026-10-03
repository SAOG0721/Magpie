// SPDX-License-Identifier: GPL-3.0-or-later
// Runtime by AMDNR/3zwr1; neural network and runtime origin by lmxxf/Kien.
#include "pch.h"
#include "AmdLmxxfNRFilter.h"
#include "DeviceResources.h"
#include "Logger.h"
#include <cmath>
#include <stdexcept>
#ifdef MP_ENABLE_AMD_LMXXF_NR
#include "LmxxfNrApi.h"
#include <d3d12.h>
#include <d3dcompiler.h>
#include "NativeOutputValidation.h"

namespace Magpie {
struct AmdLmxxfNRFilter::Impl {
	ID3D11Device5* d11{};
	ID3D11DeviceContext4* c11{};
	winrt::com_ptr<ID3D12Device> d12;
	winrt::com_ptr<ID3D12CommandQueue> queue;
	winrt::com_ptr<ID3D12CommandAllocator> allocator;
	winrt::com_ptr<ID3D12GraphicsCommandList> list;
	winrt::com_ptr<ID3D11Fence> f11;
	winrt::com_ptr<ID3D12Fence> fence;
	winrt::com_ptr<ID3D11Texture2D> in11, out11, converted;
	winrt::com_ptr<ID3D12Resource> in12, out12;

	winrt::com_ptr<ID3D11UnorderedAccessView> inUav, convertedUav;
	winrt::com_ptr<ID3D11ComputeShader> shader;
	HMODULE dll{};
	LmxxfNrApi api{};
	void* runtime{};
	LmxxfNrJob job{};
	uint64_t tick{}, sequence{}, lastFrame{}, lastRevision{}, lastHistory{};
	bool cached = false, faulted = false;
	UINT width{}, height{};
	static constexpr auto srvState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
	void hr(HRESULT rc) {
		if (FAILED(rc))
			throw std::runtime_error(fmt::format("HRESULT=0x{:08x}", uint32_t(rc)));
	}
	void check(int rc, const char* stage) {
		if (!rc)
			return;
		char error[2048]{};
		if (api.GetLastError)
			api.GetLastError(error, sizeof(error));
		throw std::runtime_error(fmt::format("{} rc={} {}", stage, rc, error));
	}
	void wait(uint64_t value) {
		auto completed = fence->GetCompletedValue();
		if (completed == UINT64_MAX)
			throw std::runtime_error("D3D12 device removed");
		if (completed >= value)
			return;
		wil::unique_event_nothrow event;
		hr(event.create());
		hr(fence->SetEventOnCompletion(value, event.get()));
		// Synchronous baseline: resources cannot be destroyed while GPU uses them.
		if (WaitForSingleObject(event.get(), INFINITE) != WAIT_OBJECT_0 || fence->GetCompletedValue() == UINT64_MAX)
			throw std::runtime_error("NR fence wait failed");
	}
	void sync() {
		hr(queue->Signal(fence.get(), ++tick));
		wait(tick);
	}
	void begin() {
		hr(allocator->Reset());
		hr(list->Reset(allocator.get(), nullptr));
	}
	void submit() {
		hr(list->Close());
		ID3D12CommandList* lists[]{list.get()};
		queue->ExecuteCommandLists(1, lists);
		sync();
	}
	void barrier(ID3D12Resource* r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
		D3D12_RESOURCE_BARRIER b{};
		b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		b.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
		list->ResourceBarrier(1, &b);
	}
	void shared(winrt::com_ptr<ID3D11Texture2D>& t11, winrt::com_ptr<ID3D12Resource>& t12) {
		D3D11_TEXTURE2D_DESC d{};
		d.Width = width;
		d.Height = height;
		d.MipLevels = 1;
		d.ArraySize = 1;
		d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		d.SampleDesc.Count = 1;
		d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		d.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
		hr(d11->CreateTexture2D(&d, nullptr, t11.put()));
		auto dxgi = t11.as<IDXGIResource1>();
		HANDLE raw{};
		hr(dxgi->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &raw));
		wil::unique_handle handle(raw);
		hr(d12->OpenSharedHandle(handle.get(), IID_PPV_ARGS(t12.put())));
	}
	void convert(ID3D11Texture2D* source, ID3D11UnorderedAccessView* target) {
		winrt::com_ptr<ID3D11ShaderResourceView> view;
		hr(d11->CreateShaderResourceView(source, nullptr, view.put()));
		auto* s = view.get();
		c11->CSSetShaderResources(0, 1, &s);
		c11->CSSetUnorderedAccessViews(0, 1, &target, nullptr);
		c11->CSSetShader(shader.get(), nullptr, 0);
		c11->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
		s = nullptr;
		target = nullptr;
		c11->CSSetShaderResources(0, 1, &s);
		c11->CSSetUnorderedAccessViews(0, 1, &target, nullptr);
		c11->CSSetShader(nullptr, nullptr, 0);
	}
	void status() {
		char buffer[4096]{};
		check(api.GetStatus(runtime, buffer, sizeof(buffer)), "GetStatus");
		Logger::Get().Info(fmt::format("AMD lmxxf NR status: {}", buffer));
	}
	~Impl() {
		if (runtime) {
			// Complete any submitted producer/consumer before retiring failed jobs.
			if (queue && fence) {
				try {
					sync();
				} catch (...) {
				}
			}
			if (job.handle) {
				uint32_t state{};
				if (api.Poll(runtime, job.handle, &state) == LMXXF_NR_OK && state <= LMXXF_NR_JOB_PRODUCER_SUBMITTED)
					api.CancelUnsubmitted(runtime, job.handle);
				else if (api.AbandonJob)
					api.AbandonJob(runtime, job.handle);
			}
			api.Drain(runtime);
			if (queue && fence) {
				try {
					sync();
				} catch (...) {
				}
			}
			api.Destroy(runtime);
		}
		if (dll)
			FreeLibrary(dll);
	}
};

AmdLmxxfNRFilter::AmdLmxxfNRFilter() = default;
AmdLmxxfNRFilter::~AmdLmxxfNRFilter() = default;
bool AmdLmxxfNRFilter::Initialize(DeviceResources& r, ID3D11Texture2D* input, ID3D11Texture2D* output,
								  const EffectOption& option) noexcept {
	try {
		Settings settings;
		if (!ReadSettings(option, settings))
			throw std::runtime_error("invalid AMD NR parameters");
		D3D11_TEXTURE2D_DESC in{}, out{};
		input->GetDesc(&in);
		output->GetDesc(&out);
		// No HDR reinterpretation and no hidden resampling. Verified 360 tier only.
		if (in.Width != out.Width || in.Height != out.Height || in.Width > 640 || in.Height > 360 ||
			(in.Format != DXGI_FORMAT_B8G8R8A8_UNORM && in.Format != DXGI_FORMAT_R8G8B8A8_UNORM) ||
			out.Format != DXGI_FORMAT_R8G8B8A8_UNORM)
			throw std::runtime_error("NR requires same-size SDR UNORM input/RGBA8 output, at most 640x360");
		auto p = std::make_unique<Impl>();
		p->width = in.Width;
		p->height = in.Height;
		p->d11 = r.GetD3DDevice();
		p->c11 = r.GetD3DDC();
		p->hr(D3D12CreateDevice(r.GetGraphicsAdapter(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(p->d12.put())));
		D3D12_COMMAND_QUEUE_DESC q{};
		p->hr(p->d12->CreateCommandQueue(&q, IID_PPV_ARGS(p->queue.put())));
		p->hr(p->d12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(p->allocator.put())));
		p->hr(p->d12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, p->allocator.get(), nullptr,
										IID_PPV_ARGS(p->list.put())));
		p->hr(p->list->Close());
		p->hr(p->d11->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(p->f11.put())));
		HANDLE raw{};
		p->hr(p->f11->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &raw));
		wil::unique_handle handle(raw);
		p->hr(p->d12->OpenSharedHandle(handle.get(), IID_PPV_ARGS(p->fence.put())));
		p->shared(p->in11, p->in12);
		p->shared(p->out11, p->out12);
		p->hr(p->d11->CreateUnorderedAccessView(p->in11.get(), nullptr, p->inUav.put()));
		out.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
		out.MiscFlags = 0;
		out.Usage = D3D11_USAGE_DEFAULT;
		out.CPUAccessFlags = 0;
		p->hr(p->d11->CreateTexture2D(&out, nullptr, p->converted.put()));
		p->hr(p->d11->CreateUnorderedAccessView(p->converted.get(), nullptr, p->convertedUav.put()));
		// Runtime contract matches the probe: gamma-encoded SDR RGB [0,1], alpha 1.
		// UNORM SRV performs BGRA channel mapping; no sRGB decode/encode is applied.
		constexpr char shader[] = R"(Texture2D<float4> source:register(t0); RWTexture2D<float4> target:register(u0);
  [numthreads(8,8,1)] void main(uint3 p:SV_DispatchThreadID) { uint w,h; target.GetDimensions(w,h);
  if(p.x<w && p.y<h) target[p.xy]=float4(saturate(source.Load(int3(p.xy,0)).rgb),1); })";
		winrt::com_ptr<ID3DBlob> blob, errors;
		p->hr(D3DCompile(shader, sizeof(shader) - 1, nullptr, nullptr, nullptr, "main", "cs_5_0",
						 D3DCOMPILE_ENABLE_STRICTNESS, 0, blob.put(), errors.put()));
		p->hr(p->d11->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, p->shader.put()));
		wchar_t executable[32768]{};
		if (!GetModuleFileNameW(nullptr, executable, 32768))
			throw std::runtime_error("executable path unavailable");
		auto path = std::filesystem::path(executable).parent_path() / L"AMDNR";
		p->dll = LoadLibraryExW((path / L"LmxxfNrRuntime.dll").c_str(), nullptr,
								LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
		if (!p->dll)
			throw std::runtime_error(fmt::format("Load AMDNR DLL error={}", GetLastError()));
		auto get = reinterpret_cast<int32_t (*)(uint32_t, LmxxfNrApi*)>(GetProcAddress(p->dll, "LmxxfNrGetApi"));
		if (!get)
			throw std::runtime_error("LmxxfNrGetApi missing");
		p->api.struct_size = sizeof(p->api);
		p->check(get(1, &p->api), "GetApi");
		if (!p->api.Create || !p->api.Destroy || !p->api.PrepareSession || !p->api.PrepareFrame ||
			!p->api.RecordInputs || !p->api.EnqueueHip || !p->api.RecordOutputs || !p->api.Retire || !p->api.Poll ||
			!p->api.CancelUnsubmitted || !p->api.Drain || !p->api.GetStatus)
			throw std::runtime_error("incomplete runtime ABI table");
		auto pak = (path / L"LmxxfNrRuntime.pak").wstring();
		LmxxfNrCreateInfo ci{};
		ci.struct_size = sizeof(ci);
		ci.device = p->d12.get();
		ci.queue = p->queue.get();
		ci.assets_directory = pak.c_str();
		p->check(p->api.Create(&ci, &p->runtime), "Create");
		p->check(p->api.PrepareSession(p->runtime), "PrepareSession");
		auto tier = reinterpret_cast<int32_t (*)(void*, uint32_t)>(GetProcAddress(p->dll, "LmxxfNrSetTierPolicy"));
		if (!tier)
			throw std::runtime_error("small tier policy export missing");
		p->check(tier(p->runtime, 1), "SetTierPolicy");
		p->status();
		Logger::Get().Info(fmt::format("AMD lmxxf NR initialized {}x{} ABI=1 frame_size=128 tier=1 temporal=off "
									   "GPU-only SDR (parameters logged per dispatch)",
									   in.Width, in.Height));
		_settings = settings;
		_impl = std::move(p);
		return true;
	} catch (const std::exception& e) {
		Logger::Get().Error(fmt::format("AMD lmxxf NR initialize: {}", e.what()));
		return false;
	}
}
bool AmdLmxxfNRFilter::Drain() noexcept {
	if (!_impl)
		return true;
	try {
		_impl->check(_impl->api.Drain(_impl->runtime), "Drain");
		_impl->sync();
		return true;
	} catch (const std::exception& e) {
		Logger::Get().Error(fmt::format("AMD lmxxf NR drain: {}", e.what()));
		return false;
	}
}
bool AmdLmxxfNRFilter::Resize(DeviceResources& r, ID3D11Texture2D* in, ID3D11Texture2D* out) noexcept {
	if (!Drain())
		return false;
	_impl.reset();
	EffectOption option;
	option.parameters = {{"style", _settings.style},
						 {"intensity", _settings.intensity},
						 {"localToneStrength", _settings.tone},
						 {"localStructureStrength", _settings.structure},
						 {"skinStructureStrength", _settings.skin},
						 {"useAutoMask", _settings.autoMask ? 1.f : 0.f},
						 {"multiPass", float(_settings.passes)}};
	return Initialize(r, in, out, option);
}
bool AmdLmxxfNRFilter::Draw(const NativeEffectDrawContext& c) noexcept {
	if (!_impl || _impl->faulted)
		return false;
	auto& p = *_impl;
	try {
		if (p.cached && c.frameId == p.lastFrame && c.inputRevision == p.lastRevision &&
			c.inputHistoryRevision == p.lastHistory && !c.inputHistoryReset) {
			p.c11->CopyResource(c.output, p.converted.get());
			return true;
		}
		const auto start = std::chrono::steady_clock::now();
		p.convert(c.input, p.inUav.get());
		p.hr(p.c11->Signal(p.f11.get(), ++p.tick));
		p.c11->Flush();
		p.hr(p.queue->Wait(p.fence.get(), p.tick));
		p.sync();
		const auto inputDone = std::chrono::steady_clock::now();
		p.begin();
		p.barrier(p.in12.get(), D3D12_RESOURCE_STATE_COMMON, Impl::srvState);
		LmxxfNrFrameInfo fi{};
		fi.struct_size = 128;
		fi.session_id = 1;
		fi.frame_id = ++p.sequence;
		fi.list_generation = p.sequence;
		fi.command_list = p.list.get();
		fi.color_width = p.width;
		fi.color_height = p.height;
		fi.color = p.in12.get();
		fi.color_state = Impl::srvState;
		fi.flags = LMXXF_NR_FRAME_FLAG_STRENGTH;
		fi.transfer_strength = _settings.intensity;
		fi.color_strength = _settings.intensity;
		fi.model_scale = 1;
		fi.history_reset = 1;
		fi.passes = _settings.passes;
		fi.control_tone = _settings.tone;
		fi.control_style = _settings.style;
		// AMDNR NrControls.h policy: native character/scene split, not a generated mask texture.
		fi.control_structure = _settings.autoMask ? 1.f : _settings.structure;
		fi.control_skin = _settings.autoMask ? _settings.skin : -1.f;
		fi.control_other = _settings.autoMask ? _settings.structure : -1.f;
		if (fi.control_tone != 1 || fi.control_style != 1 || fi.control_structure != 1 || fi.control_skin != 1 ||
			fi.control_other != 1) {
			fi.struct_size = sizeof(fi);
			fi.flags |= LMXXF_NR_FRAME_FLAG_CONTROLS;
		}
		if (p.sequence <= 3 || p.sequence % 60 == 0)
			Logger::Get().Info(fmt::format("AMD NR parameters: frame_size={} style={} intensity={} tone={} "
										   "structure={} skin={} autoMask={} passes={} controls={},{},{},{},{}",
										   fi.struct_size, _settings.style, _settings.intensity, _settings.tone,
										   _settings.structure, _settings.skin, _settings.autoMask, _settings.passes,
										   fi.control_tone, fi.control_structure, fi.control_skin, fi.control_other,
										   fi.control_style));
		p.job = {};
		p.job.struct_size = sizeof(p.job);
		p.check(p.api.PrepareFrame(p.runtime, &fi, &p.job), "PrepareFrame");
		p.check(p.api.RecordInputs(p.runtime, p.job.handle, p.list.get()), "RecordInputs");
		p.submit();
		p.check(p.api.EnqueueHip(p.runtime, p.job.handle, p.queue.get()), "EnqueueHip");
		p.sync();
		const auto modelDone = std::chrono::steady_clock::now();
		p.begin();
		p.check(p.api.RecordOutputs(p.runtime, p.job.handle, p.list.get()), "RecordOutputs");
		auto* result = static_cast<ID3D12Resource*>(p.job.private_output);
		if (!result)
			throw std::runtime_error("missing NR output");
		auto rd = result->GetDesc();
		if (rd.Width != p.width || rd.Height != p.height || rd.Format != DXGI_FORMAT_R16G16B16A16_FLOAT)
			throw std::runtime_error("NR output geometry/format mismatch");
		p.barrier(result, Impl::srvState, D3D12_RESOURCE_STATE_COPY_SOURCE);
		p.barrier(p.out12.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
		p.list->CopyResource(p.out12.get(), result);
		p.barrier(result, D3D12_RESOURCE_STATE_COPY_SOURCE, Impl::srvState);
		p.barrier(p.out12.get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
		p.barrier(p.in12.get(), Impl::srvState, D3D12_RESOURCE_STATE_COMMON);
		p.submit();
		p.check(p.api.Retire(p.runtime, p.job.handle), "Retire");
		p.job.handle = nullptr;
		p.hr(p.c11->Wait(p.f11.get(), p.tick));
		p.convert(p.out11.get(), p.convertedUav.get());
		p.c11->CopyResource(c.output, p.converted.get());
		// Complete D3D11 consumer before shared surfaces are reused or released.
		p.hr(p.c11->Signal(p.f11.get(), ++p.tick));
		p.c11->Flush();
		p.wait(p.tick);
		if (p.sequence == 2 && !ValidateNativeOutput(p.d11, p.c11, c.input, c.output, "AMDNR"))
			throw std::runtime_error("output diagnostic failed");
		p.cached = true;
		p.lastFrame = c.frameId;
		p.lastRevision = c.inputRevision;
		p.lastHistory = c.inputHistoryRevision;
		if (p.sequence <= 3 || p.sequence % 60 == 0) {
			p.status();
			Logger::Get().Info(fmt::format(
				"AMD lmxxf NR frame={} previous={} revision={} history={} reset={} new={} sequence={} "
				"synchronous_total_ms={:.3f} input_gpu_sync_wall_ms={:.3f} runtime_producer_hip_sync_wall_ms={:.3f} "
				"output_gpu_sync_wall_ms={:.3f}",
				c.frameId, c.previousCaptureFrameId, c.inputRevision, c.inputHistoryRevision, c.inputHistoryReset,
				c.isNewCaptureFrame, p.sequence,
				std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(),
				std::chrono::duration<double, std::milli>(inputDone - start).count(),
				std::chrono::duration<double, std::milli>(modelDone - inputDone).count(),
				std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - modelDone).count()));
		}
		return true;
	} catch (const std::exception& e) {
		p.faulted = true;
		p.cached = false;
		Logger::Get().Error(fmt::format("AMD lmxxf NR draw failed (no fallback): {}", e.what()));
		return false;
	}
}
} // namespace Magpie
#else
namespace Magpie {
struct AmdLmxxfNRFilter::Impl {};
AmdLmxxfNRFilter::AmdLmxxfNRFilter() = default;
AmdLmxxfNRFilter::~AmdLmxxfNRFilter() = default;
bool AmdLmxxfNRFilter::Initialize(DeviceResources&, ID3D11Texture2D*, ID3D11Texture2D*, const EffectOption&) noexcept {
	Logger::Get().Error("AMD lmxxf NR requires EnableAmdLmxxfNR=true (x64)");
	return false;
}
bool AmdLmxxfNRFilter::Resize(DeviceResources& r, ID3D11Texture2D* i, ID3D11Texture2D* o) noexcept {
	return Initialize(r, i, o, {});
}
bool AmdLmxxfNRFilter::Draw(const NativeEffectDrawContext&) noexcept {
	return false;
}
bool AmdLmxxfNRFilter::Drain() noexcept {
	return true;
}
} // namespace Magpie
#endif

namespace Magpie {
bool AmdLmxxfNRFilter::ReadSettings(const EffectOption& option, Settings& candidate) noexcept {
	auto read = [&](const char* name, float fallback, float maximum) {
		auto it = option.parameters.find(name);
		float value = it == option.parameters.end() ? fallback : it->second;
		if (!std::isfinite(value) || value < 0 || value > maximum)
			throw std::runtime_error("invalid NR control value");
		return value;
	};
	try {
		candidate.style = read("style", 1, 4);
		candidate.intensity = read("intensity", 1, 1);
		candidate.tone = read("localToneStrength", 1, 4);
		candidate.structure = read("localStructureStrength", 1, 4);
		candidate.skin = read("skinStructureStrength", 1, 4);
		float mask = read("useAutoMask", 1, 1), passes = read("multiPass", 1, 3);
		if ((mask != 0 && mask != 1) || passes < 1 || passes != std::floor(passes))
			return false;
		candidate.autoMask = mask != 0;
		candidate.passes = uint32_t(passes);
		return true;
	} catch (...) {
		return false;
	}
}
EffectParameterApplyMode AmdLmxxfNRFilter::GetParameterApplyMode(std::string_view name) const noexcept {
	for (auto supported : {"style", "intensity", "localToneStrength", "localStructureStrength", "skinStructureStrength",
						   "useAutoMask", "multiPass"})
		if (name == supported)
			return EffectParameterApplyMode::Live;
	return EffectParameterApplyMode::Unavailable;
}
bool AmdLmxxfNRFilter::ApplyLiveParameters(const EffectOption& option, std::span<const std::string> names) noexcept {
	for (const auto& name : names)
		if (GetParameterApplyMode(name) != EffectParameterApplyMode::Live)
			return false;
	Settings candidate;
	if (!ReadSettings(option, candidate))
		return false;
	// The renderer serializes this with Draw. Every previous job has been fully retired.
	_settings = candidate;
#ifdef MP_ENABLE_AMD_LMXXF_NR
	if (_impl)
		_impl->cached = false;
#endif
	return true;
}
} // namespace Magpie
