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
	std::shared_ptr<const uint8_t> runtimeIdentity = std::make_shared<const uint8_t>(uint8_t{0});
	std::string oklabLut;

	bool HasScale() const noexcept {
		return scalingType != ScalingType::Normal ||
			!IsApprox(scale.first, 1.0f) || !IsApprox(scale.second, 1.0f);
	}

	explicit operator EffectOption() const noexcept;
};

struct ScalingMode {
	std::wstring name;
	std::vector<EffectItem> effects;
	// Recovery-only compatibility for trial group configurations. New modes
	// infer their domains from manually placed converters.
	std::optional<bool> legacyHdrCaptureIntent;
	std::string hdrMigrationBackup;
	bool hdrMigrationInvalid = false;
	std::string hdrMigrationRecovery;
	// Session-only identity for diagnostic navigation; never serialized.
	std::shared_ptr<const uint8_t> runtimeIdentity = std::make_shared<const uint8_t>(uint8_t{0});
};

}
