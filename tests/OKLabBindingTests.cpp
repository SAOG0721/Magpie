// Compiled production template/list getters; isolated services and layout panels.
#include "pch.h"
#include "ScalingModesPage.h"
#include "EffectParametersViewModel.h"
#include "ScalingModesPage.xaml.g.hpp"
#include "XamlBindingInfo.xaml.g.hpp"
#include <winrt/Windows.UI.Xaml.Hosting.h>
#include <windows.ui.xaml.hosting.desktopwindowxamlsource.h>
#include <iostream>
#include <stdexcept>
using namespace winrt;
namespace Test = winrt::Magpie::implementation;
struct BindingTestApp : ApplicationT<BindingTestApp, Markup::IXamlMetadataProvider> {
    winrt::Microsoft::UI::Xaml::XamlTypeInfo::XamlControlsXamlMetaDataProvider provider{nullptr};
    auto& Provider(){if(!provider)provider=winrt::Microsoft::UI::Xaml::XamlTypeInfo::XamlControlsXamlMetaDataProvider();return provider;}
    Markup::IXamlType GetXamlType(Interop::TypeName const& t){return Provider().GetXamlType(t);}
    Markup::IXamlType GetXamlType(hstring const& t){return Provider().GetXamlType(t);}
    com_array<Markup::XmlnsDefinition> GetXmlnsDefinitions(){return Provider().GetXmlnsDefinitions();}
};
void Require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void Pump(){for(int n=0;n<12;++n){MSG m{};while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}Sleep(5);}}
template<class T> T Find(DependencyObject node){
    if(auto r=node.try_as<T>())return r;
    for(int i=0;i<Media::VisualTreeHelper::GetChildrenCount(node);++i)if(auto r=Find<T>(Media::VisualTreeHelper::GetChild(node,i)))return r;
    return nullptr;
}
int main() try {
    init_apartment(apartment_type::single_threaded);
    auto app=make_self<BindingTestApp>();
    auto manager=Hosting::WindowsXamlManager::InitializeForCurrentThread();app->Provider();
    MUXC::XamlControlsResources resources;resources.ControlsResourcesVersion(MUXC::ControlsResourcesVersion::Version2);app->Resources().MergedDictionaries().Append(resources);
    HWND host=CreateWindowExW(0,L"STATIC",L"OKLab binding regression",WS_OVERLAPPEDWINDOW,0,0,2400,1300,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    Require(host!=nullptr,"Create isolated host");
    Hosting::DesktopWindowXamlSource island;auto native=island.as<IDesktopWindowXamlSourceNative>();check_hresult(native->AttachToWindow(host));
    HWND child{};check_hresult(native->get_WindowHandle(&child));SetWindowPos(child,nullptr,0,0,2400,1300,SWP_NOZORDER);
    unsigned cases=0;
    for(int language=0;language<3;++language)for(bool ok:{false,true}){
        Test::testLanguage=language;
        auto page=make_self<Test::ScalingModesPage>();auto vm=make_self<Test::EffectParametersViewModel>();vm->ok=ok;
        page->ParameterContent().Content(*vm);island.Content(*page);Pump();
        page->Measure({2400,1300});page->Arrange({0,0,2400,1300});
        auto root=Find<Grid>(page->ParameterContent());Require(bool(root),"Template root loaded");
        const auto style=page->Resources().Lookup(box_value(L"EffectParametersPresenterStyle")).as<Style>();
        FlyoutPresenter presenter,reference;
        presenter.Style(style);reference.Style(style.BasedOn());
        presenter.Content(box_value(L"Parameters"));
        presenter.ApplyTemplate();presenter.Measure({1200,700});
        Require(bool(Find<ContentPresenter>(presenter)),"Production presenter template must actually be instantiated");
        Require(!Find<ScrollViewer>(presenter),"Parameter presenter must not recreate a focusable scrolling wrapper");
        Require(presenter.FontSize()==reference.FontSize() && presenter.Padding()==reference.Padding()
            && presenter.BorderThickness()==reference.BorderThickness() && presenter.CornerRadius()==reference.CornerRadius(),
            "No-scroll presenter preserves inherited typography, padding, border and corners");
        auto columns=root.Children().GetAt(1).as<Grid>().Children().GetAt(0).as<StackPanel>();
        auto common=columns.Children().GetAt(0).as<StackPanel>();
        auto context=columns.Children().GetAt(1).as<StackPanel>();
        auto section=common.Children().GetAt(0).as<ComboBox>();
        const std::vector<ComboBox> combos{section,context.Children().GetAt(0).as<ComboBox>(),context.Children().GetAt(1).as<ComboBox>(),context.Children().GetAt(3).as<ComboBox>(),context.Children().GetAt(4).as<ComboBox>(),common.Children().GetAt(2).as<ComboBox>()};
        const unsigned sizes[]{14,8,2,6,3,6};
        Require(vm->groupsRead,"Binding must reach Groups after all OKLab list initialization");
        for(size_t i=0;i<combos.size();++i){
            Require(combos[i].Items().Size()==sizes[i],"Real getter must populate all native ComboBox items");
            for(unsigned j=0;j<sizes[i];++j){Require(!unbox_value<hstring>(combos[i].Items().GetAt(j)).empty(),"Localized option label must be present");combos[i].SelectedIndex(j);Require(combos[i].SelectedIndex()==int(j),"Every option must be selectable");}
            combos[i].SelectedIndex(0);
        }
        Require((section.Visibility()==Visibility::Visible)==ok,"Section selector isolated to OKLab");
        Require((common.Visibility()==Visibility::Visible)==ok,"Common column isolated to OKLab");
        for(int s=0;s<(ok?14:1);++s){
            vm->SelectSection(s);Pump();
            Require((context.Visibility()==Visibility::Visible)==(ok&&(s==3||s==4||s==5||s==8||s==13)),"Context tools must follow section and effect type");
            ++cases;
        }
        if(ok){
            section.SelectedIndex(4);combos.back().SelectedIndex(3);Pump();
            Require(vm->OKSection()==4 && vm->OKPreset()==3,"User dropdown choices reach the model");
            vm->OKColor(6);vm->OKSlot(1);vm->OKCurve(5);vm->OKWheel(2);
            vm->OKSection(-1);vm->OKPreset(-1);vm->OKColor(-1);vm->OKSlot(-1);vm->OKCurve(-1);vm->OKWheel(-1);
            Require(vm->OKSection()==4 && vm->OKPreset()==3 && vm->OKColor()==6 && vm->OKSlot()==1
                && vm->OKCurve()==5 && vm->OKWheel()==2,"Transient empty selection must not reset any editor choice");
            island.Content(nullptr);root.DataContext(nullptr);Pump();
            root.DataContext(*vm);island.Content(*page);Pump();
            Require(vm->OKSection()==4 && vm->OKPreset()==3,"Reloading the same template must not write old dropdown indices into the model");
            Require(section.SelectedIndex()==4 && combos.back().SelectedIndex()==3,"Reloaded dropdowns must retain their model selection");
            const auto contentTemplate=page->ParameterContent().ContentTemplate();
            page->ParameterContent().ContentTemplate(nullptr);Pump();
            page->ParameterContent().ContentTemplate(contentTemplate);Pump();
            const auto freshRoot=Find<Grid>(page->ParameterContent());
            Require(freshRoot!=root && vm->OKSection()==4 && vm->OKPreset()==3,
                "Recreating a closed view preserves the same model and its selections");
            const auto freshCommon=freshRoot.Children().GetAt(1).as<Grid>().Children().GetAt(0).as<StackPanel>().Children().GetAt(0).as<StackPanel>();
            Require(freshCommon.Children().GetAt(0).as<ComboBox>().SelectedIndex()==4,
                "Freshly opened view displays the retained section");
        }
        island.Content(nullptr);Pump();
    }
    island.Close();DestroyWindow(host);
    std::cout<<"PASS "<<cases<<" compiled-template effect/section cases, 3 languages, actual 6 production list getters and native selectable labels; no Magpie config or process used.\n";
}catch(hresult_error const&e){std::cerr<<std::hex<<e.code().value<<' '<<to_string(e.message())<<'\n';return 1;}
catch(std::exception const&e){std::cerr<<e.what()<<'\n';return 1;}
