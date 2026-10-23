#include "../src/Magpie.Core/ProfilerHistory.h"
#include "../src/Magpie.Core/PresentationFrameRate.h"
#include "../src/Magpie.Core/include/OverlayWindowGeometry.h"
#include <imgui.h>
#include <imgui_internal.h>
#define FMT_HEADER_ONLY
#include <fmt/format.h>
#include <cassert>
#include <cstdio>
#include <limits>
#include <map>
#include <string>
#include <vector>
#include <ranges>
#include <unordered_map>
#include "ProfilerUiResources.inc"
using namespace std::chrono;
using namespace Magpie;

struct EffectsProfilerSnapshot { ProfilerHistory history; bool gpuAvailable = true; };
struct MockRenderer {
    PresentationProfilerSnapshot output;
    int reads=0;
    PresentationProfilerSnapshot PresentationProfile(ProfilerHistory::Time) {++reads;return output;}
};
struct ScalingWindow {
    MockRenderer renderer;
    static ScalingWindow& Get() {static ScalingWindow s;return s;}
    MockRenderer& Renderer() {return renderer;}
};
struct Options { std::unordered_map<std::string, OverlayWindowOption> windows{{"profiler", {2,1,60,0.5f}}}; };
struct Input { bool canceled = false; bool FrameInputCanceled() const { return canceled; } };
struct StrHelper { template<class... T> static std::string Concat(T&&... t) { std::string s; (s.append(t),...); return s; } };
static const char* PROFILER_WINDOW_ID = "profiler";

class OverlayDrawer {
public:
    void _DrawProfilerGraph(const ProfilerHistory::Plot&, int, bool) noexcept;
    bool _BeginProfilerWindow() noexcept;
    bool _DrawProfilerGraphs(const EffectsProfilerSnapshot&) noexcept;
    bool Developer(bool, bool, bool&);
    const std::string& _GetResourceString(const wchar_t* key) { return resources[language].at(key); }
    float _dpiScale = 1;
    std::array<float,3> _profilerGraphMax{};
    PresentationProfilerSnapshot _profilerOutputProfile{};
    Options testOptions;
    Options* _overlayOptions = &testOptions;
    Input _imguiImpl;
    bool _isProfilerVisible = true;
    bool _profilerWindowLayoutInitialized = false, _profilerWindowLayoutDirty = false;
    ImVec2 _profilerViewport{};
    OverlayWindowRect _profilerWindowRect{};
    int graphDrawCalls = 0;
    ImRect graphHeader;
    bool graphToggled = false;
    int language = 0;
};
#include "ProfilerUiProduction.inc"

static void Frame() {
    auto& io = ImGui::GetIO();
    io.DisplaySize = {1600,1200};
    io.DeltaTime = 0.5f;
    ImGui::NewFrame();
}

static void Near(float actual, float expected) { assert(std::abs(actual-expected)<1.1f); }

static void PanelFrame(OverlayDrawer& ui, const EffectsProfilerSnapshot& profile,
    ImVec2 viewport, ImVec2 mouse = {-100,-100}, bool down = false, int collapse = -1) {
    auto& io = ImGui::GetIO();
    io.DisplaySize = viewport; io.DeltaTime = 1.0f / 60;
    io.AddMousePosEvent(mouse.x,mouse.y); io.AddMouseButtonEvent(0,down);
    ImGui::NewFrame();
    if(collapse >= 0) ImGui::SetNextWindowCollapsed(collapse!=0);
    if(ui._BeginProfilerWindow()) ui.graphToggled=ui._DrawProfilerGraphs(profile);
    const auto* window=ImGui::GetCurrentWindow();
    assert(!(window->Flags & (ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoResize)));
    // ScrollMax uses last frame's content size when a scrollbar first appears.
    // Measure this frame's real layout extent instead.
    const float overflow=window->DC.CursorMaxPos.x-window->WorkRect.Max.x;
    if(overflow>1.0f) std::fprintf(stderr,
        "Overflow language=%d dpi=%.2f viewport=%.0fx%.0f window=%.0fx%.0f content=%.1f overflow=%.1f graphs=%d\n",
        ui.language,ui._dpiScale,viewport.x,viewport.y,window->Size.x,window->Size.y,
        window->ContentSize.x,overflow,ui.graphDrawCalls);
    assert(overflow<=1.0f);
    ImGui::End(); ImGui::Render();
    const auto& r=ui._profilerWindowRect;
    assert(r.x>=0 && r.y>=0 && r.width>0 && r.height>0);
    assert(r.x+r.width<=viewport.x+1 && r.y+r.height<=viewport.y+1);
    for(const auto* list : ImGui::GetDrawData()->CmdLists) for(const auto& v : list->VtxBuffer)
        assert(std::isfinite(v.pos.x) && std::isfinite(v.pos.y));
}

