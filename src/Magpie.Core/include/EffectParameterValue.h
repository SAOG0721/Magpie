#pragma once
#include "EffectDesc.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace Magpie {

inline bool IsChoiceEffectParameter(
	const EffectParameterDesc& parameter
) noexcept {
	return parameter.constant.index() == 1 && !parameter.choices.empty();
}

inline bool IsBooleanEffectParameter(const EffectParameterDesc& parameter) noexcept {
	if (parameter.constant.index() != 1 || IsChoiceEffectParameter(parameter)) {
		return false;
	}
	const EffectConstant<int>& constant = std::get<1>(parameter.constant);
	return constant.minValue == 0 && constant.maxValue == 1 && constant.step == 1;
}

inline float NormalizeEffectParameterValue(
	const EffectParameterDesc& parameter,
	float value
) noexcept {
	if (IsChoiceEffectParameter(parameter)) {
		const int defaultValue = std::get<1>(parameter.constant).defaultValue;
		if (!std::isfinite(value) || value != std::floor(value) ||
			double(value) < std::numeric_limits<int>::min() || double(value) > std::numeric_limits<int>::max()) {
			return static_cast<float>(defaultValue);
		}
		const int candidate = static_cast<int>(value);
		const auto it = std::ranges::find(
			parameter.choices, candidate, &EffectParameterChoice::value);
		return static_cast<float>(it == parameter.choices.end()
			? defaultValue : it->value);
	}
	if (parameter.constant.index() == 0) {
		const EffectConstant<float>& constant = std::get<0>(parameter.constant);
		if (!std::isfinite(constant.defaultValue) ||
			!std::isfinite(constant.minValue) ||
			!std::isfinite(constant.maxValue) ||
			constant.minValue > constant.maxValue) {
			return 0.0f;
		}
		if (!std::isfinite(value)) {
			value = constant.defaultValue;
		}
		value = std::clamp(value, constant.minValue, constant.maxValue);
		if (parameter.inputStep <= 0 && std::isfinite(constant.step) && constant.step > 0.0f) {
			const double minValue = constant.minValue;
			const double maxValue = constant.maxValue;
			const double step = constant.step;
			const double regularValue = std::clamp(
				minValue + std::round((double(value) - minValue) / step) * step,
				minValue, maxValue);
			// Treat MAX as a legal endpoint even when a third-party effect declares
			// a range that is not an exact multiple of STEP.
			value = static_cast<float>(
				std::abs(maxValue - value) < std::abs(regularValue - value)
					? maxValue : regularValue);
		}
		return std::clamp(value, constant.minValue, constant.maxValue);
	}

	const EffectConstant<int>& constant = std::get<1>(parameter.constant);
	if (constant.minValue > constant.maxValue) {
		return 0.0f;
	}
	const double finiteValue = std::isfinite(value)
		? std::clamp<double>(value, constant.minValue, constant.maxValue)
		: constant.defaultValue;
	int intValue = static_cast<int>(std::llround(finiteValue));
	intValue = std::clamp(intValue, constant.minValue, constant.maxValue);
	if (parameter.inputStep <= 0 && constant.step > 0) {
		const int64_t regularValue = std::clamp<int64_t>(
			int64_t(constant.minValue) + static_cast<int64_t>(std::llround(
				double(int64_t(intValue) - constant.minValue) / constant.step)) * constant.step,
			constant.minValue, constant.maxValue);
		if (std::abs(int64_t(constant.maxValue) - intValue) <
			std::abs(regularValue - intValue)) {
			intValue = constant.maxValue;
		} else {
			intValue = static_cast<int>(regularValue);
		}
	}
	return static_cast<float>(std::clamp(
		intValue, constant.minValue, constant.maxValue));
}

inline float NormalizeEffectParameterInput(const EffectParameterDesc& parameter, float value) noexcept {
	if (parameter.inputStep <= 0) return NormalizeEffectParameterValue(parameter, value);
	const auto minimum = std::visit([](auto& c) { return float(c.minValue); }, parameter.constant);
	const auto maximum = std::visit([](auto& c) { return float(c.maxValue); }, parameter.constant);
	value = NormalizeEffectParameterValue(parameter, value);
	value = float(std::round(double(value) / parameter.inputStep) * parameter.inputStep);
	return std::clamp(value, std::max(minimum, parameter.editMinimum), maximum);
}

inline std::vector<float> GetEffectParameterInteractionTicks(const EffectParameterDesc& parameter,
	bool coarse = false) {
	std::vector<float> ticks;
	if (parameter.inputStep <= 0) return ticks;
	const float minimum = std::max(parameter.editMinimum,
		std::visit([](auto& c) { return float(c.minValue); }, parameter.constant));
	const float maximum = std::visit([](auto& c) { return float(c.maxValue); }, parameter.constant);
	const float defaultValue = std::visit([](auto& c) { return float(c.defaultValue); }, parameter.constant);
	auto addBand = [&](float low, float high, float step) {
		if (!std::isfinite(step) || step <= 0 || low > high || (high - low) / step > 100000) return;
		for (double v = std::ceil(double(low) / step) * step; v <= high; v += step) ticks.push_back(float(v));
	};
	if (parameter.segmentedSteps && !coarse) {
		addBand(minimum, std::min(maximum, 400.f), 5);
		addBand(std::max(minimum, 400.f), std::min(maximum, 1000.f), 10);
		addBand(std::max(minimum, 1000.f), maximum, 50);
	} else addBand(minimum, maximum, coarse ? parameter.coarseStep : parameter.sliderStep);
	ticks.push_back(minimum); ticks.push_back(maximum);
	if (defaultValue >= minimum && defaultValue <= maximum) ticks.push_back(defaultValue);
	std::ranges::sort(ticks);
	ticks.erase(std::unique(ticks.begin(), ticks.end()), ticks.end());
	return ticks;
}

