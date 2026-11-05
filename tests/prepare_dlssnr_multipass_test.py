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
sparse = read("src/Magpie.Core/DLSSNRSparse.inl")
live += sparse[sparse.index("void DLSSNRFilter::UpdateSparseParameters("):sparse.index("bool DLSSNRFilter::DrawSparse(")]
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
assert len(blocks) == 50
assert "opticalFlowScaleOrder" not in blocks
assert "residualMaxAgeFrames" not in blocks
assert "residualMaxAgeFrames" not in source
assert list(blocks)[0] == "modelPreset"
assert re.findall(r"//!OPTION (\d+) (.*)", blocks["modelPreset"]) == [("0", "Automatic (DLL Default)"), ("1", "HT"), ("2", "CC Small FP8")]
assert "//!DEFAULT 1\n" in blocks["enableInputResolutionScaling"]
for field, value in [("MIN", "0"), ("MAX", "1"), ("DEFAULT", "0"), ("STEP", "0.05")]:
    assert f"//!{field} {value}\n" in blocks["residualChromaProtection"]
for obsolete in ["residualNeutralProtection", "residualSkinProtection", "residualShowColorZones",
                 "residualShadowChroma", "residualMidtoneChroma", "residualHighlightChroma"]:
    assert obsolete not in blocks
assert list(blocks).index("residualMidtoneStrength") == list(blocks).index("shadowStructureMultiplier") + 1
assert list(blocks).index("reflectionGlowMultiplier") == list(blocks).index("residualMidtoneStrength") + 1
assert re.findall(r"//!OPTION (\d+) (.*)", blocks["residualTransfer"]) == [("0", "Historical Residual in Place"), ("1", "Historical Residual Reprojection")]
performance = [name for name, block in blocks.items() if "//!GROUP Performance Advanced Adjustments" in block]
assert performance == ["resolutionControl", "inputResolutionPercent", "processingMode", "sparseUpdateInterval", "residualTransfer", "opticalFlowMethod", "amdOpticalFlowMode", "nvidiaOpticalFlowQuality"]
for retired in ("recoveryPolicy", "bufferedDelayFrames", "asyncResidualComposition", "__dlssnrModeVersion"):
    assert retired not in blocks
    assert "pass2_" + retired not in blocks and "pass3_" + retired not in blocks
assert "//!DEFAULT 0" in blocks["resolutionControl"]
assert "//!DEFAULT 0" in blocks["processingMode"]
assert re.findall(r"//!OPTION (\d+) (.*)", blocks["processingMode"]) == [
    ("0", "Every Frame"), ("1", "Alternating Frames")]
assert "//!DEFAULT 2\n" in blocks["sparseUpdateInterval"]
assert re.findall(r"//!OPTION (\d+) (.*)", blocks["sparseUpdateInterval"]) == [
    ("2", "Every 2 Frames"), ("3", "Every 3 Frames"), ("4", "Every 4 Frames")]
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
xaml = ET.fromstring(read("src/Magpie/ScalingModesPage.xaml"))
namespace = "{http://schemas.microsoft.com/winfx/2006/xaml/presentation}"
template = next(element for element in xaml.iter(namespace + "DataTemplate")
                if element.attrib.get("{http://schemas.microsoft.com/winfx/2006/xaml}Key") == "EffectParametersFlyout")
assert not list(template.iter(namespace + "ScrollViewer")), "Parameter panels must fit without scrolling"
for lang in ("en-US", "zh-Hans", "zh-Hant"):
    root = ET.fromstring(read(f"src/Magpie/Resources.language-{lang}.resw"))
    entries = {item.attrib["name"]: item.findtext("value") for item in root.findall("data")}
    assert not any("DLSSNR_DLSSNR_AI_Filter_opticalFlowScaleOrder_" in key for key in entries)
    assert entries["EffectParam_DLSSNR_DLSSNR_AI_Filter_multiPass_Label"] == "Multi Pass"
    processing_labels = {
        "en-US": ("Every Frame", "Alternating Frames"),
        "zh-Hans": ("逐帧", "隔帧"),
        "zh-Hant": ("逐幀", "隔幀")}
    for i, label in enumerate(processing_labels[lang]):
        assert entries[f"EffectParam_DLSSNR_DLSSNR_AI_Filter_processingMode_Option_{i}"] == label
    for name in ("recoveryPolicy", "bufferedDelayFrames", "asyncResidualComposition"):
        assert not any(key.startswith(f"EffectParam_DLSSNR_DLSSNR_AI_Filter_{name}_") for key in entries)
    for i in (2, 3, 4, 5, 6):
        assert f"EffectParam_DLSSNR_DLSSNR_AI_Filter_processingMode_Option_{i}" not in entries
    assert "EffectParam_DLSSNR_DLSSNR_AI_Filter_FixedResolutionRequired" not in entries
    assert entries["FrameSync_Mode_Async.Content"] == "Async"
    for suffix in ("Label", "Description"):
        assert entries[f"EffectParam_DLSSNR_DLSSNR_AI_Filter_sparseUpdateInterval_{suffix}"]
    for i in (2, 3, 4):
        expected = {"en-US": f"Every {i} Frames", "zh-Hans": f"每 {i} 帧", "zh-Hant": f"每 {i} 幀"}
        assert entries[f"EffectParam_DLSSNR_DLSSNR_AI_Filter_sparseUpdateInterval_Option_{i}"] == expected[lang]
    for i in range(5):
        assert entries[f"EffectParam_DLSSNR_DLSSNR_AI_Filter_antiFlicker_Option_{i}"]
    for i in (1, 2, 3):
        assert entries[f"EffectParam_DLSSNR_DLSSNR_AI_Filter_Group_DLSSNR____Pass_{i}"] == f"DLSSNR · Pass {i}"
print("Multi Pass metadata: 50 unique keys, two contiguous processing modes, capture-interval choices 2/3/4 with default 2, retired controls absent, independent Async frame sync, adaptive/flow defaults, performance order, fixed-age/transfer choices, midpoint position, viewport and localization passed.")