static void CheckPanel(OverlayDrawer& ui) {
    const float dpi=ui._dpiScale;
    const ImVec2 viewport{1600*dpi,1200*dpi};
    EffectsProfilerSnapshot profile;
    const auto start=steady_clock::now()-10s;
    profile.history.Reset(start);
    for(int i=0;i<600;++i) {
        const auto sample=start+microseconds(i*16667);
        profile.history.Frame(sample); profile.history.Effects(sample,i==580 ? 28.0 : 2.0);
    }
    profile.history.Observe(steady_clock::now());
    auto& renderer=ScalingWindow::Get().Renderer();
    renderer.reads=0;
    ProfilerHistory output;output.Reset(start);
    for(int i=0;i<1200;++i) output.Frame(start+microseconds(i*8333));
    output.Observe(steady_clock::now());
    renderer.output={output.Read(steady_clock::now()),ContentFrameCountSource::Reported,true,true};
    ui.graphDrawCalls=0;
    PanelFrame(ui,profile,viewport); PanelFrame(ui,profile,viewport);
    assert(ui.graphDrawCalls==0); // Opening the profiler must not draw any graph.
    assert(renderer.reads==0); // Folded graphs must not even build an output plot.
    Near(ui._profilerWindowRect.width,310*dpi); Near(ui._profilerWindowRect.height,360*dpi);
    assert(!ui._profilerWindowLayoutDirty);

    auto click=[&](ImVec2 point) {
        PanelFrame(ui,profile,viewport,point);
        PanelFrame(ui,profile,viewport,point,true);
        PanelFrame(ui,profile,viewport,point,false);
    };
    click(ui.graphHeader.GetCenter());
    assert(ui.graphToggled && ui.graphDrawCalls==3);
    assert(renderer.reads==1);
    Near(ui._profilerWindowRect.height,360*dpi); // Expand uses scroll, not auto-resize.
    PanelFrame(ui,profile,viewport);
    assert(ui.graphDrawCalls==6);
    click(ui.graphHeader.GetCenter());
    assert(ui.graphToggled);
    const int before=ui.graphDrawCalls;
    const int reads=renderer.reads;
    PanelFrame(ui,profile,viewport);
    assert(ui.graphDrawCalls==before);
    assert(renderer.reads==reads);

    // Exercise ImGui's actual bottom-right grip, not an injected window size.
    const auto original=ui._profilerWindowRect;
    const ImVec2 grip{original.x+original.width-3*dpi,original.y+original.height-3*dpi};
    const ImVec2 enlarged{grip.x+140*dpi,grip.y+120*dpi};
    PanelFrame(ui,profile,viewport,grip);
    PanelFrame(ui,profile,viewport,grip,true);
    PanelFrame(ui,profile,viewport,enlarged,true);
    PanelFrame(ui,profile,viewport,enlarged,false);
    assert(ui._profilerWindowLayoutDirty);
    auto& saved=ui.testOptions.windows.at("profiler");
    assert(saved.width>400 && saved.height>450);
    const auto preference=saved;
    ui._profilerWindowLayoutDirty=false;
    const auto resized=ui._profilerWindowRect;
    // SDK batch counts may show output FPS, but must explicitly lack intervals.
    renderer.output.source=ContentFrameCountSource::Submitted;
    renderer.output.individualTimestamps=false;
    for(auto& point:renderer.output.plot.points) point.interval={};
    click(ui.graphHeader.GetCenter());
    Near(ui._profilerWindowRect.width,resized.width); Near(ui._profilerWindowRect.height,resized.height);
    assert(!ui._profilerWindowLayoutDirty);
    PanelFrame(ui,profile,viewport,{-100,-100},false,1);
    Near(saved.height,preference.height);
    PanelFrame(ui,profile,viewport,{-100,-100},false,0);

    // Temporary constraints preserve preferred size and anchoring.
    PanelFrame(ui,profile,{280*dpi,200*dpi}); PanelFrame(ui,profile,{280*dpi,200*dpi});
    assert(!ui._profilerWindowLayoutDirty);
    Near(saved.width,preference.width); Near(saved.height,preference.height);
    PanelFrame(ui,profile,viewport); PanelFrame(ui,profile,viewport);
    Near(ui._profilerWindowRect.width,resized.width); Near(ui._profilerWindowRect.height,resized.height);
    Near(ui._profilerWindowRect.x,resized.x); Near(ui._profilerWindowRect.y,resized.y);

    // Reopening restores user size; a DPI change scales DIPs, not saved pixels.
    ui._profilerWindowLayoutInitialized=false;
    PanelFrame(ui,profile,viewport);
    Near(ui._profilerWindowRect.width,preference.width*dpi);
    ui._dpiScale=dpi*1.25f; ui._profilerWindowLayoutInitialized=false;
    PanelFrame(ui,profile,{2000*dpi,1500*dpi});
    Near(ui._profilerWindowRect.width,preference.width*ui._dpiScale);
    Near(ui._profilerWindowRect.height,preference.height*ui._dpiScale);
    assert(!ui._profilerWindowLayoutDirty);
}

