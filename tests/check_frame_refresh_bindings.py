"""Check saved-model bindings, localization and frontend scheduling boundaries.

This is a source contract check, not a native UI or GPU acceptance test.
"""
from pathlib import Path
import re
import xml.etree.ElementTree as ET

repo = Path(__file__).resolve().parents[1]
def read(path):
    return (repo / path).read_text(encoding="utf-8-sig")

x = "{http://schemas.microsoft.com/winfx/2006/xaml}"
local = "{using:Magpie}"
profile = ET.fromstring(read("src/Magpie/ProfilePage.xaml"))
group = next(n for n in profile.iter() if n.get(x + "Name") == "FrameRefreshGroup")
uids = {n.get(x + "Uid"): n for n in group.iter() if n.get(x + "Uid")}
sections = [n for n in group if n.tag in (local+"SettingsExpander", local+"SettingsCard")]
assert [n.get(x+"Uid") for n in sections] == ["FrameRefresh_RateGroup", "Rtss_Takeover", "FrameRefresh_StaticGroup", "FrameRefresh_Cursor", "FrameRefresh_VRR"]
assert sections[3].get("Visibility") == "{x:Bind ViewModel.IsAdvancedFrameRefresh, Mode=OneWay}"
assert "Visibility" not in sections[1].attrib and "Visibility" not in sections[4].attrib
for section in sections:
    assert any(n.tag.endswith(".HeaderIcon") and len(n) == 1 for n in section), "group icon missing"
for mode, number in (("FrameRefresh_Content_Input", "FrameRefresh_ContentRate_Input"),
                     ("FrameRefresh_Idle_Input", "FrameRefresh_IdleRate_Input"),
                     ("Rtss_Mode_Input", "Rtss_CustomRate_Input"),
                     ("FrameRefresh_CursorSupplement_Input", "FrameRefresh_CursorRate_Input")):
    pair = next(n for n in group.iter() if uids[mode] in list(n))
    assert pair.tag.endswith("Grid") and uids[number] in list(pair.iter()), "mode and rate share one responsive editor"
    assert pair.get("Loaded") == "PairEditor_Loaded" and pair.get("SizeChanged") == "PairEditor_SizeChanged"
header_actions = next(n for n in group.iter() if n.get(x+"Name") == "FrameRefreshHeaderActions")
assert uids["FrameRefresh_Mode_Input"] in list(header_actions.iter())
assert uids["FrameRefresh_ResetButton"] in list(header_actions), "reset belongs beside mode selection"
assert "Visibility" not in uids["FrameRefresh_ResetButton"].attrib, "reset is available in both modes"
assert uids["FrameRefresh_Mode_Input"].tag.endswith("ListBox") and uids["FrameRefresh_Mode_Input"].get("SelectionMode") == "Single"
mode_style = next(n for n in profile.iter() if n.get(x+"Key") == "FrameRefreshModeItemStyle")
states = {n.get(x+"Name") for n in mode_style.iter() if n.tag.endswith("VisualState")}
assert {"Selected", "SelectedUnfocused", "SelectedPointerOver", "SelectedPressed", "Disabled"} <= states
assert "FrameRefresh_Advanced" not in uids
assert "FrameRefresh_Mode" not in uids
assert [c.get(x+"Uid") for c in uids["FrameRefresh_Content_Input"]] == ["FrameRefresh_Source", "FrameRefresh_Custom"]
for uid, prop in {"FrameRefresh_Pacing":"ShowContentPacing", "FrameRefresh_CursorSupplement":"ShowCursorSupplement"}.items():
    assert uids[uid].get("Visibility") == f"{{x:Bind ViewModel.{prop}, Mode=OneWay}}"
for uid, prop in {"FrameRefresh_ContentRate_Input":"ShowContentFrameRate", "FrameRefresh_IdleRate_Input":"ShowIdleRedrawRate", "FrameRefresh_CursorRate_Input":"ShowCursorSupplementRate", "Rtss_CustomRate_Input":"ShowRtssCustomRate"}.items():
    parent = next(n for n in group.iter() if uids[uid] in list(n))
    assert parent.get("Visibility") == f"{{x:Bind ViewModel.{prop}, Mode=OneWay}}"
assert all(n.get("IsWrapEnabled") == "True" for n in group.iter(local+"SettingsCard"))
for n in group.iter("{using:Microsoft.UI.Xaml.Controls}NumberBox"):
    assert n.get("Minimum") == "15" and n.get("Maximum") == "360"
    assert n.get("SmallChange") == "1" and "IntegerFormatter" in n.get("NumberFormatter", "")
