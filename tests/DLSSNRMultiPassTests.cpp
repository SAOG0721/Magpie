#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <ranges>
#include <span>
#include <string>
#include <vector>
#include "DLSSNRParameters.h"
#include "DlssNrStatus.h"
#include "EffectParameterRules.h"
#include "MotionVectorRequest.h"

// Execute the production chain with deterministic SDK and texture doubles.
// This checks routing/lifetime/failure behavior without invoking NGX or a GPU.
using HRESULT = int;
constexpr int D3D11_USAGE_DEFAULT = 0;
constexpr int D3D11_BIND_SHADER_RESOURCE = 1;
constexpr int D3D11_BIND_UNORDERED_ACCESS = 2;
inline bool FAILED(HRESULT value) { return value < 0; }
struct D3D11_TEXTURE2D_DESC {
	int Width = 1280, Height = 720, Usage = 0, BindFlags = 0, CPUAccessFlags = 0, MiscFlags = 0;
};
struct ID3D11Texture2D {
	D3D11_TEXTURE2D_DESC desc;
	int value = 0;
	void GetDesc(D3D11_TEXTURE2D_DESC* result) { *result = desc; }
};
namespace winrt {
template<class T> struct com_ptr {
	std::shared_ptr<T> value;
	T* raw = nullptr;
	com_ptr() = default;
	com_ptr(com_ptr&& other) noexcept : value(std::move(other.value)), raw(other.raw) { other.raw = nullptr; }
	com_ptr& operator=(com_ptr&&) = default;
	~com_ptr() { if (!value) delete raw; }
	T** put() { return &raw; }
	T* get() const { return value ? value.get() : raw; }
};
}
namespace fmt {
template<class... Args> std::string format(const char* value, Args&&...) { return value; }
}
namespace Magpie {
struct EffectOption { std::map<std::string, float> parameters; };
enum class EffectParameterApplyMode { Live, RestartRequired, Unavailable };
enum class EffectParameterRestartReason { None, ResourceRecreation, FrameGuidance };
struct HdrEffectBoundaryContext {};
struct NativeEffectDrawContext {
	ID3D11Texture2D* input;
	ID3D11Texture2D* output;
	uint64_t inputRevision = 0;
	uint64_t inputHistoryRevision = 0;
	bool inputHistoryReset = false;
	bool isNewCaptureFrame = true;
};
struct NativeEffectBackend {
	virtual ~NativeEffectBackend() = default;
	virtual void SetHdrBoundary(HdrEffectBoundaryContext value) noexcept { _hdrBoundary = value; }
	virtual FrameGuidanceRequirements GetFrameGuidanceRequirements() const noexcept = 0;
	virtual bool Drain() noexcept = 0;
	virtual DlssNrFailure GetDlssNrFailure() const { return {}; }
	virtual bool IsDlssNrProcessing() const noexcept { return false; }
	virtual bool Resize(struct DeviceResources&, ID3D11Texture2D*, ID3D11Texture2D*) noexcept = 0;
	virtual bool Draw(const NativeEffectDrawContext&) noexcept = 0;
	virtual EffectParameterApplyMode GetParameterApplyMode(std::string_view) const noexcept = 0;
	virtual EffectParameterRestartReason GetParameterRestartReason(std::string_view) const noexcept = 0;
	virtual bool ApplyLiveParameters(const EffectOption&, std::span<const std::string>) noexcept = 0;
	HdrEffectBoundaryContext _hdrBoundary;
};
struct ID3D11DeviceContext { void CopyResource(ID3D11Texture2D* target, ID3D11Texture2D* from) { target->value = from->value; } };
struct DeviceResources {
	ID3D11DeviceContext context;
	ID3D11DeviceContext* GetD3DDC() { return &context; }
	int failTextureAt = -1, created = 0;
	DeviceResources* GetD3DDevice() { return this; }
	HRESULT CreateTexture2D(const D3D11_TEXTURE2D_DESC* desc, void*, ID3D11Texture2D** texture) {
		if (created++ == failTextureAt) return -1;
		*texture = new ID3D11Texture2D{*desc}; return 0;
	}
};
struct NgxD3D12Core {};
struct DLSSNRTemporal {
	inline static int resets = 0, draws = 0;
	inline static float timeMs = 0;
	inline static ID3D11Texture2D* lastBase = nullptr;
	ID3D11Texture2D* raw = nullptr;
	bool Initialize(DeviceResources&, ID3D11Texture2D*, ID3D11Texture2D* base,
		ID3D11Texture2D* value, ID3D11Texture2D*, int, bool, float time) {
		raw = value; lastBase = base; timeMs = time; return true;
	}
	void Reset() { ++resets; }
	void SetTimeConstant(float value) { timeMs = NormalizeDLSSNRAntiFlickerTimeMs(value); }
	void ConfigureDetail(float, bool) {}
	bool Draw(const NativeEffectDrawContext& context) { ++draws; context.output->value = raw->value; return true; }
};
struct Logger {
	static Logger& Get() { static Logger value; return value; }
	void Info(std::string_view) {}
	void Error(std::string_view) {}
	void ComError(std::string_view, HRESULT) {}
};
inline EffectOption ParseDLSSNRSettings(const EffectOption& value, bool) { return value; }
using DLSSNRSettings = EffectOption;
struct DLSSNRFilter {
	inline static int liveInstances = 0, featureCount = 0;
	inline static bool failInit = false, failApply = false, failDraw = false, healthy = true;
	inline static std::vector<std::string> receivedNames;
	std::vector<EffectOption> settings;
	DLSSNRFilter() { ++liveInstances; }
	~DLSSNRFilter() { --liveInstances; featureCount -= static_cast<int>(settings.size()); }
	std::string LibraryInfo() const { return "fixture library"; }
	bool IsHealthy() const { return healthy; }
	DlssNrFailure GetDlssNrFailure() const { return { DlssNrStage::Evaluate, DlssNrReason::Unknown, DlssNrCodeKind::Ngx, 0xBAD00001, "fixture Evaluate" }; }
	bool InitializeChain(DeviceResources&, NgxD3D12Core&, ID3D11Texture2D* input,
		ID3D11Texture2D* output, std::span<const EffectOption> options) {
		assert(input != output);
		if (failInit) return false;
		settings.assign(options.begin(), options.end());
		featureCount += static_cast<int>(settings.size());
		return true;
	}
	void SetHdrBoundary(HdrEffectBoundaryContext) {}
	FrameGuidanceRequirements GetFrameGuidanceRequirements() const { return {true}; }
	bool Drain() { return true; }
	EffectParameterApplyMode GetParameterApplyMode(std::string_view name) const {
		return name == "enableInputResolutionScaling" ? EffectParameterApplyMode::RestartRequired : EffectParameterApplyMode::Live;
	}
	EffectParameterRestartReason GetParameterRestartReason(std::string_view) const { return EffectParameterRestartReason::ResourceRecreation; }
	bool ApplyLiveParameters(const EffectOption& option, std::span<const std::string> names) {
		if (failApply) return false;
		receivedNames.assign(names.begin(), names.end());
		for (size_t i = 0; i < settings.size(); ++i) settings[i] = DLSSNRPassOption(option, static_cast<int>(i + 1));
		return true;
	}
	bool Draw(const NativeEffectDrawContext& context) {
		if (failDraw) return false;
		int value = context.input->value;
		for (const auto& pass : settings) {
			const auto it = pass.parameters.find("intensity");
			value = value * 10 + (it == pass.parameters.end() ? 1 : static_cast<int>(it->second));
		}
		context.output->value = value;
		return true;
	}
};}
#include "DLSSNRMultiPassUnderTest.h"

