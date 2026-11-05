"""Extract production layout/controls and real shader metadata for headless ImGui.

The fixture supplies backend availability and values; no Magpie process, GPU,
configuration write, or desktop input is involved.
"""
from pathlib import Path
import json
import re
import sys
import xml.etree.ElementTree as ET

root = Path(__file__).resolve().parents[1]
output = Path(sys.argv[1])
output.mkdir(parents=True, exist_ok=True)
source = (root / 'src/Magpie.Core/OverlayDrawer.cpp').read_text(encoding='utf-8-sig')
def between(start, end):
    return source.split(start, 1)[1].split(end, 1)[0]
equal = 'static bool ParameterValuesEqual' + between('static bool ParameterValuesEqual', '\n}') + '\n}\n'
helper = ''
slider = 'static bool DrawEffectParameterSlider' + between('static bool DrawEffectParameterSlider', '\nbool OverlayDrawer::_DrawEffectParameters')
imgui_source = (root / 'src/Magpie.Core/ImGuiImpl.cpp').read_text(encoding='utf-8-sig')
tooltip=imgui_source[imgui_source.index('void ImGuiImpl::Tooltip('):imgui_source.index('\nImVec2 ImGuiImpl::_CaptureMousePos')]
(output/'ParameterTooltip.inc').write_text(tooltip.replace('void ImGuiImpl::Tooltip(', 'static void ProductionTooltip('),encoding='utf-8')
viewport = 'static void SetOverlayViewport' + imgui_source.split('static void SetOverlayViewport', 1)[1].split('\nvoid ImGuiImpl::NewFrame', 1)[0]
widgets = '\t\t\tImGui::PushID(int(parameterIdx));' + between('\t\t\tImGui::PushID(int(parameterIdx));', '\n\t\t\tif (changed && parameterValid')
desc = (root / 'src/Magpie.Core/include/EffectDesc.h').read_text(encoding='utf-8-sig')
desc = 'template <typename T>' + desc.split('template <typename T>', 1)[1].split('struct EffectPassFlags', 1)[0]
values = (root / 'src/Magpie.Core/include/EffectParameterValue.h').read_text(encoding='utf-8-sig')
values = '\n'.join(x for x in values.splitlines() if not x.startswith(('#include', '#pragma')))
glyph_source=(root/'src/Magpie.Core/OverlayHelper.cpp').read_text(encoding='utf-8-sig')
glyph_source=glyph_source[glyph_source.index('static void UnpackAccumulativeOffsetsIntoRanges'):glyph_source.index('static uint32_t GetSeed')]
glyph_source='namespace Magpie { struct OverlayHelper { static const ImWchar* GetGlyphRangesChineseSimplifiedOfficial() noexcept; static const ImWchar* GetGlyphRangesChineseTraditionalOfficial() noexcept; };\n'+glyph_source+'\n}\n'
(output/'OverlayGlyphRanges.inc').write_text(glyph_source,encoding='utf-8')
glyph_samples=[]
for locale in ['zh-Hans','zh-Hant']:
    texts=[d.findtext('value','') for d in ET.parse(root/f'src/Magpie/Resources.language-{locale}.resw').getroot().findall('data') if d.get('name').startswith(('OKLab','EffectParam_OKLabImageAdjustment','EffectParam_DLSSNR'))]
    glyph_samples.append('L'+json.dumps(''.join(sorted(set(''.join(texts)))),ensure_ascii=False))
(output/'OverlayGlyphSamples.inc').write_text('static const wchar_t* glyphSamples[]{'+','.join(glyph_samples)+'};',encoding='utf-8')
(output / 'SinglePageProduction.inc').write_text('namespace Magpie {\n' + desc + '}\n' + values + '\n' + equal + helper + slider + viewport, encoding='utf-8')
(output / 'SinglePageWidgets.inc').write_text(widgets + '\nImGui::PopID();\nreturn changed;\n', encoding='utf-8')

labels = {}
for lang, filename in [('zh', 'Resources.language-zh-Hans.resw'), ('en', 'Resources.language-en-US.resw')]:
    labels[lang] = {d.attrib['name']: d.findtext('value', '') for d in ET.parse(root / 'src/Magpie' / filename).getroot().findall('data')}
q = lambda s: json.dumps(s, ensure_ascii=False)
resources = ['static const std::map<std::string,std::map<std::wstring,std::string>> resources{']
for lang, entries in labels.items():
    resources.append('{' + q(lang) + ',{')
    resources.extend('{L' + q(k) + ',' + q(v) + '},' for k, v in entries.items())
    resources.append('}},')
resources.append('};')
(output / 'SinglePageResources.inc').write_text('\n'.join(resources), encoding='utf-8')

metadata = ['static std::vector<EffectParameterDesc> Parameters(std::string_view effect) {', 'std::vector<EffectParameterDesc> result;']
for effect, path in [('DLSSNR\\DLSSNR_AI_Filter', 'DLSSNR/DLSSNR_AI_Filter.hlsl'), ('OKLabImageAdjustment', 'OKLabImageAdjustment.hlsl')]:
    shader = (root / 'src/Effects' / path).read_text(encoding='utf-8-sig')
    metadata.append('if(effect==' + q(effect) + '){')
    for block in shader.split('//!PARAMETER')[1:]:
        match = re.search(r'\b(int|float)\s+(\w+)\s*;', block)
        if not match:
            continue
        kind, name = match.groups()
        def prop(key, default):
            m = re.search(r'//!' + key + r'\s+([^\r\n]+)', block)
            return m[1] if m else default
        prefix = 'EffectParam_' + effect.replace('\\', '_') + '_'
        resource_name = re.sub(r'^pass[23]_', '', name)
        group = prop('GROUP', '')
        metadata.append('{EffectParameterDesc p;p.name=' + q(name) + ';p.label=Resource(L' + q(prefix + resource_name + '_Label') + ',' + q(prop('LABEL', name)) + ');')
        metadata.append('p.group=Resource(L' + q(prefix + 'Group_' + re.sub(r'[^\w]', '_', group)) + ',' + q(group) + ');')
        constants = [prop('DEFAULT', '0'), prop('MIN', '0'), prop('MAX', '100'), prop('STEP', '1' if kind == 'int' else '.01')]
        metadata.append('p.constant=EffectConstant<' + kind + '>{' + ','.join(kind + '(' + v + ')' for v in constants) + '};')
        for val, text in re.findall(r'//!OPTION\s+(\d+)\s+([^\r\n]+)', block):
            metadata.append('p.choices.push_back({' + val + ',Resource(L' + q(prefix + resource_name + '_Option_' + val) + ',' + q(text) + ')});')
        metadata.append('result.push_back(std::move(p));}')
    metadata.append('}')
metadata.append('return result;}')
(output / 'SinglePageMetadata.inc').write_text('\n'.join(metadata), encoding='utf-8')
print('Extracted production layout, slider, parameter widgets, zh/en resources and NR/OKLab shader metadata')