# The shared page layout must preserve the original non-refresh controls too.
page_uids = {n.get(x+"Uid"): n for n in profile.iter() if n.get(x+"Uid")}
number_style = next(n for n in profile.iter() if n.get(x+"Key") == "ProfileNumberBoxStyle")
assert any(n.get("Property") == "SpinButtonPlacementMode" and n.get("Value") == "Hidden" for n in number_style)
numbers = list(profile.iter("{using:Microsoft.UI.Xaml.Controls}NumberBox"))
assert len(numbers) == 10
assert all(n.get("Loaded") == "NumberBox_Loaded" and n.get("Style") == "{StaticResource ProfileNumberBoxStyle}" for n in numbers)
for direction in ("Left", "Right", "Top", "Bottom"):
    n = page_uids[f"Profile_Crop{direction}_Input"]
    assert n.get("Value") == f"{{x:Bind ViewModel.Cropping{direction}, Mode=TwoWay}}"
    assert n.get("Minimum") == "0" and "DoubleFormatter" in n.get("NumberFormatter", "")
for uid, minimum in (("Profile_InitialScale_Input", "1"), ("Profile_CursorScale_Input", "0.1")):
    assert page_uids[uid].get("Minimum") == minimum and page_uids[uid].get("Maximum") == "10"
    assert page_uids[uid].get("SmallChange") == "0.1"
launch = page_uids["Profile_Advanced_LaunchParameters"]
assert launch.get("ContentAlignment") == "Vertical" and "IsNotDefaultProfile" in launch.get(x+"Load", "")
assert page_uids["Profile_SourceWindow_CaptureTitleBar"].get("IsEnabled") == "{x:Bind ViewModel.CanCaptureTitleBar, Mode=OneWay}"
assert "HasMultipleMonitors" in page_uids["Profile_General_Multimonitor"].get(x+"Load", "")
page_cpp = read("src/Magpie/ProfilePage.cpp")
assert "SettingsCardContentMinWidth" not in page_cpp, "scale selection must not restore the old global width"
assert "ControlHelper::NumberBox_Loaded(sender)" in page_cpp and "ControlHelper::ComboBox_DropDownOpened(sender)" in page_cpp
home = read("src/Magpie/HomePage.xaml")
assert "MinFrameRateIndex" not in home and "DefaultFrameRefreshSummary" in home and "EditDefaultFrameRefresh" in home
assert "Profile_Cursor_Refresh" not in read("src/Magpie/ProfilePage.xaml")
assert "Profile_Performance_FrameRateLimiter" not in read("src/Magpie/ProfilePage.xaml")
idl = read("src/Magpie/ProfileViewModel.idl")
header = read("src/Magpie/ProfileViewModel.h")
vm = read("src/Magpie/ProfileViewModel.cpp")
names = set(re.findall(r"ViewModel\.([A-Za-z]+)", ET.tostring(group, encoding="unicode")))
for name in names:
    assert name in idl and name in header and "ProfileViewModel::" + name + "(" in vm, name
    if name not in ("ResetFrameRefresh", "RetryRtss", "RefreshRtss", "LaunchRtss", "LaunchAfterburner", "InitializeMagpieRtss"):
        assert f'L"{name}"' in vm, name
