#include <array>
#include <string>
#include <vector>
#include <winrt/base.h>
#include "EffectParameterValue.h"
#include <cassert>
#include <cmath>
#include <iostream>

namespace {

// Simulates WinUI RangeBase property coercion during XAML x:Bind Update_()
// when ProcessBindings() sets _isInitialized = true before updating
// Maximum -> Minimum -> StepFrequency -> Value.
struct SimulatedXamlSlider {
	double minimum = 0.0;
	double maximum = 100.0;
	double value = 0.0;
	int writebackCount = 0;

	template<class OnValueChanged>
	void SetMaximum(double next, OnValueChanged&& onValueChanged) {
		maximum = next;
		if (value > maximum) {
			value = maximum;
			++writebackCount;
			onValueChanged(value);
		}
	}

	template<class OnValueChanged>
	void SetMinimum(double next, OnValueChanged&& onValueChanged) {
		minimum = next;
		if (value < minimum) {
			value = minimum;
			++writebackCount;
			onValueChanged(value);
		}
	}

	template<class OnValueChanged>
	void SetValue(double next, OnValueChanged&& onValueChanged) {
		const double clamped = std::clamp(next, minimum, maximum);
		if (value != clamped) {
			value = clamped;
			++writebackCount;
			onValueChanged(value);
		}
	}
};

struct SimulatedParameter {
	Magpie::EffectParameterDesc desc;
	double value = 0.0;
	int modelMutationCount = 0;

	bool UseTicks() const noexcept {
		int tick = 0, maximum = 0;
		return Magpie::GetEffectParameterTicks(desc, float(value), tick, maximum);
	}
	double SliderMinimum() const noexcept {
		return UseTicks() ? 0.0 : std::visit([](const auto& c) { return double(c.minValue); }, desc.constant);
	}
	double SliderMaximum() const noexcept {
		int tick = 0, maximum = 0;
		if (Magpie::GetEffectParameterTicks(desc, float(value), tick, maximum)) {
			return double(maximum);
		}
		return std::visit([](const auto& c) { return double(c.maxValue); }, desc.constant);
	}
	double SliderValue() const noexcept {
		int tick = 0, maximum = 0;
		if (Magpie::GetEffectParameterTicks(desc, float(value), tick, maximum)) {
			return double(tick);
		}
		return value;
	}
	void SliderValue(double sliderVal) {
		int tick = 0, maximum = 0;
		if (Magpie::GetEffectParameterTicks(desc, float(value), tick, maximum)) {
			const int targetTick = int(std::lround(sliderVal));
			if (targetTick == tick) return;
			const double next = Magpie::GetEffectParameterValueFromTick(desc, targetTick, maximum);
			if (value != next) {
				value = next;
				++modelMutationCount;
			}
			return;
		}
		if (value != sliderVal) {
			value = sliderVal;
			++modelMutationCount;
		}
	}
};

void VerifyBindPreservesValue(const Magpie::EffectParameterDesc& desc, double savedValue) {
	SimulatedParameter param{ desc, savedValue };
	SimulatedXamlSlider slider;
	auto onChanged = [&](double v) { param.SliderValue(v); };

	// Exact x:Bind Update_() order from ScalingModesPage.xaml.g.hpp
	slider.SetMaximum(param.SliderMaximum(), onChanged);
	slider.SetMinimum(param.SliderMinimum(), onChanged);
	slider.SetValue(param.SliderValue(), onChanged);

	assert(param.modelMutationCount == 0);
	assert(std::abs(param.value - savedValue) <= 1e-5);
	assert(slider.minimum == 0.0);
}

} // namespace

int main() {
	using namespace Magpie;

	// 1. DLSS FG / XeSS FG multiplier: int [2, 4], step 1
	{
		EffectParameterDesc fgMultiplier{};
		fgMultiplier.name = "multiplier";
		fgMultiplier.constant = EffectConstant<int>{ 2, 2, 4, 1 };
		for (int saved : { 2, 3, 4 }) {
			VerifyBindPreservesValue(fgMultiplier, saved);
		}
		SimulatedParameter param{ fgMultiplier, 2.0 };
		assert(param.SliderMinimum() == 0.0);
		assert(param.SliderMaximum() == 2.0);
		assert(param.SliderValue() == 0.0);
		param.SliderValue(1.0);
		assert(param.value == 3.0);
		param.SliderValue(2.0);
		assert(param.value == 4.0);
		param.SliderValue(0.0);
		assert(param.value == 2.0);
	}

	// 2. Nvidia VFX VFG multiplier: int [2, 8], step 1
	{
		EffectParameterDesc vfgMultiplier{};
		vfgMultiplier.name = "multiplier";
		vfgMultiplier.constant = EffectConstant<int>{ 2, 2, 8, 1 };
		for (int saved = 2; saved <= 8; ++saved) {
			VerifyBindPreservesValue(vfgMultiplier, saved);
		}
		SimulatedParameter param{ vfgMultiplier, 2.0 };
		assert(param.SliderMaximum() == 6.0);
		param.SliderValue(6.0);
		assert(param.value == 8.0);
	}

	// 3. Negative-max float slider (e.g. CRT_Lottes): float [-4.0, -0.5], step 0.5
	{
		EffectParameterDesc negMax{};
		negMax.name = "hardPix";
		negMax.constant = EffectConstant<float>{ -3.0f, -4.0f, -0.5f, 0.5f };
		for (double saved : { -4.0, -3.0, -2.5, -1.0, -0.5 }) {
			VerifyBindPreservesValue(negMax, saved);
		}
	}

	// 4. Positive-min float slider: float [0.01, 1.0], step 0.01
	{
		EffectParameterDesc posMin{};
		posMin.name = "strength";
		posMin.constant = EffectConstant<float>{ 0.5f, 0.01f, 1.0f, 0.01f };
		VerifyBindPreservesValue(posMin, 0.75);
	}

	// 5. Precise input parameter (inputStep > 0) keeps existing behavior
	{
		EffectParameterDesc precise{};
		precise.name = "peakNits";
		precise.constant = EffectConstant<float>{ 1000.0f, 400.0f, 4000.0f, 10.0f };
		precise.inputStep = 1.0f;
		precise.sliderStep = 10.0f;
		precise.coarseStep = 100.0f;
		precise.segmentedSteps = true;
		// Non-grid saved value must not be quantized on binding initialization
		VerifyBindPreservesValue(precise, 1003.0);
	}

	std::cout << "EffectParameterSliderTicksTests passed.\n";
	return 0;
}
