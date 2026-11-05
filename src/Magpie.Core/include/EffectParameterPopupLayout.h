#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>

namespace Magpie {

struct EffectParameterPopupSize {
	double width, height;
};

inline EffectParameterPopupSize GetEffectParameterPopupSize(
	double workWidthPixels, double workHeightPixels, double monitorScale, double rootScale) noexcept {
	const double scale = std::max({ 1.0, monitorScale, rootScale });
	return { std::max(1.0, workWidthPixels / scale - 72.0),
		std::max(1.0, workHeightPixels / scale - 72.0) };
}

struct EffectParameterColumnLayout {
	double columnWidth, viewportWidth;
};

inline double GetEffectParameterContentHeight(double availableHeight, bool isOKLab) noexcept {
	// OKLab edits one section at a time. Leave room for its actions and use
	// existing vertical wrapping instead of stretching one column down the screen.
	return std::max(1.0, isOKLab ? std::min(600.0, availableHeight) : availableHeight);
}

inline double GetPairedCheckboxOffset(double columnWidth, double leftWidth, double gap) noexcept {
	return std::max(columnWidth / 2.0, leftWidth + gap);
}

inline EffectParameterColumnLayout GetEffectParameterColumnLayout(
	size_t visibleColumns, double availableWidth) noexcept {
	const double count = double(std::max<size_t>(1, visibleColumns));
	const double spacing = 24.0 * (count - 1);
	const double column = std::isfinite(availableWidth)
		? std::clamp((availableWidth - spacing) / count, 240.0, 260.0) : 260.0;
	return { column, std::min(column * count + spacing, std::max(0.0, availableWidth)) };
}

}
