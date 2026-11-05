#include "../src/Magpie.Core/include/DLSSNRSparseState.h"
#include "../src/Magpie.Core/DLSSNRColorShader.h"
#include "../src/Magpie.Core/DLSSNRSparseShader.h"
#include <d3dcompiler.h>
#include <cassert>
#include <iostream>
#include <string>

using namespace Magpie;

int main() {
	assert(DLSSNRSparseBudget{}.interval==2);
	for (unsigned interval:{2u,3u,4u}) {
		DLSSNRSparseBudget budget; budget.interval=interval;
		unsigned submissions=0;
		for (uint64_t capture=1;capture<=25;++capture) {
			budget.Advance(capture);
			const bool due=(capture-1)%interval==0;
			assert(budget.CanSubmit(capture,false)==due);
			if (due) { assert(!budget.Submit(capture,false)); ++submissions; }
			for (unsigned redraw=0;redraw<20;++redraw) {
				budget.Advance(capture);
				assert(!budget.CanSubmit(capture,false));
			}
		}
		assert(submissions==1+24/interval);
	}
	DLSSNRSparseBudget b; b.interval=3; b.Advance(1);
	assert(b.CanSubmit(1,false)); assert(!b.Submit(1,false));
	b.Advance(2); assert(!b.CanSubmit(2,false));
	b.Advance(4); assert(b.CanSubmit(4,false)); assert(!b.Submit(4,false));
	b.Advance(5); assert(b.CanSubmit(5,true)); assert(b.Submit(5,true));
	assert(!b.burstAvailable && b.refillAt==11);
	b.Advance(6); assert(!b.CanSubmit(6,false) && !b.CanSubmit(6,true));
	b.Advance(8); assert(b.CanSubmit(8,false)); b.Submit(8,false);
	b.Advance(10); assert(!b.CanSubmit(10,true));
	b.Advance(11); assert(b.burstAvailable && b.CanSubmit(11,false));
	assert(!b.CanSubmit(8,true));
	// A late mailbox is charged at actual submission time. Neither the old
	// source nor idle/redraw callbacks buy another accepted capture.
	DLSSNRSparseBudget late; late.interval=3; late.Advance(1); late.Submit(1,false);
	late.Advance(10); late.Submit(7,false); late.Advance(11);
	assert(!late.CanSubmit(11,false)); late.Advance(13); assert(late.CanSubmit(13,false));
	for (int i=0;i<20;++i) late.Advance(13);
	assert(late.currentCapture==13);
	late.Submit(13,false); late.Advance(14); late.RecordRefresh(13);
	assert(late.lastSource==13 && late.lastAdmission==14);
	late.Advance(16); assert(!late.CanSubmit(16,false));
	late.Advance(17); assert(late.CanSubmit(17,false));

	DLSSNRSparseObservationClock observations;
	DLSSNRSparseObservationClock::Result result;
	assert(result.Begin(1,1,1,1,1));
	assert(!result.Begin(1,1,1,2,2)); // exactly one asynchronous sample per result.
	assert(!observations.Confirm(result,1,1,1,1,false)); // all current raw, no NR observation.
	assert(observations.revision==0 && !result.observed && result.ready==0);
	assert(!result.Begin(1,1,1,1,2)); // same-target redraw cannot retry a zero-support sample.
	assert(result.Begin(1,1,1,2,2)); // later capture can acquire real support.
	assert(observations.Confirm(result,1,1,1,1,true));
	assert(observations.revision==1 && result.observed);
	for (uint64_t target=2;target<10;++target) {
		assert(!result.Begin(1,1,1,target,3));
		assert(!observations.Confirm(result,1,1,1,1,true));
		assert(observations.revision==1);
	}
	assert(result.source==1 && result.target==2); // redraw never refreshes source or contribution target.
	DLSSNRSparseObservationClock::Result overwritten;
	assert(overwritten.Begin(3,1,2,3,3));
	assert(!observations.Confirm(overwritten,7,1,3,1,true)); // new ring identity discards stale data.
	assert(observations.revision==1 && !overwritten.observed);
	overwritten={};
	assert(overwritten.Begin(7,1,3,7,4));
	assert(!observations.Confirm(overwritten,7,1,3,2,true)); // hard epoch reset while pending.
	assert(observations.revision==1);
	overwritten={}; // publication cancels the old pending sample before buffer reuse.
	assert(overwritten.Begin(8,2,4,8,5));
	overwritten={};
	assert(!observations.Confirm(overwritten,8,2,4,2,true));
	assert(overwritten.Begin(9,2,5,9,6));
	assert(observations.Confirm(overwritten,9,2,5,2,true));
	assert(observations.revision==2 && overwritten.source==9);
	DLSSNRSparseObservationClock delayed;
	DLSSNRSparseObservationClock::Result source3;
	assert(source3.Begin(3,1,1,4,1,1500000));
	// Fence confirmation can arrive while capture 6 is displayed. Its timestamp
	// remains target 4's, and the actual NR source remains capture 3.
	assert(delayed.Confirm(source3,3,1,1,1,true));
	assert(delayed.timestamp==1500000 && source3.source==3 && source3.target==4);
	assert(!source3.Begin(3,1,1,6,2,1833334));
	assert(!delayed.Confirm(source3,3,1,1,1,true) && delayed.timestamp==1500000);
	// A zero-support readback can still be pending when the next captured
	// target uses the same result with actual support, then the source pauses.
	DLSSNRSparseObservationClock paused;
	DLSSNRSparseObservationClock::Result pendingZero;
	assert(pendingZero.Begin(3,1,1,4,1,1500000));
	assert(!pendingZero.Begin(3,1,1,5,2,1666667));
	assert(!pendingZero.NeedsRetry(5)); // one readback remains in flight.
	assert(pendingZero.deferredTarget==5 && pendingZero.deferredTimestamp==1666667);
	assert(!pendingZero.Begin(3,1,1,5,2,1999999)); // duplicate target cannot refresh its timestamp.
	assert(pendingZero.deferredTimestamp==1666667);
	assert(!paused.Confirm(pendingZero,3,1,1,1,false));
	assert(paused.revision==0 && pendingZero.NeedsRetry(5) && !pendingZero.NeedsRetry(4));
	const auto retryTimestamp=pendingZero.deferredTimestamp;
	assert(pendingZero.Begin(3,1,1,5,2,retryTimestamp));
	assert(!pendingZero.NeedsRetry(5) && pendingZero.deferredTarget==0);
	assert(paused.Confirm(pendingZero,3,1,1,1,true));
	assert(paused.revision==1 && paused.timestamp==1666667 && pendingZero.source==3 && pendingZero.target==5);
	assert(!pendingZero.Begin(3,1,1,5,3,2333333));
	assert(!paused.Confirm(pendingZero,3,1,1,1,true) && paused.revision==1);
	// The one deferred retry may itself be all raw. It is consumed once and
	// never turns a held zero-support target into an infinite wake/retry loop.
	DLSSNRSparseObservationClock::Result stillZero;
	assert(stillZero.Begin(3,1,2,4,3,1500000));
	assert(!stillZero.Begin(3,1,2,5,4,1666667));
	assert(!paused.Confirm(stillZero,3,1,2,1,false) && stillZero.NeedsRetry(5));
	assert(stillZero.Begin(3,1,2,5,4,stillZero.deferredTimestamp));
	assert(!paused.Confirm(stillZero,3,1,2,1,false));
	assert(!stillZero.NeedsRetry(5) && !stillZero.Begin(3,1,2,5,5,2333333));
	assert(paused.revision==1);
	// One latest skipped target replaces older retry requests; identities and
	// hard resets discard deferred work together with its pending readback.
	DLSSNRSparseObservationClock::Result replaced;
	assert(replaced.Begin(3,1,3,4,5,1500000));
	assert(!replaced.Begin(3,1,3,5,6,1666667));
	assert(!replaced.Begin(3,1,3,6,6,1833334));
	assert(!replaced.Begin(7,1,4,7,6,2000001));
	assert(replaced.deferredTarget==6 && replaced.deferredTimestamp==1833334);
	assert(!paused.Confirm(replaced,7,1,4,1,true));
	assert(replaced.deferredTarget==0 && !replaced.NeedsRetry(6));
	replaced={};
	assert(replaced.Begin(3,1,3,4,5,1500000));
	assert(!replaced.Begin(3,1,3,5,6,1666667));
	assert(!paused.Confirm(replaced,3,1,3,2,true));
	assert(replaced.deferredTarget==0 && !replaced.NeedsRetry(5));
	replaced={};
	assert(replaced.Begin(3,1,3,4,5,1500000));
	assert(!replaced.Begin(3,1,3,5,6,1666667));
	replaced.Cancel();
	assert(replaced.ready==0 && replaced.deferredTarget==0 && !replaced.NeedsRetry(5));
	DLSSNRSparseObservationClock::Result invalid;
	assert(!invalid.Begin(0,1,1,1,1)); assert(!invalid.Begin(1,0,1,1,1));
	assert(!invalid.Begin(1,1,0,1,1)); assert(!invalid.Begin(1,1,1,0,1));
	assert(!invalid.Begin(1,1,1,1,0));

	const std::string source=std::string(DLSSNR_COLOR_HLSL)+std::string(DLSSNR_SPARSE_HLSL);
	for (const char* entry:{"MapMotion","ComposeResidual","FillResidual","ExtractResidual","OutputColor"}) {
		ID3DBlob* compiled=nullptr; ID3DBlob* errors=nullptr;
		const auto hr=D3DCompile(source.data(),source.size(),"Sparse CPU shader validation",nullptr,nullptr,entry,"cs_5_0",
			D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&compiled,&errors);
		if (FAILED(hr) && errors) std::cerr.write(static_cast<const char*>(errors->GetBufferPointer()),errors->GetBufferSize());
		if (errors) errors->Release(); if (compiled) compiled->Release();
		if (FAILED(hr)) return 2;
	}
	std::cout << "Sparse interval 2/3/4, bounded budget, asynchronous first-contribution/overwrite/epoch/redraw and five CPU shader checks passed\n";
	return 0;
}