int main() {
	using namespace Magpie;
	EffectOption option{{{"intensity", 2.f}, {"style", 2.f}, {"residualMultiplier", 1.5f}}};
	auto get = [&](std::string_view name, float fallback) {
		const auto it = option.parameters.find(std::string(name));
		return it == option.parameters.end() ? fallback : it->second;
	};
	assert(DLSSNRPassCount(get) == 1);
	assert(DLSSNRAntiFlickerMode(get) == 0);
	for (float invalid : {-1.f, 5.f, 1.5f, std::numeric_limits<float>::quiet_NaN()}) {
		option.parameters["antiFlicker"] = invalid;
		assert(DLSSNRAntiFlickerMode(get) == 0);
	}
	option.parameters.erase("antiFlicker");
	assert(DLSSNRPassOption(option, 1).parameters.at("intensity") == 2);
	for (int pass : {2, 3}) {
		const auto fresh = DLSSNRPassOption(option, pass);
		assert(!fresh.parameters.contains("intensity") && !fresh.parameters.contains("style"));
		assert(fresh.parameters.at("residualMultiplier") == 1.5f);
	}
	for (float invalid : {-1.f, 4.f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
		option.parameters["multiPass"] = invalid;
		assert(DLSSNRPassCount(get) == 1);
	}
	for (int count : {1, 2, 3, 2, 1}) {
		option.parameters["multiPass"] = static_cast<float>(count);
		assert(IsEffectParameterVisible("DLSSNR\\DLSSNR_AI_Filter", "pass2_intensity", get) == (count >= 2));
		assert(IsEffectParameterVisible("DLSSNR\\DLSSNR_AI_Filter", "pass3_intensity", get) == (count >= 3));
		assert(IsEffectParameterVisible("Custom", "pass3_intensity", get));
	}
	option.parameters["multiPass"] = 3;
	option.parameters["pass2_intensity"] = 2;
	option.parameters["pass3_intensity"] = 3;
	DeviceResources resources;
	NgxD3D12Core core;
	ID3D11Texture2D input, output;
	{
		DLSSNRMultiPass chain;
		assert(chain.Initialize(resources, core, &input, &output, option, false));
		assert(DLSSNRFilter::liveInstances == 1 && DLSSNRFilter::featureCount == 3 && resources.created == 0);
		assert(chain.Draw({&input, &output, 7}) && output.value == 223);
		std::vector<std::string> names{"pass2_intensity"};
		option.parameters["pass2_intensity"] = 1;
		assert(chain.ApplyLiveParameters(option, names));
		assert(DLSSNRFilter::receivedNames == names);
		assert(chain.Draw({&input, &output, 7}) && output.value == 213);
		DLSSNRFilter::failApply = true;
		option.parameters["pass2_intensity"] = 2;
		assert(!chain.ApplyLiveParameters(option, names));
		assert(chain.Draw({&input, &output, 7}) && output.value == 213);
		DLSSNRFilter::failApply = false;
		assert(chain.Resize(resources, &input, &output));
		assert(chain.Draw({&input, &output, 7}) && output.value == 213);
		for (int count : {2, 1, 3, 2, 1}) {
			option.parameters["multiPass"] = static_cast<float>(count);
			assert(chain.Initialize(resources, core, &input, &output, option, false));
			assert(DLSSNRFilter::liveInstances == 1 && DLSSNRFilter::featureCount == count);
		}
		DLSSNRFilter::failDraw = true;
		assert(!chain.Draw({&input, &output, 7}));
		DLSSNRFilter::failDraw = false;
		option.parameters["multiPass"] = 3;
		assert(chain.Initialize(resources, core, &input, &output, option, false));
		DLSSNRFilter::healthy = false;
		assert(!chain.Draw({&input, &output, 7}));
		DLSSNRFilter::healthy = true;
	}
	assert(DLSSNRFilter::liveInstances == 0 && DLSSNRFilter::featureCount == 0);
	DLSSNRFilter::failInit = true;
	{ DLSSNRMultiPass chain; assert(!chain.Initialize(resources, core, &input, &output, option, false)); }
	DLSSNRFilter::failInit = false;
	assert(DLSSNRFilter::liveInstances == 0);
	for (int mode : {1, 2, 3, 4}) {
		option.parameters["antiFlicker"] = static_cast<float>(mode);
		for (int count : {1, 2, 3}) {
			option.parameters["multiPass"] = static_cast<float>(count);
			DLSSNRMultiPass chain;
			assert(chain.Initialize(resources, core, &input, &output, option, false));
			assert(DLSSNRTemporal::lastBase == &input);
			assert(chain.GetFrameGuidanceRequirements().HasMotion() == (mode >= 2));
			assert(chain.GetParameterApplyMode("antiFlicker") == EffectParameterApplyMode::RestartRequired);
			assert(chain.Draw({&input, &output, 7}));
			assert(output.value == (count == 1 ? 2 : count == 2 ? 22 : 223));
			const int resets = DLSSNRTemporal::resets;
			std::vector<std::string> names{"residualMultiplier"};
			assert(chain.ApplyLiveParameters(option, names));
			assert(DLSSNRTemporal::resets == resets + 1);
			names = {"residualShowAdvanced"};
			assert(chain.ApplyLiveParameters(option,names));
			assert(DLSSNRTemporal::resets == resets + 1);
			if (count == 1) {
				names = {"pass3_intensity"};
				assert(chain.ApplyLiveParameters(option, names));
				assert(DLSSNRTemporal::resets == resets + 1);
			}
			// The chain owns time-only edits: no NR setter, history reset, resource
			// creation or draw. Mixed edits still keep their original reset rules.
			const int textures = resources.created;
			const int draws = DLSSNRTemporal::draws;
			for (float time : {5.f, 80.f, 200.f}) {
				option.parameters["antiFlickerTimeMs"] = time;
				names = {"antiFlickerTimeMs"};
				DLSSNRFilter::receivedNames.clear();
				assert(chain.GetParameterApplyMode(names[0]) == EffectParameterApplyMode::Live);
				assert(chain.ApplyLiveParameters(option, names));
				assert(DLSSNRTemporal::timeMs == time && DLSSNRTemporal::resets == resets + 1);
				assert(DLSSNRFilter::receivedNames.empty() && resources.created == textures);
				assert(DLSSNRTemporal::draws == draws);
				assert(!DLSSNRParameterChangesCurrentOutput(names[0], count));
			}
			names = {"antiFlickerTimeMs", "intensity"};
			assert(chain.ApplyLiveParameters(option, names));
			assert(DLSSNRTemporal::resets == resets + 2);
			assert(DLSSNRFilter::receivedNames == std::vector<std::string>{"intensity"});
			assert(chain.Resize(resources, &input, &output));
			assert(DLSSNRTemporal::timeMs == 200);
		}
	}
	resources.failTextureAt = resources.created;
	{ DLSSNRMultiPass chain; assert(!chain.Initialize(resources, core, &input, &output, option, false)); }
	assert(DLSSNRFilter::liveInstances == 0);
	std::cout << "Production chain wrapper: independent defaults, one backend, routing, rollback, rebuild, original temporal base and failures passed.\n";
}
