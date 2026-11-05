#pragma once
#include "MotionVectorRequest.h"
#include "HdrFrame.h"
#include "FrameGuidanceAvailability.h"

namespace Magpie {

using FrameGuidanceFrameId = uint64_t;

struct FrameGuidanceExtent {
	uint32_t width = 0;
	uint32_t height = 0;

	bool IsValid() const noexcept {
		return width != 0 && height != 0;
	}

	bool operator==(const FrameGuidanceExtent&) const noexcept = default;
};

struct FrameGuidanceRegion {
	uint32_t x = 0;
	uint32_t y = 0;
	uint32_t width = 0;
	uint32_t height = 0;

	static FrameGuidanceRegion Full(FrameGuidanceExtent extent) noexcept {
		return { 0, 0, extent.width, extent.height };
	}

	bool IsInside(FrameGuidanceExtent extent) const noexcept {
		return width != 0 && height != 0 && x <= extent.width &&
			y <= extent.height && width <= extent.width - x &&
			height <= extent.height - y;
	}

	bool operator==(const FrameGuidanceRegion&) const noexcept = default;
};

enum class FrameGuidanceResetReason : uint8_t {
	None,
	Initialize,
	Resize,
	SceneChange,
	CaptureInterrupted,
	DeviceRecreated,
	LongPause,
	ProviderFailure
};

enum class FrameGuidanceMotionDirection : uint8_t {
	CurrentToPrevious,
	PreviousToCurrent
};

enum class FrameGuidanceMotionUnit : uint8_t {
	SourcePixels
};

struct FrameGuidanceSyncPoint {
	// Null means that the texture is ordered on Renderer’s immediate D3D11
	// context. Providers that execute elsewhere must publish a fence and value.
	ID3D11Fence* fence = nullptr;
	uint64_t value = 0;
};

struct FrameGuidanceMetadata {
    FrameGuidanceFrameId frameId = 0;
    uint64_t captureSequence = 0;
    uint64_t resourceGeneration = 0;
    int64_t timestamp100ns = 0;
	FrameGuidanceExtent sourceExtent{};
	FrameGuidanceRegion validRegion{};
	FrameGuidanceSyncPoint sync{};
	FrameGuidanceResetReason resetReason = FrameGuidanceResetReason::None;
	bool valid = false;
	bool isZero = false;
	bool requiresHistoryReset = false;
};

struct FrameGuidanceResource {
	ID3D11Texture2D* texture = nullptr;
	DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
	FrameGuidanceMetadata metadata{};

	bool IsValid(
		DXGI_FORMAT expectedFormat,
		FrameGuidanceFrameId expectedFrameId,
		FrameGuidanceExtent expectedExtent
	) const noexcept {
		return texture && format == expectedFormat && metadata.valid &&
			metadata.frameId == expectedFrameId &&
			metadata.sourceExtent == expectedExtent &&
			metadata.validRegion.IsInside(expectedExtent);
	}
};

// Both fields belong to the pair published with currentFrameId. Forward
// vectors are sampled in current coordinates; reverse vectors in previous
// coordinates. Frame IDs may skip rejected captures, so never infer the pair
// from arithmetic on either ID.
struct FrameGuidanceMotionPair {
	FrameGuidanceFrameId currentFrameId = 0;
	FrameGuidanceFrameId previousFrameId = 0;
	uint64_t currentCaptureSequence = 0;
	uint64_t previousCaptureSequence = 0;
	bool valid = false;

	bool operator==(const FrameGuidanceMotionPair&) const noexcept = default;
};

struct FrameGuidanceView {
	FrameGuidanceResource depth{};
	FrameGuidanceResource motion{};
	FrameGuidanceResource confidence{};
	FrameGuidanceMotionDirection motionDirection =
		FrameGuidanceMotionDirection::CurrentToPrevious;
	FrameGuidanceMotionUnit motionUnit = FrameGuidanceMotionUnit::SourcePixels;
	bool requiresHistoryReset = false;
	// Optional channels. Existing consumers and IsValidFor retain their
	// three-channel contract; absence never changes ordinary forward guidance.
	FrameGuidanceResource reverseMotion{};
	FrameGuidanceResource reverseConfidence{};
	FrameGuidanceMotionPair motionPair{};
	FrameGuidanceAvailability motionAvailability = FrameGuidanceAvailability::Unavailable;