int main() try {
    int cases=0;
    for (float dpi : {1.0f,1.5f,2.0f}) for (int language : {0,1,2}) for (bool dark : {false,true}) {
        ImGui::CreateContext();
        auto& io=ImGui::GetIO(); io.IniFilename=nullptr;
        ImFontGlyphRangesBuilder ranges;
        ranges.AddRanges(io.Fonts->GetGlyphRangesDefault());
        for(const auto& [key,value] : resources[language]) ranges.AddText(value.c_str());
        ImVector<ImWchar> glyphs; ranges.BuildRanges(&glyphs);
        assert(io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/msyh.ttc",18*dpi,nullptr,glyphs.Data));
        unsigned char* pixels; int w,h; io.Fonts->GetTexDataAsRGBA32(&pixels,&w,&h);
        io.Fonts->SetTexID((ImTextureID)1);
        if(dark) ImGui::StyleColorsDark(); else ImGui::StyleColorsLight();
        ImGui::GetStyle().ScaleAllSizes(dpi);
        OverlayDrawer ui; ui._dpiScale=dpi; ui.language=language;
        const ProfilerHistory::Time start(100s);
        ProfilerHistory output;output.Reset(start);
        for(int i=0;i<1200;++i) output.Frame(start+microseconds(i*8333));
        output.Observe(start+10s);
        ui._profilerOutputProfile={output.Read(start+10s),ContentFrameCountSource::Reported,true,true};
        ProfilerHistory history; history.Reset(start);
        for(int i=0;i<600;++i){
            auto t=start+microseconds(i*16667);
            history.Frame(t); history.Effects(t,i==580 ? 28.0 : 2.0);
        }
        history.Observe(start+10s);
        const auto plot=history.Read(start+10s);
        for(int frame=0;frame<3;++frame){
            Frame();
            ImGui::SetNextWindowSize({310*dpi,500*dpi});
            ImGui::SetNextWindowPos({20,20});
            ImGui::Begin("profiler-test",nullptr,ImGuiWindowFlags_NoSavedSettings);
            const float begin=ImGui::GetCursorPosY();
            bool passes=true;
            ui.Developer(true,false,passes);
            assert(!passes && ImGui::GetCursorPosY()==begin);
            ui.Developer(false,true,passes);
            assert(!passes && ImGui::GetCursorPosY()==begin);
            ImGui::SetNextItemOpen(false,ImGuiCond_Always);
            ui.Developer(true,true,passes);
            assert(ImGui::GetCursorPosY()>begin);
            passes=true;
            const auto after=ImGui::GetCursorPosY();
            ui.Developer(true,false,passes);
            assert(!passes && ImGui::GetCursorPosY()==after);
            for(int metric=0;metric<3;++metric){
                const float before=ImGui::GetCursorPosY();
                ui._DrawProfilerGraph(plot,metric,true);
                assert(ImGui::GetCursorPosY()>before+50*dpi);
            }
            // Content is allowed to scroll vertically, never horizontally.
            const auto* window=ImGui::GetCurrentWindow();
            // The first vertical scrollbar uses last frame's wider content.
            // Check actual layout immediately, then stabilized scroll bounds.
            assert(window->DC.CursorMaxPos.x <= window->WorkRect.Max.x+1.0f);
            if(frame>1) assert(window->ScrollMax.x < 1.0f);
            ImGui::End(); ImGui::Render();
            auto* data=ImGui::GetDrawData(); assert(data && data->TotalVtxCount>0);
            for(const auto* list : data->CmdLists) for(const auto& v : list->VtxBuffer)
                assert(std::isfinite(v.pos.x) && std::isfinite(v.pos.y));
        }
        CheckPanel(ui);
        ImGui::DestroyContext(); ++cases;
    }
    std::printf("Production ImGui profiler: %d language/theme/DPI layouts; developer state, graphs folded/open/closed, real grip resize, persistence, viewport and DPI restoration passed.\n",cases);
} catch(const std::exception& error) {
    std::fprintf(stderr,"Profiler UI test failed: %s\n",error.what());
    return 1;
}
