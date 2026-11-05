#include "../src/Magpie.Core/include/OverlayProfilerOptions.h"
#include <rapidjson/document.h>
#include <rapidjson/writer.h>
#include <rapidjson/stringbuffer.h>
#include <array>
#include <cassert>
#include <cstdio>
#include <string>

using namespace Magpie;

static std::array<bool, 6> Values(const OverlayProfilerOptions& options) {
	return { options.windowCollapsed, options.timingsExpanded, options.graphsExpanded,
		options.developerExpanded, options.debugExpanded, options.showPasses };
}

static std::string Serialize(const OverlayProfilerOptions& options) {
	rapidjson::StringBuffer buffer;
	rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
	WriteOverlayProfilerOptions(writer, options);
	return { buffer.GetString(), buffer.GetSize() };
}

static OverlayProfilerOptions Load(const char* text) {
	rapidjson::Document document;
	document.Parse(text);
	assert(!document.HasParseError() && document.IsObject());
	OverlayProfilerOptions result;
	ReadOverlayProfilerOptions(document.GetObj(), result);
	return result;
}

int main() {
	const OverlayProfilerOptions defaults;
	assert(Values(defaults) == (std::array{ false, true, false, true, true, false }));
	assert(Values(Load("{}")) == Values(defaults));
	assert(Values(Load(R"({"windowCollapsed":1,"timingsExpanded":"false","graphsExpanded":null,
		"developerExpanded":[],"debugExpanded":{},"showPasses":2})")) == Values(defaults));
	for (unsigned mask = 0; mask != 64; ++mask) {
		OverlayProfilerOptions options{ bool(mask & 1), bool(mask & 2), bool(mask & 4),
			bool(mask & 8), bool(mask & 16), bool(mask & 32) };
		const auto json = Serialize(options);
		const auto loaded = Load(json.c_str());
		assert(Values(loaded) == Values(options));
		assert(Serialize(loaded) == json);
	}
	const auto partial = Load(R"({"graphsExpanded":true,"showPasses":true,"futureField":42})");
	assert(partial.graphsExpanded && partial.showPasses && partial.timingsExpanded &&
		partial.developerExpanded && !partial.windowCollapsed);
	std::puts("Profiler settings: 64 JSON round trips, legacy defaults, partial and malformed fields passed.");
}