	bool HasReverseMotionForPair(FrameGuidanceFrameId currentFrameId,
		FrameGuidanceFrameId previousFrameId, FrameGuidanceExtent extent) const noexcept {
		return IsValidFor(currentFrameId, extent) &&
			motionDirection == FrameGuidanceMotionDirection::CurrentToPrevious &&
			motionUnit == FrameGuidanceMotionUnit::SourcePixels &&
			motionPair.valid && motionPair.currentFrameId == currentFrameId &&
			motionPair.previousFrameId == previousFrameId && !requiresHistoryReset &&
			reverseMotion.IsValid(DXGI_FORMAT_R16G16_FLOAT, currentFrameId, extent) &&
			reverseConfidence.IsValid(DXGI_FORMAT_R8_UNORM, currentFrameId, extent) &&
			!reverseMotion.metadata.isZero && !reverseConfidence.metadata.isZero &&
			!reverseMotion.metadata.requiresHistoryReset && !reverseConfidence.metadata.requiresHistoryReset &&
			reverseMotion.metadata.validRegion == reverseConfidence.metadata.validRegion &&
			reverseMotion.metadata.resourceGeneration == reverseConfidence.metadata.resourceGeneration &&
			reverseMotion.metadata.resourceGeneration == motion.metadata.resourceGeneration &&
			motion.metadata.captureSequence == motionPair.currentCaptureSequence &&
			reverseMotion.metadata.captureSequence == motionPair.currentCaptureSequence &&
			reverseConfidence.metadata.captureSequence == motionPair.currentCaptureSequence;
	}

	bool IsValidFor(
		FrameGuidanceFrameId frameId,
		FrameGuidanceExtent extent
	) const noexcept {
		return depth.IsValid(DXGI_FORMAT_R32_FLOAT, frameId, extent) &&
			motion.IsValid(DXGI_FORMAT_R16G16_FLOAT, frameId, extent) &&
			confidence.IsValid(DXGI_FORMAT_R8_UNORM, frameId, extent) &&
			depth.metadata.validRegion == motion.metadata.validRegion &&
			motion.metadata.validRegion == confidence.metadata.validRegion;
	}
};

inline FrameGuidanceView SelectFrameGuidanceChannels(
	const FrameGuidanceView& produced,
	const FrameGuidanceView& zero,
	FrameGuidanceFrameId frameId,
	FrameGuidanceExtent extent,
	bool useMotion
) noexcept {
	if (!zero.IsValidFor(frameId, extent)) return {};

	FrameGuidanceView selected = produced.IsValidFor(frameId, extent) ?
		produced : zero;
	selected.depth = zero.depth;
	if (!useMotion) {
		selected.motion = zero.motion;
		selected.confidence = zero.confidence;
		selected.reverseMotion = {};
		selected.reverseConfidence = {};
		selected.motionPair = {};
		selected.motionAvailability = FrameGuidanceAvailability::Unavailable;
	}
	selected.requiresHistoryReset =
		selected.depth.metadata.requiresHistoryReset ||
		selected.motion.metadata.requiresHistoryReset ||
		selected.confidence.metadata.requiresHistoryReset;
	return selected.IsValidFor(frameId, extent) ? selected : zero;
}

struct MotionVectorProviderOutput;

struct FrameGuidanceFrame {
    ID3D11Texture2D* color = nullptr;
    FrameGuidanceFrameId frameId = 0;
    uint64_t captureSequence = 0;
    uint64_t resourceGeneration = 0;
    int64_t timestamp100ns = 0;
    ColorDescription colorDescription{};
	FrameGuidanceExtent sourceExtent{};
	FrameGuidanceRegion validRegion{};

	bool ContinuesMotionHistory(const FrameGuidanceFrame& previous) const noexcept {
		// CaptureSequence names the capture session, not a per-frame counter.
		// Accepted frame IDs may skip rejected attempts within that same session.
		return frameId > previous.frameId && captureSequence == previous.captureSequence &&
			resourceGeneration == previous.resourceGeneration && sourceExtent == previous.sourceExtent &&
			(!timestamp100ns || !previous.timestamp100ns || timestamp100ns > previous.timestamp100ns);
	}
};

struct DepthProviderOutput {
	FrameGuidanceResource depth{};
};

struct MotionVectorProviderOutput {
	FrameGuidanceResource motion{};
	FrameGuidanceResource confidence{};
	FrameGuidanceResource reverseMotion{};
	FrameGuidanceResource reverseConfidence{};
	FrameGuidanceMotionPair motionPair{};
};

}
