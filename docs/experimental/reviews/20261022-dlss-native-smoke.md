# Native DLSS candidate smoke — 2026-10-22

Candidate source: `247158ef7e89e6c7720c6ebc296c89cc89ead673`, 0.6.11-beta1. Physical GPU: **NVIDIA GeForce RTX 5070 Ti (SM120)**; installed driver **617.42** (`nvidia-smi`), CUDA Driver API **13040**. Tests used production SR/FG adapter objects and the production NR chain/temporal implementation. No Magpie window was launched or stopped; user application settings and NVIDIA driver profiles were not changed.

## Results

| Native path | Cases | Actual execution evidence |
|---|---|---|
| SR, RGBA8 640x360 → 1280x720 | Auto / J / K / L / M, separate process each | Each created a Feature and completed six Draw calls plus six fence-completed GPU readbacks. Finite, spatially varying RGB output changed with input. Runtime logs explicitly recognize Balanced app hints J/K/L/M; Auto selects title default K. |
| FG, RGBA8 1280x720, no optical flow, honest zero/HUD-less contract | Auto / A / B / C at 2x | Each created a Feature, completed six Draw calls, and delivered five actual generated output callbacks/readbacks after the initial history-reset frame. |
| FG requested C, same fixture | 3x and 4x | Six Draw calls per case; ten and fifteen generated output callbacks/readbacks respectively; output varies across inputs. |
| DLSSNRMultiPass, one Pass, anti-flicker mode 1 | SDR RGBA8 and linear FP16, 512x288, separate process each | Six accepted synthetic captures each; initialized tau=5ms, live changed to 80 then 200ms. Each duplicate Draw immediately after a time-only edit preserved the exact previous output hash; subsequent new-capture output changed. Finite FP16 output verified. |

All completed cases signaled and awaited D3D11 readback fences, mapped output successfully and reported healthy D3D11 devices. FG/NR also drained production D3D12 work and returned `S_OK` from `GetDeviceRemovedReason`. These are bounded synthetic native execution checks, not real application presentation, image-quality or performance comparisons.

**Native NR Evaluate counters were not instrumented.** Duplicate output hashes are observable; they do not independently prove the network was not invoked. The no-extra-Evaluate assertion remains supported by the separate source/unit tests. No 250ms/reset behavior was changed by this harness.

## Confirmed global FG override

The upstream backend's read-only `drs_dlssg` event reports `profile="Base Profile"`, `profile_source="current_global"`. Key `0x10e41df1` (Override DLSS-FG preset) is **3**, and `0x10e41e03` (Enable DLSS-FG override) is **1**, both `predefined=false` and `location=current_profile`. Thus the test EXE inherited an existing global driver-profile override. The tool or person that originally wrote those settings is unknown.

For each FG case, the NGX callback reports `Feature [FrameGeneration] Override Reported: ModelPreset, Value: 3, Requested: Yes, Applied: Yes`. The pinned upstream [INSTALL.en.md](https://github.com/sdli1995/dlssg_for_sm86/blob/6f047bdc885e861575e8b049413ca31dc171a924/docs/INSTALL.en.md) maps DRS preset key `0x10E41DF1=3` to C/FG Lite. Managed INI remains `Preset=Auto`; no driver-profile writes were performed.

Accordingly, A/B/C application **requests and runtime execution** passed, but this machine did not provide independent effective-A/effective-B/effective-C comparisons. Identical small-fixture output hashes alone are not proof of an identical model; the explicit override callback and DRS record are the evidence. B's missing HUD-less/UI planes remain documented, even though Feature creation succeeded under the global override. SR's app-hint messages confirm the requested channel was recognized, but do not establish image quality or performance differences.

## Exact runtime identities

| DLL | File version | Bytes | SHA-256 |
|---|---|---:|---|
| nvngx_dlss.dll | 310.7.0.0 | 58,977,904 | `be6e434a94ca32499515eb62ca0e6c274526055d568d0426e4c652dcdfb6ee6e` |
| nvngx_dlssg.dll | 310.9.16.0 | 7,506,544 | `cadee1aa6db3907babdc3285da2aebc4553d92b89c38ba3eae8abd8bba1bea72` |
| nvngx_dlssnr.dll | 310.8.0.0 | 165,840,496 | `984bee0f775c277d5829b8fd6775d53a7b0f75396c852b3aaf06a18375f81014` |

Loaded paths were the isolated candidate-copy paths (FG uses its verified cache copy). The [receipt](20261022-dlss-native-smoke.json) records runtime and wrapper object hashes, per-case exit status/readback counts, NGX observations and the read-only profile record. These tests did not replace any DLL inside the candidate or deployed product.

## Reproduction and isolation

In a VS x64 developer shell:

```powershell
python tests/run_dlss_preset_smoke.py --source <candidate-source> --build-root <candidate-build-root> --output <new-probe-directory> --case sr 11 2
python tests/run_dlss_preset_smoke.py --source <candidate-source> --build-root <candidate-build-root> --output <same-probe-directory> --skip-build --case fg 3 4
python tests/run_dlss_preset_smoke.py --source <candidate-source> --build-root <candidate-build-root> --output <same-probe-directory> --skip-build --case nr 0 1
python tests/run_dlss_preset_smoke.py --source <candidate-source> --build-root <candidate-build-root> --output <same-probe-directory> --skip-build --case nr 1 1
```

SR values: Auto=0/J=10/K=11/L=12/M=13. FG values: Auto=0/A=1/B=2/C=3. NR's middle argument selects SDR=0 or FP16=1. Each invocation starts a new child process with a 120-second bound and retains its complete stdout/stderr/product log. A new output directory snapshots candidate objects/tlogs and only required adjacent runtime libraries. Reuse never refreshes an existing snapshot. The helper reconstructs a private static archive from the candidate objects because packaging strips `Magpie.Core.lib`; no app build is performed.

Only the SM86 loader's KnownFolder root is replaced in the helper, so its actual module/hash/cache/preparation logic runs against a task-local root. Original attempts used an overly deep task cache: first directory creation, then proxy/backend loading failed with path-length errors (206). Those failed processes/logs remain retained and were never retried in-process. Using the short filesystem spelling of `.sm86-audit` resolved the test layout limitation; normal production `LOCALAPPDATA` resolution was not exercised by this shim. No temp-directory exception or write to the installed Magpie cache was ultimately needed.

Local full evidence root for this run: `C:/Users/81443/Documents/Magpie enhance/worktrees/1022-sm86-fg/.sm86-audit/native-smoke`; upstream per-process loader/backend logs: `.sm86-audit/Magpie/DlssgSm86/`. The path-length failures are a harness-layout finding; longer real user-profile paths remain a product validation boundary.

Remaining checks: real video/game image quality, sustained performance/latency, HDR SR/FG presentation, real optical flow, all anti-flicker modes/pass counts, restart/resize/effect-group transitions in the GUI, and RTX20/30/40 hardware. No result here claims these passed.
