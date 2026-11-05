"""Cross-module wiring checks; native regression tests exercise the predicates."""
from pathlib import Path
import subprocess
import re
import xml.etree.ElementTree as ET

root = Path(__file__).resolve().parents[1]


def read(path):
    return (root / path).read_text(encoding='utf-8-sig')


checks = 0


def check(value, message):
    global checks
    checks += 1
    assert value, message


renderer = read('src/Magpie.Core/Renderer.cpp')
presenter = read('src/Magpie.Core/FSR3FGPresenter.cpp')
overlay = read('src/Magpie.Core/OverlayDrawer.cpp')
base = read('src/Magpie.Core/PresenterBase.cpp')
check('Present(_PresentSyncInterval(flags), flags)' in base, 'effective sync interval reaches real SDK entry')
check('"syncInterval={} presentFlags=' in base, 'log reflects effective sync interval')
check('bool FSR3FGPresenter::EndFrame(bool waitForGpu) noexcept {\n\t_lastSubmittedFrameCount.reset();' in presenter,
      'every attempt clears the prior content submission result before any failing API')
check('motionAvailability = FrameGuidanceAvailability::SynchronizationFailed' in renderer, 'failed handoff keeps cause')
check('_sharedMotionAvailability[sharedTextureSlot] = motionAvailability' in renderer and
      '_frontendMotionAvailability = _sharedMotionAvailability[sharedTextureSlot]' in renderer,
      'motion failure is carried by the shared slot')
check('_presenter->SetFrameGuidanceAvailability(_frontendMotionAvailability)' in renderer and
      'motionAdmission.Accept(impl.motionAvailability, impl.sourceSample)' in presenter,
      'presenter validates availability independently from the reset bit')
check('publicationMetadata.outputHistoryRevision = _backendOutputHistoryRevision' in renderer and
      'SetOutputHistoryRevision(sourceMetadata.outputHistoryRevision)' in renderer,
      'final effect history epoch travels with color to the presenter')
check('publicationMetadata.contentRedraw = _HasProxyFrameGenerator() && _backendOutputRedraw' in renderer and
      'idleRedraw |= !stableBaseOnly && _HasProxyFrameGenerator() && sourceMetadata.contentRedraw' in renderer,
      'live parameter publication selects non-FG frontend path')
check('const bool contentFrame = !stableBaseOnly && !idleRedraw && _frontendBaseNeedsPresent' in renderer,
      'non-capture revision redraw cannot count as real input')
check('if (!stableBaseOnly && !idleRedraw && IsContentUpdate(sourceMetadata.updateReason)) {\n\t\t_presenter->SetSourceTiming' in renderer,
      'redraw does not supply fabricated timing/motion samples')
check('submitted && !stableBaseOnly && _frameSyncEnabled && _frameSyncUsesSharedSlot' in renderer,
      'a consumed revision redraw acknowledges its slot')
check('_frontendFrameMetadata.outputHistoryRevision == _frontendPresentedFrameMetadata.outputHistoryRevision' in renderer,
      'pending new revision is not discarded as an unchanged capture')
check('ContentFrameCountSource::Submitted' in overlay and 'Overlay_FG_SubmittedGeneratedRate' in overlay,
      'rate source reaches toolbar/profiler labels')
check('Overlay_FG_InputCopyTiming' in overlay and 'Overlay_FG_UnmeasuredTiming' in overlay and
      'Overlay_FG_MeasuredTotal' in overlay, 'proxy marker timing scope is explicit')
check('FrameGenerationEffectKind::XeSSX2' in overlay and 'FrameGenerationEffectKind::XeSSMultiFrame' in overlay,
      'same profiler limitation is labeled on XeSS')


def errors(text):
    body = re.search(r'enum class ScalingError\s*\{(.*?)\};', text, re.S)[1]
    return re.findall(r'^\s*(\w+)\s*,?\s*$', re.sub(r'//[^\n]*', '', body), re.M)


old = subprocess.check_output(['git', 'show', 'a90c7da2:src/Magpie.Core/include/ScalingOptions.h'], cwd=root).decode('utf-8-sig')
old_errors = errors(old)
new_errors = errors(read('src/Magpie.Core/include/ScalingOptions.h'))
check(new_errors[:len(old_errors)] == old_errors, 'existing MP error numbers remain stable')
check(new_errors[len(old_errors):len(old_errors) + 2] == ['FSR3FGEffectChainUnsupported', 'FSR3FGHdrUnsupported'], 'precise FSR3 errors retain their appended positions')
service = read('src/Magpie/ErrorService.cpp')
check('error == ScalingError::FSR3FGEffectChainUnsupported' in service and
      'fmt::runtime(std::wstring(_summary)), StrHelper::UTF8ToUTF16(context)' in service,
      'first-screen message includes the exact incompatible effect name')
for language in ('en-US', 'zh-Hans', 'zh-Hant'):
    entries = ET.fromstring(read(f'src/Magpie/Resources.language-{language}.resw')).findall('data')
    values = {n.attrib['name']: n.findtext('value') for n in entries}
    check(len(entries) == len(values), f'unique resource keys: {language}')
    for key in ('SubmittedRateHelp', 'SubmittedRate', 'SubmittedGeneratedRate', 'InputCopyTiming', 'UnmeasuredTiming', 'MeasuredTotal'):
        check(bool(values.get('Overlay_FG_' + key)), f'localized {language}/{key}')
    check(values['Message_FSR3FGEffectChainUnsupported'].count('{}') == 1, f'effect name placeholder: {language}')
    check(bool(values['Message_FSR3FGHdrUnsupported']), f'localized SDR restriction: {language}')
print(f'FSR3FG review integration: {checks} checks passed (source/XML only).')
