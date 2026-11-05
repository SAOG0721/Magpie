#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>
#include "FrameGuidanceAvailability.h"

struct ID3D11Texture2D {};
struct ID3D11Fence {};
struct ID3D11DeviceContext4 {
	std::vector<std::pair<ID3D11Fence*, uint64_t>> waits;
	bool fail = false;
	int Wait(ID3D11Fence* fence, uint64_t value) { waits.emplace_back(fence,value); return fail ? -1 : 0; }
};
bool FAILED(int value) { return value < 0; }
enum DXGI_FORMAT { DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R8_UNORM };
namespace Magpie {
#include "DLSSNRGuidanceTypesUnderTest.h"
struct TextureRef { ID3D11Texture2D* texture; ID3D11Texture2D* get() const { return texture; } };
struct DLSSNRFilter { struct Impl {
	uint32_t sourceWidth=17, sourceHeight=11, width=7, height=3;
	TextureRef reducedMotion11, reducedDepth11, reducedConfidence11;
}; };
struct FrameGuidanceD3D12Interop { bool WaitForProducer(ID3D11DeviceContext4*, const FrameGuidanceView&, bool = false) noexcept; };
#include "DLSSNRGuidanceUnderTest.h"
}

int main() {
	using namespace Magpie;
	ID3D11Texture2D motion, depth, confidence, replacement, reducedMotion, reducedDepth, reducedConfidence;
	ID3D11Fence firstFence, secondFence;
	FrameGuidanceMetadata metadata{.frameId=5, .resourceGeneration=2, .sourceExtent={17,11},
		.validRegion={2,1,11,9}, .valid=true, .isZero=true};
	FrameGuidanceView a{{&depth,DXGI_FORMAT_R32_FLOAT,metadata}, {&motion,DXGI_FORMAT_R16G16_FLOAT,metadata},
		{&confidence,DXGI_FORMAT_R8_UNORM,metadata}};
	assert(a.IsValidFor(5,{17,11}));
	auto b=a;
	assert(SameGuidance(a,b));
	b.motion.metadata.frameId=6;
	assert(!SameGuidance(a,b) && SameGuidance(a,b,true));
	b=a; b.confidence.texture=&replacement;
	assert(!SameGuidance(a,b,true));
	b=a; b.depth.metadata.resourceGeneration++;
	assert(!SameGuidance(a,b,true));
	b=a; b.motion.metadata.requiresHistoryReset=true;
	assert(!SameGuidance(a,b) && SameGuidance(a,b,true));
	b=a; b.depth.metadata.validRegion={0,0,17,11};
	assert(!SameGuidance(a,b,true));
	DLSSNRFilter::Impl impl;
	impl.reducedMotion11={&reducedMotion}; impl.reducedDepth11={&reducedDepth}; impl.reducedConfidence11={&reducedConfidence};
	const auto scaled=MakeReducedGuidance(impl,a);
	assert(scaled.IsValidFor(5,{7,3}));
	assert(scaled.motion.texture==&reducedMotion && scaled.depth.texture==&reducedDepth);
	assert((scaled.depth.metadata.validRegion==FrameGuidanceRegion{0,0,6,3}));
	assert(scaled.depth.metadata.isZero && scaled.depth.metadata.resourceGeneration==2);
	assert(ScaleGuidanceRegion({0,0,1,1},{1,1},{1,1})==FrameGuidanceRegion::Full({1,1}));
	FrameGuidanceD3D12Interop interop;
	ID3D11DeviceContext4 dc;
	a.motion.metadata.sync={&firstFence,7}; a.depth.metadata.sync={&firstFence,9}; a.confidence.metadata.sync={&secondFence,11};
	assert(interop.WaitForProducer(&dc,a,true));
	assert(dc.waits.size()==2 && dc.waits[0].first==&firstFence && dc.waits[0].second==9 && dc.waits[1].second==11);
	dc.waits.clear(); a.confidence.metadata.sync={&firstFence,13};
	assert(interop.WaitForProducer(&dc,a,true));
	assert(dc.waits.size()==1 && dc.waits[0].second==13);
	dc.waits.clear();
	assert(interop.WaitForProducer(&dc,a));
	assert(dc.waits.size()==1 && dc.waits[0].second==9); // FG does not consume confidence
	dc.fail=true; assert(!interop.WaitForProducer(&dc,a,true));
	std::cout << "Production guidance: identities/generations/regions, constant reuse metadata, confidence waits, fence dedup and failures passed.\n";
}
