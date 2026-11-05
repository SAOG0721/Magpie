"""Extract the production shader fragments and constant layout, without SDKs."""
from pathlib import Path
import re
import sys

repo = Path(__file__).resolve().parents[1]
out = Path(sys.argv[1])
out.mkdir(parents=True, exist_ok=True)
source = (repo / "src/Magpie.Core/DLSSNRFilter.cpp").read_text(encoding="utf-8-sig")
arrays = dict(re.findall(r'constexpr char (\w+)\[\] = R"\((.*?)\)";', source, re.S))
# Shader names are obtained from the actual source, including later renames.
names = list(arrays)
layout = re.search(r"struct ResampleConstants \{.*?static_assert\(sizeof\(ResampleConstants\) == 96\);", source, re.S)
assert layout
header = '#include "DLSSNRColorShader.h"\n#include "DLSSNRDetailShader.h"\n'
header += layout[0] + "\n"
for name in names:
    header += f'inline const std::string {name} = R"fixture({arrays[name]})fixture";\n'
header += 'inline const std::string prepareShader = std::string(Magpie::DLSSNR_COLOR_HLSL) + RESIDUAL_PREPARE_HLSL + std::string(Magpie::DLSSNR_DETAIL_HLSL);\n'
header += 'inline const std::string verticalShader = std::string(Magpie::DLSSNR_COLOR_HLSL) + RESIDUAL_VERTICAL_COMPOSITE_HLSL;\n'
(out / "DLSSNRResidualUnderTest.h").write_text(header, encoding="utf-8")
print("Production shader fragments and 96-byte resample layout extracted.")
