"""Extract the real paired-checkbox row and its resize handler for native measurement."""
from copy import deepcopy
from pathlib import Path
import json
import sys
import xml.etree.ElementTree as ET

root = Path(__file__).resolve().parents[1]
output = Path(sys.argv[1])
ns = '{http://schemas.microsoft.com/winfx/2006/xaml/presentation}'
x = '{http://schemas.microsoft.com/winfx/2006/xaml}'
ET.register_namespace('', ns[1:-1])
ET.register_namespace('x', x[1:-1])
page = ET.parse(root / 'src/Magpie/ScalingModesPage.xaml').getroot()
group = next(node for node in page.iter(ns+'DataTemplate') if node.get(x+'DataType') == 'local:EffectParameterGroupViewModel')
column = deepcopy(group.find(ns+'Grid'))
for child in list(column):
    if child.tag != ns+'Border':
        column.remove(child)
for node in column.iter():
    for key, value in list(node.attrib.items()):
        if value.startswith('{x:Bind'):
            del node.attrib[key]
row = deepcopy(next(node for node in page.iter(ns+'Grid') if node.get('SizeChanged') == 'ParameterCheckboxRow_SizeChanged'))
for node in row.iter():
    for key, value in list(node.attrib.items()):
        if value.startswith(('{x:Bind', '{Binding')) or key == 'SizeChanged':
            del node.attrib[key]
checkboxes = row.findall(ns+'CheckBox')
checkboxes[0].set(x+'Name', 'Left')
checkboxes[1].set(x+'Name', 'Right')
# The production row supplies the TextBlock; only its text is populated by
# the measurement fixture. Actual compiled bindings have a separate app probe.
assert checkboxes[1].find(ns+'TextBlock') is not None
source = (root / 'src/Magpie/ScalingModesPage.cpp').read_text(encoding='utf-8-sig')
start = source.index('void ScalingModesPage::ParameterCheckboxRow_SizeChanged(')
end = source.index('\n}\n', start) + 2
handler = source[start:end].replace('ScalingModesPage::', '')
lines = ['static const wchar_t* PairedRowXaml = LR"XAML(' + ET.tostring(row, encoding='unicode') + ')XAML";',
         'static const wchar_t* ColumnShellXaml = LR"XAML(' + ET.tostring(column, encoding='unicode') + ')XAML";', handler]
lines.append('static const std::array<std::array<const wchar_t*,4>,3> PairLabels{{')
for language in ['zh-Hans', 'zh-Hant', 'en-US']:
    xml = ET.parse(root / f'src/Magpie/Resources.language-{language}.resw').getroot()
    values = {node.get('name'):node.findtext('value') for node in xml.findall('data')}
    names = ['useAutoMask', 'uiCorrection', 'residualShowAdvanced', 'performanceShowAdvanced']
    labels = [values['EffectParam_DLSSNR_DLSSNR_AI_Filter_'+name+'_Label'] for name in names]
    lines.append('{' + ','.join('L'+json.dumps(label, ensure_ascii=False) for label in labels) + '},')
lines.append('}};')
(output / 'PairedCheckboxProduction.inc').write_text('\n'.join(lines), encoding='utf-8')
