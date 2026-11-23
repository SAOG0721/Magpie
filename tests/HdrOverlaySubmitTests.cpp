#include <windows.h>
#include "OverlayColor.h"
#include <cassert>
#include <iostream>
#include <memory>
using namespace Magpie;
struct Window {
	const Window& Options() const { return *this; }
	bool IsHdrCompatibilityEnabled() const { return true; }
	static Window& Get() { static Window window; return window; }
};
using ScalingWindow = Window;
struct Cursor { bool NeedRedraw() const {return false;} bool IsBackgroundDependent() const {return false;}
	bool HasOriginalRefreshPending() const {return false;} };
struct Overlay { bool NeedRedraw(int) const {return false;} };
struct Timer { int FPS() const {return 60;} };
struct Reference { void OnPresented() {} };
struct Renderer {
	std::unique_ptr<int> _frontendPresentedBaseTexture=std::make_unique<int>(), _frontendBaseTexture=std::make_unique<int>();
	HdrFrameMetadata _frontendFrameMetadata, _frontendPresentedFrameMetadata;
	float _overlayMonitorSdrWhiteNits=203, visibleScale=203.f/80;
	bool _frontendPresentedBaseValid=false, _frontendBaseIsPresented=false, _isPassThroughActive=false;
	Cursor _cursorDrawer; Overlay _overlayDrawer; Timer _stepTimer; Reference _passThroughFrames;
	unsigned overlayDraws=0;
	uint64_t _overlayActionRevision=0, _presentedOverlayActionRevision=0;
	bool overlayAvailable=true;
	bool _HasPendingOverlayAction() const {return _overlayActionRevision!=_presentedOverlayActionRevision;}
	bool _FrontendOverlayRender(bool) {
		if (!overlayAvailable) return false;
		++overlayDraws;
		visibleScale=ResolveOverlayWhiteScale(true,DXGI_FORMAT_R16G16B16A16_FLOAT,_frontendPresentedFrameMetadata,_overlayMonitorSdrWhiteNits);
		_presentedOverlayActionRevision=_overlayActionRevision;
		return true;
	}
	void Submit(bool submitted, bool stableBaseOnly=false, bool uiInIndependentLayer=true) {
#include "OverlaySubmitProduction.inc"
	}
};
int main() {
	Renderer renderer;
	renderer._frontendPresentedFrameMetadata.overlayReferenceWhiteNits=203;
	renderer._frontendFrameMetadata.overlayReferenceWhiteNits=280;
	*renderer._frontendBaseTexture=17;
	renderer.Submit(false); // Failed submission leaves the last visible UI/scene intact.
	assert(renderer.overlayDraws==0 && renderer.visibleScale==203.f/80);
	assert(!renderer._frontendBaseIsPresented && *renderer._frontendPresentedBaseTexture==0);
	renderer.Submit(true);
	assert(renderer.overlayDraws==1 && renderer.visibleScale==3.5f);
	assert(renderer._frontendBaseIsPresented && *renderer._frontendPresentedBaseTexture==17);
	renderer._frontendFrameMetadata.overlayReferenceWhiteNits=160;
	*renderer._frontendBaseTexture=41;
	renderer._frontendBaseIsPresented=false;
	renderer.Submit(true,true); // Stable replay must not consume a newer pending white point.
	assert(renderer.overlayDraws==1 && renderer.visibleScale==3.5f);
	assert(!renderer._frontendBaseIsPresented && *renderer._frontendPresentedBaseTexture==17);
	renderer.Submit(true);
	assert(renderer.overlayDraws==2 && renderer.visibleScale==2);
	assert(*renderer._frontendPresentedBaseTexture==41);
	renderer.Submit(true); // Same white point needs no independent UI update.
	assert(renderer.overlayDraws==2);
	assert(*renderer._frontendPresentedBaseTexture==41);
	renderer._frontendFrameMetadata.overlayReferenceWhiteNits=203; // Applied reset.
	renderer.Submit(true);
	assert(renderer.overlayDraws==3 && renderer.visibleScale==203.f/80);
	renderer._frontendFrameMetadata.overlayReferenceWhiteNits=500;
	renderer.Submit(true);
	assert(renderer.overlayDraws==4 && renderer.visibleScale==3.75f);
	renderer._frontendFrameMetadata.overlayReferenceWhiteNits=4000;
	renderer.Submit(true);
	assert(renderer.overlayDraws==4); // Identical capped result, no pumping with scene peaks.
	renderer._frontendFrameMetadata.overlayReferenceWhiteNits=160;
	renderer.Submit(true,false,false);
	assert(renderer.overlayDraws==4); // Main-surface path already drew UI with that frame.
	renderer._frontendFrameMetadata.overlayReferenceWhiteNits=240;
	renderer.overlayAvailable=false;
	renderer.Submit(true);
	assert(renderer.overlayDraws==4 && renderer._HasPendingOverlayAction());
	renderer.overlayAvailable=true;
	renderer.Submit(true); // Same scene metadata, retry retained after independent target failure.
	assert(renderer.overlayDraws==5 && renderer.visibleScale==3 && !renderer._HasPendingOverlayAction());
	std::cout << "PASS submitted color snapshots: failed submit, stable replay, live edit/reset, same/capped white and main-surface path\n";
}
