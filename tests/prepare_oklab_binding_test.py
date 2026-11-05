"""Compile the production parameter template and actual OKLab list getters.

Layout panels are substituted with StackPanel to isolate binding initialization;
the existing layout tests and native product acceptance cover panel geometry.
Application services are replaced by a small model; collection construction and
localized option lists are extracted unchanged from production.
"""
from pathlib import Path
import json
import re
import sys
import xml.etree.ElementTree as ET

repo = Path(__file__).resolve().parents[1]
out = Path(sys.argv[1])
out.mkdir(parents=True, exist_ok=True)
page = (repo / 'src/Magpie/ScalingModesPage.xaml').read_text(encoding='utf-8-sig')
style_start = page.index('<Style x:Key="EffectParametersPresenterStyle"')
presenter_style = page[style_start:page.index('</Style>', style_start)+len('</Style>')]
assert '<ScrollViewer' not in presenter_style
assert 'FlyoutPresenterStyle="{StaticResource EffectParametersPresenterStyle}"' in page
template = presenter_style + page[page.index('<DataTemplate x:Key="EffectParametersFlyout"'):page.index('</Page.Resources>')]
template = re.sub(r'x:Uid="[^"]+"', '', template)
template = template.replace('local:SimpleStackPanel', 'StackPanel').replace('local:WrapPanel', 'StackPanel')
template = re.sub(r'\s+(HorizontalSpacing|VerticalSpacing)="[^"]+"', '', template)
(out / 'ScalingModesPage.xaml').write_text('''<Page x:Class="Magpie.ScalingModesPage"
 xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
 xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml"
 xmlns:local="using:Magpie">
 <Page.Resources>
''' + template + '''</Page.Resources>
 <ContentControl x:Name="ParameterContent" ContentTemplate="{StaticResource EffectParametersFlyout}" />
</Page>''', encoding='utf-8')

events = sorted({name for _, name in re.findall(r'(\w+)="(Parameter\w+)"', template)})
(out / 'ScalingModesPage.h').write_text('''#pragma once
#include "ScalingModesPage.g.h"
namespace winrt::Magpie::implementation {
struct ScalingModesPage : ScalingModesPageT<ScalingModesPage> {
 winrt::Magpie::ScalingModesViewModel ViewModel() { return nullptr; }
''' + ''.join(f' template<class... T> void {name}(T&&...) {{}}\n' for name in events) + '};\n}', encoding='utf-8')

resources = []
for lang in ['zh-Hans', 'zh-Hant', 'en-US']:
    values = {n.get('name'): n.findtext('value') for n in ET.parse(repo / f'src/Magpie/Resources.language-{lang}.resw').getroot().findall('data')}
    resources.append('{' + ','.join('{L' + json.dumps(k, ensure_ascii=False) + ',L' + json.dumps(v, ensure_ascii=False) + '}'
                                   for k, v in values.items() if k.startswith(('OKLab_', 'EffectParam_OKLabImageAdjustment_Group_'))) + '}')

source = (repo / 'src/Magpie/EffectParametersViewModel.OKLab.cpp').read_text(encoding='utf-8-sig')
lists = source[source.index('IVector<hstring> OKList('):source.index('\n}\n}', source.index('IVector<hstring> OKList(')) + 2]
getters = source[source.index('IVector<hstring> EffectParametersViewModel::OKSections()'):source.index('void EffectParametersViewModel::_OKNotifyVisuals')]
list_names = set(re.findall(r'EffectParametersViewModel::(\w+)\(', getters))
assert len(list_names) == 6
idl = (repo / 'src/Magpie/EffectParametersViewModel.idl').read_text(encoding='utf-8-sig')
body = idl.split('runtimeclass EffectParametersViewModel :')[1].split('{', 1)[1].rsplit('}', 2)[0]
header = (repo / 'src/Magpie/EffectParametersViewModel.h').read_text(encoding='utf-8-sig')
types = {'Boolean': 'bool', 'Int32': 'int32_t', 'UInt32': 'uint32_t', 'Double': 'double', 'String': 'hstring', 'Object': 'IInspectable'}
def cpp_type(value):
    for a, b in types.items():
        value = re.sub(r'\b' + a + r'\b', b, value)
    return value.replace('.', '::')

