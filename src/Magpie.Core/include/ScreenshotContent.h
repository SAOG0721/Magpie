#pragma once
#include <cstdint>

namespace Magpie {

// Persisted values and Home's SelectedIndex share this stable order.
enum class ScreenshotContent : uint32_t {
	InputAndOutput = 0,
	OutputOnly = 1,
	InputOnly = 2
};

constexpr ScreenshotContent NormalizeScreenshotContent(uint32_t value) noexcept {
	return value <= 2 ? static_cast<ScreenshotContent>(value) : ScreenshotContent::OutputOnly;
}

struct ScreenshotFrameIdentity {
	uint64_t frameId = 0;
	uint64_t captureSequence = 0;
	uint64_t resourceGeneration = 0;

	constexpr bool Matches(const ScreenshotFrameIdentity& other, bool generated) const noexcept {
		// Sequence zero is valid for capture methods that never interrupt.
		return !generated && frameId != 0 && resourceGeneration != 0 &&
			frameId == other.frameId && captureSequence == other.captureSequence &&
			resourceGeneration == other.resourceGeneration;
	}
};

// A pending pair can finish against the output fixed at request time, or a
// later successfully presented real candidate. Dropped inputs must be retried,
// never paired with an unrelated latest output. No content/pacing counters here.
enum class ScreenshotSelection { Input, FixedOutput, PresentedOutput, RetryInput, Wait };
constexpr ScreenshotSelection SelectScreenshotCandidate(ScreenshotContent content,
	const ScreenshotFrameIdentity& input, const ScreenshotFrameIdentity& fixedOutput,
	bool fixedGenerated, const ScreenshotFrameIdentity& presentedOutput, bool presentedGenerated) noexcept {
	if (content == ScreenshotContent::InputOnly) return ScreenshotSelection::Input;
	if (input.Matches(fixedOutput, fixedGenerated)) return ScreenshotSelection::FixedOutput;
	if (input.Matches(presentedOutput, presentedGenerated)) return ScreenshotSelection::PresentedOutput;
	if (presentedOutput.captureSequence == input.captureSequence &&
		presentedOutput.resourceGeneration == input.resourceGeneration &&
		presentedOutput.frameId > input.frameId) return ScreenshotSelection::RetryInput;
	return ScreenshotSelection::Wait;
}

}
