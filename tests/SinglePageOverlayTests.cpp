#include <imgui.h>
#include <imgui_internal.h>
#include <windows.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <map>
#include <ranges>
#include <set>
#include <string>
#include <variant>
#include <vector>
#define FMT_HEADER_ONLY
#include <fmt/format.h>
#include "EffectParameterRules.h"
#include "EffectParameterPopupLayout.h"
#include "EffectParameterResetGesture.h"
#include "OKLabOverlayEditor.h"
using namespace Magpie;
#include "SinglePageResources.inc"
static std::string language="zh";
static std::string Resource(std::wstring_view key,const char* fallback="") {
    const auto& r=resources.at(language);auto it=r.find(std::wstring(key));
    return it==r.end()?fallback:it->second;
}
namespace EffectParameterLocalization {static std::string Resource(std::wstring_view key,const char* fallback=""){return ::Resource(key,fallback);}}
#include "SinglePageProduction.inc"
#include "SinglePageMetadata.inc"
#include "OverlayGlyphRanges.inc"
#include "OverlayGlyphSamples.inc"
static ImFont* AddUIFont(const char* locale) {
    auto* atlas=ImGui::GetIO().Fonts;
    static constexpr ImWchar primaryRanges[]{0x20,0x7e,0x0394,0x0394,0};
    ImFontConfig config;
    auto* font=atlas->AddFontFromFileTTF("C:/Windows/Fonts/SegUIVar.ttf",18,&config,primaryRanges);
    if(std::string_view(locale)!="en"){
        const bool traditional=std::string_view(locale)=="zh-hant";
        config.MergeMode=true;config.FontNo=1;
        atlas->AddFontFromFileTTF(traditional?"C:/Windows/Fonts/msjh.ttc":"C:/Windows/Fonts/msyh.ttc",18,&config,
            traditional?OverlayHelper::GetGlyphRangesChineseTraditionalOfficial():OverlayHelper::GetGlyphRangesChineseSimplifiedOfficial());
    }
    unsigned char* pixels;int width,height;atlas->GetTexDataAsRGBA32(&pixels,&width,&height);
    return font;
}
static void CheckGlyphCoverage(){
    for(int locale=0;locale<2;++locale){
        ImGui::CreateContext();auto* font=AddUIFont(locale?"zh-hant":"zh");
        for(const wchar_t* p=glyphSamples[locale];*p;++p)if(*p>=0x20 && !font->FindGlyphNoFallback(ImWchar(*p))){
            std::cerr<<"Missing parameter glyph locale="<<locale<<" U+"<<std::hex<<unsigned(*p)<<std::dec<<'\n';std::exit(1);
        }
        ImGui::DestroyContext();
    }
    std::cout<<"PASS production font ranges cover every NR/OKLab zh-Hans/zh-Hant resource character, including thresholds and delta.\n";
}
static std::map<std::string,float> draft;
static float Get(std::string_view n,float fallback){auto it=draft.find(std::string(n));return it==draft.end()?fallback:it->second;}
static bool DrawControl(const EffectParameterDesc& parameter,float& value,const std::string& effectName,size_t parameterIdx) {
    const size_t effectIdx=0;const int description=0;
    const auto getDraftValue=[](size_t,int,std::string_view n,float f){return Get(n,f);};
    const bool parameterValid=true,parameterEnabled=true,forcedFollowMode=false;
    const bool displayOnly=effectName=="DLSSNR\\DLSSNR_AI_Filter"&&IsDLSSNRDisplayParameter(parameter.name);
    const bool isBoolean=IsBooleanEffectParameter(parameter),isChoice=IsChoiceEffectParameter(parameter);
    const auto& label=parameter.label;
    const std::string displayBadge=Resource(L"Overlay_EffectParameters_Live");
    const std::string badge=displayBadge+" · "+Resource(L"Overlay_EffectParameters_NotApplied");
    const float placeholderValue=0,parameterScale=ImGui::GetFontSize()/18.f;
    const bool _parameterFocusSwitchingEnabled=true;
    auto& resetInput=ImGui::GetIO();const auto& style=ImGui::GetStyle();
    struct Input {bool LeftPressHasControl(){return false;} uint64_t LeftPressTimeUs(){return 1;}} _imguiImpl;
    EffectParameterResetGesture _parameterResetGesture;bool resetPressClaimed=false;
    int currentTick=0,maximumTick=1;GetEffectParameterTicks(parameter,value,currentTick,maximumTick);
    (void)forcedFollowMode;(void)badge;(void)resetPressClaimed;
#include "SinglePageWidgets.inc"
}
static void Check(bool value,const char* message){if(!value){std::cerr<<message<<'\n';std::exit(1);}}

