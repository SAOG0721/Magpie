#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>
#include <map>
#include <string>
#include "DLSSNRPerformanceState.h"
#include "DLSSNREnhancementPool.h"
#include "EffectParameterRules.h"

int main() {
	using namespace Magpie;
	static_assert(int(DLSSNRProcessingMode::EveryFrame) == 0);
	static_assert(int(DLSSNRProcessingMode::Alternating) == 1);
	static_assert(int(DLSSNRProcessingMode::Pipeline) == 2);
	static_assert(int(DLSSNRProcessingMode::SparseEconomy) == 3);
	static_assert(DLSSNR_RESIDUAL_MAX_AGE_FRAMES == 1);
	for (uint32_t interval : {2u,3u,4u}) {
		DLSSNRPerformanceState source;
		source.Configure(DLSSNRProcessingMode::Alternating,interval);
		assert(source.updateInterval == interval && source.maxAgeFrames == interval-1);
		DLSSNREnhancementPool pool;
		uint64_t lastSubmission=0,lastEpoch=0;
		unsigned observations=0;
		for (uint64_t frame=1;frame<=24;++frame) {
			// Sparse renderer IDs represent exactly one accepted capture each.
			source.Begin(frame*17,frame,1,10'000'000+int64_t(frame)*166'667,true,false);
			assert(source.sequence == frame);
			const bool due=(frame-1)%interval == 0;
			assert(source.NeedsFullUpdate(lastSubmission,lastEpoch) == due);
			if (due) {
				const auto slot=pool.Reserve(UINT64_MAX);
				pool.Publish(slot,source.sequence,source.epoch,0);
				lastSubmission=source.sequence;lastEpoch=source.epoch;++observations;
			}
			const auto selected=pool.Latest(source,UINT64_MAX);
			assert(selected != pool.None && source.sequence-pool[selected].sequence == (frame-1)%interval);
			const auto observation=pool[selected].sequence;
			for (unsigned redraw=0;redraw<5;++redraw) {
				// Duplicate capture flags, redraws, completion visits and generated
				// display opportunities do not create accepted source captures.
				source.Begin(source.frame,source.revision,1,source.timestamp100ns,redraw==0,false);
				assert(!source.newCapture && !source.reset && source.sequence == frame);
				assert(!source.NeedsFullUpdate(lastSubmission,lastEpoch));
				assert(pool[selected].sequence == observation);
			}
		}
		assert(observations == 24/interval);
		const auto oldEpoch=source.epoch;
		source.Begin(25*17,25,2,source.timestamp100ns+166'667,true,false);
		assert(source.reset && source.epoch != oldEpoch);
		assert(source.NeedsFullUpdate(lastSubmission,lastEpoch));
		assert(pool.Latest(source,UINT64_MAX) == pool.None);
		// A hard rebuild becomes the next cycle's actual NR observation.
		lastSubmission=source.sequence;lastEpoch=source.epoch;
		source.Begin(26*17,26,2,source.timestamp100ns+166'667,true,false);
		assert(!source.NeedsFullUpdate(lastSubmission,lastEpoch));
		source.Invalidate();
		assert(source.NeedsFullUpdate(lastSubmission,lastEpoch));
	}
	for (auto mode : {DLSSNRProcessingMode::EveryFrame}) {
		for (uint32_t interval : {2u,3u,4u}) {
			DLSSNRPerformanceState source;
			source.Configure(mode,interval);
			source.Begin(17,1,1,1,true,false);
			const auto origin=source.sequence,epoch=source.epoch;
			source.Begin(34,2,1,166'668,true,false);
			assert(source.Usable(origin,epoch));
			source.Begin(51,3,1,333'335,true,false);
			assert(!source.Usable(origin,epoch) && source.maxAgeFrames == 1);
		}
	}
	// Pipeline keeps the true NR source ordinal while bounded late completions
	// remain publishable. Neither adoption nor redraw renews an observation.
	for (uint32_t interval : {2u,3u,4u}) for (uint32_t completionAge : {2u,3u}) {
		DLSSNRPerformanceState source;
		source.Configure(DLSSNRProcessingMode::Pipeline,interval);
		assert(source.maxAgeFrames == 3);
		source.Begin(17,1,1,1,true,false);
		const auto origin=source.sequence,epoch=source.epoch;
		DLSSNREnhancementPool pool;
		const auto slot=pool.Reserve(0);
		for (uint32_t age=1;age<=completionAge;++age) {
			source.Begin(17*(age+1),age+1,1,1+int64_t(age)*166'667,true,false);
			assert(source.Usable(origin,epoch));
			assert(pool.Latest(source,UINT64_MAX) == pool.None);
		}
		pool.Publish(slot,origin,epoch,7);
		assert(pool.Latest(source,6) == pool.None && pool.Latest(source,7) == slot);
		pool.ReadSubmitted(slot,8);
		assert(pool[slot].sequence == origin && source.sequence-origin == completionAge);
		source.Begin(source.frame,source.revision,1,source.timestamp100ns,false,false);
		assert(!source.newCapture && !source.reset && source.sequence-origin == completionAge);
		for (uint32_t age=completionAge+1;age<=4;++age) {
			source.Begin(17*(age+1),age+1,1,1+int64_t(age)*166'667,true,false);
			assert(source.Usable(origin,epoch) == (age<=3));
			assert((pool.Latest(source,UINT64_MAX) == slot) == (age<=3));
			assert(pool[slot].sequence == origin);
		}
		source.Invalidate();
		assert(!source.Usable(source.sequence,epoch));
	}
	for (uint32_t invalid : {0u,1u,5u,UINT32_MAX}) {
		DLSSNRPerformanceState source;
		source.Configure(DLSSNRProcessingMode::Alternating,invalid);
		assert(source.updateInterval == 2 && source.maxAgeFrames == 1);
	}
	// Expiry uses the same actual source budget as selection and publication.
	for (uint32_t interval : {2u,3u,4u}) {
		DLSSNRPerformanceState source;
		source.Configure(DLSSNRProcessingMode::Alternating,interval);
		source.Begin(100,1,1,100,true,false);
		DLSSNREnhancementPool pool;
		const auto slot=pool.Reserve(0);
		pool.Publish(slot,source.sequence,source.epoch,2);
		assert(pool.Latest(source,1) == pool.None && pool.Latest(source,2) == slot);
		const auto origin=source.sequence,epoch=source.epoch;
		for (uint64_t age=1;age<=interval;++age) {
			source.Begin(100+age*17,age+1,1,100+int64_t(age)*166'667,true,false);
			assert(source.Usable(origin,epoch) == (age<interval));
			assert((pool.Latest(source,2) == slot) == (age<interval));
			assert(pool.ExpireUnconsumed(source) == unsigned(age==interval));
			assert(pool.ExpireUnconsumed(source) == 0);
		}
		assert(!source.Usable(source.sequence+1,epoch));
		source.Invalidate();
		assert(!source.Usable(source.sequence,epoch));
	}
	{
		DLSSNREnhancementPool pool;
		DLSSNRPerformanceState source;
		source.Begin(100,1,1,100,true,false);
		const auto a=pool.Reserve(0),b=pool.Reserve(0),c=pool.Reserve(0);
		assert(a!=b && a!=c && b!=c && pool.Reserve(0) == pool.None);
		pool.Publish(a,1,source.epoch,2);pool.ReadSubmitted(a,8);
		assert(pool.Reserve(7) == pool.None); // CPU return cannot retire a GPU read.
		source.Begin(200,2,1,200,true,false);
		pool.Publish(b,2,source.epoch,3);pool.Publish(c,1,source.epoch,4);
		assert(pool.Latest(source,4) == b); // later completion cannot replace newer source.
		pool.ReadSubmitted(b,10);pool.ReadSubmitted(c,9);
		assert(pool.Reserve(7) == pool.None && pool.Reserve(8) == a);
		pool.Cancel(a);
		assert(pool.Version() == 3);
	}
	{
		DLSSNRPerformanceState source;
		DLSSNRPostOutputState published;
		source.Begin(1,1,1,1,true,false);
		assert(!published.CanSubmit(source,1,source.epoch,true,false));
		published.Published(source);
		assert(published.CanSubmit(source,1,source.epoch,true,false));
		assert(!published.CanSubmit(source,1,source.epoch,true,true));
		assert(!published.CanSubmit(source,1,source.epoch,false,false));
		const auto oldSequence=source.sequence,oldEpoch=source.epoch;
		source.Begin(2,2,1,166'668,true,false);
		assert(!published.CanSubmit(source,source.sequence,source.epoch,true,false));
		assert(!published.CanSubmit(source,oldSequence,oldEpoch,true,false));
		published.Published(source);
		assert(published.CanSubmit(source,source.sequence,source.epoch,true,false));
		source.Invalidate();
		assert(!published.CanSubmit(source,source.sequence,source.epoch,true,false));
	}
	{
		DLSSNRPerformanceState source;
		source.Begin(1,1,1,1,true,false);
		auto epoch=source.epoch;
		source.Begin(1,2,1,1,false,false); // editing one capture rebuilds without advancing capture age.
		assert(source.sequence == 1 && source.epoch != epoch);
		epoch=source.epoch;
		source.Begin(2,3,1,6'000'001,true,false);
		assert(source.epoch != epoch && !source.Usable(1,epoch));
		epoch=source.epoch;
		source.Begin(3,4,2,6'166'668,true,false);assert(source.epoch != epoch);
		epoch=source.epoch;
		source.Begin(4,5,2,10,true,false);assert(source.epoch != epoch);
		assert(!source.Usable(0,source.epoch));
		epoch=source.epoch;
		source.Begin(source.frame,source.revision+1,source.historyRevision,source.timestamp100ns,false,false);
		assert(!source.Usable(source.sequence,epoch)); // late completion cannot cross an upstream edit.
	}
	std::map<std::string,float> values;
	auto get=[&](std::string_view name,float fallback) {
		const auto it=values.find(std::string(name));return it==values.end()?fallback:it->second;
	};
	constexpr auto effect="DLSSNR\\DLSSNR_AI_Filter";
	assert(IsDLSSNRPerformanceConfigurationValid(get));
	assert(DLSSNRSparseUpdateInterval(get) == 2);
	for (int mode : {0,1}) {
		values={{"processingMode",float(mode)},{"resolutionControl",1.f},{"opticalFlowMethod",1.f},
			{"residualTransfer",1.f},{"performanceShowAdvanced",1.f}};
		assert(DLSSNRMode(get) == DLSSNRProcessingMode(mode));
		assert(IsDLSSNRPerformanceConfigurationValid(get));
		assert(IsEffectParameterVisible(effect,"sparseUpdateInterval",get) == (mode==1));
		for (float interval : {2.f,3.f,4.f}) {
			values["sparseUpdateInterval"]=interval;
			assert(IsDLSSNRPerformanceConfigurationValid(get));
			assert(DLSSNRSparseUpdateInterval(get) == unsigned(interval));
		}
		// Deleted controls have no configuration effect or restart identity.
		struct Option { std::map<std::string,float> parameters; } before{values},after{values};
		for (const char* name : {"recoveryPolicy","bufferedDelayFrames","asyncResidualComposition","residualMaxAgeFrames"}) {
			after.parameters[name]=std::numeric_limits<float>::quiet_NaN();
			assert(!IsDLSSNRPerformanceParameter(name) && SameDLSSNRPerformanceControls(before,after));
		}
	}
	for (const float retired : {2.f,3.f}) {
		values["processingMode"] = retired;
		assert(DLSSNRMode(get) == (retired == 3.f ? DLSSNRProcessingMode::Alternating : DLSSNRProcessingMode::EveryFrame));
		assert(!IsDLSSNRPerformanceConfigurationValid(get));
	}
	std::cout << "Two-mode public contracts and legacy internal capture cadence, true age, epochs, bounded fences and post-output contracts passed.\n";
}
