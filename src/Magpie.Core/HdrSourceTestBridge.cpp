#include "pch.h"
#include "HdrSourceTestBridge.h"
#include "EffectCompiler.h"
#include "EffectProtocolCatalogC.h"
#include "GroupAHdrRoutes.h"
#include "GroupBHdrRoutes.h"
#include "HdrAdapterDispatcher.h"

namespace Magpie {

namespace {
HdrFormatRoutes RoutesFor(std::string_view name) noexcept {
	const size_t separator = name.find('\\');
	const std::string_view group = separator == std::string::npos ? name : name.substr(0, separator);
	if (group == "CAS") return GetGroupAHdrRoutes(group, 0);
	if (group == "DLSSNR") return GetGroupBHdrRoutes(group, false);
	if (group == "DLSS" || group == "FSR" || group == "FSR2" || group == "FSR3" || group == "FSR4" || group == "NIS") return GetGroupBHdrRoutes(group);
	return EffectProtocolC::GetGroupCHdrRoutes(group);
}
}

std::vector<HdrSourceTestResult> HdrSourceTestBridge::CompileAndDescribeAll(
	const std::vector<std::string>& effectNames, bool noFP16) noexcept {
	std::vector<HdrSourceTestResult> results;
	results.reserve(effectNames.size());
	for (const std::string& effectName : effectNames) {
		EffectDesc desc{ .name = effectName };
		const uint32_t flags = noFP16 ? EffectCompilerFlags::NoFP16 : 0;
		const bool compiled = EffectCompiler::Compile(desc, flags, nullptr) == 0;
		const HdrFormatRoutes routes = RoutesFor(effectName);
		const HdrFormatRoute* route = SelectDefaultHdrRoute(routes);
		HdrSourceTestResult result;
		result.effectId = effectName;
		result.routeId = route ? route->Id() : "";
		result.routeValid = route && route->IsValid();
		result.passCount = compiled ? static_cast<uint32_t>(desc.passes.size()) : 0;
		result.textureCount = compiled ? static_cast<uint32_t>(desc.textures.size()) : 0;
		if (route) {
			result.inputFormat = route->inputFormat;
			result.outputFormat = route->outputFormat;
			result.canonicalInput = route->inputFormat == DXGI_FORMAT_R16G16B16A16_FLOAT || route->adapterProfile == HdrAdapterProfile::SDRCompatible;
			result.canonicalOutput = route->outputFormat == DXGI_FORMAT_R16G16B16A16_FLOAT || route->adapterProfile == HdrAdapterProfile::SDRCompatible;
		}
		results.push_back(std::move(result));
	}
	return results;
}

}
