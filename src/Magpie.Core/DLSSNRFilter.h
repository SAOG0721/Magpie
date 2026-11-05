#pragma once
#include "NativeEffectBackend.h"
#include "GroupBEffectProtocol.h"
#include "DLSSNRPerformanceParameters.h"

namespace Magpie {

class DeviceResources;
class NgxD3D12Core;
class DLSSNRTemporal;

struct DLSSNRPerformanceSnapshot {
	uint64_t fullSubmissions = 0, sameSpecRecoveries = 0, continuousReuseOutputs = 0;
	uint64_t captureSequence = 0, fullCompleted = 0, fullLate = 0, fullPublished = 0;
	uint64_t foregroundGpuSamples = 0;
	uint64_t fullGpuSamples = 0, fullReadySamples = 0, fullAdoptionSamples = 0;
	uint64_t fullAdopted = 0, coverageSamples = 0;
	double foregroundGpuMs = 0, fullTurnaroundMs = 0;
	double fullGpuMs = 0, compositionGpuMs = 0;
	double fullReadyMs = 0, fullAdoptionMs = 0;
	double enhancementCoverage = -1;
	uint64_t sparseFullSubmissions = 0, sparseHeldOutputs = 0, sparseBurstSubmissions = 0;
	double sparsePastContributionFraction = 0;
};

struct DLSSNRSparseOutputInfo {
	bool active = false, held = false;
	uint64_t captureSequence = 0, targetSequence = 0, oldestSourceSequence = 0, epoch = 0;
	uint64_t pastSourceFrameId = 0;
	uint64_t observationRevision = 0, outputRevision = 0;
	int64_t observationTimestamp100ns = 0;
	unsigned maxSourceAge = 0;
	ID3D11ShaderResourceView* validity = nullptr;
	double pastContributionFraction = 0;
};

struct DLSSNRSettings {
	int modelPreset = 0;
	DLSSNRProcessingMode processingMode = DLSSNRProcessingMode::EveryFrame;
	DLSSNRResidualTransfer residualTransfer = DLSSNRResidualTransfer::InPlace;
	bool enableInputResolutionScaling = true;
	uint32_t inputResolutionPercent = 100;
	uint32_t sparseUpdateInterval = 2;
	float residualMultiplier = 1.0f;
	float residualSaturation = 1.0f;
	float residualLightness = 1.0f;
	float shadowStructureMultiplier = 1.0f;
	float residualMidtoneStrength = 1.0f;
	float reflectionGlowMultiplier = 1.0f;
	float residualHueProtection = 0;
	float residualChromaProtection = 0;
	float residualDarkProtection = 0;
	float residualHighlightProtection = 0;
	float residualLocalCompression = 0;
	float residualLowFrequencyGain = 1;
	float residualDetailGain = 1;
	float residualChromaTemporalStrength = 0;
	int residualDebugView = 0;
	int style = 0;
	float intensity = 1.0f;
	float localToneStrength = 1.0f;
	float localStructureStrength = 1.0f;
	float skinStructureStrength = 0.0f;
	bool useAutoMask = false;
	bool uiCorrection = false;
	MotionVectorRequest motionRequest{};
	// Experimental FP16 path. SDR RGBA8 remains the default.
	DlssnrExperimentProtocol experimentalHdr{};
};

DLSSNRSettings ParseDLSSNRSettings(const EffectOption& option, bool hdrEnabled = false) noexcept;

// Experimental same-resolution DLSS neural filter. Magpie only owns the
// composited colour frame. Motion uses shared optical flow when selected;
// depth and unavailable motion use explicit zero guides.
class DLSSNRFilter final : public NativeEffectBackend {
public:
	struct Impl;

	DLSSNRFilter();
	DLSSNRFilter(const DLSSNRFilter&) = delete;
	DLSSNRFilter& operator=(const DLSSNRFilter&) = delete;
	~DLSSNRFilter() override;

	FrameGuidanceRequirements GetFrameGuidanceRequirements() const noexcept override;
	bool Drain() noexcept override;
	EffectParameterApplyMode GetParameterApplyMode(
		std::string_view parameterName
	) const noexcept override;
	EffectParameterRestartReason GetParameterRestartReason(
		std::string_view parameterName
	) const noexcept override;
	bool ApplyLiveParameters(
		const EffectOption& option,
		std::span<const std::string> parameterNames
	) noexcept override;

	bool Initialize(
		DeviceResources& resources,
		NgxD3D12Core& ngxCore,
		ID3D11Texture2D* input,
		ID3D11Texture2D* output,
		const DLSSNRSettings& settings
	) noexcept;

	bool Resize(
		DeviceResources& resources,
		ID3D11Texture2D* input,
		ID3D11Texture2D* output
	) noexcept override;
	// One scheduling/resource owner, with independent features and cached outputs.
	bool InitializeChain(DeviceResources& resources, NgxD3D12Core& ngxCore,
		ID3D11Texture2D* input, ID3D11Texture2D* output,
		std::span<const DLSSNRSettings> passes) noexcept;