static float _fittsLawAdjustment=0;
#include "ParameterTooltip.inc"
using Rects=std::map<std::string,ImRect>;
static Rects Scenario(int section,float width,float height,float zoom,float dpi=1.f) {
    ImGui::CreateContext();auto& io=ImGui::GetIO();io.DisplaySize={2560,1440};io.DeltaTime=1.f/60;io.IniFilename=nullptr;
    AddUIFont(language.c_str());io.FontGlobalScale=dpi;
    const float scale=zoom*dpi;auto& style=ImGui::GetStyle();style.ScaleAllSizes(scale);style.MouseCursorScale=1;
    auto nr=Parameters("DLSSNR\\DLSSNR_AI_Filter"),ok=Parameters("OKLabImageAdjustment");
    draft.clear();for(const auto& p:nr)draft[p.name]=std::visit([](auto c){return float(c.defaultValue);},p.constant);
    draft["multiPass"]=3;draft["residualShowAdvanced"]=draft["performanceShowAdvanced"]=1;
    draft["processingMode"]=1;draft["opticalFlowMethod"]=1;
    OKLabEditorState state;state.section=section;Rects result;std::map<std::string,ImGuiID> ids;
    for(int frame=0;frame<4;++frame){
        ImGui::NewFrame();SetOverlayViewport(RECT{0,0,2560,1440},RECT{0,0,2560,1440});ImGui::SetNextWindowPos({20,20});ImGui::SetNextWindowSize({width,height});
        ImGui::Begin("Parameters",nullptr,ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse);ImGui::SetWindowFontScale(zoom);
        const auto restore=Resource(L"Overlay_EffectParameters_Revert"),apply=Resource(L"Overlay_EffectParameters_ApplyAndRestart");
        const float statusHeight=ImGui::GetTextLineHeight()+style.ItemSpacing.y;
        const ParameterPanelFooterLayout footer(restore.c_str(),apply.c_str(),scale,statusHeight);
        Check(footer.contentHeight>=std::min(120.f*scale,ImGui::GetContentRegionAvail().y)-.6f,"Footer starved the parameter viewport");
        ImGui::BeginChild("content",{0,footer.contentHeight},ImGuiChildFlags_None,ImGuiWindowFlags_AlwaysVerticalScrollbar);
        auto* child=ImGui::GetCurrentWindow();const auto origin=ImGui::GetCursorScreenPos();const float available=ImGui::GetContentRegionAvail().x;
        ParameterPanelLayout layout(available,scale);
        auto record=[&](const std::string& name){layout.End();auto rect=ImRect(ImGui::GetItemRectMin(),ImGui::GetItemRectMax());
            if(rect.Min.x<origin.x-.6f || rect.Max.x>origin.x+available+.6f)std::cerr<<"case "<<language<<" section="<<section<<" width="<<width<<" height="<<height<<" scale="<<scale<<" block="<<name<<" bounds="<<rect.Min.x<<","<<rect.Max.x<<" allowed="<<origin.x<<","<<origin.x+available<<"\n";
            Check(rect.Min.x>=origin.x-.6f && rect.Max.x<=origin.x+available+.6f,"Content escaped horizontally");
            rect.Translate({-origin.x,-origin.y});result[name]=rect;};
        ImGui::PushID("frameRefresh");layout.Section("header");ParameterHeading(Resource(L"FrameRefresh/Header").c_str());record("refresh header");
        for(const auto* key:{L"FrameRefresh_Content/Header",L"FrameRefresh_Pacing/Header",L"FrameRefresh_ContentRate/Header",L"FrameRefresh_Cursor/Header"}){
            auto label=Resource(key);layout.Item(label.c_str());ImGui::TextWrapped("%s",label.c_str());ImGui::SetNextItemWidth(-1);
            if(ImGui::BeginCombo(label.c_str(),"60 FPS"))ImGui::EndCombo();record(label);
        }ImGui::PopID();
        for(int effect=0;effect<2;++effect){
            ImGui::PushID(effect);auto& params=effect==0?nr:ok;const std::string effectName=effect==0?"DLSSNR\\DLSSNR_AI_Filter":"OKLabImageAdjustment";
            const std::string prefix=std::to_string(effect)+":";
            layout.Section("header");ParameterHeading(effectName.c_str());record(prefix+"header");
            if(effect==1){const bool compact=layout.Common("oklabCommon");
                DrawOKLabEditor(state,Get,[](std::string_view,float){},[](std::wstring_view key){return Resource(key);},[&]{
                    record("common");layout.EndCommon();if(state.HasSectionTools())layout.Group("tools");
                },compact);
                if(state.HasSectionTools())record("tools");
            }
            std::string group,block;bool open=false;
            auto close=[&]{if(open){record(prefix+block);open=false;}};
            for(size_t i=0;i<params.size();++i){const auto& param=params[i];
                if(effect==1?!OKLab::Visible(param.name,state.section,state.color,state.slot,state.curve):!IsEffectParameterVisible(effectName,param.name,Get))continue;
                if(group!=param.group){close();group=param.group;layout.Group(group.c_str());ParameterHeading(group.c_str());record(prefix+group);}
                const bool paired=effect==0&&(param.name=="performanceShowAdvanced"||DLSSNRBaseParameter(param.name)=="uiCorrection");
                if(!paired){close();layout.Item(param.name.c_str());open=true;block=param.name;}
                float value=Get(param.name,std::visit([](auto c){return float(c.defaultValue);},param.constant));
                DrawControl(param,value,effectName,i);
                const ImGuiID id=ImGui::GetItemID();const auto key=prefix+param.name;
                if(ids.contains(key))Check(ids[key]==id,"Width layout changed a control ID between frames");ids[key]=id;
            }
            close();ImGui::PopID();
        }
        layout.Finish();
        if(frame>1)Check(child->ScrollMax.x<.6f,"Vertical panel acquired horizontal scrolling");
        if(!footer.scrollWithContent)ImGui::EndChild();
        ImGui::Separator();ParameterButton(restore.c_str(),footer.inlineButtons?footer.revertWidth:footer.width);
        if(footer.inlineButtons)ImGui::SameLine();ParameterButton(apply.c_str(),footer.inlineButtons?footer.applyWidth:footer.width);
        Check(ImGui::GetItemRectMax().x<=ImGui::GetWindowPos().x+ImGui::GetWindowWidth()+.6f,"Footer button escaped horizontally");
        if(!footer.scrollWithContent)Check(ImGui::GetItemRectMax().y<=ImGui::GetWindowPos().y+ImGui::GetWindowHeight()+.6f,"Fixed footer escaped vertically");
        ImGui::Dummy({0,statusHeight});if(footer.scrollWithContent)ImGui::EndChild();
        ImGui::End();ImGui::Render();
    }
    ImGui::DestroyContext();return result;
}
static void PairAndTooltipChecks(){
    ImGui::CreateContext();auto& io=ImGui::GetIO();io.DisplaySize={1200,900};io.DeltaTime=1.f/60;io.IniFilename=nullptr;
    language="en";AddUIFont("en");auto nr=Parameters("DLSSNR\\DLSSNR_AI_Filter");
    size_t left=0,right=0;for(size_t i=0;i<nr.size();++i){if(nr[i].name=="residualShowAdvanced")left=i;if(nr[i].name=="performanceShowAdvanced")right=i;}
    float reference=0;
    for(float scroll:{0.f,80.f,160.f})for(int f=0;f<3;++f){
        ImGui::NewFrame();ImGui::SetNextWindowPos({10,10});ImGui::SetNextWindowSize({1000,500});ImGui::Begin("Pair");
        ImGui::SetNextWindowContentSize({1600,700});ImGui::BeginChild("scroll",{950,400},ImGuiChildFlags_None,ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::SetScrollX(scroll);ImGui::SetCursorPos({300,40});ParameterPanelLayout layout(540,1);layout.Group("pair");float value=0;
        DrawControl(nr[left],value,"DLSSNR\\DLSSNR_AI_Filter",left);const float x=ImGui::GetItemRectMin().x;
        DrawControl(nr[right],value,"DLSSNR\\DLSSNR_AI_Filter",right);const float offset=ImGui::GetItemRectMin().x-x;
        if(f==2){if(scroll==0)reference=offset;Check(std::abs(offset-reference)<.1f,"Paired checkbox offset depends on scroll");}
        layout.Finish();ImGui::EndChild();ImGui::End();ImGui::Render();
    }
    const auto base=ImGui::GetStyle();
    for(float zoom:{.5f,1.f,1.5f,2.f})for(int frame=0;frame<3;++frame){
        ImGui::GetStyle()=base;ImGui::GetStyle().ScaleAllSizes(zoom);ImGui::GetStyle().MouseCursorScale=base.MouseCursorScale;
        ImGui::NewFrame();ImGui::SetNextWindowSize({800,500});ImGui::Begin("Tooltip caller");ImGui::SetWindowFontScale(zoom);
        const float measured=ImGui::GetFontSize();ProductionTooltip("Parameter help text must use the same font size when measuring and drawing, including wrapped lines.",zoom,"An additional explanation.",0);
        const auto* tip=ImGui::FindWindowByName("tooltip");Check(std::abs(tip->CalcFontSize()-measured)<.1f,"Tooltip font differs from its measured font");
        if(frame>0){Check(tip->ScrollMax.x<.6f&&tip->ScrollMax.y<.6f,"Tooltip text is clipped");Check(tip->Pos.x>=0&&tip->Pos.y>=0&&tip->Pos.x+tip->Size.x<=io.DisplaySize.x+.6f&&tip->Pos.y+tip->Size.y<=io.DisplaySize.y+.6f,"Tooltip escaped viewport");}
        ImGui::End();ImGui::Render();
    }
    float zoom=1;ImGui::NewFrame();ImGui::Begin("Zoom");io.KeyCtrl=true;io.MouseWheel=1;
    Check(UpdateEffectParameterZoom(zoom,true)&&zoom>1&&io.MouseWheel==0,"Ctrl wheel is not consumed by zoom");
    ImGui::End();ImGui::Render();ImGui::DestroyContext();
    std::cout<<"PASS production paired-checkbox scroll coordinates, tooltip scales/bounds and Ctrl wheel.\n";
}

static void AnchorAndScrollChecks(){
    ImGui::CreateContext();auto& io=ImGui::GetIO();io.DisplaySize={1400,1000};io.DeltaTime=1.f/60;io.IniFilename=nullptr;AddUIFont("en");
    const auto base=ImGui::GetStyle();float zoom=1,previousZoom=1,value=.5f;ParameterPanelAnchor hovered;
    auto parameter=Parameters("OKLabImageAdjustment").front();parameter.inputStep=.05f;
    ImGuiWindow* child=nullptr;ImRect target;ImGuiID expected=0;
    auto frame=[&](bool initialScroll=false){
        ImGui::GetStyle()=base;ImGui::GetStyle().ScaleAllSizes(zoom);ImGui::NewFrame();
        ImGui::SetNextWindowPos({20,20});ImGui::SetNextWindowSize({800,600});ImGui::Begin("Anchor");ImGui::SetWindowFontScale(zoom);
        ImGui::BeginChild("content",{0,320},ImGuiChildFlags_None,ImGuiWindowFlags_AlwaysVerticalScrollbar);
        child=ImGui::GetCurrentWindow();if(initialScroll)ImGui::SetScrollY(300);
        ParameterPanelLayout layout(ImGui::GetContentRegionAvail().x,zoom,zoom!=previousZoom?hovered:ParameterPanelAnchor{});
        for(int i=0;i<60;++i){ImGui::PushID(i);layout.Item("parameter");ImGui::TextUnformatted("Parameter");ImGui::SetNextItemWidth(-1);int tick=10;
            DrawEffectParameterSlider("##slider",parameter,value,tick,20,"value");
            if(i==20)expected=ImGui::GetID("parameter");layout.End();
            if(i==20)target={ImGui::GetItemRectMin(),ImGui::GetItemRectMax()};ImGui::PopID();
        }
        layout.Finish();hovered=layout.hovered;ImGui::EndChild();ImGui::End();ImGui::Render();previousZoom=zoom;
    };
    frame();frame(true);frame();io.AddMousePosEvent(target.GetCenter().x,target.GetCenter().y);frame();
    Check(hovered.id==expected,"Mouse anchor must select the actual parameter block");
    zoom=2;frame();frame();
    Check(target.Overlaps(child->ClipRect)&&target.Contains(io.MousePos),"Zoom/reflow lost the parameter under the mouse");
    const float before=child->Scroll.y;io.AddMouseWheelEvent(0,-1);frame();frame();
    Check(child->Scroll.y>before&&value==.5f,"Scrolling over a parameter must scroll without changing its value");
    Check(child->ScrollMax.x<.6f,"Zoom introduced horizontal scroll extent");
    ImGui::DestroyContext();std::cout<<"PASS zoom anchor survives two-to-one-column reflow; vertical wheel scroll preserves slider value.\n";
}

static void ColumnCapacityChecks(){
    ImGui::CreateContext();auto& io=ImGui::GetIO();io.DisplaySize={2560,1440};io.DeltaTime=1.f/60;io.IniFilename=nullptr;AddUIFont("en");
    for(auto scenario:std::array<std::array<float,3>,5>{{{500,1,1},{1000,1,3},{1400,1,4},{2000,1,6},{2000,2,3}}}){
        ImGui::NewFrame();ImGui::SetNextWindowSize({2200,900});ImGui::Begin("Column capacity");ImGui::SetWindowFontScale(scenario[1]);
        ParameterPanelLayout layout(scenario[0],scenario[1]);int firstRow=0;float top=0;
        for(int i=0;i<10;++i){ImGui::PushID(i);layout.Item("item");const auto position=ImGui::GetCursorScreenPos();if(i==0)top=position.y;
            if(std::abs(position.y-top)<.1f)++firstRow;
            Check(ImGui::GetContentRegionAvail().x>=280*scenario[1]-.1f || scenario[2]==1,"Multi-column layout made a column narrower than the readable minimum");
            ImGui::TextUnformatted("Parameter");layout.End();ImGui::PopID();
        }
        Check(firstRow==int(scenario[2]),"Wide panel did not expose the expected additional columns");
        layout.Finish();ImGui::End();ImGui::Render();
    }
    ImGui::DestroyContext();std::cout<<"PASS actual row capacities: 1, 3, 4, 6 columns, reduced to 3 at 200% zoom.\n";
}
int main(){
    CheckGlyphCoverage();PairAndTooltipChecks();AnchorAndScrollChecks();ColumnCapacityChecks();int cases=0;
    for(const char* lang:{"zh","en"}){language=lang;
        for(int section=0;section<14;++section)for(auto viewport:std::array<std::array<float,3>,6>{{{360,1,1},{720,1,1},{1100,1,1},{360,2,1},{1100,.5f,1},{1600,1,1.5f}}}){
            auto shortView=Scenario(section,viewport[0],420,viewport[1],viewport[2]);
            auto tallView=Scenario(section,viewport[0],900,viewport[1],viewport[2]);
            Check(shortView.size()==tallView.size(),"Height changed visible controls");
            for(const auto& [name,rect]:shortView){const auto& other=tallView.at(name);Check(std::abs(rect.Min.x-other.Min.x)<.6f&&std::abs(rect.Min.y-other.Min.y)<.6f,"Height changed parameter placement");}
            ++cases;
        }
    }
    std::cout<<"PASS "<<cases<<" width/zoom/language/section pairs at two heights; real metadata, no horizontal overflow, stable IDs and height-independent positions.\n";
}
