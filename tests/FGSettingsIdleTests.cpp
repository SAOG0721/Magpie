#include "FrameGenerationParameters.h"
#include "IdlePresentationClock.h"
#include "FSR3FGPolicy.h"
#include "PresentationFrameRate.h"
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <initializer_list>
#include <map>
#include <optional>
#include <string>

using namespace Magpie;
using namespace std::chrono_literals;
static unsigned checks = 0;
static void Check(bool value, const char* message) {
	++checks;
	if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
template<class Char> struct Effect {
	std::basic_string<Char> name;
	std::map<std::basic_string<Char>, float> parameters;
};
template<class Char> static void Parameters() {
	const auto key = [](std::string_view value) { return std::basic_string<Char>(value.begin(), value.end()); };
	const std::optional<float> methods[]{std::nullopt, 0.f, -1.f, 3.f, 1.5f,
		std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()};
	for (const auto method : methods) {
		Effect<Char> effect{key("FSR3FG\\FSR3_FrameGeneration"), {{key("amdOpticalFlowMode"), 0.f}}};
		if (method) effect.parameters[key("opticalFlowMethod")] = *method;
		effect.parameters[key("duplicateFrameFiltering")] = 1;
		effect.parameters[key("customValue")] = 42;
		Check(NormalizeFrameGenerationParameters(effect), "old/missing/invalid method migrates");
		Check(effect.parameters.at(key("opticalFlowMethod")) == 1 &&
			effect.parameters.at(key("amdOpticalFlowMode")) == 1, "default is the full AMD Quality pair");
		Check(!effect.parameters.contains(key("duplicateFrameFiltering")), "retired override removed");
		Check(effect.parameters.at(key("customValue")) == 42, "unrelated parameters preserved");
		Check(!NormalizeFrameGenerationParameters(effect), "migration is idempotent");
		auto copy = effect;
		Check(!NormalizeFrameGenerationParameters(copy) && copy.parameters == effect.parameters, "copy/export preserves normalized values");
		copy.parameters.clear();
		NormalizeFrameGenerationParameters(copy);
		Check(copy.parameters.at(key("amdOpticalFlowMode")) == 1, "reset defaults to Quality");
	}
	for (float method : {1.f, 2.f}) for (float amd : {0.f, 1.f}) for (float nv : {1.f, 2.f, 3.f, 4.f, 5.f}) {
		Effect<Char> effect{key("FSR3FG\\FSR3_FrameGeneration"), {
			{key("opticalFlowMethod"), method}, {key("amdOpticalFlowMode"), amd}, {key("nvidiaOpticalFlowQuality"), nv}}};
		Check(!NormalizeFrameGenerationParameters(effect), "explicit valid provider and qualities preserved");
	}
	for (float bad : {-1.f, 6.f, 0.5f, std::numeric_limits<float>::quiet_NaN()}) {
		Effect<Char> effect{key("FSR3FG\\FSR3_FrameGeneration"), {
			{key("opticalFlowMethod"), 2.f}, {key("amdOpticalFlowMode"), bad}, {key("nvidiaOpticalFlowQuality"), bad}}};
		Check(NormalizeFrameGenerationParameters(effect), "invalid qualities normalize");
		Check(effect.parameters.at(key("opticalFlowMethod")) == 2 &&
			effect.parameters.at(key("amdOpticalFlowMode")) == 1 &&
			effect.parameters.at(key("nvidiaOpticalFlowQuality")) == 2, "quality repair preserves provider");
	}
	for (const char* id : {"DLSSFG\\DLSS_FrameGeneration", "XeSSFG\\XeSS_FrameGeneration", "NvidiaVFX\\VFG"}) {
		Effect<Char> effect{key(id), {{key("duplicateFrameFiltering"), 0.f}, {key("opticalFlowMethod"), 0.f}}};
		Check(NormalizeFrameGenerationParameters(effect), "all FG families drop override");
		Check(effect.parameters.at(key("opticalFlowMethod")) == 0, "other FG flow semantics unchanged");
	}
	for (const char* id : {"Custom\\FG", "FSR3\\FSR3_SR", "DLSSNR\\DLSSNR_AI_Filter"}) {
		Effect<Char> effect{key(id), {{key("duplicateFrameFiltering"), 1.f}, {key("opticalFlowMethod"), 0.f}}};
		const auto before = effect.parameters;
		Check(!NormalizeFrameGenerationParameters(effect) && before == effect.parameters, "custom/SR parameters untouched");
	}
}
int main() {
	Parameters<char>(); Parameters<wchar_t>();
	IdlePresentationClock clock;
	const auto start = IdlePresentationClock::Clock::time_point{};
	clock.SetRate(60);
	Check(!clock.Remaining(start + 60s), "no replay before first completed output");
	clock.Presented(start, true);
	for (int fps : {24, 30, 60}) {
		clock.Reset(); clock.Presented(start, true);
		for (int i = 1; i <= fps * 2; ++i) {
			const auto now = start + std::chrono::microseconds(i * 1000000 / fps);
			Check(clock.Remaining(now) && *clock.Remaining(now) > 0ns, "normal input gaps must not toggle FG");
			clock.Presented(now, true);
		}
	}
	clock.Reset(); clock.Presented(start, true);
	Check(*clock.Remaining(start + 499ms) > 0ns, "idle entry grace period");
	Check(*clock.Remaining(start + 500ms) == 0ns, "static output reaches idle deadline");
	clock.Attempted(start + 500ms);
	Check(*clock.Remaining(start + 510ms) > 0ns, "failed capacity attempt cannot busy spin");
	Check(*clock.Remaining(start + 517ms) == 0ns, "steady idle follows target rate");
	clock.Presented(start + 517ms, false);
	Check(*clock.Remaining(start + 518ms) > 15ms, "successful replay advances deadline");
	clock.Presented(start + 520ms, true);
	Check(*clock.Remaining(start + 550ms) > 0ns, "new content exits idle");
	clock.SetRate(0);
	Check(!clock.Remaining(start + 60s), "idle off removes deadline");
	clock.SetRate(std::numeric_limits<double>::quiet_NaN());
	Check(!clock.Remaining(start), "invalid rate disables replay");
	clock.Reset(); clock.SetRate(1); clock.Presented(start, true);
	Check(*clock.Remaining(start + 500ms) > 0ns && *clock.Remaining(start + 1s) == 0ns, "low idle rate respected");

	FSR3FGHistory history;
	const FSR3FGSourceSample first{1, 7, 3, 10000000};
	history.Submitted(first);
	for (uint64_t id = 2; id <= 12; ++id) {
		Check(history.Replay().sdkFrameId == id, "SDK replay IDs are unique");
		Check(!history.Next(first, true, true).submit, "replay cannot make old input new");
		history.ReplaySubmitted();
	}
	const auto resumed = history.Next({2,7,3,10166667}, false, true);
	Check(resumed.sdkFrameId == 13 && resumed.submit && resumed.reset == uint32_t(FSR3FGReset::Geometry), "resume preserves source chronology and resets SDK history");

	PresentationFrameRate stats;
	stats.Record(std::nullopt, 1, start);
	for (int i = 1; i <= 30; ++i) stats.RecordIdleSubmission(start + std::chrono::milliseconds(i * 20));
	const auto rate = stats.Get(start + 1s);
	Check(rate.windowStart == start && rate.windowEnd == start + 1s &&
		stats.Get(start + 1500ms).windowEnd == rate.windowEnd,
		"UI reads reuse the measured window rather than inventing output samples");
	Check(rate.real == 1 && rate.idleSubmissions == 30 && !rate.totalKnown, "idle cannot inflate input/generated rate or fill unknown output");
	Check(stats.WaitingForContent(start + 1s), "idle replay does not refresh content activity");
	const auto empty = stats.Get(start + 2s);
	Check(empty.real == 0 && empty.total == 0 && empty.idleSubmissions == 0, "quiet counters decay to zero");
	stats.Record(2, 1, start + 2s);
	Check(!stats.WaitingForContent(start + 2s), "new content clears waiting");
	const auto resumedRate = stats.Get(start + 3s);
	Check(resumedRate.total == 2 && resumedRate.real == 1 && resumedRate.generated == 1 && resumedRate.idleSubmissions == 0, "known generated frames stay separate from replays");
	stats.Reset(ContentFrameCountSource::Submitted);
	for (int i = 0; i < 60; ++i)
		stats.Record(2, 1, start + std::chrono::microseconds(i * 1000000 / 60), ContentFrameCountSource::Submitted);
	const auto submitted = stats.Get(start + 1s);
	Check(submitted.total == 120 && submitted.real == 60 && submitted.generated == 60 &&
		submitted.source == ContentFrameCountSource::Submitted, "120/60 submission rate retains provenance");
	stats.Reset(ContentFrameCountSource::Submitted);
	stats.Record(1, 1, start, ContentFrameCountSource::Submitted);
	const auto resetRate = stats.Get(start + 1s);
	Check(resetRate.totalKnown && resetRate.generated == 0 && resetRate.total == 1, "reset copy is not generated content");
	stats.Record(std::nullopt, 1, start + 1s, ContentFrameCountSource::Submitted);
	stats.Record(2, 1, start + 1500ms, ContentFrameCountSource::Submitted);
	Check(!stats.Get(start + 2s).totalKnown, "missing callback poisons the whole window instead of inventing 2x");
	stats.RecordIdleSubmission(start + 2s);
	const auto quiet = stats.Get(start + 3s);
	Check(quiet.totalKnown && quiet.real == 0 && quiet.total == 0 && quiet.idleSubmissions == 1 &&
		quiet.source == ContentFrameCountSource::Submitted, "redraw changes only R/s and preserves rate provenance");
	stats.Reset();
	stats.Record(2, 1, start);
	stats.Record(2, 1, start + 500ms, ContentFrameCountSource::Submitted);
	Check(!stats.Get(start + 1s).totalKnown, "mixed source window cannot claim one complete output count");
	stats.Reset(ContentFrameCountSource::Submitted);
	Check(stats.Get(start).real == 0 && stats.Get(start).generated == 0 && !stats.WaitingForContent(start + 10s),
		"resize/session reset discards old counts and content clock");
	std::printf("FG settings/idle production policy: %u checks passed (no SDK/display acceptance).\n", checks);
}
