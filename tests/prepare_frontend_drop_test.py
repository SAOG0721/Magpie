"""Exercise production slot rejection/dispatch with deterministic GPU doubles.

Extract through the stale check; accepted texture copies are outside this test.
The frontend dispatch and DLSS drop-completion blocks are also production code.
"""
from pathlib import Path
import re
import sys

repo = Path(__file__).resolve().parents[1]
out = Path(sys.argv[1])
out.mkdir(parents=True, exist_ok=True)
s = (repo / 'src/Magpie.Core/Renderer.cpp').read_text(encoding='utf-8-sig')

def block(source, marker):
    start = source.index(marker)
    brace = source.index('{', start)
    end, depth = brace + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

update = block(s, 'Renderer::FrontendBaseResult Renderer::_UpdateFrontendBase(')
update = update[:update.index('\n\t// IDs are protected by accessLock')] + '\nreturn FrontendBaseResult::Ready;\n}'
render = block(s, 'bool Renderer::_FrontendRender(')
# Match the standalone dispatch, not the preceding pacing `else if` branch.
dispatch = block(render, '\n\tif (!stableBaseOnly) {')
assert '_UpdateFrontendBase(sharedTextureSlot)' in dispatch
dlss = block(s, 'DLSSFGFrameRenderResult Renderer::RenderDLSSFGFrame(')
drop = block(dlss, 'if (droppedFrame) {')
renderer_header = (repo / 'src/Magpie.Core/Renderer.h').read_text(encoding='utf-8-sig')
capture_guards = '\n'.join(block(renderer_header, signature) for signature in (
    'bool _IsFrameSyncPublicationPending()', 'bool _FrameSyncBlocksCapture()'))
assert 'if (_FrameSyncBlocksCapture()) continue;' in s
nr = (repo / 'src/Magpie.Core/DLSSNRFilter.cpp').read_text(encoding='utf-8-sig')
nr_header = (repo / 'src/Magpie.Core/DLSSNRFilter.h').read_text(encoding='utf-8-sig')
settings = block(nr_header, 'struct DLSSNRSettings {') + ';'
parse = block(nr, 'DLSSNRSettings ParseDLSSNRSettings(')
effect = (repo / 'src/Effects/DLSSNR/DLSSNR_AI_Filter.hlsl').read_text(encoding='utf-8-sig')
skin = effect.split('//!LABEL Skin Structure Strength', 1)[1].split('float skinStructureStrength;', 1)[0]
assert re.search(r'//!DEFAULT 0\s', skin) and re.search(r'//!MIN 0\s', skin) and re.search(r'//!MAX 2\s', skin)

prefix = r'''
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include "DLSSNRPerformanceParameters.h"
using namespace Magpie;
using HRESULT = int;
constexpr int WAIT_TIMEOUT = 258;
bool FAILED(int x) { return x < 0; }
int HRESULT_FROM_WIN32(int x) { return -x; }
int acquireResult = 0, releaseResult = 0, releases = 0, signals = 0, presents = 0;
struct ID3D11Texture2D {};
struct IDXGIKeyedMutex {};
template<class T> struct Ptr { T value; T* get() { return &value; } };
HRESULT AcquirePresentationTextures(const std::array<IDXGIKeyedMutex*,3>&, uint64_t, int, size_t* failed=nullptr) {
  if (failed) *failed=0;
  return acquireResult;
}
HRESULT ReleasePresentationTextures(const std::array<IDXGIKeyedMutex*,3>&, uint64_t) { ++releases; return releaseResult; }
namespace fmt { template<class... T> int format(T...) { return 0; } }
struct Logger { static Logger& Get() { static Logger l; return l; } void Info(int) {} void ComError(const char*, int) {} };
namespace FrameTrace {
enum class Event { FrontendBase, FrontendAcquireBusy, FrontendAcquire, FrameSyncAck };
struct Scope { template<class... T> Scope(T...) {} void Data(int, int64_t) {} void End() {} };
template<class... T> void Mark(T...) {}
}
enum class ScalingError { PassThroughUnavailable };
struct Window {
  std::function<void(int, ScalingError, const char*, uint32_t)> reportErrorDetails;
  const Window& Options() const { return *this; }
  const Window& SrcTracker() const { return *this; }
  int Handle() const { return 0; }
};
struct ScalingWindow { static Window& Get() { static Window w; return w; } };
struct PassThrough { IDXGIKeyedMutex* FrontendMutex(uint32_t) { return nullptr; } bool DisableSharing() { return false; } };
void SetEvent(int) { ++signals; }
struct Event { int get() const { return 1; } explicit operator bool() const { return true; } };
enum class DLSSFGFrameRenderResult { Presented, Retry, Dropped };
struct Renderer {
  enum class FrontendBaseResult { Ready, Retry, Dropped };
  uint32_t _sharedTextureSlotCount = 2;
  std::array<Ptr<ID3D11Texture2D>,2> _frontendSharedTextures;
  std::array<Ptr<IDXGIKeyedMutex>,2> _frontendSharedTextureMutexes, _frontendSharedMotionTextureMutexes;
  std::array<std::mutex,2> _sharedTextureAccessMutexes;
  std::array<std::atomic<uint64_t>,2> _sharedTextureCaptureSequences{}, _sharedTextureResourceGenerations{}, _sharedTextureMutexKeys{};
  std::array<uint64_t,2> _lastAccessMutexKeys{}, _discardedFrontendKeys{};
  std::atomic<uint64_t> _activeCaptureSequence{2}, _activeResourceGeneration{1}, _frameSyncAcknowledgedKey{2};
  bool _frameSyncEnabled=true, _frameSyncUsesSharedSlot=true, _frontendBaseValid=true, _frontendBaseNeedsPresent=false;
  bool _frameSyncCaptureAheadEnabled=false, _pendingFrameSyncOutput=false;
  Event _frameSyncConsumedEvent;
  std::array<Event,2> _sharedTextureAvailableEvents;
  int pending = 1;
  PassThrough _passThroughFrames;
  FrontendBaseResult _UpdateFrontendBase(uint32_t sharedTextureSlot) noexcept;
  bool frontend(uint32_t sharedTextureSlot, bool stableBaseOnly, bool* droppedFrame) {
    *droppedFrame = false;
''' + dispatch + r'''
    ++presents;
    return true;
  }
  DLSSFGFrameRenderResult dlss(uint32_t sharedTextureSlot) {
    auto consumePendingFrame = [this] { --pending; };
    bool droppedFrame = false;
    bool presented = frontend(sharedTextureSlot, false, &droppedFrame);
''' + drop + r'''
    return presented ? DLSSFGFrameRenderResult::Presented : DLSSFGFrameRenderResult::Retry;
  }
''' + capture_guards + r'''
  bool backendBlocked() { return _FrameSyncBlocksCapture(); }
};
''' + update + r'''
struct MotionVectorRequest {};
struct DlssnrExperimentProtocol { bool enabled=false; float scale=1; };
struct EffectOption { std::map<std::string,float> parameters; };
MotionVectorRequest ParseDlssOpticalFlowRequest(const EffectOption&) { return {}; }
''' + settings + '\n' + parse

