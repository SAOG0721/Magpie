#pragma once
#include "ScalingOptions.h"

namespace Magpie {

struct EffectItem {
	std::wstring name;
	phmap::flat_hash_map<std::wstring, float> parameters;
	ScalingType scalingType = ScalingType::Normal;
	std::pair<float, float> scale = { 1.0f,1.0f };
	bool isRecoveryInvalid = false;
	std::string recoveryOriginal;
	// 临时禁用：效果仍保留在配置与列表中，但不参与渲染。
	// 放在末尾以免影响任何按位置聚合初始化的写法。
	bool enabled = true;

	bool HasScale() const noexcept {
		return scalingType != ScalingType::Normal ||
			!IsApprox(scale.first, 1.0f) || !IsApprox(scale.second, 1.0f);
	}

	explicit operator EffectOption() const noexcept;
};

struct ScalingMode {
	std::wstring name;
	std::vector<EffectItem> effects;
};

}
