"""Static wiring checks supplement native policy/disk tests; no GPU acceptance."""
from pathlib import Path
import xml.etree.ElementTree as ET

repo = Path(__file__).resolve().parents[1]
def read(path): return (repo / path).read_text(encoding='utf-8-sig')
xaml = ET.fromstring(read('src/Magpie/HomePage.xaml'))
uid = '{http://schemas.microsoft.com/winfx/2006/xaml}Uid'
cards = [e.attrib[uid] for e in xaml.iter() if uid in e.attrib]
assert cards.index('Home_Toolbar_InitialState') < cards.index('Home_Toolbar_ScreenshotContent') < cards.index('Home_Toolbar_ScreenshotSaveDirectory')
card = next(e for e in xaml.iter() if e.get(uid) == 'Home_Toolbar_ScreenshotContent')
assert card.get('IsWrapEnabled') == 'True'
items = [e.get(uid) for e in card.iter() if e.tag.endswith('ComboBoxItem')]
assert items == [f'Home_Toolbar_ScreenshotContent_{s}' for s in ['InputAndOutput','OutputOnly','InputOnly']]
assert any(e.get('SelectedIndex') == '{x:Bind ViewModel.ScreenshotContent, Mode=TwoWay}' for e in card.iter())
keys = ['Home_Toolbar_ScreenshotContent.Header','Home_Toolbar_ScreenshotContent.Description',*[i+'.Content' for i in items], 'Message_ScreenshotBusy','Message_ScreenshotPairSaved','Message_ScreenshotPartialSaved']
locales = list((repo/'src/Magpie').glob('Resources.language-*.resw'))
for path in locales:
    entries = list(ET.parse(path).getroot().findall('data'))
    values = {e.get('name'):e.findtext('value') for e in entries}
    assert len(values) == len(entries), f'duplicate resource: {path}'
    assert all(values.get(k) for k in keys), path
    assert all('{0}' in values[k] and '{1}' in values[k] for k in ['Message_ScreenshotPairSaved','Message_ScreenshotPartialSaved']), path
renderer = read('src/Magpie.Core/Renderer.cpp')
new = read('src/Magpie.Core/RendererScreenshot.cpp')
snapshot = renderer.index('_SnapshotScreenshotInput();')
effect_loops = [renderer.find(marker, snapshot) for marker in (
    'for (uint32_t i = 0; i < _effectDrawers.size(); ++i)',
    'for (uint32_t i = 0; i < preEnd; ++i)')]
assert snapshot < min(index for index in effect_loops if index >= 0)
assert '_isPassThroughActive' not in new
transfer = read('src/Magpie.Core/SceneScreenshot.h')
assert 'CopyResource(input.get(), source)' in transfer and 'GetPipelineTexture()' in new
assert 'metadata.width = desc.Width' in transfer
assert 'metadata.height = desc.Height' in transfer
assert '_sharedScreenshotFrames' not in renderer
request = new[new.index('void Renderer::TakeDisplayedScreenshot'):new.index('void Renderer::_FixScreenshotOutput')]
assert '_SnapshotScreenshotInput();' in request and '_forceNextRender' not in request
assert '_TryCompleteSceneScreenshot();' in renderer
save = new[new.index('winrt::fire_and_forget Renderer::_SaveSceneScreenshot('):]
suspended = save[save.index('co_await winrt::resume_background();'):]
assert 'ScalingWindow::' not in suspended and '[this]' not in suspended
# The SINGLETHREADED frontend may not touch DXGI from the background wait.
input_wait = save[save.index('const bool inputReady'):save.index('snapshot(original.get()')]
assert input_wait.index('co_await dispatcher;') < input_wait.index('device->OpenSharedResource1')
assert 'mutex->AcquireSync(1, 0)' in input_wait
assert 'mutex->AcquireSync(1, 10000)' not in save
assert 'request->CancelPending()' in new
assert 'input->metadata.color' in save and 'outputMetadata.color' in save
assert 'images.size() == 2' in save and 'fmt::format(L"{}_{}.png", stem, image.side)' in save
assert 'request->timestamp = ScreenshotHelper::CaptureTimestamp();' in request
assert 'FindUnusedScreenshotStem(request->directory, request->timestamp)' in save
export = renderer[renderer.index('winrt::IAsyncOperation<bool> Renderer::_TakeScreenshotImpl('):]
assert export.index('ScreenshotHelper::CaptureTimestamp()') < export.index('co_await dispatcher')
assert 'FindUnusedScreenshotStem(screenshotsDir, timestamp)' in export
assert 'effect_{:02}_output' in export and 'effect_{:02}_pass_{:02}_texture_{:02}' in export
assert 'FindUnusedScreenshotNum' not in renderer and 'FindUnusedScreenshotNum' not in new
assert 'ScreenshotHelper::SaveMutex()' in renderer and 'ScreenshotHelper::SaveMutex()' in save
assert 'SaveTextureExclusive' in renderer and 'SaveTextureExclusive' in save
assert 'TakeDisplayedScreenshot();' in renderer
assert 'TakeDisplayedScreenshot();' in read('src/Magpie.Core/OverlayDrawer.cpp')
assert 'Int32 ScreenshotContent;' in read('src/Magpie/HomeViewModel.idl')
assert 'ScalingService::Get().UpdateScreenshotContent(content)' in read('src/Magpie/HomeViewModel.cpp')
assert 'window.UpdateScreenshotContent(content)' in read('src/Magpie.Core/ScalingRuntime.cpp')
for path in ['src/Magpie.Core/Magpie.Core.vcxproj','src/Magpie.Core/Magpie.Core.vcxproj.filters']:
    project = ET.fromstring(read(path))
    assert any(e.get('Include') == 'RendererScreenshot.cpp' for e in project.iter()), path
print(f'PASS: XAML/IDL binding, {len(locales)} locale resources, native input boundary, unified entries and async lifetime wiring')
