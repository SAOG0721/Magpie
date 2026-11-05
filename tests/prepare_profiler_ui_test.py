"""Extract the production ImGui graph and developer section; no replacement UI."""
from pathlib import Path
import json
import sys
import xml.etree.ElementTree as ET

root = Path(__file__).resolve().parents[1]
output = Path(sys.argv[1])
text = (root / 'src/Magpie.Core/OverlayDrawer.cpp').read_text(encoding='utf-8-sig')
start = text.index('void OverlayDrawer::_DrawProfilerGraph(')
end = text.index('\nbool OverlayDrawer::_DrawProfiler(', start)
graph = text[start:end]
graph = graph.replace('static constexpr const wchar_t* LABELS[]', '++graphDrawCalls;\n\tstatic constexpr const wchar_t* LABELS[]')
graph = graph.replace('const bool toggled = ImGui::IsItemToggledOpen();',
                      'const bool toggled = ImGui::IsItemToggledOpen();\n\tgraphHeader = ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());')
start = text.index('\t// 开发者选项', end)
end = text.index('\n#ifdef MP_DEBUG_INFO_ON_OVERLAY', start)
developer = 'bool OverlayDrawer::Developer(bool developerMode, bool multiPass, bool& effectivePasses) {\n'
developer += 'bool needRedraw=false; struct Options { bool value; bool IsDeveloperMode() const {return value;} } options{developerMode};\n'
developer += 'struct Effect { std::vector<float> passTimings; }; std::vector<Effect> effectDrawInfos{{std::vector<float>(multiPass ? 2 : 1)}};\n'
developer += text[start:end] + '\neffectivePasses=showPasses; return needRedraw;\n}\n'
developer = developer.replace('if (ImGui::Button(buttonStr.c_str())) {',
    'const bool clicked = ImGui::Button(buttonStr.c_str());\n'
    'developerButton = ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());\nif (clicked) {')
graph = graph.replace('const bool open = ImGui::CollapsingHeader(label);',
    'const bool open = ImGui::CollapsingHeader(label);\nlastHeader = ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());')
start = text.index('\t\t\t\t\tconst float leftWidth = ImGui::GetItemRectMax().x')
end = text.index('\n\t\t\t\t}', start)
checkbox = 'void PositionPairedCheckbox(float _dpiScale) {\n' + text[start:end] + '\n}\n'
(output / 'ProfilerUiProduction.inc').write_text(graph + developer + checkbox, encoding='utf-8')
lines = ['static const std::array<std::map<std::wstring,std::string>,3> resources{{']
for language in ['zh-Hans','zh-Hant','en-US']:
    xml = ET.parse(root / f'src/Magpie/Resources.language-{language}.resw').getroot()
    values = {n.get('name').replace('.', '/'):n.findtext('value') for n in xml.findall('data')}
    checkbox_keys = ['EffectParam_DLSSNR_DLSSNR_AI_Filter_' + name + '_Label' for name in
                     ['useAutoMask', 'uiCorrection', 'residualShowAdvanced', 'performanceShowAdvanced']]
    keys = [key for key in values if key.startswith('Overlay_Profiler') or
            key == 'Home_Advanced_DeveloperOptions/Header' or key in checkbox_keys]
    lines.append('{')
    for key in keys:
        lines.append('{L' + json.dumps(key, ensure_ascii=False) + ',' + json.dumps(values[key], ensure_ascii=False) + '},')
    lines.append('},')
lines.append('}};')
(output / 'ProfilerUiResources.inc').write_text('\n'.join(lines), encoding='utf-8')

renderer = (root / 'src/Magpie.Core/Renderer.cpp').read_text(encoding='utf-8-sig')
start = renderer.index('\tif (submitted && contentFrame && _frontendCaptureFrameId != 0')
end = renderer.index('\n\tbool overlayColorChanged', start)
(output / 'ProfilerPresentationGate.inc').write_text(renderer[start:end], encoding='utf-8')

