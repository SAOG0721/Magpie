"""Extract the production shaders and bounded functions, without loading NGX."""
from pathlib import Path
import re
import sys

repo = Path(__file__).resolve().parents[1]
out = Path(sys.argv[1])
out.mkdir(parents=True, exist_ok=True)
nr = (repo / 'src/Magpie.Core/DLSSNRFilter.cpp').read_text(encoding='utf-8-sig')
fg = (repo / 'src/Magpie.Core/DLSSFrameGenerator.cpp').read_text(encoding='utf-8-sig')

def function(source, signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

shaders = re.findall(r'constexpr char (\w+)_HLSL\[\] = R"\((.*?)\)";', nr, re.S)
assert len(shaders) == 6, len(shaders)
constants = function(nr, 'struct ResampleConstants') + ';'
content = ['#include <string>\n#include "DLSSNRColorShader.h"\n#include "DLSSNRDetailShader.h"', 'namespace Nr {', constants]
for name, body in shaders:
    content.append(f'constexpr char {name}_HLSL[] = R"({body})";')
content.append('inline const std::string RESIDUAL_PREPARE_SHADER = std::string(Magpie::DLSSNR_COLOR_HLSL) + RESIDUAL_PREPARE_HLSL + std::string(Magpie::DLSSNR_DETAIL_HLSL);')
content.append('inline const std::string RESIDUAL_VERTICAL_COMPOSITE_SHADER = std::string(Magpie::DLSSNR_COLOR_HLSL) + RESIDUAL_VERTICAL_COMPOSITE_HLSL;')
content.extend(function(nr, signature) for signature in (
    'static bool CreateCompositeOutput(',
    'static bool CompositeResidual('))
content.append('}\nnamespace Fg {')
content.extend(function(fg, signature) for signature in (
    'static bool WaitForFence(', 'static bool CreateInterpolationDisableResources(',
    'static std::optional<bool> ReadInterpolationDisabled('))
content.append('}')
(out / 'dlss_r2_production.h').write_text('\n\n'.join(content), encoding='utf-8')
print(f'Extracted {len(shaders)} production shaders and NR/FG functions')
