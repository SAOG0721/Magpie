#include "../src/Magpie.Core/ProfilerHistory.h"
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
#include "ProfilerUiResources.inc"
using namespace std::chrono;
using namespace Magpie;

class OverlayDrawer {
public:
    void _DrawProfilerGraph(const ProfilerHistory::Plot&, int, bool) noexcept;
    bool Developer(bool, bool, bool&);
    const std::string& _GetResourceString(const wchar_t* key) { return resources[language].at(key); }
    float _dpiScale = 1;
    std::array<float,3> _profilerGraphMax{};
    struct _ProfilerOutputSample { ProfilerHistory::Time begin{},end{}; float fps=0; bool known=false; };
    std::array<_ProfilerOutputSample,12> _profilerOutputs{};
    ProfilerHistory::Time _profilerStartedAt{};
    bool _profilerHasOutput=true, _profilerOutputSubmitted=false;
    int language = 0;
};
#include "ProfilerUiProduction.inc"

static void Frame() {
    auto& io = ImGui::GetIO();
    io.DisplaySize = {1600,1200};
    io.DeltaTime = 0.5f;
    ImGui::NewFrame();
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
        assert(io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/msyh.ttc",14*dpi,nullptr,glyphs.Data));
        unsigned char* pixels; int w,h; io.Fonts->GetTexDataAsRGBA32(&pixels,&w,&h);
        io.Fonts->SetTexID((ImTextureID)1);
        if(dark) ImGui::StyleColorsDark(); else ImGui::StyleColorsLight();
        ImGui::GetStyle().ScaleAllSizes(dpi);
        OverlayDrawer ui; ui._dpiScale=dpi; ui.language=language;
        const ProfilerHistory::Time start(100s);
        ui._profilerStartedAt=start;
        ui._profilerOutputs[0]={start+9s,start+10s,120,true};
        ui._profilerOutputs[1]={start+8s,start+9s,240,false};
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
            if(frame>0) assert(window->ScrollMax.x < 1.0f);
            ImGui::End(); ImGui::Render();
            auto* data=ImGui::GetDrawData(); assert(data && data->TotalVtxCount>0);
            for(const auto* list : data->CmdLists) for(const auto& v : list->VtxBuffer)
                assert(std::isfinite(v.pos.x) && std::isfinite(v.pos.y));
        }
        ImGui::DestroyContext(); ++cases;
    }
    std::printf("Production ImGui profiler: %d language/theme/DPI layouts; empty/disabled/collapsed developer state and graph geometry passed.\n",cases);
} catch(const std::exception& error) {
    std::fprintf(stderr,"Profiler UI test failed: %s\n",error.what());
    return 1;
}
