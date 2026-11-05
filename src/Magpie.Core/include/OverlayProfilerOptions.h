#pragma once

namespace Magpie {

struct OverlayProfilerOptions {
	bool windowCollapsed = false;
	bool timingsExpanded = true;
	bool graphsExpanded = false;
	bool developerExpanded = true;
	bool debugExpanded = true;
	bool showPasses = false;
};

template<class Writer>
void WriteOverlayProfilerOptions(Writer& writer, const OverlayProfilerOptions& options) {
	writer.StartObject();
	writer.Key("windowCollapsed"); writer.Bool(options.windowCollapsed);
	writer.Key("timingsExpanded"); writer.Bool(options.timingsExpanded);
	writer.Key("graphsExpanded"); writer.Bool(options.graphsExpanded);
	writer.Key("developerExpanded"); writer.Bool(options.developerExpanded);
	writer.Key("debugExpanded"); writer.Bool(options.debugExpanded);
	writer.Key("showPasses"); writer.Bool(options.showPasses);
	writer.EndObject();
}

template<class Object>
void ReadOverlayProfilerOptions(const Object& object, OverlayProfilerOptions& options) {
	auto read = [&](const char* name, bool& value) {
		const auto it = object.FindMember(name);
		if (it != object.MemberEnd() && it->value.IsBool()) value = it->value.GetBool();
	};
	read("windowCollapsed", options.windowCollapsed);
	read("timingsExpanded", options.timingsExpanded);
	read("graphsExpanded", options.graphsExpanded);
	read("developerExpanded", options.developerExpanded);
	read("debugExpanded", options.debugExpanded);
	read("showPasses", options.showPasses);
}

}
