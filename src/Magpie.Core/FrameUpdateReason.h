#pragma once
#include <cstdint>

namespace Magpie {

// Describes the pixels in one publication, independently of slot consumption.
// Only captured/generated content advances content pacing or temporal input.
enum class FrameUpdateReason : uint8_t {
	NewCapture,
	QualityRefresh,
	ParameterUpdate,
	RenderRefresh,
	GeneratedFrame
};

constexpr bool IsContentUpdate(FrameUpdateReason reason) noexcept {
	return reason == FrameUpdateReason::NewCapture || reason == FrameUpdateReason::GeneratedFrame;
}

}
