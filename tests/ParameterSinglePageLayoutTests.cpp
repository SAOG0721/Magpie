// Unattached WinUI measurement: production controls, panels and visibility rules.
#include "pch.h"
#include "SimpleStackPanel.h"
#include "WrapPanel.h"
#include "EffectParameterPopupLayout.h"
#include "ParameterItems.h"
#include "EffectParameterRules.h"
#include "OKLabParameters.h"
#include <winrt/Windows.UI.Xaml.Hosting.h>
#include <array>
#include <cassert>
#include <iostream>
#include <map>
#include <set>
using namespace winrt;
using namespace ::Magpie;
struct LayoutTestApp : ApplicationT<LayoutTestApp, Markup::IXamlMetadataProvider> {
    winrt::Microsoft::UI::Xaml::XamlTypeInfo::XamlControlsXamlMetaDataProvider provider{nullptr};
    auto& Provider() { if(!provider)provider=winrt::Microsoft::UI::Xaml::XamlTypeInfo::XamlControlsXamlMetaDataProvider();return provider; }
    Markup::IXamlType GetXamlType(Interop::TypeName const& type){return Provider().GetXamlType(type);}
    Markup::IXamlType GetXamlType(hstring const& type){return Provider().GetXamlType(type);}
    com_array<Markup::XmlnsDefinition> GetXmlnsDefinitions(){return Provider().GetXmlnsDefinitions();}
};
#include "ParameterSinglePageProduction.inc"
auto Stack(Orientation orientation=Orientation::Vertical,double spacing=0) {
    auto panel=make_self<winrt::Magpie::implementation::SimpleStackPanel>();
    panel->Orientation(orientation);panel->Spacing(spacing);return panel;
}
// XamlReader has no application metadata provider for local classes. Replace
// marked stand-ins with the actual production panel before measuring anything.
UIElement ProductionPanels(UIElement element) {
    if(auto panel=element.try_as<Controls::Panel>()) {
        for(uint32_t i=0;i<panel.Children().Size();++i) {
            const auto child=panel.Children().GetAt(i);const auto replacement=ProductionPanels(child);
            if(child!=replacement)panel.Children().SetAt(i,replacement);
        }
    }
    if(auto stack=element.try_as<StackPanel>();stack && stack.Tag() && unbox_value<hstring>(stack.Tag())==L"ProductionSimpleStackPanel") {
        const auto panel=Stack(stack.Orientation(),stack.Spacing());
        panel->Width(stack.Width());panel->Height(stack.Height());panel->MaxWidth(stack.MaxWidth());panel->MaxHeight(stack.MaxHeight());
        panel->HorizontalAlignment(stack.HorizontalAlignment());panel->VerticalAlignment(stack.VerticalAlignment());
        panel->Margin(stack.Margin());panel->Padding(stack.Padding());panel->Visibility(stack.Visibility());
        Grid::SetRow(panel.as<FrameworkElement>(),Grid::GetRow(stack));Grid::SetColumn(panel.as<FrameworkElement>(),Grid::GetColumn(stack));
        Grid::SetRowSpan(panel.as<FrameworkElement>(),Grid::GetRowSpan(stack));Grid::SetColumnSpan(panel.as<FrameworkElement>(),Grid::GetColumnSpan(stack));
        while(stack.Children().Size()){auto child=stack.Children().GetAt(0);stack.Children().RemoveAt(0);panel->Children().Append(child);}
        return panel.as<UIElement>();
    }
    return element;
}
UIElement Load(const wchar_t* xml){return ProductionPanels(Markup::XamlReader::Load(xml).as<UIElement>());}
void Walk(UIElement element,const std::function<void(FrameworkElement)>& action) {
    if(auto framework=element.try_as<FrameworkElement>()) {
        if(framework.Visibility()==Visibility::Collapsed)return;
        action(framework);
    }
    if(auto panel=element.try_as<Controls::Panel>())for(auto child:panel.Children())Walk(child,action);
    else if(auto presenter=element.try_as<ContentPresenter>()){if(auto child=presenter.Content().try_as<UIElement>())Walk(child,action);}
    else if(auto control=element.try_as<ContentControl>()){if(auto child=control.Content().try_as<UIElement>())Walk(child,action);}
}
bool Bounds(UIElement element,FrameworkElement root) {
    bool valid=true;
    Walk(element,[&](FrameworkElement child){
        auto rect=child.TransformToVisual(root).TransformBounds({0,0,float(child.ActualWidth()),float(child.ActualHeight())});
        if(rect.X<-.6 || rect.Y<-.6 || rect.X+rect.Width>root.ActualWidth()+.6 || rect.Y+rect.Height>root.ActualHeight()+.6) {
            std::cerr<<"Overflow "<<to_string(get_class_name(child))<<" rect="<<rect.X<<','<<rect.Y<<','<<rect.Width<<','<<rect.Height<<" root="<<root.ActualWidth()<<','<<root.ActualHeight()<<'\n';valid=false;
        }
    });return valid;
}
int main() try {
	std::cout << std::unitbuf;
    init_apartment(apartment_type::single_threaded);const auto app=make_self<LayoutTestApp>();
    const auto manager=Hosting::WindowsXamlManager::InitializeForCurrentThread();app->Provider();
    MUXC::XamlControlsResources resources;resources.ControlsResourcesVersion(MUXC::ControlsResourcesVersion::Version2);app->Resources().MergedDictionaries().Append(resources);
    // Keep real ItemsControl presenters: a default Stretch wrapper made the
    // production horizontal panel request the entire monitor height, even for
    // one short group. Directly appending template Grids missed that behavior.
    {
        auto groups=Load(GroupContainerFixture).as<ItemsControl>();
        groups.ItemsSource(single_threaded_vector<IInspectable>({box_value(1)}));
        auto body=Stack(Orientation::Horizontal,24);body->Children().Append(groups);
        body->Measure({1000,1000});body->Arrange({0,0,body->DesiredSize().Width,body->DesiredSize().Height});
        assert(body->DesiredSize().Height==80);
        auto nativePanel=groups.ItemsPanelRoot().as<Panel>();
        assert(nativePanel.Children().Size()==1);
        auto presenter=nativePanel.Children().GetAt(0).as<ContentPresenter>();
        assert(presenter.VerticalAlignment()==VerticalAlignment::Top);
        nativePanel.Children().Clear();
        auto columns=Stack(Orientation::Horizontal,24);columns->Children().Append(presenter);
        columns->Measure({1000,1000});
        assert(columns->DesiredSize().Height==80);
        std::cout<<"PASS production group host and real ContentPresenter preserve natural height (80, not 1000).\n";
    }
    unsigned cases=0;
    {
        auto rows=Load(GroupContainerFixture).as<ItemsControl>();
        const auto first=box_value(1),last=box_value(3),conditional=box_value(2);
        auto items=single_threaded_observable_vector<IInspectable>({first,last});
        rows.ItemsSource(items);
        auto measure=[&]{rows.Measure({260,600});rows.Arrange({0,0,260,rows.DesiredSize().Height});};
        measure();
        const auto firstContainer=rows.ContainerFromIndex(0),lastContainer=rows.ContainerFromIndex(1);
        assert(firstContainer && lastContainer);
        SynchronizeParameterItems(items,std::vector<IInspectable>{first,conditional,last});measure();
        assert(rows.ContainerFromIndex(0)==firstContainer && rows.ContainerFromIndex(2)==lastContainer);
        SynchronizeParameterItems(items,std::vector<IInspectable>{first,last});measure();
        assert(rows.ContainerFromIndex(0)==firstContainer && rows.ContainerFromIndex(1)==lastContainer);
        std::cout<<"PASS native ItemsControl retains surviving containers across conditional insertion/removal.\n";
    }
    for(int language=0;language<3;++language)for(int effect=0;effect<3;++effect)
    for(int section=effect==1?0:-1;section<(effect==1?14:0);++section)
    for(int passes=1;passes<=(effect==0?3:1);++passes)for(bool advanced:{false,true})
    for(auto size : {Size{2488,1320},Size{1848,928},Size{1208,648}}) {
        if(effect!=0 && !advanced)continue;
        if(size.Width==1208 && effect!=1)continue; // 720p/200% DPI OKLab popup budget.
        const float width=size.Width,height=float(GetEffectParameterContentHeight(size.Height,effect==1));
        auto root=Load(Shells[language][section+1]).as<Grid>();root.MaxWidth(width);root.MaxHeight(height);
        root.RequestedTheme(cases%2?ElementTheme::Light:ElementTheme::Dark);
        auto body=Stack(Orientation::Horizontal,24);body->HorizontalAlignment(HorizontalAlignment::Left);body->VerticalAlignment(VerticalAlignment::Top);
        root.FindName(L"Body").as<Grid>().Children().Append(body.as<UIElement>());
        if(effect==1) {
            auto common=Load(Commons[language]).as<FrameworkElement>();
            body->Children().Append(common);
            body->Children().Append(Load(Contexts[language][section+1]));
        }
        std::map<std::string,float> values;for(auto& p:Fixtures[language][effect])values[p.name]=p.value;
        values["multiPass"]=float(passes);values["residualShowAdvanced"]=values["performanceShowAdvanced"]=advanced?1.f:0.f;
        values["processingMode"]=1;values["opticalFlowMethod"]=1;values["antiFlicker"]=2;
        auto get=[&](std::string_view name,float fallback){auto it=values.find(std::string(name));return it==values.end()?fallback:it->second;};
        std::vector<std::pair<std::wstring,std::vector<const ParameterFixture*>>> groups;
        std::set<std::string> expected,seen;std::vector<Grid> rows;
        for(const auto& fixture:Fixtures[language][effect]) {
            std::string name=fixture.name;
            const bool visible=effect==1?OKLab::Visible(name,section,0,0,0):IsEffectParameterVisible(effect==0?"DLSSNR\\DLSSNR_AI_Filter":"CAS",name,get);
            if(!visible)continue;
            expected.insert(name);
            if(effect==0 && (name=="performanceShowAdvanced" || name.ends_with("uiCorrection")))continue;
            auto found=std::find_if(groups.begin(),groups.end(),[&](const auto& group){return group.first==fixture.group;});
            if(found==groups.end()){groups.emplace_back(fixture.group,std::vector<const ParameterFixture*>{});found=groups.end()-1;}
            found->second.push_back(&fixture);
        }
        for(const auto& [label,parameters]:groups) {
            auto column=Load(GroupShell).as<Grid>();column.FindName(L"Title").as<TextBlock>().Text(label);
            auto wrap=make_self<winrt::Magpie::implementation::WrapPanel>();wrap->Orientation(Orientation::Vertical);wrap->HorizontalSpacing(24);wrap->VerticalSpacing(8);
            column.FindName(L"Rows").as<Grid>().Children().Append(wrap.as<UIElement>());
            for(const auto* fixture:parameters) {
                auto row=Load(fixture->xml).as<Grid>();rows.push_back(row);assert(seen.insert(fixture->name).second);
                std::string name=fixture->name;
                if(effect==0 && name.ends_with("useAutoMask"))seen.insert(name.substr(0,name.size()-11)+"uiCorrection");
                if(effect==0 && name=="residualShowAdvanced")seen.insert("performanceShowAdvanced");
                ContentPresenter presenter;presenter.Content(row);wrap->Children().Append(presenter);
            }
            body->Children().Append(column);
        }
        assert(seen==expected);
        for(int pass=0;pass<3;++pass){
            root.Measure({width,height});root.Arrange({0,0,root.DesiredSize().Width,root.DesiredSize().Height});
            Walk(root,[&](FrameworkElement element){
                if(auto grid=element.try_as<Grid>();grid && grid.Children().Size()==2 && grid.Children().GetAt(0).try_as<CheckBox>() && grid.Children().GetAt(1).try_as<CheckBox>())ParameterCheckboxRow_SizeChanged(grid,nullptr);
            });
        }
        assert(root.ActualWidth()<=width+.1 && root.ActualHeight()<=height+.1);
        if(effect==1)assert(root.ActualHeight()<=600);
        assert(Bounds(root,root));
        for(auto row:rows){
            assert(row.ActualHeight()>0 && row.ActualWidth()==260);
            if(effect==1)assert(row.TransformToVisual(root).TransformPoint({0,0}).X>=284);
        }
        if(effect==1){auto common=body->Children().GetAt(0).as<FrameworkElement>();assert(common.ActualWidth()==260);assert(common.TransformToVisual(root).TransformPoint({0,0}).X==0);}
        std::cout<<"Case "<<cases++<<" lang="<<language<<" effect="<<effect<<" section="<<section<<" pass="<<passes<<" advanced="<<advanced<<" viewport="<<width<<'x'<<height<<" rows="<<rows.size()<<" bounds="<<root.ActualWidth()<<'x'<<root.ActualHeight()<<'\n';
    }
    std::cout<<"PASS "<<cases<<" real-metadata natural-size single-page layouts; no scrolling, Viewbox or fixed row heights; exact visible parameter coverage.\n";
} catch(const hresult_error& error){std::cerr<<to_string(error.message())<<'\n';return 1;}
