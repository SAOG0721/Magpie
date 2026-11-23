# Consistency checks

Zero-dependency (Python 3.11+ stdlib) repository consistency checks. They encode
rules that are already documented by the maintainers
(`docs/experimental/RELEASE-WORKFLOW.md`, `docs/EXPERIMENTAL_HANDOFF_ZH.md`) so
mechanical drift is caught before a release or PR, not during it.

```powershell
python tests/consistency/run.py
# exit code 1 on any FAIL; --json writes consistency-report.json
```

The CI checks also run the generated catalog and effect description contracts and
the source placement CPU test. Run these separately with the same Windows build
tools as the main project.

```powershell
pwsh -NoProfile -File tests/Validate-EffectCatalog.ps1 -CatalogOnly
python tests/validate_effect_descriptions.py
pwsh -NoProfile -File tests/Run-SourcePlacementTransitionTests.ps1
```

Run `Validate-EffectCatalog.ps1` without `-CatalogOnly` when reviewing effects
or their native backends. The full audit checks the recorded backend source hashes.

Rules:

| Rule | Level semantics | What it checks |
| --- | --- | --- |
| R1 | FAIL/INFO | `version.json`, both README download entries and the public section of `docs/RELEASE_NOTES_NEXT.md` agree |
| R2 | FAIL | main `RELEASE_NOTES_v*-experimental.md` files keep the bilingual structure (Chinese block, `---`, English block) |
| R3 | FAIL/WARN | Preset effect IDs and parameter keys resolve against the current shader metadata. Known startup and import migrations produce warnings for legacy inputs |
| R4 | FAIL/INFO/WARN | every `Resources.language-*.resw` parses, has no duplicate keys; per-language coverage vs en-US is reported (zh-* gaps are WARN; other languages INFO). Translations themselves are managed on Weblate — this suite never edits them |
| R5 | INFO/WARN | effect-parameter label keys (`ScalingModes_EffectParam_<effect>_<param>`, PR #16 convention) — coverage report |
| R6 | FAIL/INFO | XAML `x:Uid` references must exist in en-US; en-US keys with zero code/XAML references are listed as cleanup candidates (maintainer decision — resw files are Weblate-managed) |
| R7 | FAIL/WARN | Relative links in `docs/**` resolve inside the checkout. The check decodes escaped paths and skips absolute local links. Missing historical review references or surrounding workspace evidence produce warnings |
| R8 | FAIL | files referenced by `.github/workflows/build.yml` exist |
| R9 | FAIL | `scripts/Build-Release.ps1` keeps the required runtime layout entries |

Adding a rule = one function named `rN_*` plus one entry in `RULES`. A rule that
crashes is reported as FAIL rather than silently skipped.
