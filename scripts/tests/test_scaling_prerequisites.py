"""CPU-only tests of extracted production prerequisites and their call order."""
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[2]
window = (root / 'src/Magpie.Core/ScalingWindow.cpp').read_text(encoding='utf-8-sig')
service = (root / 'src/Magpie/ScalingService.cpp').read_text(encoding='utf-8-sig')
options = (root / 'src/Magpie.Core/include/ScalingOptions.h').read_text(encoding='utf-8-sig')


def block(text, marker):
    start = text.index(marker)
    brace = text.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


capture = block(window, 'static ScalingError CheckCapturePrerequisites(')
start = block(window, 'ScalingError ScalingWindow::_StartImpl(')
conflict = start[start.index('if (_options.effects.empty())'):start.index('if (NgxRuntimeGuard::IsFaulted()')]
mode = block(start, 'if (_options.IsWindowedMode()) {')
for prerequisite in ('_options.effects.empty()', 'ValidateFrameGenerationChain(',
                     'NgxRuntimeGuard::IsFaulted()', '_srcTracker.Set(', '_srcTracker.IsZoomed()',
                     'CheckCapturePrerequisites(', 'Win32Helper::IsWindowHung(hwndSrc)'):
    assert start.index(prerequisite) < start.index('CreateWindowEx(')
    assert start.index(prerequisite) < start.index('_CalcFullscreenRendererRect(')
assert start.index('if (!_hwndRenderer)') < start.index('_renderer->Initialize(')
assert 'startupDiagnostic.Details()' in window and 'startupDiagnostic.SystemError()' in window
assert service.index('if (windowedMode && profile.captureMethod == CaptureMethod::DesktopDuplication)') < service.index('TouchHelper::TryLaunchTouchHelper(')
assert service.index('if (effect.isRecoveryInvalid || !EffectsService::Get().GetEffect(effect.name))') < service.index('TouchHelper::TryLaunchTouchHelper(')

# Use the real chain classifier and validation template with small effect stubs.
classifier = block(options, 'inline FrameGenerationEffectKind ClassifyFrameGenerationEffect(\n\tstd::string_view')
effect_kind = block(options, 'enum class FrameGenerationEffectKind') + ';'
validation_type = block(options, 'struct FrameGenerationChainValidation {') + ';'
# Preserve the actual template parameter name from the source.
template_line = options[:options.index('FrameGenerationChainValidation ValidateFrameGenerationChain(')].splitlines()[-1]
validation = template_line + '\n' + block(options, 'FrameGenerationChainValidation ValidateFrameGenerationChain(')

harness = r'''
#include <cassert>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>
#include <cstdint>
enum class ScalingError { NoError, CaptureMethodUnavailable, ScalingModeEmpty,
    ConflictingFrameGenerationEffects, Windowed3DGameMode, WindowedDesktopDuplication };
enum class CaptureMethod { GraphicsCapture, DesktopDuplication, GDI, DwmSharedSurface, COUNT };
EFFECT_KIND
CLASSIFIER
VALIDATION_TYPE
VALIDATION
struct Effect { std::string name; };
struct Options {
    std::vector<Effect> effects{{"Bilinear"}};
    bool windowed=false, game3d=false;
    CaptureMethod captureMethod=CaptureMethod::GraphicsCapture;
    bool IsWindowedMode() const { return windowed; }
    bool Is3DGameMode() const { return game3d; }
};
struct Logger {
    static Logger& Get() { static Logger l; return l; }
    void Error(const char*) {} void Win32Error(const char*) {} void ComError(const char*,int) {}
};
static bool wgc=true, throws=false, modern=true, dwm=true;
namespace winrt {
struct hresult_error { int code() const { return 1; } };
namespace Windows::Graphics::Capture {
struct GraphicsCaptureSession { static bool IsSupported() { if (throws) throw hresult_error{}; return wgc; } };
}}
namespace Win32Helper {
struct Version { bool Is20H1OrNewer() const { return modern; } };
Version GetOSVersion() { return {}; }
void Stub() {}
template<class T> T* LoadSystemFunction(const wchar_t*,const char*) { return dwm ? Stub : nullptr; }
}
CAPTURE
static int moves=0, windows=0, gpu=0;
ScalingError Start(const Options& _options) {
    CONFLICT
    MODE
    if (auto error=CheckCapturePrerequisites(_options.captureMethod); error!=ScalingError::NoError) return error;
    ++moves; ++windows; ++gpu;
    return ScalingError::NoError;
}
void Check(Options opt, ScalingError expected) {
    moves=windows=gpu=0;
    assert(Start(opt)==expected);
    const int count=expected==ScalingError::NoError ? 1:0;
    assert(moves==count && windows==count && gpu==count);
}
int main() {
    Check({},ScalingError::NoError);
    Options opt;
    opt.effects.clear(); Check(opt,ScalingError::ScalingModeEmpty);
    opt.effects={{"DLSSFG\\DLSS_FrameGeneration"},{"XeSSFG\\XeSS_FrameGeneration_x2_ZeroMV"}};
    Check(opt,ScalingError::ConflictingFrameGenerationEffects);
    opt.effects={{"DLSSFG\\DLSS_FrameGeneration"},{"DLSSFG\\DLSS_FrameGeneration"}};
    Check(opt,ScalingError::ConflictingFrameGenerationEffects);
    opt={}; opt.windowed=true; opt.game3d=true; Check(opt,ScalingError::Windowed3DGameMode);
    opt.game3d=false; opt.captureMethod=CaptureMethod::DesktopDuplication;
    Check(opt,ScalingError::WindowedDesktopDuplication);
    opt.windowed=false; Check(opt,ScalingError::NoError);
    modern=false; Check(opt,ScalingError::CaptureMethodUnavailable); modern=true;
    opt={}; wgc=false; Check(opt,ScalingError::CaptureMethodUnavailable); wgc=true;
    throws=true; Check(opt,ScalingError::CaptureMethodUnavailable); throws=false;
    opt.captureMethod=CaptureMethod::DwmSharedSurface; Check(opt,ScalingError::NoError);
    dwm=false; Check(opt,ScalingError::CaptureMethodUnavailable); dwm=true;
    opt.captureMethod=CaptureMethod::GDI; Check(opt,ScalingError::NoError);
    for (int invalid:{-1,4,100}) {
        opt.captureMethod=static_cast<CaptureMethod>(invalid); Check(opt,ScalingError::CaptureMethodUnavailable);
    }
    std::cout << "Production prerequisites: empty/conflicting effects, mode combinations, capture availability "
        "and query exceptions rejected before simulated moves/windows/GPU; valid paths passed.\n";
}
'''
for token, value in {'EFFECT_KIND': effect_kind, 'CLASSIFIER': classifier, 'VALIDATION_TYPE': validation_type, 'VALIDATION': validation,
                     'CAPTURE': capture, 'CONFLICT': conflict, 'MODE': mode}.items():
    harness = harness.replace(token, value)
out = Path(sys.argv[1]).resolve()
out.mkdir(parents=True, exist_ok=True)
(out / 'scaling_prerequisites.cpp').write_text(harness, encoding='utf-8')
print(out / 'scaling_prerequisites.cpp')
