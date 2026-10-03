# AMD lmxxf neural rendering

This native backend runs the AMDNR/lmxxf runtime on the captured image before native FSR upscaling. Its independent `EnableAmdLmxxfNR` build option does not require NVIDIA NGX or `EnableDLSSNR`.

## Setup

Import `presets/ScalingModes-AMDNR-experimental.json` in **Effects groups**, select **AMD NR + FSR3 2x** for the game profile, and select **Graphics Capture** (Windows Graphics Capture/WGC). Use fullscreen scaling.

Set the game client area to 640 x 360 and the display to 1280 x 720 yourself. The backend does not change the display resolution. NR preserves input dimensions, and FSR3 performs the only spatial upscale:

```text
WGC 640 x 360 -> AMDNR\AMDNR_AI_Filter 640 x 360
             -> FSR3\FSR3_SR 1280 x 720 -> fullscreen presentation
```

The optional **AMD NR + FSR3 2x + XeSS FG 2x experimental** preset adds XeSS frame generation and AMD optical flow. FSR4.1 has a separate experimental preset.

To adjust NR, expand the effect group and open the parameters button on the **AMDNR_AI_Filter** row. Exit an older Magpie instance from the notification area before launching a newly built version.

## Build

Use an x64 build with VS2022 C++ build tools, the Windows SDK and the existing Magpie dependencies. Follow the third-party SDK documentation for FSR and XeSS setup. Obtain `LmxxfNrRuntime.dll` and `LmxxfNrRuntime.pak` from the AMDNR release separately; the tested runtime was v0.3.5.1. Runtime binaries and model assets are not included in the source tree.

Example, with dependency paths adjusted to your checkout:

```powershell
pwsh scripts/Build-Release.ps1 `
  -EnableAmdLmxxfNR -AmdLmxxfNRRuntimeDir C:/Dependencies/AMDNR/runtime `
  -EnableFSR3ZeroMV -FSR3SdkDir C:/Dependencies/FidelityFX-SDK-2.3.0 `
  -EnableAmdOpticalFlow -EnableXeSSFrameGeneration `
  -XeSSSdkDir C:/Dependencies/XeSS-SDK-3.0.2
```

For an uncommitted development checkout, add `-AllowDirtySource`. Omit the optical-flow and frame-generation switches for an NR + FSR3 build. Keep the complete packaged runtime directory together. The backend loads `AMDNR/LmxxfNrRuntime.dll` using an absolute path and passes the adjacent `.pak` path as `assets_directory`.

## Controls

| Control | Range / default | Behavior |
| --- | --- | --- |
| NR style feature strength | 0–4 / 1 | lmxxf feature scaling; not NVIDIA style presets 0/1/2 |
| NR intensity | 0–1 / 1 | Runtime transfer and color strength |
| Local tone strength | 0–4 / 1 | Tone feature scaling |
| Local structure strength | 0–4 / 1 | Scene structure with auto mask; global structure without it |
| Skin / character structure strength | 0–4 / 1 | Character structure; unavailable when auto mask is disabled |
| Automatic character mask | Enabled | Model-internal character/scene separation |
| Multi Pass | 1–3 / 1 | Additional inference passes on the preceding network result |

Controls apply live and invalidate cached output for the same capture frame. Feature-control changes can rebuild the model and trigger another warmup. Default settings use 128-byte frame info; non-default feature controls use the 144-byte extension and CONTROLS flag. An incompatible runtime stops the chain with an explicit error instead of ignoring the setting.

Default intensity is already the runtime maximum. Additional passes and feature scaling increase processing time and do not guarantee better image quality. NVIDIA NGX's **NR UI correction** has no corresponding field in this AMD runtime API and is not exposed here.

## Data and synchronization

Normal rendering uses GPU shaders, shared D3D11/D3D12 textures and shared fences, with no CPU texture readback, upload or GDI presentation. BGRA/RGBA8 and FP16 conversion preserves gamma-encoded SDR RGB in [0,1], with alpha 1; it does not decode sRGB. There is no residual reconstruction or hidden spatial resampling.

This synchronous implementation uses runtime API v1 and tier policy 1, supports input dimensions up to 640 x 360, and resets NR history for every inference. The first warmup frame is passthrough. Cache reuse requires matching frame ID, input revision and history revision, with no history reset. Resize drains and recreates the runtime; initialization or dispatch failures stop the effect chain.

Window capture does not supply engine depth, motion vectors or jitter. FSR uses zero depth and zero jitter; SR-only presets use zero motion vectors, while FG uses estimated AMD optical flow. FSR upscaling does not itself enable frame generation. Provider identity, version and initialization/dispatch results are logged. FSR4.1 on this adapter remains an experimental option.

## Reproducible checks

The x64 build and WGC capture, resize/restart, NR output, FSR3 3.1.5, experimental FSR4.1.1 and actually displayed XeSS FG 2x were exercised on Radeon 780M. These observations do not imply a game frame-rate guarantee or universal FSR4 support.

`tests/Run-AmdNRCaptureSmoke.ps1` runs an animated capture fixture in an isolated portable runtime and retains evidence under the git-ignored `validation/amd-nr/`:

```powershell
pwsh tests/Run-AmdNRCaptureSmoke.ps1 -RuntimeDirectory ./bin/x64/Release -Stage NR
pwsh tests/Run-AmdNRCaptureSmoke.ps1 -RuntimeDirectory ./bin/x64/Release -Stage FSR3 -Fullscreen -NoResize
pwsh tests/Run-AmdNRCaptureSmoke.ps1 -RuntimeDirectory ./bin/x64/Release -Stage FSR4 -Fullscreen -NoResize
pwsh tests/Run-AmdNRCaptureSmoke.ps1 -RuntimeDirectory ./bin/x64/Release -Stage FG2 -Fullscreen -NoResize `
  -PresentMonPath C:/Dependencies/PresentMon/PresentMon.exe
```

Close existing Magpie instances before using the fixture. FG2 acceptance requires PresentMon 2.6-compatible frame-type metrics and actually displayed Application and Intel XeSS-FG events; SDK call counts alone do not qualify. PresentMon may request elevation. Run `scripts/Analyze-AmdNRValidation.py <evidence-directory> --require-fg2` to analyze an FG2 run.

The smoke test explicitly enables `MAGPIE_VALIDATE_NATIVE_OUTPUT=1` for diagnostic CPU readback and pixel snapshots. Leave this variable unset during normal use. Optional `EnableNativeBackendTiming` and `EnableFrameTrace` distinguish synchronized NR/SR/optical-flow wall time, CPU presentation calls and observed displayed frames; they do not isolate XeSS GPU interpolation time.

## Attribution

Runtime origin: AMDNR/3zwr1; neural network and runtime origin: lmxxf/Kien. Keep the notices and licenses in `third-party/AMDNR`, copied beside the runtime. The API header retains the lmxxf MIT license; Magpie integration code follows GPL-3.0-or-later. FSR and XeSS licenses are copied with their SDK runtime libraries.
