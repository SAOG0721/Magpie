"""Extract production frontend admission, copies and NVAPI marker boundaries.

GPU/OS calls use deterministic doubles. This exercises the actual renderer and
presenter code, including failure exits; it does not measure a display driver.
"""
from pathlib import Path
import sys

repo = Path(__file__).resolve().parents[1]
out = Path(sys.argv[1])
out.mkdir(parents=True, exist_ok=True)


def block(source, marker):
    start = source.index(marker)
    brace = source.index("{", start)
    end, depth = brace + 1, 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


renderer = (repo / "src/Magpie.Core/Renderer.cpp").read_text(encoding="utf-8-sig")
presenter = (repo / "src/Magpie.Core/AdaptivePresenter.cpp").read_text(encoding="utf-8-sig")
wait = (repo / "src/Magpie.Core/FramePacingWait.h").read_text(encoding="utf-8-sig")
# Keep the readiness/GPU split wired for every supported capture backend.
for name, gpu_call in (
    ("GraphicsCaptureFrameSource", "_deviceResources->GetD3DDC()->CopySubresourceRegion("),
    ("DesktopDuplicationFrameSource", "d3dDC->CopySubresourceRegion("),
    ("DwmSharedSurfaceFrameSource", "->OpenSharedResource("),
    ("GDIFrameSource", "_dxgiSurface->GetDC("),
):
    source = (repo / "src/Magpie.Core" / (name + ".cpp")).read_text(encoding="utf-8-sig")
    capture = block(source, "FrameSourceState " + name + "::_Update()")
    assert capture.index("_BeginCaptureRender();") < capture.index(gpu_call), name + " must mark before GPU work"
gate = block(wait, "class FrameLatencyGate {") + ";"
methods = "\n".join(block(presenter, marker) for marker in (
    "void AdaptivePresenter::SetReflexFrame(",
    "void AdaptivePresenter::BeginReflexRender(",
    "void AdaptivePresenter::CancelReflexRender(",
    "bool AdaptivePresenter::PrepareFrame(",
    "bool AdaptivePresenter::BeginFrame(",
))
update = block(renderer, "Renderer::FrontendBaseResult Renderer::_UpdateFrontendBase(")
render = block(renderer, "bool Renderer::_FrontendRender(")
admission = render[render.index("\tconst auto beginFrameStart"):render.index("\tFrameTrace::Scope traceDraw")]
end = block(presenter, "bool AdaptivePresenter::EndFrame(")
present = end[end.index("\t\tconst auto tracePresent"):end.index("\t\tFrameTrace::Presentation(")]
fixture = (repo / "tests/ReflexMarkerBoundaryTests.cpp").read_text(encoding="utf-8-sig")
motion = block((repo / "src/Magpie.Core/FrontendMotionTexture.h").read_text(encoding="utf-8-sig"),
               "inline HRESULT EnsureFrontendMotionTexture(").replace("ID3D11Device*", "Device*")
for name, value in {"GATE": gate, "METHODS": methods, "UPDATE": update,
                    "ADMISSION": admission, "PRESENT": present, "MOTION_TEXTURE": motion}.items():
    fixture = fixture.replace("/* " + name + " */", value)
(out / "reflex-marker-boundaries.cpp").write_text(fixture, encoding="utf-8")

# Mutation runs must fail: prove these assertions detect both reported errors.
(out / "late-copy-marker.cpp").write_text(
    fixture.replace("\t_presenter->BeginReflexRender();", ""), encoding="utf-8")
(out / "late-capacity.cpp").write_text(
    fixture.replace("\tif (!_presenter->PrepareFrame()) {", "\tif (false) {"), encoding="utf-8")
