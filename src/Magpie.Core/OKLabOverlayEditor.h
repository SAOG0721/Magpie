#pragma once
#include "OKLabParameters.h"
#include "OKLabColor.h"
#include "ParameterPanelLayout.h"
#include <memory>
#include <imgui.h>
#include <limits>

namespace Magpie {
struct OKLabEditorState {
    int section=0,color=0,slot=0,curve=0,wheel=0,preset=0,drag=-1;
    std::vector<float> a,undo;
    bool picking=false;
    int pickSlot=0;
    std::shared_ptr<OKLab::PickResult> pickResult;
    bool HasSectionTools() const noexcept { return section==3 || section==4 || section==5 || section==8 || section==13; }
};
// Render the actual Oklab a/b plane. The cached mesh is independent of DPI,
// and pointer/keyboard numeric controls share the same hue/chroma values.
inline bool DrawOKLabWheel(float& hue,float& chroma,bool& activated) {
    const auto origin=ImGui::GetCursorScreenPos();
    const float diameter=std::min(180.f*ImGui::GetFontSize()/13.f,ImGui::GetContentRegionAvail().x),radius=diameter*.5f;
    ImGui::InvisibleButton("##oklabWheel",{diameter,diameter});
    auto* draw=ImGui::GetWindowDrawList();
    struct Vertex {ImVec2 pos;ImU32 color;};
    static const auto vertices=[](){
        std::vector<Vertex> result;
        auto vertex=[](int ring,int sector){float angle=sector*6.283185307f/96,r=ring/12.f;float x=r*std::cos(angle),y=r*std::sin(angle);auto c=OKLab::PreviewColor(.1f*x,.1f*y);return Vertex{{x,-y},ImGui::ColorConvertFloat4ToU32({c[0],c[1],c[2],1})};};
        for(int r=0;r<12;++r)for(int s=0;s<96;++s){
            auto a=vertex(r,s),b=vertex(r+1,s),c=vertex(r+1,s+1),d=vertex(r,s+1);
            result.insert(result.end(),{a,b,c,a,c,d});
        }return result;
    }();
    const auto uv=ImGui::GetFontTexUvWhitePixel();
    draw->PrimReserve(int(vertices.size()),int(vertices.size()));
    for(const auto& v:vertices){draw->PrimWriteIdx(ImDrawIdx(draw->_VtxCurrentIdx));draw->PrimWriteVtx({origin.x+radius*(1+v.pos.x),origin.y+radius*(1+v.pos.y)},uv,v.color);}
    const float angle=hue*.01745329252f,length=std::clamp(chroma/.1f,0.f,1.f)*radius;
    ImVec2 marker{origin.x+radius+length*std::cos(angle),origin.y+radius-length*std::sin(angle)};
    draw->AddCircle(marker,5,IM_COL32(0,0,0,255),16,3);draw->AddCircle(marker,5,IM_COL32(255,255,255,255),16,1);
    activated=ImGui::IsItemActivated();
    if(!ImGui::IsItemActive())return false;
    const auto mouse=ImGui::GetIO().MousePos;
    auto value=OKLab::WheelValue((mouse.x-origin.x-radius)/radius,-(mouse.y-origin.y-radius)/radius,hue);
    const bool changed=value[0]!=hue||value[1]!=chroma;hue=value[0];chroma=value[1];return changed;
}
// All writes go through the caller's existing parameter mailbox / save path.
// These editor selections are session state and never shader parameters.
template<class Get,class Set,class Label,class BeginSection>
bool DrawOKLabEditor(OKLabEditorState& state,Get&& get,Set&& set,Label&& label,BeginSection&& beginSection,bool compact=false) {
    bool ui=false;
    auto combo=[&](const char* id,int& index,int count,auto text) {
        ImGui::TextUnformatted(id);
        ImGui::SetNextItemWidth(-1.f);
        const std::string controlId=std::string("##")+id;
        if(ImGui::BeginCombo(controlId.c_str(),text(index).c_str())) {
            for(int i=0;i<count;++i)if(ImGui::Selectable(text(i).c_str(),i==index)){index=i;ui=true;}
            ImGui::EndCombo();
        }
    };
    combo(label(L"OKLab_Section").c_str(),state.section,14,[&](int i){return label(L"EffectParam_OKLabImageAdjustment_Group_"+std::wstring(OKLab::Sections[i].begin(),OKLab::Sections[i].end()));});
    auto snapshot=[&](){std::vector<float> values;for(const auto&p:OKLab::Parameters)values.push_back(get(p.name,p.value));return values;};
    auto apply=[&](const std::vector<float>& values){if(values.size()!=std::size(OKLab::Parameters))return;for(size_t i=0;i<values.size();++i)set(OKLab::Parameters[i].name,values[i]);};
    if(!compact){ImGui::Spacing();ImGui::Separator();ImGui::Spacing();}
    combo(label(L"OKLab_Preset").c_str(),state.preset,6,[&](int i){return label(L"OKLab_Preset_"+std::to_wstring(i));});
    if(ParameterButton(label(L"OKLab_ApplyPreset").c_str(),ImGui::GetContentRegionAvail().x)){state.undo=snapshot();OKLab::Preset(state.preset,set);ui=true;}
    if(!compact){ImGui::Spacing();ImGui::Separator();ImGui::Spacing();}
    const float available=ImGui::GetContentRegionAvail().x;
    const float half=std::max(1.f,(available-ImGui::GetStyle().ItemSpacing.x)*.5f);
    const float pairMinimum=std::max({ImGui::CalcTextSize(label(L"OKLab_ResetGroup").c_str()).x,
        ImGui::CalcTextSize(label(L"OKLab_ResetAll").c_str()).x,ImGui::CalcTextSize(label(L"OKLab_StoreA").c_str()).x,
        ImGui::CalcTextSize(label(L"OKLab_SwapAB").c_str()).x})+ImGui::GetStyle().FramePadding.x*2;
    const bool pairs=half>=pairMinimum;
    const float buttonWidth=pairs?half:available;
    if(ParameterButton(label(L"OKLab_ResetGroup").c_str(),buttonWidth)){state.undo=snapshot();OKLab::Defaults(set,OKLab::Sections[state.section]);ui=true;}
    if(pairs)ImGui::SameLine();
    if(ParameterButton(label(L"OKLab_ResetAll").c_str(),buttonWidth)){state.undo=snapshot();OKLab::Defaults(set);ui=true;}
    ImGui::Spacing();
    if(ParameterButton(label(L"OKLab_StoreA").c_str(),buttonWidth)){state.a=snapshot();ui=true;}
    if(pairs)ImGui::SameLine();
    if(ParameterButton(label(L"OKLab_SwapAB").c_str(),buttonWidth)){auto current=snapshot();if(!state.a.empty()){state.undo=current;apply(state.a);}state.a=std::move(current);ui=true;}
    if(ParameterButton(label(L"OKLab_Undo").c_str(),available)){auto current=snapshot();apply(state.undo);state.undo=std::move(current);ui=true;}
    // The caller selects a right-hand content region or a new row. Keep
    // shared controls independent of the selected section's tools and height.
    beginSection();
    if(state.section==3)combo(label(L"OKLab_Color").c_str(),state.color,8,[&](int i){return label(L"OKLab_Color_"+std::to_wstring(i));});
    if(state.section==5)combo(label(L"OKLab_Slot").c_str(),state.slot,2,[](int i){return std::string(i?"B":"A");});
    if(state.section==4){
        combo(label(L"OKLab_Wheel").c_str(),state.wheel,3,[&](int i){return label(L"OKLab_Wheel_"+std::to_wstring(i));});
        const std::string prefix="wheel"+std::to_string(state.wheel);
        float hue=get(prefix+"Hue",0.f),chroma=get(prefix+"Chroma",0.f);bool activated=false;
        const bool changed=DrawOKLabWheel(hue,chroma,activated);
        if(activated)state.undo=snapshot();
        if(changed){set(prefix+"Hue",hue);set(prefix+"Chroma",chroma);ui=true;}
        ImGui::TextWrapped("%s",label(L"OKLab_WheelHelp").c_str());
    }
    if(state.section==8) {
        combo(label(L"OKLab_Curve").c_str(),state.curve,6,[&](int i){return label(L"OKLab_Curve_"+std::to_wstring(i));});
        const auto origin=ImGui::GetCursorScreenPos();const ImVec2 size(std::min(300.f*ImGui::GetFontSize()/18.f,ImGui::GetContentRegionAvail().x),160.f*ImGui::GetFontSize()/18.f);
        ImGui::InvisibleButton("##oklabCurve",size);
        auto* draw=ImGui::GetWindowDrawList();draw->AddRectFilled(origin,{origin.x+size.x,origin.y+size.y},IM_COL32(35,38,44,255),4);
        const std::string prefix="curve"+std::to_string(state.curve);
        std::array<float,8> xs{},ys{};std::array<ImVec2,8> points{};
        for(int k=0;k<8;++k){
            xs[k]=k==0?0:k==7?1:std::clamp(get(prefix+"x"+std::to_string(k),k/7.f),xs[k-1]+.001f,1-(7-k)*.001f);
            ys[k]=get(prefix+"y"+std::to_string(k),state.curve==0?k/7.f:0.f);
            if(state.curve==0)ys[k]=k==0?0:k==7?1:std::clamp(ys[k],ys[k-1],1.f);
            if(state.curve>=1&&state.curve<=3&&k==7)ys[k]=ys[0];
            points[k]={origin.x+xs[k]*size.x,origin.y+(1-(state.curve==0?ys[k]:(ys[k]+1)*.5f))*size.y};
            if(k)draw->AddLine(points[k-1],points[k],IM_COL32(90,180,255,255),2);
            draw->AddCircleFilled(points[k],4,IM_COL32(180,220,255,255));
        }
        if(ImGui::IsItemActivated()) {
            state.undo=snapshot();const auto mouse=ImGui::GetIO().MousePos;float best=std::numeric_limits<float>::max();
            for(int k=0;k<8;++k){float dx=mouse.x-points[k].x,dy=mouse.y-points[k].y;if(dx*dx+dy*dy<best){best=dx*dx+dy*dy;state.drag=k;}}
        }
        if(ImGui::IsItemActive()&&state.drag>=0) {
            const int k=state.drag;const auto mouse=ImGui::GetIO().MousePos;
            float x=std::clamp((mouse.x-origin.x)/size.x,0.f,1.f),y=std::clamp(1-(mouse.y-origin.y)/size.y,0.f,1.f);
            if(state.curve!=0)y=y*2-1;
            x=k==0?0:k==7?1:std::clamp(x,xs[k-1]+.001f,xs[k+1]-.001f);
            if(state.curve==0)y=k==0?0:k==7?1:std::clamp(y,ys[k-1],ys[k+1]);
            set(prefix+"Enabled",1.f);set(prefix+"x"+std::to_string(k),x);set(prefix+"y"+std::to_string(k),y);
            if(state.curve>=1&&state.curve<=3&&(k==0||k==7)){set(prefix+"y0",y);set(prefix+"y7",y);}ui=true;
        }
        if(ImGui::IsItemDeactivated())state.drag=-1;
        ImGui::TextWrapped("%s",label(L"OKLab_CurveHelp").c_str());
    }
    if(state.section==13)ImGui::TextWrapped("%s",label(L"OKLab_LutRestart").c_str());
    return ui;
}
}
