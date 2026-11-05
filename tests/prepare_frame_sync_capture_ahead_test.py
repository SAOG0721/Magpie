"""Extract the production bounded prefetch and check its owner-loop integration."""
from pathlib import Path
import re
import sys

repo = Path(__file__).resolve().parents[1]
output = Path(sys.argv[1]).resolve()
output.mkdir(parents=True, exist_ok=True)
source = (repo / 'src/Magpie.Core/Renderer.cpp').read_text(encoding='utf-8-sig')
header = (repo / 'src/Magpie.Core/Renderer.h').read_text(encoding='utf-8-sig')


def function(text, signature):
    start = text.index(signature)
    opening = text.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


members = '\n'.join(function(header, signature) for signature in (
    'bool _IsFrameSyncPublicationPending()', 'bool _FrameSyncBlocksCapture()'))
implementation = '\n'.join(function(source, signature) for signature in (
    'static HRESULT AcquirePresentationTextures(',
    'static HRESULT ReleasePresentationTextures(',
    'bool Renderer::_StageFrameSyncOutput(',
    'bool Renderer::_PublishPendingFrameSyncOutput('))
config_start = source.index('_frameSyncCaptureAheadEnabled = ')
config_end = source.index('\n\t\t});', config_start) + len('\n\t\t});')
configuration = source[config_start:config_end]
implementation += '\nvoid Renderer::Configure(const FrameGeneration& frameGeneration, const std::vector<Effect>& effects) {\n'
implementation += configuration + '\n}\n'

# Keep these checks tied to the actual owner loop, rather than testing a second
# handwritten scheduler that might silently lose the integration on refactor.
loop = function(source, 'void Renderer::_BackendThreadProc()')
maintenance = loop[loop.index('const auto hasReadyNativeOutput'):loop.index('\n\twhile (true)')]
assert maintenance.index('if (_pendingFrameSyncOutput) return false;') < maintenance.index('backend->ServiceAsync();')
assert loop.count('_FrameSyncBlocksCapture()') == 2
# Trace metadata may precede the wait; keep checking the actual guarded block.
capture_wait = function(loop, 'else if (!_pendingFrameSyncOutput)')
assert 'stepTimerStatus = _stepTimer.WaitForNextFrame(' in capture_wait
drain = loop.index('_PublishPendingFrameSyncOutput();')
assert loop.index('if (_sessionLifetime->IsStopping()) continue;', loop.index('traceMessages.End();')) < drain
assert drain < loop.index('_ApplyPendingEffectParameters();') < loop.index('_frameSource->Update();')
assert 'if (!_pendingFrameGenerationInput && !_pendingFrameSyncOutput)' in loop
assert 'MsgWaitForMultipleObjectsEx(consumed ? 1 : 0' in loop
assert 'if (waitedForContent) continue;' in loop
render = function(source, 'void Renderer::_BackendRender(')
assert 'if (_StageFrameSyncOutput(effectsOutput, isNewCaptureFrame)) return;\n\t_CompleteBackendFrame(' in render
resize = function(source, 'bool Renderer::OnResize()')
cancel_start = resize.index('_pendingFrameSyncOutput = nullptr;')
cancel_end = resize.index('\n\t\t_pendingFrameGenerationInput', cancel_start)
cancel = resize[cancel_start:cancel_end]
assert cancel_start < resize.index('_ResizeEffects();')
cleanup = loop[loop.index('const auto cleanupBackend'):loop.index('\n\tif (const HANDLE sharedHandle')]
assert cleanup.index('_ReleaseNgxConsumers()') < cleanup.index('_pendingFrameSyncOutput = nullptr;') < cleanup.index('_frameSource.reset();')
publish = function(source, 'bool Renderer::_PublishBackendTexture(')
assert publish.index('d3dDC->Signal(_d3dFence.get(), ++_fenceValue)') < publish.index('_fenceEvent.wait();')
assert publish.index('_fenceEvent.wait();') < publish.index('backend->ServiceAfterOutput();')
submit = function(source, 'bool Renderer::_SubmitFrontendFrame()')
assert 'if (submitted && !stableBaseOnly && _frameSyncEnabled && _frameSyncUsesSharedSlot)' in submit
assert 'if (submitStart < due)' in submit
assert '_frontEdgeClock.Submitted(_UsesVrrCaptureRefresh() ? std::chrono::steady_clock::now() :' in submit
assert '_presenter->LastSubmissionTime());' in submit

# Exercise the production admission expressions: ordinary UI/quality redraws
# must not reserve a content slot or move its deadline merely because VRR is on.
draw = function(source, 'bool Renderer::_FrontendRender(')
pacing = re.search(r'const bool paced = (.*?);', draw, re.S).group(1)
advance = re.search(r'if \((submitted && .*?) &&\s*ActiveFrameSyncBackend\(\)', submit, re.S).group(1)
def predicate(expression, **values):
    expression = expression.replace('IsContentUpdate(updateReason)', 'contentFrame')
    expression = expression.replace('_UsesVrrCaptureRefresh()', 'vrrCaptureRefresh')
    expression = expression.replace('&&', ' and ').replace('||', ' or ')
    expression = re.sub(r'!(?!=)', ' not ', expression)
    return eval(' '.join(expression.split()), {'__builtins__': {}}, values)

for stable, idle, content, vrr, expected in (
        (False, False, True, True, True),    # Fresh content.
        (False, False, True, False, True),   # Existing non-VRR content pacing.
        (True, True, False, True, True),     # Stable VRR heartbeat.
        (True, True, False, False, False),   # No heartbeat for ineligible VRR.
        (True, False, False, True, False),   # Ordinary UI/cursor redraw.
        (False, False, False, True, False)): # Quality/parameter refresh.
    values = dict(stableBaseOnly=stable, idleRedraw=idle, contentFrame=content,
                  vrrCaptureRefresh=vrr, frontEdgePacing=True, submitted=True)
    assert predicate(pacing, **values) == expected
    assert predicate(advance, **values) == expected
    assert not predicate(pacing, **(values | {'frontEdgePacing': False}))
    assert not predicate(advance, **(values | {'submitted': False}))
assert predicate(pacing.replace(
    '(stableBaseOnly && idleRedraw && vrrCaptureRefresh)', 'vrrCaptureRefresh'),
    stableBaseOnly=True, idleRedraw=False, contentFrame=False,
    vrrCaptureRefresh=True, frontEdgePacing=True)  # Rejected V1 regression.
print('PASS: fresh/heartbeat grid admission preserves ordinary UI and quality deadlines')

(output / 'FrameSyncCaptureAhead.members.h').write_text(members, encoding='utf-8')
(output / 'FrameSyncCaptureAhead.production.inl').write_text(implementation, encoding='utf-8')
(output / 'FrameSyncCaptureAhead.cancel.inl').write_text(cancel, encoding='utf-8')
# A controlled production mutation must be detected by the same WARP test.
negative = output / 'negative'
negative.mkdir(exist_ok=True)
needle = '(!_frameSyncCaptureAheadEnabled || _pendingFrameSyncOutput)'
assert needle in members
(negative / 'FrameSyncCaptureAhead.members.h').write_text(
    members.replace(needle, '!_frameSyncCaptureAheadEnabled'), encoding='utf-8')
for name in ('FrameSyncCaptureAhead.production.inl', 'FrameSyncCaptureAhead.cancel.inl'):
    (negative / name).write_text((output / name).read_text(encoding='utf-8'), encoding='utf-8')
print('PASS: production owner loop freezes staged maintenance/edits, preserves message waits, Present acknowledgement and publication fences')
