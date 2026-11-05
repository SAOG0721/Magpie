#pragma once
#include "pch.h"
#include "ScalingWindow.h"

// The standalone process has no XAML resource host. Only the immutable SDR
// session-options lookup is substituted; service/provider GPU code is compiled
// directly from production sources below. No window or user config is opened.
namespace Magpie {
class FrameGuidanceScalingProbeHost {
public:
	static FrameGuidanceScalingProbeHost& Get() noexcept { static FrameGuidanceScalingProbeHost host; return host; }
	const ScalingOptions& Options() const noexcept { return _options; }
private:
	ScalingOptions _options;
};
}
#define ScalingWindow FrameGuidanceScalingProbeHost
