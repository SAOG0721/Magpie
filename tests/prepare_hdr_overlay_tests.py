"""Use the production ImGui backend and submitted-frame color handoff in isolation."""
from pathlib import Path
import sys

repo = Path(__file__).resolve().parents[1]
out = Path(sys.argv[1])
out.mkdir(parents=True, exist_ok=True)
core = repo / 'src/Magpie.Core'
backend = (core / 'ImGuiBackend.cpp').read_text(encoding='utf-8-sig')
# Replace only application dependencies, not rendering code or state setup.
backend = '\n'.join(line for line in backend.splitlines() if not line.startswith('#include'))
(out / 'OverlayBackendProduction.inc').write_text(backend, encoding='utf-8')

renderer = (core / 'Renderer.cpp').read_text(encoding='utf-8-sig')
start = renderer.index('\t\tif (!stableBaseOnly && _frontendPresentedBaseTexture) {',
                       renderer.index('\tbool overlayColorChanged = false;'))
end = renderer.index('\n\t\t\t_TryCompleteSceneScreenshot();', start)
commit = '\tbool overlayColorChanged = false;\n\tif (submitted) {\n' + renderer[start:end] + '\n\t\t}\n\t}\n'
start = renderer.index('\tif (overlayColorChanged) ++_overlayActionRevision;', end)
end = renderer.index('\n\treturn submitted;', start)
(out / 'OverlaySubmitProduction.inc').write_text(commit + renderer[start:end], encoding='utf-8')

def body(signature):
    start = renderer.index(signature)
    end = renderer.index('{', start) + 1
    depth = 1
    while depth:
        depth += (renderer[end] == '{') - (renderer[end] == '}')
        end += 1
    return renderer[start:end]

# The integration assertions guard the metadata/target choice that is outside
# the GPU fixture. The submitted-state behavior itself is executed below.
normal = body('bool Renderer::_FrontendRender(')
independent = body('bool Renderer::_FrontendOverlayRender(')
assert 'const auto& sourceMetadata = stableBaseOnly ? _frontendPresentedFrameMetadata : _frontendFrameMetadata;' in normal
assert '_DrawOverlay(frameTex.get(), sourceMetadata, drawOffset);' in normal
assert '_DrawOverlay(frameTex.get(), _frontendPresentedFrameMetadata, drawOffset);' in independent
draw = body('void Renderer::_DrawOverlay(')
assert 'target->GetDesc(&desc)' in draw and 'ResolveOverlayWhiteScale(hdrOutput, desc.Format, frame,' in draw
assert '_runtimeEffectOptions' not in draw and '_pipelineOutputMetadata' not in draw
assert 'monitor = MonitorFromWindow(window.Handle(),' in draw
assert 'if (!_overlayWhiteMonitor || _overlayWhiteMonitor != monitor)' in draw

# Same production style scope as OverlayDrawer::Draw; test steady HDR/SDR
# panels with actual ImGui draw lists and the production GPU backend.
drawer = (core / 'OverlayDrawer.cpp').read_text(encoding='utf-8-sig')
start = drawer.index('\t_isHdrOverlay = hdrWhiteScale > 0;')
end = drawer.index('\n\tif (!_isEffectParametersVisible', start)
style = drawer[start:end]
(out / 'OverlayStyleProduction.inc').write_text(style, encoding='utf-8')
print('PASS: production backend/style/submit extraction and frontend bindings')