fallback_root = ET.fromstring(read("src/Magpie/Resources.language-en-US.resw"))
fallback = {n.get("name"): n.findtext("value") for n in fallback_root.findall("data")}
for file in (repo / "src/Magpie").glob("Resources.language-*.resw"):
    lang = file.stem.removeprefix("Resources.language-")
    root = ET.fromstring(read(f"src/Magpie/Resources.language-{lang}.resw"))
    resources = {n.get("name"): n.findtext("value") for n in root.findall("data")}
    assert len(resources) == len(root.findall("data")), lang
    assert all(k in resources for k in ("FrameRefresh_Mode.Header", "FrameRefresh_Mode.Description", "FrameRefresh_Mode_Simple.Content", "FrameRefresh_Mode_Advanced.Content"))
    resources = fallback | resources
    for uid in ("Profile_InitialScale_Input", "Profile_CursorScale_Input", "Profile_CropLeft_Input", "Profile_CropRight_Input", "Profile_CropTop_Input", "Profile_CropBottom_Input"):
        assert resources[uid + ".[using:Windows.UI.Xaml.Automation]AutomationProperties.Name"], (lang, uid)
    for n in group.iter():
        uid = n.get(x + "Uid", "")
        if not uid:
            continue
        if n.tag in (local + "SettingsCard", local + "SettingsExpander", local + "SettingsGroup"):
            assert resources[uid + ".Header"], (lang, uid)
        elif n.tag.endswith("ToggleSwitch"):
            assert resources[uid + ".OffContent"] and resources[uid + ".OnContent"], (lang, uid)
            assert resources[uid + ".[using:Windows.UI.Xaml.Automation]AutomationProperties.Name"], (lang, uid)
        elif n.tag.endswith("ToggleButton"):
            assert resources[uid + ".[using:Windows.UI.Xaml.Automation]AutomationProperties.Name"], (lang, uid)
        elif n.tag.endswith(("NumberBox", "ComboBox", "ListBox")):
            assert resources[uid + "_Input.[using:Windows.UI.Xaml.Automation]AutomationProperties.Name" if not uid.endswith("_Input") else uid + ".[using:Windows.UI.Xaml.Automation]AutomationProperties.Name"], (lang, uid)
        else:
            assert resources.get(uid + ".Content") or resources.get(uid + ".Text") or resources.get(uid + ".Header"), (lang, uid)
    for name in ("Home_FrameRefresh_Summary", "Home_FrameRefresh_Edit.Content", "FrameRefresh_RestartNotice", "FrameRefresh_LegacyNotice"):
        assert resources[name], (lang, name)
renderer = read("src/Magpie.Core/Renderer.cpp")
assert re.search(r"const bool paced = frontEdgePacing &&\s+\(\(!stableBaseOnly && IsContentUpdate\(updateReason\)\)\s*\|\|\s*\(stableBaseOnly && idleRedraw && vrrCaptureRefresh\)\);", renderer)
assert re.search(r"else if \(!stableBaseOnly && !frontEdgePacing\) \{\s+_frontEdgeClock\.Reset\(\);", renderer)
assert re.search(r"if \(submitted && \(contentFrame \|\| \(stableBaseOnly && idleRedraw && _UsesVrrCaptureRefresh\(\)\)\) &&\s+ActiveFrameSyncBackend\(\) == FrameSyncBackend::FrontEdge", renderer)
assert re.search(r"!stableBaseOnly && _frontendPresentedBaseValid && overlayPending &&\s+_FrontendRender\(false, sharedTextureSlot, nullptr, true\);", renderer)
window = read("src/Magpie.Core/ScalingWindow.cpp")
service = read("src/Magpie/ScalingService.cpp")
assert "ApplyFrameRefreshSettings(_options, frameRefresh, _options.isSimpleFrameRefresh)" in window
assert "ApplyFrameRefreshSettings(options, profile.frameRefresh, options.isSimpleFrameRefresh)" in service
settings = read("src/Magpie/AppSettings.cpp")
assert "_LoadProfile(scaleProfilesArray[0].GetObj(), _defaultProfile, true, legacyParameterFocusSwitching, legacyFrameSync, legacyIdle, migrateLegacyRefresh)" in settings
assert "_LoadProfile(scaleProfilesArray[i].GetObj(), rule, false, legacyParameterFocusSwitching, legacyFrameSync, legacyIdle, migrateLegacyRefresh)" in settings
assert "MergeFrameRefreshSettings(mergedFrameRefresh, request.previousFrameRefresh, request.frameRefresh)" in service
assert "FindProfileByIdentity" in service and "scalingRunId" in service
overlay = read("src/Magpie.Core/OverlayDrawer.cpp")
assert "FrameRefresh_Mode" not in overlay, "Toolbar must not expose the global mode selector"
assert 'L"FrameRefresh_Source/Content", L"FrameRefresh_Custom/Content"' in overlay
assert "const bool simpleRefresh = ScalingWindow::Get().Options().isSimpleFrameRefresh;" in overlay
assert "_parameterSessionSnapshot.simpleFrameRefresh != options.isSimpleFrameRefresh" in overlay
assert "_parameterSessionSnapshot.refreshPolicyChanged" in overlay
assert "_StartScale(request.hwndSource, *frameSyncProfile, sessionOptions.IsWindowedMode(), true);" in service
assert "FrameRefreshModeChanged(auto_revoke" in service
print("Unified refresh UI/model/localization/frontend contracts passed (not native UI/GPU acceptance).")
