"""Check metadata/UI contracts and extract the production settings parser."""
from pathlib import Path
import re
import sys
import xml.etree.ElementTree as ET

repo = Path(__file__).resolve().parents[1]
out = Path(sys.argv[1])
read = lambda path: (repo / path).read_text(encoding="utf-8-sig")
header = read("src/Magpie.Core/DLSSNRFilter.h")
settings = header[header.index("struct DLSSNRSettings {"):header.index("DLSSNRSettings ParseDLSSNRSettings(")]
source = read("src/Magpie.Core/DLSSNRFilter.cpp")
parser = source[source.index("DLSSNRSettings ParseDLSSNRSettings("):source.index("\n}\n\n#ifdef MP_ENABLE_DLSSNR")]
(out / "DLSSNRSettingsUnderTest.h").write_text(settings + "DLSSNRSettings ParseDLSSNRSettings(const EffectOption&, bool = false) noexcept;\n" + parser, encoding="utf-8")
session = source[source.index("static bool InitializeSignedSnippet(\n"):source.index("static void SetEvaluateParametersUnsafe(")]
(out / "DLSSNRSessionUnderTest.h").write_text(session, encoding="utf-8")
live = source[source.index("EffectParameterApplyMode DLSSNRFilter::GetParameterApplyMode("):source.index("EffectParameterRestartReason DLSSNRFilter::GetParameterRestartReason(")]
live += source[source.index("static bool SameNRSettings("):source.index("bool DLSSNRFilter::Initialize(")]
(out / "DLSSNRLiveUnderTest.h").write_text(live, encoding="utf-8")
guidance_types = read("src/Magpie.Core/FrameGuidanceTypes.h")
guidance_types = guidance_types[guidance_types.index("using FrameGuidanceFrameId"):guidance_types.index("struct MotionVectorProviderOutput;")]
guidance = source[source.index("static FrameGuidanceRegion ScaleGuidanceRegion("):source.index("static bool CompositeResidual(")]
guidance += source[source.index("static bool SameGuidance("):source.index("static void TransitionColor(")]
interop = read("src/Magpie.Core/FrameGuidanceD3D12Interop.cpp")
guidance += interop[interop.index("bool FrameGuidanceD3D12Interop::WaitForProducer("):interop.index("void FrameGuidanceD3D12Interop::Transition(")]
(out / "DLSSNRGuidanceTypesUnderTest.h").write_text(guidance_types, encoding="utf-8")
(out / "DLSSNRGuidanceUnderTest.h").write_text(guidance, encoding="utf-8")

shader = read("src/Effects/DLSSNR/DLSSNR_AI_Filter.hlsl")
blocks = {}
for block in shader.split("//!PARAMETER\n")[1:]:
    match = re.search(r"^(?:int|float) (\w+);", block, re.M)
    assert match and match[1] not in blocks
    blocks[match[1]] = block[:match.end()]
assert len(blocks) == 42
assert "residualColorMode" not in blocks and "residualShowProtection" not in blocks
anti = blocks["antiFlicker"]
assert list(blocks).index("antiFlicker") == list(blocks).index("multiPass") + 1
assert "//!GROUP DLSSNR · Pass 1" in anti and "//!DEFAULT 0" in anti
assert re.findall(r"//!OPTION (\d+) (.*)", anti) == [("0", "None"), ("1", "Static Accumulation"), ("2", "Optical Flow Accumulation"), ("3", "Optical Flow Accumulation+"), ("4", "Low-frequency Temporal Reconstruction")]
time = blocks["antiFlickerTimeMs"]
assert list(blocks).index("antiFlickerTimeMs") == list(blocks).index("antiFlicker") + 1
for field, value in (("DEFAULT", "80"), ("MIN", "5"), ("MAX", "200"), ("STEP", "5")):
    assert f"//!{field} {value}\n" in time
assert "pass2_antiFlickerTimeMs" not in blocks and "pass3_antiFlickerTimeMs" not in blocks
count = blocks["multiPass"]
assert list(blocks).index("multiPass") == list(blocks).index("uiCorrection") + 1
assert re.search(r"//!LABEL (.*)", count)[1] == "Multi Pass"
assert "//!GROUP DLSSNR · Pass 1" in count and "//!DEFAULT 1" in count
assert re.findall(r"//!OPTION (\d+) (\d+)", count) == [("1", "1"), ("2", "2"), ("3", "3")]
names = ("style", "intensity", "localToneStrength", "localStructureStrength",
         "skinStructureStrength", "useAutoMask", "uiCorrection")
for name in names:
    original = blocks[name]
    assert "//!GROUP DLSSNR · Pass 1" in original
    for i in (2, 3):
        later = blocks[f"pass{i}_{name}"]
        assert f"//!GROUP DLSSNR · Pass {i}" in later
        for field in ("DEFAULT", "MIN", "MAX", "STEP", "LABEL"):
            pattern = rf"//!{field} (.*)"
            assert re.search(pattern, original)[1] == re.search(pattern, later)[1]
vm = read("src/Magpie/EffectParametersViewModel.cpp")
assert 'GetEffectParameterColumnLayout(visibleGroupCount, _availableLayoutWidth)' in vm
xaml = ET.fromstring(read("src/Magpie/ScalingModesPage.xaml"))
namespace = "{http://schemas.microsoft.com/winfx/2006/xaml/presentation}"
template = next(element for element in xaml.iter(namespace + "DataTemplate")
                if element.attrib.get("{http://schemas.microsoft.com/winfx/2006/xaml}Key") == "EffectParametersFlyout")
scroll = next(template.iter(namespace + "ScrollViewer"))
assert scroll.attrib["HorizontalScrollBarVisibility"] == "Auto"
assert scroll.attrib["HorizontalScrollMode"] == "Enabled"
for lang in ("en-US", "zh-Hans", "zh-Hant"):
    root = ET.fromstring(read(f"src/Magpie/Resources.language-{lang}.resw"))
    entries = {item.attrib["name"]: item.findtext("value") for item in root.findall("data")}
    assert entries["EffectParam_DLSSNR_DLSSNR_AI_Filter_multiPass_Label"] == "Multi Pass"
    for i in range(5):
        assert entries[f"EffectParam_DLSSNR_DLSSNR_AI_Filter_antiFlicker_Option_{i}"]
    for i in (1, 2, 3):
        assert entries[f"EffectParam_DLSSNR_DLSSNR_AI_Filter_Group_DLSSNR____Pass_{i}"] == f"DLSSNR · Pass {i}"
print("Multi Pass shader defaults/ranges, 42 unique keys, dynamic-column viewport and localization contracts passed.")
