"""Check startup feedback ordering and existing cancellation/focus contracts."""
from pathlib import Path
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]


def read(path):
    return (ROOT / path).read_text(encoding="utf-8-sig")


runtime = read("src/Magpie.Core/ScalingRuntime.cpp")
start = runtime.split("bool ScalingRuntime::Start(", 1)[1].split(
    "void ScalingRuntime::ToggleScaling(", 1)[0]
notice = start.index("options.showToast(hwndSrc, message)")
initialize = start.index("scalingWindow.Start(hwndSrc, std::move(options))")
assert start.index("_State(ScalingState::Starting)") < notice < initialize
assert start.index("dispatcher.TryEnqueue") < notice
assert start.index("scalingWindow.Stop()") < notice
assert start.rfind("_commandGeneration.load", 0, notice) > start.index("scalingWindow.Stop()")
assert start.index("catch (...)") < initialize, "A failed notice must not abort initialization"
assert "Sleep(" not in start and "sleep_for(" not in start

service = read("src/Magpie/ScalingService.cpp")
cancel = service.split("else if (state == ScalingState::Starting)", 1)[1].split(
    "else if (state == ScalingState::Stopping)", 1)[0]
assert "_scalingRuntime->Stop();" in cancel

toast = read("src/Magpie/ToastPage.cpp")
assert "SWP_NOACTIVATE" in toast and "SWP_NO_ACTIVATE_MOVE_SIZE" in toast
assert "SetForegroundWindow(" not in toast and ".Focus(" not in toast and "SetFocus(" not in toast

for locale in ("en-US", "zh-Hans", "zh-Hant"):
    resources = ET.fromstring(read(f"src/Magpie/Resources.language-{locale}.resw"))
    values = [node.findtext("value") for node in resources.findall("data")
              if node.attrib.get("name") == "Message_ScalingPreparing"]
    assert len(values) == 1 and values[0], f"Missing or duplicate preparation message: {locale}"

print("PASS startup notice follows generation admission, precedes initialization, preserves cancellation and uses the non-activating toast host; three locales present")
