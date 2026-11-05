// Actual product XBF, callbacks and ViewModel; isolated startup and configuration.
#include "ScalingModesPage.h"
#include "EffectParametersViewModel.h"
#include "ScalingMode.h"
#include "EffectDesc.h"
namespace winrt::Magpie::implementation {
using namespace ::Magpie;
template<class F> void VisitDiagnostic(const DependencyObject& node, const F& visit) {
    visit(node);
    for (int i=0; i<VisualTreeHelper::GetChildrenCount(node); ++i)
        VisitDiagnostic(VisualTreeHelper::GetChild(node,i),visit);
}
template<class F> Windows::Foundation::IAsyncAction WaitDiagnostic(F ready) {
    const auto dispatcher=App::Get().Dispatcher();
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(!ready()) {
        if(std::chrono::steady_clock::now()>deadline) throw hresult_error(E_FAIL,L"Diagnostic timeout");
        co_await resume_after(std::chrono::milliseconds(20)); co_await dispatcher;
    }
}
static unsigned CheckParameterCheckboxRows(const DependencyObject& root, bool editSingles) {
    std::vector<CheckBox> singles;
    unsigned rows=0;
    VisitDiagnostic(root,[&](const auto& node){
        const auto row=node.template try_as<Grid>();
        if(!row || row.Name()!=L"ParameterCheckBoxRow")return;
        const auto parameter=row.DataContext().template as<winrt::Magpie::ScalingModeParameter>();
        CheckBox left{nullptr},right{nullptr};
        for(const auto& child:row.Children()) {
            const auto box=child?child.template try_as<CheckBox>():nullptr;
            if(!box)continue;
            if(Grid::GetColumn(box)==1)right=box;else left=box;
        }
        if(!left || bool(right)!=parameter.HasCompanion())throw hresult_error(E_FAIL,L"Incorrect realized checkbox structure");
        const auto text=left.Content().template try_as<TextBlock>();
        if(!text || text.Text()!=parameter.Label() || text.ActualWidth()<=0)
            throw hresult_error(E_FAIL,L"Missing checkbox label");
        const auto expected=right?GridUnitType::Auto:GridUnitType::Star;
        if(row.ColumnDefinitions().GetAt(0).Width().GridUnitType!=expected)
            throw hresult_error(E_FAIL,L"Incorrect checkbox column sizing");
        if(!right)singles.push_back(left);
        ++rows;
    });
    if(editSingles) for(const auto& box:singles) {
        const auto parameter=box.DataContext().as<winrt::Magpie::ScalingModeParameter>();
        const bool before=parameter.BooleanValue();
        const auto& name=EffectsService::Get().GetEffect(AppSettings::Get().ScalingModes().at(0).effects.at(0).name)->params.at(get_self<ScalingModeParameter>(parameter)->Index()).name;
        const auto key=std::wstring(to_hstring(name));
        auto& values=AppSettings::Get().ScalingModes().at(0).effects.at(0).parameters;
        box.IsChecked(!before);
        if(parameter.BooleanValue()==before || values.at(key)!=float(!before))
            throw hresult_error(E_FAIL,L"Checkbox edit did not reach saved model");
        box.IsChecked(before);
        if(parameter.BooleanValue()!=before || values.at(key)!=float(before))
            throw hresult_error(E_FAIL,L"Checkbox restore failed");
    }
    return rows;
}
static fire_and_forget RunParameterFlyoutAppProbe() {
    const auto dispatcher=App::Get().Dispatcher(); bool passed=false;
    try {
        co_await WaitDiagnostic([]{return App::Get().RootPage().as<FrameworkElement>().IsLoaded();});
        const auto host=App::Get().MainWindow().Handle();
        ShowWindow(host,SW_SHOWNOACTIVATE); ShowWindow(host,SW_SHOWNOACTIVATE);
        App::Get().RootPage()->NavigateToScalingModes();
        co_await WaitDiagnostic([]{
            bool found=false;
            VisitDiagnostic(App::Get().RootPage().as<DependencyObject>(),[&](const auto& node){
                if(auto e=node.template try_as<winrt::Magpie::SettingsExpander>()){e.IsExpanded(true);found=true;}
            }); return found;
        });
        Button anchor{nullptr}; Controls::Flyout flyout{nullptr};
        co_await WaitDiagnostic([&]{
            VisitDiagnostic(App::Get().RootPage().as<DependencyObject>(),[&](const auto& node){
                const auto b=node.template try_as<Button>();
                const auto f=b?b.Flyout().template try_as<Controls::Flyout>():nullptr;
                const auto c=f?f.Content().template try_as<ContentControl>():nullptr;
                if(c && c.Content().template try_as<winrt::Magpie::EffectParametersViewModel>()){anchor=b;flyout=f;}
            }); return anchor && anchor.IsLoaded() && anchor.ActualWidth()>0;
        });
        wchar_t sectionText[16]{}; GetEnvironmentVariableW(L"MAGPIE_DIAG_SECTION",sectionText,16);
        const int section=_wtoi(sectionText);
        const auto vm=flyout.Content().as<ContentControl>().Content().as<winrt::Magpie::EffectParametersViewModel>();
        const auto impl=get_self<EffectParametersViewModel>(vm);
        Logger::Get().Info(fmt::format("CheckboxProbe opening effect={} section={}",
            to_string(AppSettings::Get().ScalingModes().at(0).effects.at(0).name),section));
        bool opened=false;
        auto openToken=flyout.Opened(auto_revoke,[&](auto&&,auto&&){opened=true;});
        auto closeToken=flyout.Closed(auto_revoke,[&](auto&&,auto&&){opened=false;});
        unsigned rowChecks=0;
        for(int reopen=0;reopen<2;++reopen) {
            flyout.ShowAt(anchor);co_await WaitDiagnostic([&]{return opened;});
            co_await WaitDiagnostic([&]{
                flyout.Content().as<FrameworkElement>().UpdateLayout();
                return flyout.Content().as<FrameworkElement>().ActualHeight()>0;
            });
            ComboBox selector{nullptr};
            if(impl->IsOKLab()) {
                VisitDiagnostic(flyout.Content().as<DependencyObject>(),[&](const auto& node){
                    if(auto combo=node.template try_as<ComboBox>();combo && combo.Items().Size()==14)selector=combo;
                });
                if(!selector)throw hresult_error(E_FAIL,L"Section selector missing");
                selector.SelectedIndex(section);
            }
            const int variants=impl->IsOKLab()?(section==5?2:section==8?6:1):1;
            for(int variant=0;variant<variants;++variant) {
                if(impl->IsOKLab()) {
                    if(section==5)impl->OKSlot(variant);
                    if(section==8)impl->OKCurve(variant);
                }
                flyout.Content().as<FrameworkElement>().UpdateLayout();
                rowChecks+=CheckParameterCheckboxRows(flyout.Content().as<DependencyObject>(),true);
            }
            if(selector && section!=0) {
                selector.SelectedIndex(0);flyout.Content().as<FrameworkElement>().UpdateLayout();
                if(CheckParameterCheckboxRows(flyout.Content().as<DependencyObject>(),false)!=0)
                    throw hresult_error(E_FAIL,L"Old section rows survived removal");
                selector.SelectedIndex(section);flyout.Content().as<FrameworkElement>().UpdateLayout();
                rowChecks+=CheckParameterCheckboxRows(flyout.Content().as<DependencyObject>(),false);
            }
            flyout.Hide();co_await WaitDiagnostic([&]{return !opened;});
        }
        Logger::Get().Info(fmt::format("CheckboxProbe PASS section={} rowChecks={} opens=2",section,rowChecks));
        passed=true;
    } catch(hresult_error const&e){Logger::Get().Error(fmt::format("CheckboxProbe FAIL {}",to_string(e.message())));}
      catch(std::exception const&e){Logger::Get().Error(fmt::format("CheckboxProbe FAIL {}",e.what()));}
    Logger::Get().Flush();
    App::Get().MainWindow().Destroy();
    PostQuitMessage(passed?0:1);
}
}
