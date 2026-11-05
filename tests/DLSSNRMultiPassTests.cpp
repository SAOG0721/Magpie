#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <chrono>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <vector>
#include "DLSSNRParameters.h"
#include "DlssNrStatus.h"
#include "EffectParameterRules.h"
#include "MotionVectorRequest.h"
#include "DLSSNRAdaptiveResolution.h"

// Execute the production chain with deterministic SDK and texture doubles.
// This checks routing/lifetime/failure behavior without invoking NGX or a GPU.
using HRESULT = int;
constexpr int D3D11_USAGE_DEFAULT = 0;
constexpr int D3D11_BIND_SHADER_RESOURCE = 1;
constexpr int D3D11_BIND_UNORDERED_ACCESS = 2;
constexpr int DXGI_FORMAT_R16G16B16A16_FLOAT = 10;
inline bool FAILED(HRESULT value) { return value < 0; }
struct D3D11_TEXTURE2D_DESC {
	int Width = 1280, Height = 720, Usage = 0, BindFlags = 0, CPUAccessFlags = 0, MiscFlags = 0, Format = 0;
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
struct EffectOption { std::map<std::string, float> parameters; uint32_t inputResolutionPercent = 100; };
enum class EffectParameterApplyMode { Live, RestartRequired, Unavailable };
enum class EffectParameterRestartReason { None, ResourceRecreation, FrameGuidance };
struct HdrEffectBoundaryContext {};
struct FixtureFrameMetadata {
	uint64_t frameId = 0, captureSequence = 0, resourceGeneration = 0;
	int64_t timestamp100ns = 0, captureAdmissionTime100ns = 0;
	bool operator==(const FixtureFrameMetadata&) const noexcept = default;
};
struct FixtureFrameGuidance {
	struct {
		const void* texture = nullptr;
		struct { uint64_t frameId = 0, timestamp100ns = 0; } metadata;
	} motion;
	bool requiresHistoryReset = false;
};
struct NativeEffectDrawContext {
	ID3D11Texture2D* input = nullptr;
	ID3D11Texture2D* output = nullptr;
	uint64_t frameId = 1;
	uint64_t inputRevision = 0;
	uint64_t inputHistoryRevision = 0;
	bool inputHistoryReset = false;
	bool isNewCaptureFrame = true;
	double targetContentFrameRate = 0;
	double guidanceGpuMs = 0;
	uint64_t guidanceGpuSample = 0;
	FixtureFrameGuidance frameGuidance;
	uint64_t previousCaptureFrameId = 0, captureFrameIndex = 0;
	int64_t captureAdmissionTime100ns = 0;
	uint64_t captureReflexFrameId = 0;
	FixtureFrameMetadata inputMetadata, outputMetadata;
	FixtureFrameGuidance zeroFrameGuidance;
};
struct NativeEffectBackend {
	virtual ~NativeEffectBackend() = default;
	virtual void SetHdrBoundary(HdrEffectBoundaryContext value) noexcept { _hdrBoundary = value; }
	virtual FrameGuidanceRequirements GetFrameGuidanceRequirements() const noexcept = 0;
	virtual bool Drain() noexcept = 0;
	virtual DlssNrFailure GetDlssNrFailure() const { return {}; }
	virtual bool IsDlssNrProcessing() const noexcept { return false; }
	virtual bool HasReadyOutput() const noexcept { return false; }
	virtual uint64_t ReadyOutputVersion() const noexcept { return 0; }
	virtual void ServiceAsync() noexcept {}
	virtual bool HasPendingPostOutputWork() const noexcept { return false; }
	virtual void ServiceAfterOutput() noexcept {}
	virtual uint32_t GetDlssNrResolutionPercent() const noexcept { return 0; }
	virtual bool Resize(struct DeviceResources&, ID3D11Texture2D*, ID3D11Texture2D*) noexcept = 0;
	virtual bool Draw(const NativeEffectDrawContext&) noexcept = 0;
	virtual NativeEffectDrawContext OutputDrawContext(const NativeEffectDrawContext& input) const noexcept { return input; }
	virtual uint64_t OutputContentVersion() const noexcept { return 0; }
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
	inline static std::optional<NativeEffectDrawContext> lastContext;
	ID3D11Texture2D* raw = nullptr;
	bool Initialize(DeviceResources&, ID3D11Texture2D*, ID3D11Texture2D* base,
		ID3D11Texture2D* value, ID3D11Texture2D*, int, bool, float time) {
		raw = value; lastBase = base; timeMs = time; return true;
	}
	void Reset() { ++resets; }
	void SetTimeConstant(float value) { timeMs = NormalizeDLSSNRAntiFlickerTimeMs(value); }
	void ConfigureDetail(float, bool) {}
	bool Draw(const NativeEffectDrawContext& context, bool = false) {
		++draws; lastContext = context; context.output->value = raw->value; return true;
	}
};
struct Logger {
	static Logger& Get() { static Logger value; return value; }
	void Info(std::string_view) {}
	void Error(std::string_view) {}
	void Warn(std::string_view) {}
	void ComError(std::string_view, HRESULT) {}
};
inline EffectOption ParseDLSSNRSettings(const EffectOption& value, bool) { return value; }
using DLSSNRSettings = EffectOption;
struct DLSSNRFilter {
	inline static int liveInstances = 0, featureCount = 0;
	inline static bool failInit = false, failApply = false, failDraw = false, healthy = true;
	inline static bool failPrewarm = false, failWarm = false;
	inline static int prewarmCalls = 0, warmCalls = 0, hotInvalidations = 0;
	inline static std::vector<std::string> operations;
	inline static std::vector<std::string> receivedNames;
	inline static std::vector<uint32_t> initializedPercents;
	// Script the SDK owner's next successful result. The chain under test must
	// transport this result's target, not substitute the physical capture.
	inline static std::optional<NativeEffectDrawContext> nextOutputContext;
	inline static uint64_t nextOutputVersion = 0;
	std::optional<NativeEffectDrawContext> drawnOutputContext;
	uint64_t drawnOutputVersion = 0;
	std::vector<EffectOption> settings;
	uint32_t hotFull = 0;
	bool prewarmed = false, hot = false;
	DLSSNRFilter() { ++liveInstances; }
	~DLSSNRFilter() { --liveInstances; featureCount -= static_cast<int>(settings.size()); }
	std::string LibraryInfo() const { return "fixture library"; }
	bool IsHealthy() const { return healthy; }
	void ConfigureTemporalHistory(DLSSNRTemporal&) const noexcept {}
	bool HasReadyOutput() const noexcept { return false; }
	uint64_t ReadyOutputVersion() const noexcept { return 0; }
	NativeEffectDrawContext OutputDrawContext(const NativeEffectDrawContext& context) const noexcept {
		return drawnOutputContext.value_or(context);
	}
	uint64_t OutputContentVersion() const noexcept { return drawnOutputVersion; }
	bool IsGpuIdle() const noexcept { return true; }
	void ServiceAsync() noexcept {}
	bool HasPendingPostOutputWork() const noexcept { return false; }
	void MarkOutputPublished() noexcept {}
	void ServiceAfterOutput() noexcept {}
	bool CanPrepareResolution() const noexcept { return true; }
	bool PrepareResolution(DeviceResources&, ID3D11Texture2D*, ID3D11Texture2D*, uint32_t) noexcept { return !failInit; }
	bool CommitPreparedResolution() noexcept { return true; }
	void CancelPreparedResolution() noexcept {}
	double ResolutionPreparationMs() const noexcept { return 0; }
	bool PrewarmResolution(DeviceResources&, ID3D11Texture2D*, ID3D11Texture2D*, uint32_t full) noexcept {
		++prewarmCalls; hotFull = full;
		prewarmed = !failPrewarm; return prewarmed;
	}
	bool WarmPreparedResolution(const NativeEffectDrawContext&) noexcept {
		++warmCalls; operations.push_back("warm");
		hot = prewarmed && !failWarm; prewarmed = false; return hot;
	}
	bool GetHotResolution(uint32_t& full) const noexcept {
		if (!hot) return false;
		full = hotFull; return true;
	}
	bool HasHotResolution(uint32_t full) const noexcept { return hot && hotFull == full; }
	void InvalidateHotResolution() noexcept { ++hotInvalidations; hot = prewarmed = false; }
	struct Snapshot {
		double foregroundGpuMs = 0, fullTurnaroundMs = 0, enhancementCoverage = 1;
		uint64_t foregroundGpuSamples = 0, fullCompleted = 0, fullLate = 0, fullPublished = 0;
		double fullGpuMs = 0, compositionGpuMs = 0, fullReadyMs = 0, fullAdoptionMs = 0;
		uint64_t fullGpuSamples = 0, fullReadySamples = 0, fullAdoptionSamples = 0, fullAdopted = 0, coverageSamples = 0;
	};
	Snapshot GetPerformanceSnapshot() const noexcept { return {}; }
	DlssNrFailure GetDlssNrFailure() const { return { DlssNrStage::Evaluate, DlssNrReason::Unknown, DlssNrCodeKind::Ngx, 0xBAD00001, "fixture Evaluate" }; }
	bool InitializeChain(DeviceResources&, NgxD3D12Core&, ID3D11Texture2D* input,
		ID3D11Texture2D* output, std::span<const EffectOption> options) {
		assert(input != output);
		if (failInit) return false;
		settings.assign(options.begin(), options.end());
		initializedPercents.clear();
		for (const auto& setting:settings) initializedPercents.push_back(setting.inputResolutionPercent);
		featureCount += static_cast<int>(settings.size());
		return true;
	}
	void SetHdrBoundary(HdrEffectBoundaryContext) {}
	FrameGuidanceRequirements GetFrameGuidanceRequirements() const {
		FrameGuidanceRequirements result{true};
		if (!settings.empty()) {
			const auto it = settings.front().parameters.find("opticalFlowMethod");
			if (it != settings.front().parameters.end() && it->second == 1.f)
				result.Add(MotionVectorRequest::Amd(AmdOpticalFlowMode::Quality));
		}
		return result;
	}
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
		operations.push_back("draw");
		if (failDraw) return false;
		const auto& target = nextOutputContext ? *nextOutputContext : context;
		int value = target.input->value;
		for (const auto& pass : settings) {
			const auto it = pass.parameters.find("intensity");
			value = value * 10 + (it == pass.parameters.end() ? 1 : static_cast<int>(it->second));
		}
		context.output->value = value;
		drawnOutputContext = nextOutputContext;
		drawnOutputVersion = nextOutputContext ? nextOutputVersion : 0;
		return true;
	}
};}
#define private public
#include "DLSSNRMultiPassUnderTest.h"
#undef private

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
	option.parameters["antiFlicker"] = 2;
	option.parameters["opticalFlowMethod"] = 0;
	{ DLSSNRMultiPass chain; assert(!chain.Initialize(resources, core, &input, &output, option, false)); }
	for (int mode : {1, 2, 3, 4}) {
		option.parameters["antiFlicker"] = static_cast<float>(mode);
		option.parameters["opticalFlowMethod"] = mode >= 2 ? 1.f : 0.f;
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
	// Manual percentages must reach every backend pass and stay truthful after
	// Draw/Resize; the automatic policy's 5% tier quantization is inactive here.
	for (int mode=0;mode<2;++mode) for (uint32_t percent:{25u,37u,50u,62u,75u,87u,100u}) {
		EffectOption manual{{{"resolutionControl",1.f},{"processingMode",float(mode)},
			{"opticalFlowMethod",2.f},{"residualTransfer",1.f},{"multiPass",3.f}}};
		manual.inputResolutionPercent=percent;
		DLSSNRMultiPass chain;
		assert(chain.Initialize(resources,core,&input,&output,manual,false));
		assert(DLSSNRFilter::initializedPercents==std::vector<uint32_t>(3,percent));
		assert(chain.GetDlssNrResolutionPercent()==percent);
		assert(chain.Draw({&input,&output,1}) && chain.GetDlssNrResolutionPercent()==percent);
		assert(chain.Resize(resources,&input,&output));
		assert(DLSSNRFilter::initializedPercents==std::vector<uint32_t>(3,percent));
	}
	for (const float retired : {2.f,3.f}) {
		EffectOption unsupported{{{"processingMode",retired}}};
		DLSSNRMultiPass chain;
		assert(!chain.Initialize(resources,core,&input,&output,unsupported,false));
	}
	// Adaptive startup pays for one neighboring tier before the first output.
	// A failed prebuild/evaluation retains the active chain without retrying it
	// during subsequent captures; a live edit invalidates that exact cache key.
	EffectOption automatic{{{"resolutionControl",0.f},{"processingMode",0.f},{"intensity",2.f}}};
	automatic.inputResolutionPercent = 65;
	for (bool failPrewarm : {false,true}) for (bool failWarm : {false,true}) {
		DLSSNRFilter::failPrewarm = failPrewarm; DLSSNRFilter::failWarm = failWarm;
		DLSSNRFilter::prewarmCalls = DLSSNRFilter::warmCalls = 0;
		DLSSNRFilter::operations.clear();
		DLSSNRMultiPass chain;
		assert(chain.Initialize(resources,core,&input,&output,automatic,false));
		assert(DLSSNRFilter::prewarmCalls == 1 && chain.GetDlssNrResolutionPercent() == 65);
		assert(chain.Draw({&input,&output,1}));
		assert(output.value == 2);
		assert(DLSSNRFilter::operations == (failPrewarm ? std::vector<std::string>{"draw"} :
			std::vector<std::string>{"warm","draw"}));
		assert(chain.Draw({&input,&output,2}));
		assert(DLSSNRFilter::warmCalls == (failPrewarm ? 0 : 1));
	}
	DLSSNRFilter::failPrewarm = DLSSNRFilter::failWarm = false;
	{
		DLSSNRMultiPass chain;
		assert(chain.Initialize(resources,core,&input,&output,automatic,false));
		const int invalidations = DLSSNRFilter::hotInvalidations;
		const int features = DLSSNRFilter::featureCount, warms = DLSSNRFilter::warmCalls;
		const std::vector<std::string> names{"performanceShowAdvanced"};
		for (float expanded : {0.f,1.f,0.f}) {
			automatic.parameters["performanceShowAdvanced"] = expanded;
			assert(chain.GetParameterApplyMode(names.front()) == EffectParameterApplyMode::Live);
			assert(chain.ApplyLiveParameters(automatic,names));
			assert(DLSSNRFilter::hotInvalidations == invalidations);
			assert(DLSSNRFilter::featureCount == features);
		}
		assert(chain.Draw({&input,&output,1}));
		assert(DLSSNRFilter::warmCalls == warms+1); // Display edits retain startup prewarm.
		automatic.parameters["performanceShowAdvanced"] = 1;
		assert(chain.ApplyLiveParameters(automatic,names));
		assert(DLSSNRFilter::hotInvalidations == invalidations);
		assert(DLSSNRFilter::featureCount == features);
	}
	{
		DLSSNRMultiPass chain;
		assert(chain.Initialize(resources,core,&input,&output,automatic,false));
		const int invalidations = DLSSNRFilter::hotInvalidations, warms = DLSSNRFilter::warmCalls;
		const std::vector<std::string> names{"intensity"};
		assert(chain.ApplyLiveParameters(automatic,names));
		assert(DLSSNRFilter::hotInvalidations == invalidations+1);
		assert(chain.Draw({&input,&output,1}));
		assert(DLSSNRFilter::warmCalls == warms);
	}
	{
		automatic.parameters["resolutionControl"] = 1;
		const int prewarms = DLSSNRFilter::prewarmCalls;
		DLSSNRMultiPass chain;
		assert(chain.Initialize(resources,core,&input,&output,automatic,false));
		assert(DLSSNRFilter::prewarmCalls == prewarms);
	}
	// Retain the internal legacy output contract by setting its private fixture
	// flag directly. No supported selector can enable this async wrapper path.
	for (int temporal : {0,1}) for (int passes : {1,3}) {
		ID3D11Texture2D physicalInput, firstTargetInput, nextTargetInput, failedTargetInput;
		physicalInput.value = 9; firstTargetInput.value = 4; nextTargetInput.value = 6; failedTargetInput.value = 8;
		EffectOption route{{{"processingMode",1.f},{"resolutionControl",1.f},
			{"residualTransfer",1.f},{"opticalFlowMethod",2.f},{"nvidiaOpticalFlowQuality",1.f},
			{"antiFlicker",float(temporal)},{"multiPass",float(passes)}}};
		route.inputResolutionPercent = 75;
		const auto contextFor = [&](ID3D11Texture2D& texture,uint64_t frame,uint64_t previous,uint64_t capture) {
			NativeEffectDrawContext context{&texture,&output,frame};
			context.previousCaptureFrameId = previous;
			context.inputRevision = frame * 10;
			context.inputHistoryRevision = 3;
			context.captureFrameIndex = capture;
			context.captureReflexFrameId = 1000 + capture;
			context.captureAdmissionTime100ns = int64_t(frame * 100 + 1);
			context.inputMetadata = {frame,capture,7,int64_t(frame*100),context.captureAdmissionTime100ns};
			context.outputMetadata = context.inputMetadata;
			context.frameGuidance.motion.texture = &texture;
			context.frameGuidance.motion.metadata = {frame,frame*100};
			context.zeroFrameGuidance.motion.texture = &texture;
			context.zeroFrameGuidance.motion.metadata = {frame,frame*100};
			return context;
		};
		const auto expectTarget = [](const NativeEffectDrawContext& actual,const NativeEffectDrawContext& expected) {
			assert(actual.input == expected.input && actual.output == expected.output && actual.frameId == expected.frameId);
			assert(actual.previousCaptureFrameId == expected.previousCaptureFrameId && actual.inputRevision == expected.inputRevision);
			assert(actual.inputHistoryRevision == expected.inputHistoryRevision && actual.inputHistoryReset == expected.inputHistoryReset);
			assert(actual.captureFrameIndex == expected.captureFrameIndex && actual.captureAdmissionTime100ns == expected.captureAdmissionTime100ns);
			assert(actual.captureReflexFrameId == expected.captureReflexFrameId);
			assert(actual.inputMetadata == expected.inputMetadata && actual.outputMetadata == expected.outputMetadata);
			assert(actual.isNewCaptureFrame == expected.isNewCaptureFrame);
			assert(actual.frameGuidance.motion.texture == expected.frameGuidance.motion.texture &&
				actual.frameGuidance.motion.metadata.frameId == expected.frameGuidance.motion.metadata.frameId &&
				actual.frameGuidance.motion.metadata.timestamp100ns == expected.frameGuidance.motion.metadata.timestamp100ns &&
				actual.frameGuidance.requiresHistoryReset == expected.frameGuidance.requiresHistoryReset);
			assert(actual.zeroFrameGuidance.motion.texture == expected.zeroFrameGuidance.motion.texture &&
				actual.zeroFrameGuidance.motion.metadata.frameId == expected.zeroFrameGuidance.motion.metadata.frameId);
		};
		auto physical = contextFor(physicalInput,40,30,9);
		physical.inputHistoryRevision = 999; physical.inputHistoryReset = true;
		physical.frameGuidance.requiresHistoryReset = true;
		auto target = contextFor(firstTargetInput,20,11,6);
		DLSSNRFilter::nextOutputContext = target; DLSSNRFilter::nextOutputVersion = 7;
		DLSSNRTemporal::lastContext.reset();
		DLSSNRMultiPass chain;
		const auto prewarms = DLSSNRFilter::prewarmCalls;
		assert(chain.Initialize(resources,core,&physicalInput,&output,route,false));
		chain._sparseProcessing = true;
		assert(chain.OutputContentVersion() == 0 && DLSSNRFilter::prewarmCalls == prewarms);
		assert(chain.Draw(physical));
		expectTarget(chain.OutputDrawContext(physical),target);
		assert(output.value == (passes == 1 ? 41 : 4111));
		const auto initialVersion = chain.OutputContentVersion();
		assert(initialVersion != 0);
		if (temporal) { assert(DLSSNRTemporal::lastContext); expectTarget(*DLSSNRTemporal::lastContext,target); }
		else assert(!DLSSNRTemporal::lastContext);
		// A new physical capture is not a new delivered target or pixel version.
		physical = contextFor(physicalInput,41,40,10);
		target.isNewCaptureFrame = false; DLSSNRFilter::nextOutputContext = target;
		assert(chain.Draw(physical));
		expectTarget(chain.OutputDrawContext(physical),target);
		assert(chain.OutputContentVersion() == initialVersion && output.value == (passes == 1 ? 41 : 4111));
		if (temporal) expectTarget(*DLSSNRTemporal::lastContext,target);
		// Post-control edits can change the same target. The wrapper revision
		// changes once even if the underlying filter's target version is held.
		route.parameters["residualMultiplier"] = .5f;
		const std::vector<std::string> postEdit{"residualMultiplier"};
		assert(chain.ApplyLiveParameters(route,postEdit) && chain.Draw(physical));
		const auto editedVersion = chain.OutputContentVersion();
		assert(editedVersion == initialVersion + 1);
		expectTarget(chain.OutputDrawContext(physical),target);
		assert(chain.Draw(physical) && chain.OutputContentVersion() == editedVersion);
		// Display and response-time edits do not create an immediate new image.
		route.parameters["performanceShowAdvanced"] = 1;
		const std::vector<std::string> displayEdit{"performanceShowAdvanced"};
		assert(chain.ApplyLiveParameters(route,displayEdit) && chain.Draw(physical));
		assert(chain.OutputContentVersion() == editedVersion);
		route.parameters["antiFlickerTimeMs"] = 5;
		const std::vector<std::string> timeEdit{"antiFlickerTimeMs"};
		assert(chain.ApplyLiveParameters(route,timeEdit) && chain.Draw(physical));
		assert(chain.OutputContentVersion() == editedVersion);
		// A later completed target replaces all source identities together.
		auto nextTarget = contextFor(nextTargetInput,30,20,7);
		DLSSNRFilter::nextOutputContext = nextTarget; DLSSNRFilter::nextOutputVersion = 8;
		physical = contextFor(physicalInput,42,41,11);
		assert(chain.Draw(physical));
		expectTarget(chain.OutputDrawContext(physical),nextTarget);
		assert(chain.OutputContentVersion() == editedVersion + 1 && output.value == (passes == 1 ? 61 : 6111));
		if (temporal) expectTarget(*DLSSNRTemporal::lastContext,nextTarget);
		// A failed candidate draw commits neither a target nor a pixel revision.
		DLSSNRFilter::nextOutputContext = contextFor(failedTargetInput,35,30,8);
		DLSSNRFilter::nextOutputVersion = 9; DLSSNRFilter::failDraw = true;
		assert(!chain.Draw(physical));
		assert(chain.OutputContentVersion() == editedVersion + 1);
		expectTarget(chain.OutputDrawContext(physical),nextTarget);
		DLSSNRFilter::failDraw = false;
		DLSSNRFilter::nextOutputContext.reset(); DLSSNRFilter::nextOutputVersion = 0;
	}
	resources.failTextureAt = resources.created;
	{ DLSSNRMultiPass chain; assert(!chain.Initialize(resources, core, &input, &output, option, false)); }
	assert(DLSSNRFilter::liveInstances == 0);
	std::cout << "Production chain wrapper: two public modes, rejected async selectors, integer manual ratios, one backend, routing, rollback, rebuild, original temporal base, retained legacy target/guidance/clocks and failures passed.\n";
}
