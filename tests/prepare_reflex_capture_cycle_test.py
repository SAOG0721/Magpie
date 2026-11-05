"""Exercise production capture wiring and guards with the real ReflexController.

Only source acquisition and the NVAPI function itself are deterministic doubles.
The native Sleep wrapper is extracted too, to verify its real-call trace scope.
"""
from pathlib import Path
import sys

repo = Path(__file__).resolve().parents[1]
out = Path(sys.argv[1])
out.mkdir(parents=True, exist_ok=True)


def block(source, marker):
    begin = source.index(marker)
    brace = source.index("{", begin)
    end, depth = brace + 1, 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[begin:end]


renderer = (repo / "src/Magpie.Core/Renderer.cpp").read_text(encoding="utf-8-sig")
renderer_header = (repo / "src/Magpie.Core/Renderer.h").read_text(encoding="utf-8-sig")
source = (repo / "src/Magpie.Core/FrameSourceBase.cpp").read_text(encoding="utf-8-sig")
source_header = (repo / "src/Magpie.Core/FrameSourceBase.h").read_text(encoding="utf-8-sig")
driver = (repo / "src/Magpie.Core/ReflexController.cpp").read_text(encoding="utf-8-sig")
trace = (repo / "src/Magpie.Core/FrameTrace.cpp").read_text(encoding="utf-8-sig")
trace_start = block(trace, "bool Start() noexcept")
native_stages_init = "session->nativeNrStages = NativeNrStagesRequested();"
assert trace_start.count(native_stages_init) == 1
assert trace_start.index(native_stages_init) < trace_start.index("lane = &session->frontend;")
assert trace.count("NativeNrStagesRequested();") == 1
init = block(renderer, "bool Renderer::_InitFrameSource()")
begin = init.index("\tconst bool inputValidatedReflex =")
end = init.index("\n\tif (ScalingWindow::Get().Options().IsHdrCompatibilityEnabled()", begin)
wiring = init[begin:end]

# Check the real initialization order, not a fixture's pre-populated container.
backend = block(renderer, "HANDLE Renderer::_InitBackend()")
assert backend.index("_runtimeEffectOptions =") < backend.index("_InitFrameSource()")
assert backend.index("_InitFrameSource()") < backend.index("_BuildEffects()")
frontend = block(renderer, "ScalingError Renderer::Initialize(")
assert frontend.index("ValidateFrameGenerationChain(effects)") < frontend.index(
    "_hasFrameGeneration = frameGeneration.HasFrameGeneration()")
assert frontend.index("_hasFrameGeneration = frameGeneration.HasFrameGeneration()") < frontend.index("_InitBackend")
assert "_nativeEffectBackends" not in wiring

discard = block(renderer, "\t\tconst auto discardReflexRender =") + ");"
draw_start = "\tif (isNewCaptureFrame) _reflex.BeginCaptureRender();"
render = block(renderer, "void Renderer::_BackendRender(")
assert draw_start in render
accepted = block(render, "\tif (isNewCaptureFrame) {\n\t\tconst auto captureTime")
begin = accepted.index("\t\t_previousCapturedFrameId =")
end = accepted.index("\n\t\tFrameTrace::SetFrame", begin)
accept_identity = accepted[begin:end]
begin = render.index("\t_processedFrame = {};")
end = render.index("\n\tconst size_t preEnd", begin)
processed = render[begin:end]
begin = render.index("\t\t\t\t\t_processedFrame.frameId = target.frameId;")
end = render.index("\n\t\t\t\t}", begin)
redirect = render[begin:end]
begin = renderer_header.index("\tFrameGuidanceFrameId _capturedFrameId =")
end = renderer_header.index("\n\tNgxD3D12Core", begin)
capture_fields = renderer_header[begin:end]
assert renderer.count(".captureReflexFrameId = _processedFrame.reflexFrameId") == 2
complete = block(renderer, "void Renderer::_CompleteBackendFrame(")
begin = complete.index("\tif (isNewCaptureFrame) _reflex.EndCaptureRender();")
end = complete.index("\n\t});", begin) + len("\n\t});")
finish = complete[begin:end]
loop = block(renderer, "void Renderer::_BackendThreadProc()")
interrupted = loop[loop.index("\t\tcase FrameSourceState::Waiting:"):]
interrupted = interrupted[interrupted.index("\t\t\tif (_frameSource->IsCaptureInterrupted())"):]
assert "_reflex.CompleteCapture();" in block(interrupted, "\t\t\tif (_frameSource->IsCaptureInterrupted())")
assert "_reflex.CompleteCapture();" in loop[loop.index("\t\tcase FrameSourceState::Error:"):]
assert "_reflex.CompleteCapture();" in loop[:loop.index("FrameTrace::BindBackend()")]
backend_exit = block(loop, "\tconst auto finishReflexCapture =") + ");"

fixture = (repo / "tests/ReflexCaptureCycleTests.cpp").read_text(encoding="utf-8-sig")
parts = {
    "WIRING": wiring,
    "CAPTURE_HOOK": block(source, "void FrameSourceBase::_BeginCaptureRender()"),
    "CAPTURE_SETTER": block(source_header, "\tvoid SetReflexController("),
    "DISCARD": discard,
    "DRAW_START": draw_start,
    "ACCEPT_IDENTITY": accept_identity,
    "CAPTURE_FIELDS": capture_fields,
    "PROCESSED_DEFINITION": block(renderer_header, "\tstruct ProcessedFrame {") + ";",
    "PROCESSED_FRAME": processed,
    "REDIRECT_OUTPUT": redirect,
    "FINISH": finish,
    "BACKEND_EXIT": backend_exit,
    "SLEEP": block(driver, "\tint Sleep() noexcept override"),
    "NATIVE_STAGES_REQUEST": block(trace, "bool NativeNrStagesRequested() noexcept"),
    "NATIVE_STAGES_INIT": native_stages_init,
    "NATIVE_STAGES_ENABLED": block(trace, "bool NativeNrStagesEnabled() noexcept"),
}
for name, text in parts.items():
    fixture = fixture.replace("/* " + name + " */", text)
(out / "reflex-capture-cycle.cpp").write_text(fixture, encoding="utf-8")

mutations = {
    "legacy-capture-hook": fixture.replace(
        "inputValidatedReflex ? nullptr : &_reflex", "&_reflex"),
    "broaden-nr-scope": fixture.replace(
        'return effect.name == "DLSSNR\\\\DLSSNR_AI_Filter";', "return !effect.name.empty();"),
    "broaden-fg-scope": fixture.replace("_reflex.CanResume() && !_hasFrameGeneration &&", "_reflex.CanResume() &&"),
    "future-candidate-identity": fixture.replace(
        "_processedFrame.reflexFrameId = _acceptedCaptureReflexFrameId;",
        "_processedFrame.reflexFrameId = _reflex.CaptureFrameId();"),
    "prior-output-identity": fixture.replace("\t_processedFrame = {};", "\tconst auto previousOutputReflexId = _processedFrame.reflexFrameId;\n\t_processedFrame = {};").replace(
        "_processedFrame.reflexFrameId = _acceptedCaptureReflexFrameId;",
        "_processedFrame.reflexFrameId = isNewCaptureFrame ? _acceptedCaptureReflexFrameId : previousOutputReflexId;"),
}
for name, text in mutations.items():
    assert text != fixture, name
    (out / (name + ".cpp")).write_text(text, encoding="utf-8")
