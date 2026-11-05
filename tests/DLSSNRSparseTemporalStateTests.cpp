#include "DLSSNRTemporalState.h"
#include <cassert>
#include <cmath>
#include <iostream>

using namespace Magpie;

int main() {
	DLSSNRSparseTemporalObservation observation;
	assert(!observation.Accepts(4,0,1,1,4));
	assert(!observation.Accepts(4,5,1,1,4));
	assert(!observation.Accepts(4,4,1,0,0));
	assert(observation.Accepts(4,4,1,1,0));
	assert(!observation.Accepts(5,4,1,1,0)); // zero age never disables freshness.
	assert(observation.Weight(1,1,1000000,false,80)==0);
	observation.Observe(1,1,1,1000000);
	for (uint64_t target=2;target<=4;++target) {
		assert(observation.Accepts(target,1,1,1,3));
		assert(observation.Weight(1,1,1000000+int64_t(target-1)*166667,false,80)==1);
		observation.Observe(1,1,1,1000000+int64_t(target-1)*166667);
		assert(observation.source==1 && observation.timestamp==1000000);
	}
	assert(!observation.Accepts(5,1,1,1,3));
	assert(observation.Accepts(4,4,1,1,3)); // actual support can change without another NR result.
	observation.Observe(4,1,1,1500000);
	assert(observation.source==4 && observation.timestamp==1000000);
	assert(!observation.Accepts(8,4,1,1,3)); // the current real source still expires by target age.
	const float expected=std::exp(-.05f/.08f);
	assert(std::abs(observation.Weight(1,2,1500000,false,80)-expected)<1e-6f);
	assert(observation.Weight(2,2,1500000,false,80)==0); // epochs remain hard boundaries.
	assert(observation.Weight(1,2,1500000,true,80)==0);
	assert(observation.Weight(1,2,1000000,false,80)==0); // same-target replacement is not elapsed time.
	assert(observation.Expired(3500001));
	assert(observation.Weight(1,1,3500001,false,80)==0);
	observation.Observe(1,1,1,3500001);
	assert(observation.Expired(3500001)); // repeated output cannot extend bounded support.
	observation.Observe(5,1,2,3500001);
	assert(!observation.Expired(3500001));
	assert(!observation.Accepts(6,5,1,1,3)); // stale result revision.
	assert(observation.Accepts(6,5,2,1,3)); // a new epoch has independent revisions.
	observation.Observe(5,2,1,4000000);
	assert(!observation.Accepts(6,5,1,3,3)); // a newer revision cannot revive an older epoch.
	assert(!observation.Accepts(6,5,0,3,3));
	observation.Observe(6,1,3,5000000);
	observation.Observe(6,2,0,5000000);
	assert(observation.source==5 && observation.epoch==2 && observation.revision==1 && observation.timestamp==4000000);
	// Source 3 first contributed on target 4, and its asynchronous confirmation
	// arrives on target 6. Temporal time belongs to target 4, never completion.
	DLSSNRSparseTemporalObservation delayed;
	delayed.Observe(1,1,1,1000000);
	assert(delayed.Accepts(6,3,1,2,5));
	const float delayedWeight=delayed.Weight(1,2,1833334,false,80,1500000);
	assert(std::abs(delayedWeight-std::exp(-.05f/.08f))<1e-6f);
	delayed.Observe(3,1,2,1500000);
	assert(delayed.source==3 && delayed.revision==2 && delayed.timestamp==1500000);
	for (unsigned redraw=0;redraw<20;++redraw) {
		delayed.Observe(3,1,2,1833334);
		assert(delayed.timestamp==1500000 && delayed.source==3);
		assert(delayed.Weight(1,2,1833334,false,80,1500000)==1);
	}
	assert(!delayed.Expired(4000000));
	assert(delayed.Expired(4000001));
	assert(delayed.Weight(1,2,4000001,false,80,1500000)==0);
	assert(!delayed.Accepts(9,3,1,2,5)); // real source age still bounds reuse.
	delayed.Observe(7,1,3,4166667);
	assert(delayed.timestamp==4166667 && !delayed.Expired(4166667));
	assert(!delayed.Accepts(8,7,1,2,5));
	for (int fps : {30,60,120}) {
		DLSSNRSparseTemporalObservation clock;
		clock.Observe(1,1,1,1000000);
		const int64_t timestamp=1000000+std::llround(3.*10000000/fps);
		assert(std::abs(clock.Weight(1,2,timestamp,false,80)-std::exp(-3.f/fps/.08f))<1e-6f);
	}
	DLSSNRSparseTemporalDrawIdentity drawn;
	drawn.frame=105; drawn.inputRevision=3; drawn.historyRevision=2;
	drawn.target=12; drawn.source=10; drawn.epoch=2; drawn.observationRevision=4; drawn.outputRevision=8;
	drawn.input=1; drawn.output=2; drawn.zeroGeneration=drawn.motionGeneration=5;
	drawn.resetFlags=2; // A target's already-consumed reset remains stable on hold.
	const auto held=drawn;
	assert(held==drawn); // No physical isNewCaptureFrame/motion flag in the identity.
	assert(held.ReusesPixelsFrom(drawn));
	auto changed=held; ++changed.outputRevision; assert(changed!=drawn);
	assert(!changed.ReusesPixelsFrom(drawn));
	changed=held; ++changed.observationRevision; assert(changed!=drawn);
	changed=held; ++changed.epoch; assert(changed!=drawn);
	changed=held; ++changed.inputRevision; assert(changed!=drawn);
	changed=held; ++changed.resetFlags; assert(changed!=drawn);
	changed=held; ++changed.output; assert(changed!=drawn);
	changed=held; changed.resetFlags=0;
	assert(changed.ReusesPixelsFrom(drawn)); // A target's consumed pulse deasserted on hold.
	assert(!drawn.ReusesPixelsFrom(changed)); // Reassertion is a new reset.
	std::cout << "Sparse temporal CPU contracts passed: source/epoch, distinct observations, bounded support and elapsed time.\n";
}
