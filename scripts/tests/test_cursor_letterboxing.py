"""Exercise production cursor transitions at image/letterbox boundaries with Win32 doubles."""
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[2]
source = (root/'src/Magpie.Core/CursorManager.cpp').read_text(encoding='utf-8-sig')
def method(signature):
    start = source.index(signature)
    return source[start:source.index('\n}', start)+2]

harness = r'''
#include <windows.h>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>
#include <memory>
#include <vector>
#include <iostream>
#undef SetWindowLong
#define SetWindowLong FakeSetWindowLong
#define GetCursorPos FakeGetCursorPos
#define GetGUIThreadInfo FakeGetGUIThreadInfo
#define IsChild FakeIsChild
#define MonitorFromPoint FakeMonitorFromPoint
namespace Magpie {
bool operator==(const POINT& a,const POINT& b) { return a.x==b.x&&a.y==b.y; }
bool operator==(const RECT& a,const RECT& b) { return a.left==b.left&&a.top==b.top&&a.right==b.right&&a.bottom==b.bottom; }
template<class T> using SmallVectorImpl=std::vector<T>;
template<class T,int> using SmallVector=std::vector<T>;
POINT pointer{}; RECT clip{}; DWORD style=0; bool sourceFocused=true;
HWND output=(HWND)1, input=(HWND)2;
struct FrameTrace { enum class Event { CursorUpdate }; struct Scope { Scope(Event){} }; };
struct Win32Helper { static SIZE GetSizeOfRect(const RECT& r) { return {r.right-r.left,r.bottom-r.top}; } };
struct ScalingOptions { bool threeD=false; bool Is3DGameMode() const {return threeD;} bool IsWindowedMode() const {return false;} };
struct Renderer {
    RECT src{128,131,1112,772}, dest{788,399,1772,1040}; bool visible=false;
    const RECT& SrcRect() const{return src;} const RECT& DestRect() const{return dest;}
    bool IsEffectParametersVisible() const{return visible;}
};
struct Tracker {bool IsMoving() const{return false;} HWND Handle() const{return input;}};
struct ScalingWindow {
    Magpie::Renderer renderer; ScalingOptions options; Tracker tracker; RECT surface{0,0,2560,1440};
    static ScalingWindow& Get(){static ScalingWindow w;return w;}
    auto& Renderer(){return renderer;} auto& Options(){return options;} auto& SrcTracker(){return tracker;}
    const RECT& RendererRect() const{return surface;}
    bool IsSourceForeground() const{return sourceFocused;} bool IsResizingOrMoving() const{return false;}
    HWND Handle() const{return output;}
};
BOOL GetCursorPos(POINT* p){*p=pointer;return TRUE;}
BOOL GetGUIThreadInfo(DWORD,GUITHREADINFO*){return TRUE;}
DWORD GetWindowExStyle(HWND){return style;} DWORD GetWindowStyle(HWND){return 0;}
LONG SetWindowLong(HWND,int,LONG value){style=value;return 0;}
BOOL IsChild(HWND,HWND){return FALSE;}
HWND WindowFromPoint(HWND,const RECT&,POINT,bool through){return through?input:output;}
HMONITOR MonitorFromPoint(POINT p,DWORD){return PtInRect(&ScalingWindow::Get().surface,p)?(HMONITOR)1:nullptr;}
SmallVector<RECT,0> ObtainMonitorRects(){return {ScalingWindow::Get().surface};}
bool AnyIntersectedMonitor(const SmallVectorImpl<RECT>& monitors,const RECT& r){
    for(auto m:monitors) if(r.left<m.right&&r.right>m.left&&r.top<m.bottom&&r.bottom>m.top)return true;
    return false;
}
bool IsEdgeArea(int16_t area){return area>=HTSIZEFIRST&&area<=HTSIZELAST;}
struct Life {bool IsStopping() const{return false;}};
struct CursorManager {
    std::shared_ptr<Life> _lifetime=std::make_shared<Life>();
    bool _isUnderCapture=false,_isCapturedOnOverlay=false,_isCapturedOnForeground=false,_isOnOverlay=false,_shouldDrawCursor=false;
    POINT _localCursorPosOnMoving{LONG_MAX,0},_cursorPos{},_lastCompletedHitTestPos{LONG_MAX,0};
    int16_t _lastCompletedHitTestResult=HTCLIENT;
    bool _UpdateParameterCursor(){return false;}
    void _RestoreClipCursor(){clip={LONG_MIN,LONG_MIN,LONG_MAX,LONG_MAX};}
    void _SetClipCursor(const RECT& r,bool=false){clip=r;}
    void _ClipCursorOnSrcMoving(){} void _ShowSystemCursor(bool){} void _ClearHitTestResult(){}
    void _SrcHitTestAsync(POINT){} void _AdjustCursorSpeed(){} void _RestoreCursorSpeed(){}
    void _ReliableSetCursorPos(POINT p){pointer=p;} void _UpdateCursorPos(){_cursorPos=pointer;}
    void _UpdateCursorState() noexcept; void _ClipCursorForMonitors(POINT) noexcept;
    void _StartCapture(POINT&) noexcept; bool _StopCapture(POINT&,bool=false) noexcept;
};
PRODUCTION
}
int main(){using namespace Magpie; auto& w=ScalingWindow::Get(); CursorManager c;
    // Every edge remains reachable before the mouse button is pressed.
    w.renderer.visible=true;
    for(POINT p : {POINT{2350,285},POINT{100,720},POINT{1280,50},POINT{1280,1300}}){
        for(bool onOverlay:{false,true}){
            c._isOnOverlay=onOverlay;c._isUnderCapture=false;pointer=p;style=WS_EX_TRANSPARENT;
            c._UpdateCursorState();
            assert(pointer==p&&!c._isUnderCapture&&!(style&WS_EX_TRANSPARENT));
            assert(PtInRect(&clip,p));
        }
        // Leaving captured image coordinates enters the displayed black border.
        pointer=ScalingToSrc(p);c._isUnderCapture=true;c._isOnOverlay=true;
        c._UpdateCursorState();assert(!c._isUnderCapture&&pointer==p&&PtInRect(&clip,p));
    }
    // An owned drag can cross any image edge without confinement to the image.
    c._isCapturedOnOverlay=true;c._UpdateCursorState();assert(clip==w.surface);
    c._isCapturedOnOverlay=false;
    // Closing the panel restores the old focused-source skip/confinement policy.
    w.renderer.visible=false;c._isUnderCapture=false;c._isOnOverlay=false;pointer={2350,720};
    c._UpdateCursorState();assert(c._isUnderCapture);
    c._isUnderCapture=false;c._ClipCursorForMonitors({1280,720});assert(clip==w.renderer.dest);
    // The existing 3D source confinement still wins.
    w.renderer.visible=true;w.options.threeD=true;c._isUnderCapture=true;
    c._UpdateCursorState();assert(clip==w.renderer.src);
    std::cout<<"PASS production cursor letterboxing: four-edge approach/hover, source capture exit, overlay drag, closed-panel policy and 3D confinement\n";
}
'''
production = source[source.index('static POINT SrcToScaling'):source.index('CursorManager::CursorManager')]
production += '\n'.join(method(s) for s in ['void CursorManager::_UpdateCursorState()',
    'void CursorManager::_ClipCursorForMonitors(', 'void CursorManager::_StartCapture(', 'bool CursorManager::_StopCapture('])
out=Path(sys.argv[1]).resolve();out.mkdir(parents=True,exist_ok=True)
(out/'cursor_letterboxing.cpp').write_text(harness.replace('PRODUCTION',production),encoding='utf-8')
