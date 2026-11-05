"""Run actual presenter/renderer methods against deterministic OS/GPU doubles.

This does not build Magpie or establish driver/Smooth Motion compatibility.
"""
from pathlib import Path
import sys

repo = Path(__file__).resolve().parents[1]
out = Path(sys.argv[1])
out.mkdir(parents=True, exist_ok=True)


def read(path):
    return (repo / path).read_text(encoding="utf-8-sig")


def block(source, marker):
    start = source.index(marker)
    brace = source.index("{", start)
    end, depth = brace + 1, 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


presenter = read("src/Magpie.Core/AdaptivePresenter.cpp")
header = read("src/Magpie.Core/AdaptivePresenter.h")
renderer = read("src/Magpie.Core/Renderer.cpp")
methods = "\n".join(block(presenter, marker) for marker in (
    "static bool ShouldLogPresentationDiagnostic(",
    "void AdaptivePresenter::SetReflexController(",
    "void AdaptivePresenter::SetReflexFrame(",
    "void AdaptivePresenter::BeginReflexRender(",
    "void AdaptivePresenter::CancelReflexRender(",
    "bool AdaptivePresenter::PrepareFrame(",
    "bool AdaptivePresenter::BeginFrame(",
    "bool AdaptivePresenter::WaitForFrameCapacity(",
    "bool AdaptivePresenter::EndFrame(",
    "bool AdaptivePresenter::OnResize(",
    "void AdaptivePresenter::OnEndResize(",
    "bool AdaptivePresenter::_ResizeSwapChain(",
    "bool AdaptivePresenter::NeedsRedraw(",
    "void AdaptivePresenter::_TryRecoverSwapChain(",
    "void AdaptivePresenter::_TryCompleteSwapChainSwitch(",
    "void AdaptivePresenter::_KeepDCompFallback(",
    "void AdaptivePresenter::_Fail(",
    "bool AdaptivePresenter::_CheckDeviceFailure(",
))
frontend = "\n".join(block(renderer, marker) for marker in (
    "bool Renderer::Render(", "bool Renderer::RenderOverlay(",
    "bool Renderer::_CanRenderOverlay(",
))
gate = block(read("src/Magpie.Core/FramePacingWait.h"), "class FrameLatencyGate {") + ";"
definition = block(header, "class AdaptivePresenter final") + ";"
definition = definition.replace("protected:", "public:").replace("private:", "public:").replace(" override", "")
fixture = read("tests/AdaptivePresenterRecoveryTests.cpp")
for name, value in {"GATE": gate, "CLASS": definition, "METHODS": methods, "FRONTEND": frontend}.items():
    fixture = fixture.replace("/* " + name + " */", value.replace("std::chrono::steady_clock", "TestClock"))
(out / "recovery.cpp").write_text(fixture, encoding="utf-8")

# Negative runs verify assertions catch early detach, retained bindings, and
# missing static-source retries. Mutation targets must still exist exactly once.
for name, before, after in (
    ("early-detach", "if (presentResult == S_OK) {", "if (true) {"),
    ("bound-buffer", "_deviceResources->GetD3DDC()->ClearState();", "/* missing unbind */"),
    ("no-retry", "if (!NeedsRedraw()) return;", "return;"),
    ("no-static-redraw", "&& !_presenter->NeedsRedraw()", ""),
):
    assert before in fixture, name
    (out / (name + ".cpp")).write_text(fixture.replace(before, after), encoding="utf-8")

# Admission is also covered by the existing marker fixture. These guards bind
# recovery to that actual pre-copy call and the existing outer message pump.
admission = block(renderer, "bool Renderer::_FrontendRender(")
assert admission.index("_presenter->PrepareFrame()") < admission.index("_UpdateFrontendBase(")
assert "_ReportPresenterFailure();" in block(admission, "if (!_presenter->PrepareFrame())")
runtime = read("src/Magpie.Core/ScalingRuntime.cpp")
assert "else if (rest.count() <= 0)" in runtime and "scalingWindow.Render();" in runtime
assert "return false;" in block(read("src/Magpie.Core/PresenterBase.h"), "virtual bool NeedsRedraw()")
init = block(presenter, "bool AdaptivePresenter::_Initialize(")
assert "return true;" in block(init, "if (ScalingWindow::Get().Options().IsDirectFlipDisabled())")
assert "DXGI_FORMAT_R16G16B16A16_FLOAT" in init and "DXGI_FORMAT_R8G8B8A8_UNORM" in init
assert "std::make_unique<AdaptivePresenter>()" in renderer
print("Extracted production recovery, capacity gate, frontend idle routing and default-off presenter contract.")
