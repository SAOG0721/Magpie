"""Extract the real cursor relocation/clip/lifetime methods, without injecting input.

The native fixture uses a real WinRT dispatcher timer and background/foreground
coroutine transitions. USER32 clip, position and source windows are test doubles.
"""
from pathlib import Path
import re
import sys

repo = Path(__file__).resolve().parents[1]
output = Path(sys.argv[1]).resolve()
output.mkdir(parents=True, exist_ok=True)
source = (repo / "src/Magpie.Core/CursorManager.cpp").read_text(encoding="utf-8-sig")


def block(text, signature):
    start = text.index(signature)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


signatures = (
    "static POINT SrcToScaling(", "enum class RoundMethod", "static POINT ScalingToSrc(",
    "CursorManager::CursorManager(", "CursorManager::~CursorManager(",
    "void CursorManager::_ReliableSetCursorPos(",
    "winrt::DispatcherQueueTimer CursorManager::_CreateCursorRepositionTimer(",
    "bool CursorManager::ReleasePendingCursorReposition(",
    "bool CursorManager::_FinishCursorReposition(",
    "bool CursorManager::_FinishCursorRepositionForShutdown(",
    "void CursorManager::_CompleteCursorReposition(",
    "bool CursorManager::_DeferClipCursor(",
    "winrt::fire_and_forget CursorManager::_RefreshCursorShapeAsync(",
    "bool CursorManager::_ReliableSetCursorPosOnDestroy(",
    "void CursorManager::_SetClipCursor(", "void CursorManager::_RestoreClipCursor(",
    "void CursorManager::_StartCapture(", "bool CursorManager::_StopCapture(",
)
(output / "CursorRepositionProduction.inc").write_text(
    "\n\n".join(block(source, s) + (";" if s.startswith("enum ") else "") for s in signatures), encoding="utf-8"
)

# Active presentation paths retain their coordinate conversion before relocation;
# only the destruction-only helper may sleep or perform a synchronous hit test.
for signature in ("void CursorManager::_ReliableSetCursorPos(",
                  "void CursorManager::_UpdateCursorState(",
                  "bool CursorManager::_UpdateParameterCursor(",
                  "void CursorManager::OnScalingPosChanged("):
    body = block(source, signature)
    assert "Sleep(" not in body and "AdvancedWindowHitTest(" not in body, signature
    assert "_ReliableSetCursorPosOnDestroy(" not in body, signature
relocate = block(source, "void CursorManager::_ReliableSetCursorPos(")
assert relocate.index("_CreateCursorRepositionTimer(id)") < relocate.index("ClipCursor(&pointClip)")
assert relocate.index("ClipCursor(&pointClip)") < relocate.index("_cursorReposition.emplace(")
header = (repo / "src/Magpie.Core/CursorManager.h").read_text(encoding="utf-8-sig")
assert "_lifetime->RequestStop();" in block(header, "void BeginShutdown(")
window = (repo / "src/Magpie.Core/ScalingWindow.cpp").read_text(encoding="utf-8-sig")
shutdown = block(window, "void ScalingWindow::_BeginDestroy(")
assert shutdown.index("BeginShutdown();") < shutdown.index("TrySetScalingWindowOwner(")
assert shutdown.index("if (result.IsApplied())") < shutdown.index("ReleasePendingCursorReposition();")
complete = block(window, "bool ScalingWindow::CompletePendingDestroy(")
assert complete.index("_retirement.Ready(") < complete.index("ReleasePendingCursorReposition()") < complete.index("_FinishDestroy();")
(output / "CursorRetirementProduction.inc").write_text(complete, encoding="utf-8")
runtime = (repo / "src/Magpie.Core/ScalingRuntime.cpp").read_text(encoding="utf-8-sig")
runtime_header = (repo / "src/Magpie.Core/include/ScalingRuntime.h").read_text(encoding="utf-8-sig")
(output / "RuntimeQuitTypes.inc").write_text(block(runtime_header, "enum class ScalingState") + ";", encoding="utf-8")
quit_request = block(runtime, "auto requestQuit =") + ";"
retirement_gate = runtime[runtime.index("// Parameter callbacks only queue changes."):runtime.index("scalingWindow.PollSourceState();")]
assert retirement_gate.index("IsRetiring()") < retirement_gate.index("if (quitRequested)")
assert runtime.count("if (msg.message == WM_QUIT)") == runtime.count("requestQuit();") == 3
# Preserve the production quit/retirement decisions inside a bool fixture call.
retirement_gate = retirement_gate.replace("continue;", "return false;").replace("return;", "return true;")
(output / "CursorQuitProduction.inc").write_text(quit_request, encoding="utf-8")
(output / "CursorQuitGateProduction.inc").write_text(retirement_gate + "\nreturn false;", encoding="utf-8")

# Exercise the real command lambdas through the real message-dispatch prefix,
# including cleanup becoming ready inside a dispatched application callback.
(output / "RuntimeQuitCommandsProduction.inc").write_text("\n\n".join(block(runtime, signature) for signature in (
    "bool ScalingRuntime::Start(", "void ScalingRuntime::ToggleScaling(",
    "bool ScalingRuntime::RestartWithEffectParameters(")), encoding="utf-8")
loop_start = runtime.index("\twhile (true) {\n\t\tscalingWindow.CompletePendingDestroy();") + len("\twhile (true) {\n")
loop_prefix = runtime[loop_start:runtime.index("\t\tscalingWindow.PollSourceState();", loop_start)]
loop_prefix = loop_prefix.replace("continue;", "return false;").replace("return;", "return true;")
(output / "RuntimeQuitMessagePumpProduction.inc").write_text(
    quit_request + "\n" + block(runtime, "auto peekPriorityMessage =") + ";\n" + loop_prefix + "\nreturn false;", encoding="utf-8")
start = block(window, "void ScalingWindow::Start(")
(output / "RuntimeWindowStartAdmission.inc").write_text(start[start.index("{") + 1:start.index("\tassert(!Handle());")], encoding="utf-8")

events = block((repo / "src/Magpie.Core/FrameTrace.h").read_text(encoding="utf-8-sig"), "enum class Event")
events = [e.strip() for e in events[events.index("{") + 1:-1].split(",") if e.strip() != "Count"]
trace = (repo / "src/Magpie.Core/FrameTrace.cpp").read_text(encoding="utf-8-sig")
names = re.findall(r'"([A-Za-z]+)"', trace[trace.index("NAMES{"):trace.index("};", trace.index("NAMES{"))])
assert names == events, "FrameTrace event/name correspondence broken"
print(f"Extracted {len(signatures)} production cursor methods; lifecycle and {len(events)} trace bindings checked.")
