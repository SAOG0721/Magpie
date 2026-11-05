#include <d3d11_4.h>
#include "FrameGuidanceTypes.h"
#include <cassert>
#include <iostream>

using namespace Magpie;

int main() {
	const FrameGuidanceExtent extent{1280,720};
	FrameGuidanceMetadata meta{};
	meta.frameId=105; meta.captureSequence=12; meta.resourceGeneration=3;
	meta.sourceExtent=extent; meta.validRegion=FrameGuidanceRegion::Full(extent); meta.valid=true;
	// Sentinel pointers are never dereferenced: this test exercises metadata only.
	auto* texture=reinterpret_cast<ID3D11Texture2D*>(uintptr_t(1));
	FrameGuidanceView view;
	view.depth={texture,DXGI_FORMAT_R32_FLOAT,meta};
	view.motion={texture,DXGI_FORMAT_R16G16_FLOAT,meta};
	view.confidence={texture,DXGI_FORMAT_R8_UNORM,meta};
	assert(view.IsValidFor(105,extent));
	assert(!view.HasReverseMotionForPair(105,101,extent));
	view.reverseMotion={texture,DXGI_FORMAT_R16G16_FLOAT,meta};
	view.reverseConfidence={texture,DXGI_FORMAT_R8_UNORM,meta};
	view.motionPair={105,101,12,12,true}; // One session, nonconsecutive accepted frame IDs.
	assert(view.HasReverseMotionForPair(105,101,extent));
	assert(!view.HasReverseMotionForPair(105,104,extent));
	assert(!view.HasReverseMotionForPair(101,105,extent));
	assert(!view.HasReverseMotionForPair(105,101,{640,360}));
	auto invalid=view; invalid.reverseConfidence.metadata.captureSequence=11;
	assert(!invalid.HasReverseMotionForPair(105,101,extent));
	invalid=view; invalid.reverseMotion.metadata.resourceGeneration=2;
	assert(!invalid.HasReverseMotionForPair(105,101,extent));
	invalid=view; invalid.requiresHistoryReset=true;
	assert(!invalid.HasReverseMotionForPair(105,101,extent));
	FrameGuidanceView zero=view;
	zero.motion.metadata.isZero=zero.confidence.metadata.isZero=true;
	const auto disabled=SelectFrameGuidanceChannels(view,zero,105,extent,false);
	assert(disabled.IsValidFor(105,extent));
	assert(!disabled.reverseMotion.texture && !disabled.motionPair.valid);
	FrameGuidanceFrame previous{};
	previous.frameId=101; previous.captureSequence=12; previous.resourceGeneration=3;
	previous.sourceExtent=extent; previous.timestamp100ns=1000;
	auto current=previous; current.frameId=105; current.timestamp100ns=1200;
	assert(current.ContinuesMotionHistory(previous)); // Equal session sequence is normal.
	current.captureSequence=13;
	assert(!current.ContinuesMotionHistory(previous));
	current.captureSequence=12; current.resourceGeneration=4;
	assert(!current.ContinuesMotionHistory(previous));
	current.resourceGeneration=3; current.frameId=101;
	assert(!current.ContinuesMotionHistory(previous));
	current.frameId=105; current.timestamp100ns=999;
	assert(!current.ContinuesMotionHistory(previous));
	std::cout << "Frame-guidance pair CPU contracts passed: explicit direction, frame pair, extent, generation and disabled consumers.\n";
}