lines = ['#pragma once', '#include <map>', '#include "EffectParametersViewModel.g.h"', '#include "EffectChoiceItems.h"', '#include "OKLabParameters.h"',
         'struct StrHelper { static std::wstring UTF8ToUTF16(std::string_view s) { return std::wstring(winrt::to_hstring(s)); } };',
         'namespace winrt::Magpie::implementation {', 'namespace OKLab = ::Magpie::OKLab;',
         'inline int testLanguage = 0;',
         'inline const std::map<std::wstring,std::wstring> optionResources[]{' + ','.join(resources) + '};',
         'inline hstring OKText(std::wstring_view key) { return hstring(optionResources[testLanguage].at(std::wstring(key))); }', lists,
         'struct EffectParametersViewModel : EffectParametersViewModelT<EffectParametersViewModel>, wil::notify_property_changed_base<EffectParametersViewModel> {',
         'bool ok = false; bool groupsRead = false; int32_t _okSection = 0, _okColor = 0, _okSlot = 0, _okCurve = 0, _okWheel = 0, _okPreset = 0, _okDragPoint = -1; bool _okWheelDragging = false;',
         'void SelectSection(int32_t value) { _okSection=value; for(auto key:{L"OKSection",L"OKColor",L"OKSlot",L"OKCurve",L"OKWheel",L"OKPreset",L"HasOKContextTools",L"OKMixerVisible",L"OKSelectionVisible",L"OKCurveVisible",L"OKWheelVisible",L"OKLutVisible"})RaisePropertyChanged(key); }']
lines.append('void _OKRefreshEditor() { SelectSection(_okSection); }')
for line in body.splitlines():
    line = line.strip()
    if not line:
        continue
    if line.startswith('void '):
        lines.append(cpp_type(line).rstrip(';') + ' {}')
        continue
    m = re.fullmatch(r'(.+?)\s+(\w+)(?:\s*(\{ get; \}))?;', line)
    if not m:
        raise ValueError(line)
    typ, name, get_only = cpp_type(m[1]), m[2], m[3]
    if name in list_names:
        lines.append(f'{typ} {name}() const;')
        continue
    if name in ['OKMixerVisible', 'OKSelectionVisible', 'OKWheelVisible', 'OKCurveVisible', 'OKLutVisible']:
        lines.append(re.search(r'bool ' + name + r'\(\) const noexcept \{[^}]+\}', header)[0])
        continue
    value = {'bool': 'false', 'int32_t': '0', 'uint32_t': '0', 'double': '0'}.get(typ, '{}')
    if typ.startswith('Windows::'):
        value = 'nullptr'
    if name == 'IsOKLab': value = 'ok'
    if name in ['OKSection','OKColor','OKSlot','OKCurve','OKWheel','OKPreset']: value = '_ok'+name[2:]
    if name == 'ContentWidth': value = '2400'
    if name == 'ContentMaxHeight': value = '1300'
    if name == 'HasOKContextTools': value = 'OKMixerVisible() || OKSelectionVisible() || OKWheelVisible() || OKCurveVisible() || OKLutVisible()'
    if name == 'Groups': value = '(groupsRead=true, single_threaded_vector<IInspectable>())'
    qualifier = ' const' if name == 'IsOKLab' else ''
    lines.append(f'{typ} {name}(){qualifier} {{ return {value}; }}')
    if not get_only:
        if name in ['OKSection','OKColor','OKSlot','OKCurve','OKWheel']:
            method = re.search(r'void EffectParametersViewModel::'+name+r'\(int32_t value\)\{[^\n]+', source)[0]
            lines.append(method.replace('EffectParametersViewModel::',''))
        elif name == 'OKPreset': lines.append(re.search(r'void OKPreset\(int32_t value\)[^\n]+',header)[0])
        else: lines.append(f'void {name}({typ} const&) {{}}')
lines += ['};', getters, '}']
(out / 'EffectParametersViewModel.h').write_text('\n'.join(lines), encoding='utf-8')