tests = r'''
void seed(Renderer& r, uint32_t slot=0) {
  acquireResult=releaseResult=releases=signals=presents=0;
  r._sharedTextureCaptureSequences[slot]=1;
  r._sharedTextureResourceGenerations[slot]=1;
  r._sharedTextureMutexKeys[slot]=3;
}
int main() {
  // Both stale dimensions, including a previous base still awaiting Present.
  for (bool generation : {false,true}) for (bool base : {false,true}) {
    Renderer r; seed(r); r._frontendBaseValid=base; r._frontendBaseNeedsPresent=true;
    if (generation) { r._sharedTextureCaptureSequences[0]=2; r._activeResourceGeneration=2; }
    bool dropped=false;
    assert(!r.frontend(0,false,&dropped) && dropped);
    assert(!r.backendBlocked() && r._frameSyncAcknowledgedKey==4);
    assert(!r._frontendBaseNeedsPresent && signals==1 && releases==1 && presents==0);
    // Repeated notifications/forced redraw cannot present a discarded slot.
    for(int i=0;i<1000;++i) assert(!r.frontend(0,false,&dropped) && dropped);
    assert(signals==1 && releases==1 && presents==0);
    // A new publication reuses the slot and is accepted.
    r._sharedTextureMutexKeys[0]=5; r._sharedTextureCaptureSequences[0]=2;
    r._sharedTextureResourceGenerations[0]=r._activeResourceGeneration.load();
    assert(r.frontend(0,false,&dropped) && !dropped && presents==1);
  }
  // Failed releases remain retryable; never falsely acknowledge or report a drop.
  {
    Renderer r; seed(r); releaseResult=-1; bool dropped=false;
    assert(!r.frontend(0,false,&dropped) && !dropped);
    assert(r._frameSyncAcknowledgedKey==2 && r._sharedTextureMutexKeys[0]==3);
    assert(r._lastAccessMutexKeys[0]==0 && r._discardedFrontendKeys[0]==0 && signals==0);
    releaseResult=0; assert(!r.frontend(0,false,&dropped) && dropped && !r.backendBlocked());
  }
  {
    Renderer r; seed(r); acquireResult=-1; bool dropped=false;
    assert(!r.frontend(0,false,&dropped) && !dropped && releases==0 && signals==0);
  }
  // Unpaced consumption must not mutate the single-slot acknowledgement.
  {
    Renderer r; seed(r); r._frameSyncEnabled=false; bool dropped=false;
    assert(!r.frontend(0,false,&dropped) && dropped);
    assert(r._frameSyncAcknowledgedKey==2 && signals==0);
  }
  // DLSS ring drops complete the FIFO job and return the actual slot once.
  {
    Renderer r; seed(r,1); r._frameSyncUsesSharedSlot=false;
    assert(r.dlss(1)==DLSSFGFrameRenderResult::Dropped);
    assert(r.pending==0 && signals==1 && presents==0 && r._frameSyncAcknowledgedKey==2);
  }
  {
    Renderer r; seed(r,1); r._frameSyncUsesSharedSlot=false; releaseResult=-1;
    assert(r.dlss(1)==DLSSFGFrameRenderResult::Retry && r.pending==1 && signals==0);
  }
  // Runtime settings enforce the same domain as the shipped UI metadata.
  assert(DLSSNRSettings{}.skinStructureStrength==0);
  EffectOption option;
  assert(ParseDLSSNRSettings(option,false).skinStructureStrength==0);
  for (auto [input, expected] : {std::pair{-1.f,0.f}, {-0.1f,0.f}, {0.f,0.f}, {0.75f,0.75f}, {2.f,2.f}, {3.f,2.f}}) {
    option.parameters["skinStructureStrength"]=input;
    assert(ParseDLSSNRSettings(option,false).skinStructureStrength==expected);
  }
  option.parameters["skinStructureStrength"]=std::numeric_limits<float>::quiet_NaN();
  assert(ParseDLSSNRSettings(option,false).skinStructureStrength==0);
  std::cout << "PASS: production stale-slot rejection, retries, no replay, pacing acknowledgement, DLSS FIFO completion; NR skin 0..2 defaults/clamping\n";
}
'''
(out/'frontend_drop.cpp').write_text(prefix + '\n' + tests, encoding='utf-8')
print('Extracted production frontend drop paths and NR settings')
