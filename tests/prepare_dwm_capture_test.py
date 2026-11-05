"""Run the real DWM capture update against deterministic CPU platform doubles.

The fixture checks the copy API contract and state transitions. It does not
reimplement capture admission or create a D3D device/window.
"""
from pathlib import Path
import re
import sys


def function(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


repo = Path(__file__).resolve().parents[1]
output = Path(sys.argv[1]).resolve()
output.mkdir(parents=True, exist_ok=True)
source = (repo / "src/Magpie.Core/DwmSharedSurfaceFrameSource.cpp").read_text(encoding="utf-8-sig")
header = (repo / "src/Magpie.Core/FrameSourceBase.h").read_text(encoding="utf-8-sig")
update = function(source, "FrameSourceState DwmSharedSurfaceFrameSource::_Update()")
state = re.search(r"enum class FrameSourceState\s*\{.*?\n\};", header, re.S)[0]

fixture = r'''
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using UINT = uint32_t;
using HWND = uintptr_t;
using HANDLE = void*;
using HRESULT = int32_t;
constexpr HRESULT S_OK = 0;
constexpr HRESULT Removed = static_cast<HRESULT>(0x887A0005u);
constexpr HRESULT Invalid = static_cast<HRESULT>(0x80070057u);
inline bool FAILED(HRESULT code) { return code < 0; }
#define IID_PPV_ARGS(value) value
STATE
static unsigned checks = 0;
static void Check(bool value, const char* message) {
    ++checks;
    if (!value) { std::cerr << "FAILED: " << message << '\n'; std::exit(42); }
}
enum class Change { None, Hide, Minimize };
struct D3D11_TEXTURE2D_DESC { UINT Width = 2552, Height = 1404; };
struct D3D11_BOX { UINT left = 8, top = 35, front = 0, right = 2552, bottom = 1404, back = 1; };
struct Platform {
    bool visible = true, iconic = false, acquireSuccess = true, nullHandle = false;
    HRESULT openResult = S_OK;
    Change acquireChange = Change::None, openChange = Change::None, descriptionChange = Change::None;
    D3D11_TEXTURE2D_DESC source;
    unsigned acquisitions = 0, opens = 0, descriptions = 0, begins = 0, copies = 0, errors = 0;
    HRESULT loggedCode = S_OK;
    std::vector<std::string> infos;
    void Apply(Change change) {
        if (change == Change::Hide) visible = false;
        if (change == Change::Minimize) iconic = true;
    }
} platform;
bool IsWindowVisible(HWND) { return platform.visible; }
bool IsIconic(HWND) { return platform.iconic; }
struct Texture {
    bool source = false;
    D3D11_TEXTURE2D_DESC destination{2544, 1369};
    void GetDesc(D3D11_TEXTURE2D_DESC* result) {
        ++platform.descriptions;
        *result = source ? platform.source : destination;
        platform.Apply(platform.descriptionChange);
    }
} platformSharedTexture{true};
namespace winrt {
template<class T> struct com_ptr {
    T* value = nullptr;
    T* get() const { return value; }
    T* operator->() const { return value; }
    T** operator&() { return &value; }
};
}
using ID3D11Texture2D = Texture;
struct Device {
    HRESULT OpenSharedResource(HANDLE, Texture** result) {
        ++platform.opens;
        platform.Apply(platform.openChange);
        if (FAILED(platform.openResult)) return platform.openResult;
        *result = &platformSharedTexture;
        return S_OK;
    }
};
struct Context {
    void CopySubresourceRegion(Texture* destination, UINT, UINT x, UINT y, UINT z,
        Texture* source, UINT, const D3D11_BOX* box) {
        ++platform.copies;
        Check(source == &platformSharedTexture, "production copy must use the opened surface");
        Check(platform.visible && !platform.iconic, "no copy while source is hidden or minimized");
        Check(box && box->left < box->right && box->top < box->bottom &&
            box->right <= platform.source.Width && box->bottom <= platform.source.Height,
            "no copy may read outside the shared source surface");
        Check(box->front == 0 && box->back == 1 && z == 0 &&
            uint64_t(x) + box->right - box->left <= destination->destination.Width &&
            uint64_t(y) + box->bottom - box->top <= destination->destination.Height,
            "copy must fit its destination texture");
    }
};
struct Resources {
    Device device;
    Context context;
    Device* GetD3DDevice() { return &device; }
    Context* GetD3DDC() { return &context; }
};
struct Tracker { HWND Handle() const { return 0x111; } };
struct ScalingWindow {
    static ScalingWindow& Get() { static ScalingWindow window; return window; }
    const Tracker& SrcTracker() const { static Tracker tracker; return tracker; }
};
bool DwmGetDxSharedSurface(HWND, HANDLE* result, void*, void*, void*, void*) {
    ++platform.acquisitions;
    platform.Apply(platform.acquireChange);
    *result = platform.nullHandle ? nullptr : reinterpret_cast<HANDLE>(uintptr_t(1));
    return platform.acquireSuccess;
}
namespace fmt {
template<class... T> std::string format(std::string_view text, T&&... values) {
    std::ostringstream result;
    result << text;
    ((result << '|' << std::forward<T>(values)), ...);
    return result.str();
}
}
struct Logger {
    static Logger& Get() { static Logger logger; return logger; }
    void Info(std::string text) { platform.infos.push_back(std::move(text)); }
    void Win32Error(const char*) { ++platform.errors; }
    void ComError(const char*, HRESULT code) { ++platform.errors; platform.loggedCode = code; }
};
class DwmSharedSurfaceFrameSource {
public:
    Resources resources;
    Resources* _deviceResources = &resources;
    Texture destination;
    winrt::com_ptr<Texture> _output;
    D3D11_BOX _frameInWnd;
    bool _captureInterrupted = false;
    uint64_t _captureSequence = 0;
    DwmSharedSurfaceFrameSource() { platform = {}; _output.value = &destination; }
    void _BeginCaptureRender() { ++platform.begins; }
    FrameSourceState _Update() noexcept;
};
UPDATE
int main() {
    using State = FrameSourceState;
    {
        DwmSharedSurfaceFrameSource capture;
        Check(capture._Update() == State::NewFrame && platform.copies == 1, "exact source crop boundary must copy");
        Check(!capture._captureInterrupted && capture._captureSequence == 0, "normal input must not invent a discontinuity");
        platform.source = {4096, 2160};
        Check(capture._Update() == State::NewFrame && platform.copies == 2, "larger valid source surface must copy");
    }
    for (const auto dimensions : {D3D11_TEXTURE2D_DESC{2551,1404}, {2552,1403}, {160,28}, {0,0}}) {
        DwmSharedSurfaceFrameSource capture;
        platform.source = dimensions;
        Check(capture._Update() == State::Waiting, "undersized visible source must wait");
        Check(platform.copies == 0 && capture._captureInterrupted && capture._captureSequence == 1,
            "invalid bounds must skip copy and open one interrupted sequence");
        Check(platform.infos.size() == 1, "first invalid surface must record dimensions and crop");
        const auto recorded = std::string("|") + std::to_string(dimensions.Width) + "|" +
            std::to_string(dimensions.Height) + "|8|35|2552|1404";
        Check(platform.infos.front().ends_with(recorded), "diagnostic must use actual source dimensions and fixed crop");
        Check(capture._Update() == State::Waiting && capture._captureSequence == 1 && platform.infos.size() == 1,
            "repeated invalid surfaces must not advance sequence or repeat the warning");
        platform.source = {};
        Check(capture._Update() == State::NewFrame && !capture._captureInterrupted && capture._captureSequence == 1,
            "first restored input must retain the one new sequence");
        Check(capture._Update() == State::NewFrame && capture._captureSequence == 1 && platform.copies == 2,
            "ordinary resumed frames must keep their sequence");
        platform.source.Width = 1;
        Check(capture._Update() == State::Waiting && capture._captureSequence == 2 && platform.infos.size() == 2,
            "a later independent interruption must advance exactly once");
    }
    for (const auto change : {Change::Hide, Change::Minimize}) {
        DwmSharedSurfaceFrameSource capture;
        platform.Apply(change);
        Check(capture._Update() == State::Waiting && capture._captureSequence == 1, "unavailable source must wait");
        Check(platform.acquisitions == 0 && platform.opens == 0 && platform.begins == 0 && platform.copies == 0,
            "initial hidden/minimized state must not acquire a surface or begin GPU work");
        Check(capture._Update() == State::Waiting && capture._captureSequence == 1, "unavailable polling must coalesce");
        platform.visible = true; platform.iconic = false;
        Check(capture._Update() == State::NewFrame && !capture._captureInterrupted && capture._captureSequence == 1,
            "visibility restoration must deliver a frame without a second sequence increment");
    }
    for (const bool missingHandle : {false, true}) {
        DwmSharedSurfaceFrameSource capture;
        platform.acquireSuccess = missingHandle;
        platform.nullHandle = missingHandle;
        Check(capture._Update() == State::Error && platform.errors == 1 && platform.opens == 0 && platform.copies == 0,
            "active source acquisition failure or missing handle must remain an error");
        Check(!capture._captureInterrupted && capture._captureSequence == 0, "real acquisition failure must not be disguised as waiting");
    }
    for (const auto change : {Change::Hide, Change::Minimize}) {
        DwmSharedSurfaceFrameSource capture;
        platform.acquireSuccess = false; platform.acquireChange = change;
        Check(capture._Update() == State::Waiting && platform.errors == 0 && platform.opens == 0,
            "source becoming unavailable inside acquisition must wait quietly");
        Check(capture._captureInterrupted && capture._captureSequence == 1, "acquisition transition must record one discontinuity");
        platform.acquireSuccess = true; platform.acquireChange = Change::None;
        platform.visible = true; platform.iconic = false;
        Check(capture._Update() == State::NewFrame && capture._captureSequence == 1, "acquisition transition must recover");
    }
    for (const auto code : {Removed, Invalid}) {
        DwmSharedSurfaceFrameSource capture;
        platform.openResult = code;
        Check(capture._Update() == State::Error && platform.errors == 1 && platform.loggedCode == code && platform.copies == 0,
            "active source OpenSharedResource failure must preserve the real error");
        Check(!capture._captureInterrupted && capture._captureSequence == 0, "real open error must not become a lifecycle wait");
    }
    for (const auto change : {Change::Hide, Change::Minimize}) {
        DwmSharedSurfaceFrameSource capture;
        platform.openResult = Removed; platform.openChange = change;
        Check(capture._Update() == State::Waiting && platform.errors == 0 && platform.copies == 0,
            "open failure concurrent with source retirement must wait without native input");
        Check(capture._captureInterrupted && capture._captureSequence == 1, "open transition must record one discontinuity");
    }
    for (const auto change : {Change::Hide, Change::Minimize}) {
        DwmSharedSurfaceFrameSource capture;
        platform.descriptionChange = change;
        Check(capture._Update() == State::Waiting && platform.descriptions == 1 && platform.copies == 0,
            "late visibility transition after GetDesc must still prevent copy");
        Check(capture._captureInterrupted && capture._captureSequence == 1, "late transition must record one discontinuity");
    }
    std::cout << "PASS: " << checks << " production DWM capture checks; bounds, visibility, racing acquisition/open, real errors, one sequence per interruption and recovery; CPU only\n";
}
'''
fixture = fixture.replace("STATE", state)
(output / "dwm_capture.cpp").write_text(fixture.replace("UPDATE", update), encoding="utf-8")

# Mutate the production body, not a second admission implementation. The same
# API contract checks must reject removal of either demonstrated protection.
for name, marker, replacement in (
    ("unbounded-copy", "if (_frameInWnd.right > sourceDesc.Width || _frameInWnd.bottom > sourceDesc.Height)", "if (false)"),
    ("unavailable-source", "if (sourceUnavailable()) return waitForSurface();", "if (sourceUnavailable() && false) return waitForSurface();"),
):
    if marker not in update:
        raise RuntimeError(f"Production mutation target missing: {name}")
    mutated = update.replace(marker, replacement)
    (output / f"{name}.cpp").write_text(fixture.replace("UPDATE", mutated), encoding="utf-8")
print("Extracted production DwmSharedSurfaceFrameSource::_Update and two negative mutations; no GPU or desktop calls.")
