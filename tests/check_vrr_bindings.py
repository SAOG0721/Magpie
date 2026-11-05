"""Check VRR integration boundaries without compiling or running Magpie.

These source/XML checks do not establish C++ compilation, driver behavior,
hardware eligibility, G-SYNC activation, input behavior, or performance.
"""
from pathlib import Path
import re
import xml.etree.ElementTree as ET

repo = Path(__file__).resolve().parents[1]


def read(path):
    return (repo / path).read_text(encoding="utf-8-sig")


xaml = ET.fromstring(read("src/Magpie/ProfilePage.xaml"))
x = "{http://schemas.microsoft.com/winfx/2006/xaml}"
group = next(node for node in xaml.iter() if node.get(x + "Name") == "FrameRefreshGroup")
card = next(node for node in group if node.get(x + "Uid") == "FrameRefresh_VRR")
assert "Visibility" not in card.attrib, "VRR is shared by both modes"
assert not any(node.get(x + "Uid") == "FrameRefresh_Advanced" for node in group.iter())
toggle = next(node for node in card.iter() if node.get(x + "Uid") == "FrameRefresh_VRR_Input")
assert toggle.get("IsOn") == "{x:Bind ViewModel.IsVRREnabled, Mode=TwoWay}"
assert toggle.get(x + "Uid") == "FrameRefresh_VRR_Input"
assert "Boolean IsVRREnabled;" in read("src/Magpie/ProfileViewModel.idl")
assert "_data->frameRefresh.vrrEnabled" in read("src/Magpie/ProfileViewModel.cpp")
model = read("src/Magpie.Core/include/FrameRefreshSettings.h")
assert "bool vrrEnabled = false;" in model
assert "MERGE_REFRESH(vrrEnabled)" in model
assert "options.isVRREnabled = effective.vrrEnabled;" in model
serialization = read("src/Magpie/ProfileFrameRefresh.h")
assert 'boolean("vrrEnabled", result.vrrEnabled);' in serialization
assert 'writer.Key("vrrEnabled"); writer.Bool(s.vrrEnabled);' in serialization
assert '"vrr"' not in serialization, "Dormant global field was migrated"
service = read("src/Magpie/ScalingService.cpp")
assert "options.isVRREnabled = false;" not in service
assert "ApplyFrameRefreshSettings(options, profile.frameRefresh, options.isSimpleFrameRefresh)" in service
for lang in ("en-US", "zh-Hans", "zh-Hant"):
    nodes = ET.fromstring(read(f"src/Magpie/Resources.language-{lang}.resw")).findall("data")
    values = {node.get("name"): node.findtext("value") for node in nodes}
    assert len(nodes) == len(values), f"Duplicate resources: {lang}"
    for key in ("FrameRefresh_VRR.Header", "FrameRefresh_VRR.Description",
                "FrameRefresh_VRR_Input.OffContent", "FrameRefresh_VRR_Input.OnContent",
                "FrameRefresh_VRR_Input.[using:Windows.UI.Xaml.Automation]AutomationProperties.Name"):
        assert values[key], (lang, key)

for name in ("AdaptivePresenter", "XeSSFGPresenter"):
    source = read(f"src/Magpie.Core/{name}.cpp")
    assert "_vrr.CreationFlags()" in source and "_vrr.ResizeFlags()" in source
    assert "_PresentWithVRR(" in source
    assert "DXGI_SWAP_EFFECT_FLIP_DISCARD" in source
    assert "DXGI_MWA_NO_ALT_ENTER" in source
    assert "DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT" in source
    assert "DXGI_PRESENT_ALLOW_TEARING" not in source, "Presenter bypasses shared policy"
    assert not re.search(r"\.Flags\s*=.*isVRREnabled", source), "Mutable request used as immutable swap-chain flags"
base = read("src/Magpie.Core/PresenterBase.cpp")
assert "flags && hr == DXGI_ERROR_INVALID_CALL" in base
assert "_vrr.RejectPresent();" in base
assert "active=unknown vendorCompatibilityRequested=" in base
assert "_PresentWithVRR(impl.swapChain.get(), false)" in read("src/Magpie.Core/XeSSFGPresenter.cpp")
assert "_PresentWithVRR(_dxgiSwapChain.get(), true)" in read("src/Magpie.Core/AdaptivePresenter.cpp")

ns = "{http://schemas.microsoft.com/developer/msbuild/2003}"
project = ET.fromstring(read("src/Magpie.Core/Magpie.Core.vcxproj"))
filters = ET.fromstring(read("src/Magpie.Core/Magpie.Core.vcxproj.filters"))
for root in (project, filters):
    assert sum(node.get("Include") == "VRRPresentationPolicy.h" for node in root.iter(ns + "ClInclude")) == 1

print("VRR model/UI/localization/project/presentation source contracts passed (no compilation or hardware proof).")
