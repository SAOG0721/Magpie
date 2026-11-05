#include "OKLabOverlayEditor.h"
#include "LocalizedStringCache.h"
#include "../src/Magpie/EffectHelper.h"
#include <iostream>
#include <map>
#include <cstdlib>
using namespace Magpie;
void Require(bool value,const char* message){if(!value){std::cerr<<message<<'\n';std::exit(1);}}
int main(){
    LocalizedStringCache labels;
    size_t loads=0;
    auto load=[&](std::wstring_view key){++loads;std::string value;for(auto c:key)value.push_back(char(c));return value;};
    const std::wstring original=L"EffectParam_OKLabImageAdjustment_Group_Basic";
    std::wstring transient=original;
    const std::string& firstCached=labels.Get(transient,load);
    const auto* firstAddress=&firstCached;
    transient.assign(transient.size(),L'X');
    transient.clear();transient.shrink_to_fit();
    Require(labels.Get(std::wstring(original),load)=="EffectParam_OKLabImageAdjustment_Group_Basic"&&loads==1,
        "Resource cache must own temporary keys after source reuse/destruction");
    for(int i=0;i<4096;++i)labels.Get(L"temporary_resource_key_"+std::to_wstring(i),load);
    Require(&labels.Get(original,load)==firstAddress&&firstCached=="EffectParam_OKLabImageAdjustment_Group_Basic",
        "Resource text references must survive cache growth");
    Require(EffectHelper::GetDisplayName(L"OKLabImageAdjustment")==L"OKLab Image Adjustment",
        "Settings must show the readable OKLab name without changing the saved ID");
    ImGui::CreateContext();auto& io=ImGui::GetIO();io.DisplaySize={800,1000};io.DeltaTime=1.f/60;io.IniFilename=nullptr;
    unsigned char* pixels;int width,height;io.Fonts->GetTexDataAsRGBA32(&pixels,&width,&height);
    float hue=47,chroma=0;bool activated=false,changed=false;ImVec2 origin;
    auto frame=[&](ImVec2 mouse,bool down,auto draw){
        io.AddMousePosEvent(mouse.x,mouse.y);io.AddMouseButtonEvent(0,down);
        ImGui::NewFrame();ImGui::SetNextWindowPos({0,0});ImGui::SetNextWindowSize({500,900});
        ImGui::Begin("Editor",nullptr,ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize);
        origin=ImGui::GetCursorScreenPos();draw();ImGui::End();ImGui::Render();
        Require(ImGui::GetDrawData()->TotalVtxCount>0,"Editor produced no render geometry");
    };
    auto wheel=[&]{changed=DrawOKLabWheel(hue,chroma,activated);};
    frame({-10,-10},false,wheel);const ImVec2 center{origin.x+90,origin.y+90};
    frame({center.x+45,center.y},false,wheel);frame({center.x+45,center.y},true,wheel);
    Require(activated&&changed&&std::abs(hue)<.001f&&std::abs(chroma-.05f)<.001f,"Wheel click did not edit real hue/chroma");
    frame({center.x,center.y-200},true,wheel);
    Require(std::abs(hue-90)<.001f&&std::abs(chroma-.1f)<.001f,"Captured drag must clamp radius outside the circle");
    frame(center,true,wheel);Require(chroma==0&&std::abs(hue-90)<.001f,"Wheel center must retain hue while zeroing chroma");
    frame(center,false,wheel);frame({center.x+60,center.y},false,wheel);Require(chroma==0,"Released wheel changed from hover");
    OKLabEditorState state;std::map<std::string,float> values;
    auto get=[&](std::string_view key,float fallback){auto it=values.find(std::string(key));return it==values.end()?fallback:it->second;};
    auto set=[&](std::string_view key,float value){values[std::string(key)]=value;};
    auto label=[&](std::wstring_view key)->const std::string& {return labels.Get(key,load);};
    ImVec2 sectionOrigin;
    auto editor=[&]{DrawOKLabEditor(state,get,set,label,[&]{sectionOrigin=ImGui::GetCursorScreenPos();});};
    for(int repeat=0;repeat<3;++repeat)for(int section=0;section<14;++section){
        state.section=section;
        frame({-10,-10},false,editor);
        frame({-10,-10},false,editor);
    }
    Require(values.empty(),"Changing editor section must not modify image parameters");
    state.section=8;state.curve=1;frame({-10,-10},false,editor);
    const float curveY=sectionOrigin.y+ImGui::GetTextLineHeightWithSpacing()+ImGui::GetFrameHeightWithSpacing();
    ImVec2 first{origin.x+1,curveY+80*ImGui::GetFontSize()/18.f};frame(first,false,editor);frame(first,true,editor);
    frame({origin.x+10,curveY+40*ImGui::GetFontSize()/18.f},true,editor);frame({origin.x+10,curveY+40*ImGui::GetFontSize()/18.f},false,editor);
    Require(get("curve1Enabled",0)==1&&get("curve1x0",-1)==0,"Curve drag must enable the selected curve and retain the endpoint");
    Require(std::abs(get("curve1y0",0)-.5f)<.02f&&get("curve1y0",0)==get("curve1y7",1),"Hue curve endpoints must stay periodic while dragging");
    Require(state.undo.size()==std::size(OKLab::Parameters)&&state.drag==-1,"Curve drag must keep undo and release capture state");
    for(int preset=0;preset<6;++preset){values["curve5Enabled"]=1;OKLab::Preset(preset,set);Require(values.size()==std::size(OKLab::Parameters)&&values["curve5Enabled"]==0,"Preset must replace hidden parameter values");}
    ImGui::DestroyContext();std::cout<<"PASS owning resource keys/stable references, readable OKLab name, repeated ImGui section frames, wheel/curve/undo and 6 presets\n";
}
