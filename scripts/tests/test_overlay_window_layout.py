"""Generate a CPU harness using production layout code and the project's Dear ImGui.

Compile with MSVC, Magpie.Core/include and the installed ImGui include/library.
No native window or GPU is created. Pass an output directory as the argument.
"""
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[2]
drawer = (root / 'src/Magpie.Core/OverlayDrawer.cpp').read_text(encoding='utf-8-sig')
panel = drawer.split('bool OverlayDrawer::_DrawEffectParameters(', 1)[1]
prepare = panel.split('const ImGuiViewport* parameterViewport =', 1)[1].split('\n\tconst std::string title', 1)[0]
prepare = 'const ImGuiViewport* parameterViewport =' + prepare
record = panel.split('const bool expanded =', 1)[1].split('\n\tif (!expanded)', 1)[0]
record = 'const bool expanded =' + record

harness = r'''
#include "OverlayWindowGeometry.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <cassert>
#include <iostream>
#include <limits>
#include <string>
#include <unordered_map>
using namespace Magpie;
constexpr float EFFECT_PARAMETERS_MIN_WIDTH = 360, EFFECT_PARAMETERS_MIN_HEIGHT = 400;
const char* EFFECT_PARAMETERS_WINDOW_ID = "effectParameters";
struct Options { std::unordered_map<std::string, OverlayWindowOption> windows; };
struct Input { bool FrameInputCanceled() { return false; } void Tooltip(const char*,float) {} };
struct ScalingWindow { static ScalingWindow& Get() { static ScalingWindow s; return s; } struct Data { struct { std::string parameters; } toolbarShortcutLabels; } data; const Data& Options() const { return data; } };
struct StrHelper { template<class... T> static std::string Concat(T&&... t) { std::string s; (s.append(t),...); return s; } };
struct Panel {
    bool _parameterFocusSwitchingEnabled = false;
    bool IsEditingParameters() const { return true; }
    std::string _GetResourceString(const wchar_t*) { return "hint"; }
    Options options{{{"effectParameters", {0, 1, 60, 0.5f}}}};
    Options* _overlayOptions = &options;
    float _dpiScale = 1;
    bool _effectParametersWindowLayoutInitialized = false;
    bool _effectParametersWindowLayoutDirty = false;
    bool _isEffectParametersVisible = true;
    ImVec2 _effectParametersViewport{};
    ImVec2 _effectParametersViewportOrigin{};
    OverlayWindowRect _effectParametersWindowRect{};
    Input _imguiImpl;
    void Draw() {
        PREPARE
        const std::string title = "Parameters##effectParameters";
        const float parameterScale=_dpiScale*windowOption.zoom;
        RECORD
        if (expanded) ImGui::TextUnformatted("parameter controls");
        ImGui::End();
    }
    auto& Saved() { return options.windows.at("effectParameters"); }
};
void near(float actual, float expected) { assert(std::abs(actual - expected) < 1.1f); }
void contained(const OverlayWindowRect& r, float w, float h) {
    assert(std::isfinite(r.x) && std::isfinite(r.y));
    assert(r.x >= 0 && r.y >= 0 && r.width > 0 && r.height > 0);
    assert(r.x + r.width <= w && r.y + r.height <= h);
}
void frame(Panel& panel, float w, float h, bool down = false,
    ImVec2 pos = {-1,-1}, ImVec2 size = {-1,-1}, int collapse = -1, ImVec2 origin = {},
    ImVec2 mouse = {-100,-100}, ImVec2 image = {}) {
    auto& io = ImGui::GetIO(); io.DisplaySize = image.x>0?image:ImVec2{w,h}; io.DeltaTime = 1.0f / 60;
    io.AddMousePosEvent(mouse.x,mouse.y); io.AddMouseButtonEvent(0,down);
    ImGui::NewFrame();
    ImGui::GetMainViewport()->Pos = ImGui::GetMainViewport()->WorkPos = origin;
    ImGui::GetMainViewport()->Size = ImGui::GetMainViewport()->WorkSize = {w,h};
    if (pos.x >= 0) ImGui::SetNextWindowPos(pos);
    if (size.x >= 0) ImGui::SetNextWindowSize(size);
    if (collapse >= 0) ImGui::SetNextWindowCollapsed(collapse != 0);
    panel.Draw(); ImGui::Render();
    auto rect = panel._effectParametersWindowRect;
    rect.x -= origin.x; rect.y -= origin.y;
    contained(rect,w,h);
}
int main() {
    OverlayWindowOption old{0,1,60,0.5f};
    auto r = RestoreEffectParametersWindow(old,1920,1080,1);
    near(r.x,60); near(r.y,240); near(r.width,420); near(r.height,600);
    OverlayWindowOption saved{2,2,25,40,650,700};
    auto large = RestoreEffectParametersWindow(saved,1920,1080,1);
    near(large.x,1245); near(large.y,340);
    for (float dpi : {1.0f,1.25f,1.5f,2.0f}) {
        for (auto viewport : {ImVec2{1920,1080},ImVec2{800,600},ImVec2{320,240},ImVec2{80,60}}) {
            r = RestoreEffectParametersWindow(saved,viewport.x,viewport.y,dpi);
            contained(r,viewport.x,viewport.y);
        }
    }
    // A temporary constraint never mutates persisted preferences.
    r = RestoreEffectParametersWindow(saved,320,240,1);
    near(saved.width,650); near(saved.height,700);
    r = RestoreEffectParametersWindow(saved,1920,1080,1);
    near(r.width,650); near(r.height,700); near(r.x,large.x); near(r.y,large.y);
    for (auto rect : {OverlayWindowRect{20,30,420,600},
        OverlayWindowRect{730,240,420,600},OverlayWindowRect{1480,450,420,600}}) {
        OverlayWindowOption o; o.width = rect.width; o.height = rect.height;
        RememberOverlayWindowPosition(o,rect,1920,1080,1);
        r = RestoreEffectParametersWindow(o,1920,1080,1);
        near(r.x,rect.x); near(r.y,rect.y);
    }
    OverlayWindowOption invalid{50,99,std::numeric_limits<float>::infinity(),
        std::numeric_limits<float>::quiet_NaN(),-1,std::numeric_limits<float>::infinity()};
    SanitizeOverlayWindowOption(invalid);
    assert(invalid.hArea == 0 && invalid.vArea == 0 && invalid.hPos == 0 && invalid.vPos == 0);
    assert(invalid.width == 0 && invalid.height == 0);
    // Exercise the exact Begin/SizeFull/recording code with real Dear ImGui.
    IMGUI_CHECKVERSION(); ImGui::CreateContext();
    auto& io = ImGui::GetIO(); io.IniFilename = nullptr;
    unsigned char* pixels; int fw,fh; io.Fonts->GetTexDataAsRGBA32(&pixels,&fw,&fh);
    ImGui::GetStyle().WindowMinSize = {10,10};
    Panel panel;
    frame(panel,1920,1080); frame(panel,1920,1080);
    assert(!panel._effectParametersWindowLayoutDirty);
    // An explicit user resize/move is persisted in DIPs and anchored.
    frame(panel,1920,1080,true,{1200,300},{650,700});
    near(panel.Saved().width,650); near(panel.Saved().height,700);
    assert(panel._effectParametersWindowLayoutDirty);
    frame(panel,1920,1080,false);
    const auto preference = panel.Saved();
    panel._effectParametersWindowLayoutDirty = false;
    frame(panel,320,240); frame(panel,320,240);
    assert(!panel._effectParametersWindowLayoutDirty);
    near(panel.Saved().width,preference.width); near(panel.Saved().height,preference.height);
    frame(panel,1920,1080);
    near(panel._effectParametersWindowRect.width,650);
    near(panel._effectParametersWindowRect.height,700);
    near(panel._effectParametersWindowRect.x,1200); near(panel._effectParametersWindowRect.y,300);
    frame(panel,1920,1080,true,{-1,-1},{-1,-1},1);
    frame(panel,1920,1080,false);
    assert(!panel._effectParametersWindowLayoutDirty);
    near(panel.Saved().height,700);
    frame(panel,800,600); frame(panel,800,600,false,{-1,-1},{-1,-1},0);
    near(panel.Saved().height,700);
    frame(panel,1920,1080);
    near(panel._effectParametersWindowRect.height,700);
    // A new renderer/session restores the previously saved geometry.
    Panel reopened; reopened.Saved() = panel.Saved();
    frame(reopened,1920,1080);
    near(reopened._effectParametersWindowRect.x,1200); near(reopened._effectParametersWindowRect.width,650);
    // Letterboxing changes the image-relative origin, not saved surface geometry.
    reopened._effectParametersWindowLayoutDirty = false;
    frame(reopened,1920,1080,false,{-1,-1},{-1,-1},-1,{-500,-200});
    near(reopened._effectParametersWindowRect.x,700);
    near(reopened._effectParametersWindowRect.y,100);
    assert(!reopened._effectParametersWindowLayoutDirty);
    frame(reopened,1920,1080);
    near(reopened._effectParametersWindowRect.x,1200);
    near(reopened._effectParametersWindowRect.y,300);
    // DPI changes use logical sizes and logical edge distances.
    reopened._dpiScale = 1.5f; reopened._effectParametersWindowLayoutInitialized = false;
    frame(reopened,2560,1600);
    near(reopened._effectParametersWindowRect.width,975);
    near(reopened._effectParametersWindowRect.height,1050);
    Panel letterboxed; letterboxed.Saved()={0,0,1220,137,1339,1149};
    const auto drawLetterboxed=[&](bool down,ImVec2 mouse){frame(letterboxed,2560,1440,down,{-1,-1},{-1,-1},-1,{-788,-399},mouse,{984,641});};
    drawLetterboxed(false,{1012,-250});drawLetterboxed(false,{1012,-250});
    const auto beforeDrag=letterboxed._effectParametersWindowRect;
    drawLetterboxed(true,{1012,-250});drawLetterboxed(true,{1012,-250});
    drawLetterboxed(true,{212,-150});drawLetterboxed(true,{212,-150});
    drawLetterboxed(false,{212,-150});
    near(letterboxed._effectParametersWindowRect.x,beforeDrag.x-800);
    near(letterboxed._effectParametersWindowRect.y,beforeDrag.y+100);
    std::cout<<"PASS production window layout: title drag across a negative image-relative origin preserves its screen displacement\n";
    ImGui::DestroyContext();
    std::cout << "Overlay layout: legacy defaults, 16 viewport/DPI combinations, anchors, invalid settings, "
        "real ImGui persistence, shrink/grow, collapse, reopen and DPI restoration passed.\n";
}
'''.replace('PREPARE', prepare).replace('RECORD', record)

output = Path(sys.argv[1]).resolve()
output.mkdir(parents=True, exist_ok=True)
(output / 'overlay_window_layout.cpp').write_text(harness, encoding='utf-8')
print(output / 'overlay_window_layout.cpp')
