#include "ScreenshotContent.h"
#include "ScreenshotNaming.h"
#include "ConfigRecovery.h"
#include <cassert>
#include <iostream>

using namespace Magpie;

int main() {
	static_assert(static_cast<uint32_t>(ScreenshotContent::InputAndOutput) == 0);
	static_assert(static_cast<uint32_t>(ScreenshotContent::OutputOnly) == 1);
	static_assert(static_cast<uint32_t>(ScreenshotContent::InputOnly) == 2);
	for (uint32_t i = 0; i < 3; ++i) assert(static_cast<uint32_t>(NormalizeScreenshotContent(i)) == i);
	for (uint32_t i : { 3u, 99u, UINT32_MAX }) assert(NormalizeScreenshotContent(i) == ScreenshotContent::OutputOnly);
	const ScreenshotFrameIdentity frame{ 12, 0, 3 };
	assert(frame.Matches({ 12, 0, 3 }, false)); // GDI/DXGI use sequence zero.
	assert(!frame.Matches({ 12, 0, 3 }, true));
	assert(!frame.Matches({ 13, 0, 3 }, false));
	assert(!frame.Matches({ 12, 1, 3 }, false));
	assert(!frame.Matches({ 12, 0, 4 }, false));
	assert(!ScreenshotFrameIdentity{}.Matches({}, false));
	const std::wstring timestamp = L"20261022_194530_123";
	assert(ScreenshotStem(timestamp, 0) == L"Magpie_20261022_194530_123");
	assert(ScreenshotStem(timestamp, 1) == L"Magpie_20261022_194530_123_01");
	assert(ScreenshotStem(timestamp, 10) == L"Magpie_20261022_194530_123_10");
	assert(ScreenshotStem(timestamp, 100) == L"Magpie_20261022_194530_123_100");
	for (auto suffix : { L"_input.png", L"_output.png", L"_effect_01_output.png",
		L"_effect_01_pass_02_texture_03.dds", L".png" })
		assert(ParseScreenshotCollision(ScreenshotStem(timestamp, 0) + suffix, timestamp) == 0);
	assert(ParseScreenshotCollision(L"magpie_20261022_194530_123_01_OUTPUT.PNG", timestamp) == 1);
	assert(ParseScreenshotCollision(L"Magpie_20261022_194530_123_4294967295_input.png", timestamp) == UINT32_MAX);
	for (auto name : { L"Magpie_001.png", L"Magpie_001_input.png", L"Magpie_001.dds",
		L"Magpie_20261022_194530_124_input.png", L"Magpie_20261022_194530_1234_input.png",
		L"Magpie_20261022_194530_123_4294967296_input.png", L"Magpie_20261022_194530_123_input.jpg",
		L"Magpie_20261022_194530_123_01_.png", L"Magpie_20261022_194530_123_01_input.png.bak" })
		assert(!ParseScreenshotCollision(name, timestamp));
	const std::string prefix = R"({"experimentalDlssnrSettingsVersion":2,"experimentalDlssSrSettingsVersion":1,"experimentalDepthRemovalVersion":1,"scalingModes":[],"overlay":{"screenshotsDir":"keep","screenshotContent":)";
	for (auto value : { "0", "1", "2" }) {
		auto plan = ConfigRecovery::Prepare(prefix + value + "}}", {}, "Recovered");
		assert(plan.kind == ConfigRecovery::Kind::None);
		assert(plan.document["overlay"]["screenshotContent"].GetUint() == std::stoul(value));
		assert(ConfigRecovery::Prepare(ConfigRecovery::Serialize(plan.document), {}, "Recovered").kind == ConfigRecovery::Kind::None);
	}
	for (auto value : { "-1", "3", "1.5", "4294967296", "null", "true", "\"input\"", "{}", "[]" }) {
		auto plan = ConfigRecovery::Prepare(prefix + value + "}}", {}, "Recovered");
		assert(plan.kind == ConfigRecovery::Kind::Repaired);
		assert(plan.fields == std::vector<std::string>{"/overlay/screenshotContent"});
		assert(!plan.document["overlay"].HasMember("screenshotContent"));
		assert(std::string_view(plan.document["overlay"]["screenshotsDir"].GetString()) == "keep");
	}
	std::cout << "PASS: screenshot modes, frame identity, timestamp/purpose naming, collision parsing and isolated configuration recovery\n";
}
