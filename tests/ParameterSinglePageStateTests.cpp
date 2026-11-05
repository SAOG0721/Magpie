// Execute production visible-group publication with an in-memory property host.
#include <vector>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <memory>
#include <ranges>
#include <string>
#include "../src/Magpie/ParameterItems.h"

namespace Magpie {
struct ScalingModeParameter {
	unsigned index; bool visible=true; double value=0;
	unsigned Index() const {return index;}
	bool IsVisible() const {return visible;}
};
}
using namespace Magpie;
template<class T> using com_ptr=std::shared_ptr<T>;
template<class T,class... A> auto make_self(A&&... a){return std::make_shared<T>(std::forward<A>(a)...);}
template<class T> T* get_self(T& value){return &value;}
struct EffectParameterGroupViewModel;
struct IInspectable {
	void* object;
	IInspectable(ScalingModeParameter& value):object(&value){}
	IInspectable(EffectParameterGroupViewModel& value):object(&value){}
	template<class T> T& as() const {return *static_cast<T*>(object);}
    bool operator==(const IInspectable&) const = default;
};
template<class T> struct Items {
    std::shared_ptr<std::vector<T>> items=std::make_shared<std::vector<T>>();
    Items()=default;
    Items(std::vector<T>&& v):items(std::make_shared<std::vector<T>>(std::move(v))){}
    uint32_t Size() const{return uint32_t(items->size());}
    T GetAt(uint32_t i) const{return items->at(i);}
    void InsertAt(uint32_t i,const T& v) const{items->insert(items->begin()+i,v);}
    void RemoveAt(uint32_t i) const{items->erase(items->begin()+i);}
    void RemoveAtEnd() const{items->pop_back();}
    auto begin() const{return items->begin();} auto end() const{return items->end();}
    auto Identity() const{return items.get();} bool empty() const{return items->empty();}
};
template<class T> auto single_threaded_observable_vector(std::vector<T>&& values){return Items<T>(std::move(values));}
struct EffectParameterGroupViewModel {
	std::wstring label; Items<IInspectable> parameters; bool separator=false;
	EffectParameterGroupViewModel(std::wstring text,std::vector<IInspectable>&& values):label(std::move(text)),parameters(std::move(values)){}
	const auto& Params() const{return parameters;}
	const auto& Label() const{return label;}
	bool HasLabel() const{return !label.empty();}
	void ShowLeadingSeparator(bool value){separator=value;}
    void UpdateParameters(const std::vector<IInspectable>& v){SynchronizeParameterItems(parameters,v);}
};
struct EffectParametersViewModel {
	std::vector<com_ptr<EffectParameterGroupViewModel>> _sourceGroups,_groupImpls;
    std::vector<uint32_t> _visibleParameters;
    Items<IInspectable> _groups;
    unsigned notifications=0;
    void RaisePropertyChanged(const wchar_t*){++notifications;}
    void _RefreshVisibleGroups();
};
#include "ParameterSinglePageStateProduction.inc"

static std::vector<unsigned> AllIds(const EffectParametersViewModel& model) {
    std::vector<unsigned> result;
    for(const auto& group:model._groupImpls)for(const auto& item:group->Params())result.push_back(item.as<ScalingModeParameter>().Index());
    return result;
}
int main() {
    std::vector<ScalingModeParameter> parameters;
    for(unsigned i=0;i<24;++i)parameters.push_back({i});
    EffectParametersViewModel model;
    for(unsigned group=0;group<3;++group){
        std::vector<IInspectable> rows;
        for(unsigned i=group*8;i<(group+1)*8;++i)rows.emplace_back(parameters[i]);
        model._sourceGroups.push_back(make_self<EffectParameterGroupViewModel>(L"Group",std::move(rows)));
    }
    model._RefreshVisibleGroups();
    for(unsigned i=0;i<24;++i)assert(AllIds(model)[i]==i);
    const auto control=model._groupImpls.front().get();const auto storage=model._groups.Identity();const auto notices=model.notifications;
    parameters[9].value=.4;model._RefreshVisibleGroups();
    assert(model._groups.Identity()==storage && model._groupImpls.front().get()==control && model.notifications==notices);
    for(auto& parameter:parameters)parameter.visible=parameter.index<2;
    model._RefreshVisibleGroups();assert(AllIds(model)==(std::vector<unsigned>{0,1}));
    assert(model._groups.Identity()==storage && model._groupImpls.front().get()==control && model.notifications==notices);
    for(auto& parameter:parameters)parameter.visible=true;
    model._RefreshVisibleGroups();assert(AllIds(model).size()==24 && parameters[9].value==.4);
    assert(model._groups.Identity()==storage && model._groupImpls.front().get()==control && model.notifications==notices);
    assert(!model._groupImpls.front()->separator && model._groupImpls[1]->separator);
    for(auto& parameter:parameters)parameter.visible=false;
    model._RefreshVisibleGroups();assert(model._groups.empty());
    std::cout<<"PASS production visible groups: exact coverage, stable collections on value edits, conditional visibility and preserved values.\n";
}
