# Production four-mode probe

The probe compiles the current Filter, Sparse engine and MultiPass wrapper,
linking a snapshot of the matching Core library. It adds scene generation,
timestamp scopes and optional image/validity diagnostics. It does not launch
Magpie or measure desktop Present, game FPS, power or input-to-photon latency.

FrameGuidanceService uses the real AMD/NVIDIA provider. Accepted captures
advance the backend's local ordinal; redraws and completion do not. The scene
helper supplies colors only: no ideal motion, oracle validity or automatic
reset is provided to NR for cuts or occlusion.

## Build and checks

Run Run-DLSSNRSparseStateTests.ps1 and Run-DLSSNRSparseShaderTests.ps1 for CPU
and WARP coverage. Build the production probe with
Run-DLSSNRSparseProductionProbe.ps1, supplying -BuildRoot, -CoreLibrary,
-Output and -CompileOnly. The four-mode stage is at workspace-relative
diagnostics/20261101-dlssnr-four-modes/stage, with Core in runtime/Magpie.Core.lib.

Choose a fresh output directory for every build. The builder snapshots Core
and records source, generated instrumentation, library and runtime hashes.
The --validate-shaders-only switch compiles fixture and optional diagnostic
shaders without a GPU device. Sparse state tests compile five production
shader entries; WARP executes local fallback and asynchronous observation.

## Current mode IDs

Mode 0 is Every Frame, 1 Alternating Frames, 2 Async Every Frame and 3 Async
Alternating Frames. These are current versioned IDs, not legacy seven-mode
configuration values. Defaults: mode 3, FP8 request 2, manual 50%, one pass,
user controls, anti-flicker 2 at 150ms, interval 2, 60 captures/s.

Examples:

    DLSSNRSparseProductionProbe.exe --mode 1 --interval 4 --flow amd1 --scene translate --warmup 8 --samples 72 --prefix alternating4
    DLSSNRSparseProductionProbe.exe --mode 2 --flow nv1 --scene cut --warmup 8 --samples 72 --prefix async-every
    DLSSNRSparseProductionProbe.exe --mode 3 --interval 3 --flow amd1 --scene ui --passes 3 --diverse-passes --reset-at 18 --idle-after 30 --idle-ms 220 --tail-ms 300 --prefix boundary

The --interval argument accepts 2/3/4 for modes 1 and 3. There is no recovery
policy or buffer delay argument. Models, integer percentage, pass count,
anti-flicker, image size, frame rate and preset are selectable. Flow values
are amd0, amd1, nv1 through nv5; scenes are static, translate, occlusion,
cut, lighting and ui. Translation is two pixels per new capture, without
passing that value to the flow provider.

The --reset-at N argument separately simulates an upstream history boundary.
Ordinary cut/occlusion tests leave it unset. The --idle-after N, --idle-ms
and --tail-ms arguments exercise completion without inventing captures.

## Evidence

- .production.json: versioned identity, effective interval, specification and
  tail status. Every output is asserted to retain the current capture's
  identity and timestamp, with zero whole-picture delay.
- .counts.json and .counters.csv: actual successful evaluations for every
  complete pass, dimensions, independent pass settings, submissions, ordinary
  hard recovery/reuse and bounded pending/waiting state. Compile-time checks
  prove that neither engine owns a lightweight branch.
- .production-frames.csv: every draw including warmup and idle, target/source
  identity, epoch, observation revision, true age and GPU-ready markers.
- .samples.csv: D3D11 frame is the draw record ID; D3D12 frame is the real NR
  source ID. Nested spans overlap. GPU-ready is not desktop Present.
- The --dump-tail N argument preallocates snapshots and reads them after
  timing; these runs are labeled quality evidence. Target input is BGRA8;
  physical input and final output are RGBA8, tightly packed at width*height*4.
- The --diagnostic-validity switch adds a probe-only GPU reduction/readback,
  labeling the run diagnostic_readback. In mode 3, zero validity denotes
  current raw fallback; partial validity includes attenuated correction.
  Without this option, fractions are -1 (unmeasured).

Run-DLSSNRFourModesMatrix.ps1 runs the eight-case Smoke or 66-case Acceptance
suite, then analyze_dlssnr_four_modes.py checks identities, clocks, pass
counts, manual percentages and queue bounds. Exact ordinary cadence with
frame-ID gaps, redraws and soft/hard invalidation is additionally tested by
DLSSNRPipelineTests.exe --alternating-only on real NGX resources.

Earlier seven-mode .recovery.json and dual-endpoint telemetry remain in
historical diagnostic directories. Their old matrix/analyzer scripts require
the matching old probe binary; they are not the current four-mode interface.
