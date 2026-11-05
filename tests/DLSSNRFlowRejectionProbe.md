# Real-flow rejection audit

This is a diagnostic derivative of `DLSSNRSparseProductionProbe.cpp`. The
normal wrapper, real flow provider, Sparse shaders and NGX histories execute.
Only a generated Sparse copy calls extra audit shaders after its normal
precheck/Compose dispatches. Product source, options and thresholds are intact.
Audit buffers are read back after the input loop. Extra GPU/CPU submission
work changes scheduling, so these runs are diagnostic evidence.

Build in a new output directory, using the current Core library:

```powershell
pwsh -NoProfile -File tests/Run-DLSSNRFlowRejectionProbe.ps1 `
  -BuildRoot 'C:\Magpie\release\v0.6.12-beta3' `
  -CoreLibrary 'C:\Magpie\build\Magpie.Core.lib' `
  -Output 'C:\Magpie\probe-output' `
  -CompileOnly
```

The prepared executable is in the output above. It and both audit shader
entries passed `--validate-shaders-only` without creating a GPU device. The
main agent then ran six GPU/NGX cases serially: mode6/NV1 static, high-frequency
translation and low-frequency translation; mode6/NV5 high-frequency
translation; mode4/AMD1 and mode4/NV1 high-frequency translation. Each run used
24 captures, no warmup, an audit of the first 12 captures and capture 24, and a
150ms tail. These are controlled input/flow/policy diagnostics, not product
image-quality or performance acceptance. The six cases' 90 Compose/Fill audit
counter pairs matched the product counters within CSV rounding (<5e-13).
The concise verified numbers and their limits are saved as
`flow-rejection-probe-v1/analysis-summary.json`. Completed run evidence and
executable directories are now frozen; use a new directory to rebuild.

The initial three-case comparison used mode 6/NV1 with only the scene changed.
The following commands describe that comparison; they are not a request to
repeat the completed runs. Use unique prefixes for any future independent run:

```powershell
./DLSSNRFlowRejectionProbe.exe --mode 6 --flow nv1 --scene static --warmup 0 --samples 24 --fps 60 --tail-ms 150 --audit-first 12 --audit-every 24 --prefix m6-nv1-static
./DLSSNRFlowRejectionProbe.exe --mode 6 --flow nv1 --scene translate --warmup 0 --samples 24 --fps 60 --tail-ms 150 --audit-first 12 --audit-every 24 --prefix m6-nv1-high
./DLSSNRFlowRejectionProbe.exe --mode 6 --flow nv1 --scene translate-low --warmup 0 --samples 24 --fps 60 --tail-ms 150 --audit-first 12 --audit-every 24 --prefix m6-nv1-low
```

Each has a nominal 0.4-second capture window; initialization, GPU retirement and
audit readback add time. Reserve about 1–2 minutes for the three short runs;
the actual run duration is reported, not inferred from the requested FPS.
Defaults remain model 2, full 50%, one pass and the production user controls.
Only if these leave a route-specific question, add mode 4/NV1/high with the same
arguments. A broad quality/mode matrix is unnecessary at this stage.

`translate` is the existing two-pixels-per-capture high-frequency input.
`translate-low` uses smooth, multi-directional RGB sinusoids with the same
translation and byte-color quantization. Neither scene supplies motion,
validity, a cut/reset, or an event oracle to the product. Analytic translation
is used only in an extra audit shader's b1 constants to score flow EPE and an
ideal-warp color reference. b1 is removed after that extra dispatch.

The normal `*.production-frames.csv` remains available for the recovery state,
displayed target, full/lite counters and attempted rejection fractions. The
three added tables separate the relevant causes:

- `*.pair-audit.csv` evaluates the physical adjacent pair densely, including
  while recovery is Low. Confidence is the provider-published dense value;
  NVIDIA may already combine cost and FB terms, while AMD publishes a fixed
  .65 because its SDK has no confidence surface. FB error is independently
  recomputed from the exported dense forward/reverse vectors when available.
  GT EPE, actual-warp max-RGB error and ideal-warp error are separate counters
  and sums. The GT denominator is `gt_interior_count`; other dense fractions
  normally use `pixels_count`. `epe_le025_precheck_rejected_count` identifies
  accurately estimated translations rejected by the physical precheck.
- `*.precheck-phases.csv` reports all 64 offsets in an 8×8 grid. Offset (0,0)
  is the production sample. Compare its rejected fraction against the dense
  `precheck_rejected_count / pixels_count` and the range of the other offsets
  before attributing a recovery trigger to the whole frame.
- `*.compose-audit.csv` uses the exact bound NR reference/residual/map SRVs
  and calls the product `SourceWeight`. Map confidence, photometric factor
  and final source weight each split zero, partial `(0,.999)` and full
  `>=.999`. `float_invalid_r8_full_count` measures pixels counted invalid by
  Compose's floating threshold but quantized to full validity before Fill.
  `r8_invalid_no_support_count` uses Fill's exact neighborhood support test.
  `without_confidence_invalid_count` and
  `without_photometric_invalid_count` are audit-only factor ablations.

Each added row contains physical/target/source IDs, signed target-to-NR source
distances, the last full/lite submitted-history frame and the recovery state
before the current decision. A missing source/distance uses frame 0 and
INT64_MIN. For this controlled probe, `captureSequence` in the audit CSV means
the Sparse accepted-input sequence, not the constant capture-session epoch.

Read the first 1–12 physical frames first. If pair EPE/FB or warp error is poor
before Low, there is a real flow/input-estimation limitation to address. If
pair EPE is good but partial source weights dominate, use the factor ablations
and high/low-frequency comparison to locate confidence/photometric sensitivity.
If physical pairs remain usable while full-history distance grows only after
Low, the later empty/expired endpoint rejection belongs to the recovery
feedback. These are separate findings and may occur together.

Current product thresholds in the audited source are: physical confidence .5,
max-RGB error .06, changed sampled area 2%; map confidence
`smoothstep(.1,.5,c)` per step; source photometric smoothstep .035–.12;
Compose full trust .999 and invalid area 2%; Fill neighbor full trust .999 and
guide-error smoothstep .035–.08. This probe changes none of them.
