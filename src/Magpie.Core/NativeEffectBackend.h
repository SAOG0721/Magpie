#pragma once
#include "FrameGuidanceTypes.h"
#include "ScalingOptions.h"
#include "HdrEffectBoundary.h"
#include "NvidiaVfxMeasurements.h"
#include <utility>

namespace Magpie {

class DeviceResources;
struct NvidiaVfxCudaFrame;

struct NativeEffectDrawContext {
	ID3D11Texture2D* input = nullptr;
	ID3D11Texture2D* output = nullptr;
	HdrFrameMetadata inputMetadata{};
	HdrFrameMetadata outputMetadata{};
	FrameGuidanceFrameId frameId = 0;
	// IDs may skip rejected Reflex GPU attempts. Motion still points to the
	// previous accepted capture, rather than necessarily frameId - 1.
	FrameGuidanceFrameId previousCaptureFrameId = 0;
	// Content version of the actual upstream output, including ordinary new
	// captures and same-frame edits. Include in cache keys, but use the separate
	// history epoch/reset below when deciding whether to discard temporal state.
	uint64_t inputRevision = 0;
	// Separate from content versions so new captures can accumulate history.
	uint64_t inputHistoryRevision = 0;
	bool inputHistoryReset = false;
	bool isNewCaptureFrame = true;
	// Accepted-content clock and admission time travel with buffered pixels.
	// They are not refreshed when the same target is held or recomposited.
	uint64_t captureFrameIndex = 0;
	int64_t captureAdmissionTime100ns = 0;
	uint64_t captureReflexFrameId = 0;
	// Content target excludes generated frames and same-capture quality updates.
	double targetContentFrameRate = 0;
	double guidanceGpuMs = 0;
	uint64_t guidanceGpuSample = 0;
	const FrameGuidanceView& frameGuidance;
	const FrameGuidanceView& zeroFrameGuidance;
};

// Common lifetime and rendering contract for native SDK-backed effects.
// Creation parameters remain in NativeEffectBackendFactory so Renderer does
// not need one parallel container and one name-dispatch branch per SDK.
class NativeEffectBackend {
public:
	virtual ~NativeEffectBackend() = default;

	virtual void SetHdrBoundary(HdrEffectBoundaryContext context) noexcept { _hdrBoundary = std::move(context); }
	const HdrEffectBoundaryContext& GetHdrBoundary() const noexcept { return _hdrBoundary; }

	virtual FrameGuidanceRequirements GetFrameGuidanceRequirements() const noexcept {
		return {};
	}
	virtual bool Drain() noexcept { return true; }
	virtual DlssNrFailure GetDlssNrFailure() const { return {}; }
	virtual bool IsDlssNrProcessing() const noexcept { return false; }
	virtual uint32_t GetDlssNrResolutionPercent() const noexcept { return 0; }
	// Queried on the backend owner thread. Allows a completed asynchronous
	// result to invalidate the image cache without inventing a new capture.
	virtual bool HasReadyOutput() const noexcept { return false; }
	// Monotonic within this backend owner, including internal resource switches.
	// Zero means there is no unconsumed asynchronous output.
	virtual uint64_t ReadyOutputVersion() const noexcept { return 0; }
	// Owner-thread progress without rendering/presenting a frame. Implementations
	// may retire completed work and submit the latest bounded background request.
	virtual void ServiceAsync() noexcept {}
	// A previously published output has bounded continuation work to retry.
	// Completion polling can revisit the after-output hook without redrawing.
	virtual bool HasPendingPostOutputWork() const noexcept { return false; }
	// Owner-thread maintenance after a non-generated output has been published.
	// Expensive resource creation/release belongs here, after the current image
	// is visible to the frontend, rather than inside Draw or completion polling.
	virtual void ServiceAfterOutput() noexcept {}
	virtual NvidiaVfxRuntimeMeasurements TakeNvidiaVfxMeasurements() noexcept { return {}; }
	virtual bool SupportsNvidiaVfxCudaInput(ID3D11Texture2D*) const noexcept { return false; }
	virtual bool DrawNvidiaVfxCudaInput(const NativeEffectDrawContext&,
		const NvidiaVfxCudaFrame&) noexcept { return false; }

	virtual EffectParameterApplyMode GetParameterApplyMode(
		std::string_view /*parameterName*/
	) const noexcept {
		return EffectParameterApplyMode::RestartRequired;
	}

	virtual EffectParameterRestartReason GetParameterRestartReason(
		std::string_view /*parameterName*/
	) const noexcept {
		return EffectParameterRestartReason::NativeBackend;
	}

	// Called on the backend thread with a complete candidate option. Implementations
	// must not recreate resources or partially commit settings when validation fails.
	virtual bool ApplyLiveParameters(
		const EffectOption& /*option*/,
		std::span<const std::string> /*parameterNames*/
	) noexcept {
		return false;
	}

	// Backend-thread transaction, serialized with Draw/Resize/destruction.
	// A model-backed implementation may load a candidate private model here;
	// renderer-owned textures and the active model survive a failed load.
	virtual bool ApplyParameters(const EffectOption& option,
		std::span<const std::string> names) noexcept {
		return ApplyLiveParameters(option, names);
	}

	virtual bool Resize(
		DeviceResources& resources,
		ID3D11Texture2D* input,
		ID3D11Texture2D* output
	) noexcept = 0;

	virtual bool Draw(const NativeEffectDrawContext& context) noexcept = 0;
	// Describes the pixels produced by the most recent successful Draw. Buffered
	// effects return their target input and its guidance, never the physical-now
	// capture's identity. Referenced resources remain owned by the backend until
	// its next Draw/Resize/Drain. Ordinary effects preserve their input identity.
	virtual NativeEffectDrawContext OutputDrawContext(
		const NativeEffectDrawContext& inputContext) const noexcept {
		return inputContext;
	}
	// Zero keeps the legacy "successful Draw changes output" contract. Buffered
	// producers return a nonzero revision of actual output pixels, so consuming
	// another physical capture while holding an image does not reset consumers.
	virtual uint64_t OutputContentVersion() const noexcept { return 0; }

protected:
	HdrEffectBoundaryContext _hdrBoundary{};
};

}