	bool Draw(const NativeEffectDrawContext& context) noexcept override;
	NativeEffectDrawContext OutputDrawContext(const NativeEffectDrawContext& context) const noexcept override;
	uint64_t OutputContentVersion() const noexcept override;
	DLSSNRSparseOutputInfo GetSparseOutputInfo() const noexcept;
	// A failed evaluation may retain legacy pass-through behavior for one pass.
	// Serial chains must detect it and reject the entire frame.
	bool IsHealthy() const noexcept;
	bool HasReadyOutput() const noexcept override;
	uint64_t ReadyOutputVersion() const noexcept override;
	void ServiceAsync() noexcept override;
	bool HasPendingPostOutputWork() const noexcept override;
	void MarkOutputPublished() noexcept;
	void ServiceAfterOutput() noexcept override;
	bool IsGpuIdle() const noexcept;
	// One bounded candidate transaction. Creation runs after output publication;
	// commit only polls initialization fences and replaces changed branches.
	bool CanPrepareResolution() const noexcept;
	bool PrepareResolution(DeviceResources& resources, ID3D11Texture2D* input,
		ID3D11Texture2D* output, uint32_t fullPercent) noexcept;
	bool CommitPreparedResolution() noexcept;
	void CancelPreparedResolution() noexcept;
	double ResolutionPreparationMs() const noexcept;
	// A single standby full chain is created before continuous output begins.
	// Its first evaluation is warmed from a real capture, never displayed.
	bool PrewarmResolution(DeviceResources& resources, ID3D11Texture2D* input,
		ID3D11Texture2D* output, uint32_t fullPercent) noexcept;
	bool WarmPreparedResolution(const NativeEffectDrawContext& context) noexcept;
	bool GetHotResolution(uint32_t& fullPercent) const noexcept;
	bool HasHotResolution(uint32_t fullPercent) const noexcept;
	void InvalidateHotResolution() noexcept;
	DLSSNRPerformanceSnapshot GetPerformanceSnapshot() const noexcept;
	void ConfigureTemporalHistory(DLSSNRTemporal& temporal) const noexcept;
	DlssNrFailure GetDlssNrFailure() const override { return _failure; }
	const std::string& LibraryInfo() const noexcept { return _libraryInfo; }

private:
	struct Sparse;
	bool InitializeSparse(DeviceResources& resources, NgxD3D12Core& core,
		ID3D11Texture2D* input, ID3D11Texture2D* output,
		std::span<const DLSSNRSettings> passes) noexcept;
	bool DrawSparse(const NativeEffectDrawContext& context) noexcept;
	bool SubmitSparseInput(bool bootstrap = false) noexcept;
	void ServiceSparse() noexcept;
	bool DrainSparse() noexcept;
	bool SparseHasPendingWork() const noexcept;
	uint64_t SparseReadyOutputVersion() const noexcept;
	void SparseAfterOutput() noexcept;
	DLSSNRPerformanceSnapshot SparsePerformanceSnapshot() const noexcept;
	void UpdateSparseParameters(bool inferenceChanged, bool residualChanged) noexcept;
	[[maybe_unused]] std::unique_ptr<Sparse> _sparse;
	struct Performance;
	struct ResolutionChange;
	bool InitializeChainResources(DeviceResources& resources, NgxD3D12Core& core,
		ID3D11Texture2D* input, ID3D11Texture2D* output,
		std::span<const DLSSNRSettings> passes, bool deferInitialization) noexcept;
	bool InitializePerformance(ID3D11Texture2D* input, ID3D11Texture2D* output) noexcept;
	bool InitializationReady() noexcept;
	void ServiceRetiredResolutions() noexcept;
	bool DrawPerformance(const NativeEffectDrawContext& context) noexcept;
	bool PublishPerformanceResult() noexcept;
	bool SubmitPerformanceInput(bool background) noexcept;
	bool DrawInference(const NativeEffectDrawContext& context, bool background = false,
		bool compositeOutput = true, ID3D11Texture2D* preparedInput = nullptr) noexcept;
	struct Submission;
	// Normal progress polls; hard recovery/live edits/teardown may reclaim CPU
	// ownership before waiting for the GPU or touching feature state.
	bool ServiceInferenceSubmission(bool wait = false) noexcept;
	bool InferenceSubmissionPending() const noexcept;
	[[maybe_unused]] std::unique_ptr<Submission> _submission;
	void UpdatePerformanceParameters(std::span<const std::string> names, bool inferenceChanged) noexcept;
	[[maybe_unused]] std::unique_ptr<Performance> _performance;
	[[maybe_unused]] std::unique_ptr<ResolutionChange> _resolutionChange;
	[[maybe_unused]] uint64_t _outputVersionBase = 0;
	[[maybe_unused]] bool _foregroundQueue = false;
	DlssNrFailure _failure;
	std::string _libraryInfo;
	// 仅在 MP_ENABLE_DLSSNR 构建中使用；无 SDK 的 CI 构建里 ClangCL -Werror 会报未使用
	[[maybe_unused]] std::unique_ptr<Impl> _impl;
	[[maybe_unused]] DLSSNRSettings _settings;
	[[maybe_unused]] std::vector<DLSSNRSettings> _passSettings;
	[[maybe_unused]] NgxD3D12Core* _ngxCore = nullptr;
};

}