inline float StepEffectParameter(const EffectParameterDesc& parameter, float value, bool up,
	bool coarse = false) {
	const auto ticks = GetEffectParameterInteractionTicks(parameter, coarse);
	if (ticks.empty()) return value;
	if (up) {
		const auto next = std::upper_bound(ticks.begin(), ticks.end(), value + 1e-5f);
		return next == ticks.end() ? ticks.back() : *next;
	}
	const auto next = std::lower_bound(ticks.begin(), ticks.end(), value - 1e-5f);
	return next == ticks.begin() ? ticks.front() : *std::prev(next);
}

inline bool GetEffectParameterTicks(
	const EffectParameterDesc& parameter,
	float value,
	int& currentTick,
	int& maximumTick
) noexcept {
	if (IsChoiceEffectParameter(parameter)) {
		return false;
	}
	if (parameter.inputStep > 0) {
		const auto ticks = GetEffectParameterInteractionTicks(parameter);
		if (ticks.size() < 2) return false;
		maximumTick = int(ticks.size()) - 1;
		const auto next = std::lower_bound(ticks.begin(), ticks.end(), value);
		currentTick = next == ticks.end() ? maximumTick : int(next - ticks.begin());
		if (currentTick > 0 && std::abs(ticks[currentTick - 1] - value) < std::abs(ticks[currentTick] - value)) --currentTick;
		return true;
	}
	double minValue;
	double maxValue;
	double step;
	if (parameter.constant.index() == 0) {
		const EffectConstant<float>& constant = std::get<0>(parameter.constant);
		minValue = constant.minValue;
		maxValue = constant.maxValue;
		step = constant.step;
		if (!std::isfinite(minValue) || !std::isfinite(maxValue) ||
			!std::isfinite(step)) {
			return false;
		}
	} else {
		const EffectConstant<int>& constant = std::get<1>(parameter.constant);
		minValue = constant.minValue;
		maxValue = constant.maxValue;
		step = constant.step;
	}
	if (minValue > maxValue || step <= 0.0) {
		return false;
	}

	const double rawTickCount = (maxValue - minValue) / step;
	if (!std::isfinite(rawTickCount) ||
		rawTickCount > std::numeric_limits<int>::max() - 1.0) {
		return false;
	}
	const double roundedTickCount = std::round(rawTickCount);
	const double tickCount = std::abs(rawTickCount - roundedTickCount) <=
		std::max(1e-7, std::abs(rawTickCount) * 1e-7)
			? roundedTickCount : std::ceil(rawTickCount);
	maximumTick = std::max(1, static_cast<int>(tickCount));
	const double normalized = NormalizeEffectParameterValue(parameter, value);
	if (std::abs(normalized - maxValue) <=
		std::max(1e-7, std::abs(maxValue) * 1e-7)) {
		currentTick = maximumTick;
	} else {
		currentTick = std::clamp(
			static_cast<int>(std::lround((normalized - minValue) / step)),
			0, maximumTick);
	}
	return true;
}

inline float GetEffectParameterValueFromTick(
	const EffectParameterDesc& parameter,
	int tick,
	int maximumTick
) noexcept {
	tick = std::clamp(tick, 0, maximumTick);
	if (parameter.inputStep > 0) {
		const auto ticks = GetEffectParameterInteractionTicks(parameter);
		return ticks.empty() ? NormalizeEffectParameterValue(parameter, 0) : ticks[std::min<size_t>(tick, ticks.size() - 1)];
	}
	if (parameter.constant.index() == 0) {
		const EffectConstant<float>& constant = std::get<0>(parameter.constant);
		const double value = tick == maximumTick
			? constant.maxValue
			: double(constant.minValue) + double(tick) * constant.step;
		return NormalizeEffectParameterValue(parameter, static_cast<float>(value));
	}
	const EffectConstant<int>& constant = std::get<1>(parameter.constant);
	const int64_t value = tick == maximumTick
		? constant.maxValue
		: int64_t(constant.minValue) + int64_t(tick) * constant.step;
	return NormalizeEffectParameterValue(parameter, static_cast<float>(std::clamp<int64_t>(
		value, constant.minValue, constant.maxValue)));
}

inline int GetEffectParameterDisplayPrecision(
	const EffectParameterDesc& parameter
) noexcept {
	if (parameter.constant.index() != 0) {
		return 0;
	}
	const double step = parameter.inputStep > 0 ? parameter.inputStep : std::get<0>(parameter.constant).step;
	if (!std::isfinite(step) || step <= 0.0) {
		return 3;
	}
	double scaled = step;
	for (int precision = 0; precision <= 6; ++precision) {
		if (std::abs(scaled - std::round(scaled)) <=
			std::max(1e-7, std::abs(scaled) * 1e-6)) {
			return precision;
		}
		scaled *= 10.0;
	}
	return 6;
}

}
