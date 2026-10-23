#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Magpie {

struct OverlayWindowOption {
	uint16_t hArea = 0;
	uint16_t vArea = 0;
	float hPos = 0.0f;
	float vPos = 0.0f;
	float width = 0.0f;
	float height = 0.0f;
};

inline void SanitizeOverlayWindowOption(OverlayWindowOption& option) noexcept {
	auto sanitizeAxis = [](uint16_t& area, float& pos) {
		if (area > 2) area = 0;
		if (!std::isfinite(pos) || pos < 0.0f) pos = 0.0f;
		if (area == 1) pos = std::min(pos, 1.0f);
	};
	sanitizeAxis(option.hArea, option.hPos);
	sanitizeAxis(option.vArea, option.vPos);
	if (!std::isfinite(option.width) || option.width < 0.0f) option.width = 0.0f;
	if (!std::isfinite(option.height) || option.height < 0.0f) option.height = 0.0f;
}

struct OverlayWindowRect {
	float x, y, width, height;
};

inline OverlayWindowRect RestoreOverlayWindow(
	OverlayWindowOption option, float viewportWidth, float viewportHeight, float dpiScale,
	float defaultWidth, float defaultHeight, float minimumWidth, float minimumHeight
) noexcept {
	SanitizeOverlayWindowOption(option);
	auto restoreAxis = [dpiScale](uint16_t area, float pos, float preferred,
		float defaultSize, float minimum, float viewport, float& origin, float& size) {
		const float maximum = std::max(1.0f, viewport - 16.0f * dpiScale);
		size = std::clamp((preferred > 0.0f ? preferred : defaultSize) * dpiScale,
			std::min(minimum * dpiScale, maximum), maximum);
		origin = area == 0 ? pos * dpiScale : area == 1 ?
			viewport * pos - size / 2.0f : viewport - pos * dpiScale - size;
		origin = std::clamp(origin, 0.0f, std::max(0.0f, viewport - size));
	};
	OverlayWindowRect rect{};
	restoreAxis(option.hArea, option.hPos, option.width, defaultWidth, minimumWidth,
		viewportWidth, rect.x, rect.width);
	restoreAxis(option.vArea, option.vPos, option.height, defaultHeight, minimumHeight,
		viewportHeight, rect.y, rect.height);
	return rect;
}

inline OverlayWindowRect RestoreEffectParametersWindow(
	OverlayWindowOption option, float viewportWidth, float viewportHeight, float dpiScale
) noexcept {
	return RestoreOverlayWindow(option, viewportWidth, viewportHeight, dpiScale,
		420.0f, 600.0f, 360.0f, 400.0f);
}

inline constexpr float PROFILER_MIN_WIDTH = 310.0f;
inline constexpr float PROFILER_MIN_HEIGHT = 220.0f;

inline OverlayWindowRect RestoreProfilerWindow(
	OverlayWindowOption option, float viewportWidth, float viewportHeight, float dpiScale
) noexcept {
	return RestoreOverlayWindow(option, viewportWidth, viewportHeight, dpiScale,
		310.0f, 360.0f, PROFILER_MIN_WIDTH, PROFILER_MIN_HEIGHT);
}

inline void RememberOverlayWindowPosition(
	OverlayWindowOption& option, const OverlayWindowRect& rect,
	float viewportWidth, float viewportHeight, float dpiScale
) noexcept {
	auto rememberAxis = [dpiScale](float pos, float size, float viewport,
		uint16_t& area, float& offset) {
		viewport = std::max(viewport, 1.0f);
		const float threshold = std::max(size / viewport, 0.2f);
		const float leading = std::max(pos, 0.0f);
		const float trailing = std::max(viewport - pos - size, 0.0f);
		if (leading < threshold * trailing) {
			area = 0;
			offset = leading / dpiScale;
		} else if (leading * threshold <= trailing) {
			area = 1;
			offset = (pos + size / 2.0f) / viewport;
		} else {
			area = 2;
			offset = trailing / dpiScale;
		}
	};
	rememberAxis(rect.x, rect.width, viewportWidth, option.hArea, option.hPos);
	rememberAxis(rect.y, rect.height, viewportHeight, option.vArea, option.vPos);
}

}
