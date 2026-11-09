# Build and verify

## Current preview.3 (2026-11-09)

Production sources match the validated measurement-stage3 snapshot: stage2 main `8ADEAA05` plus stage3 helper `446FD260`. The release distributes those exact installed artifacts. Rebuild output can differ in compiler paths, PE metadata and optimization output.

Focused snapshot regression passed **14 main + 11 helper** cases. These cover guarded source FG, admission credits and a serial negative control, early capacity waiting, dual-Pass pipeline eligibility, disabled CPU/GPU measurements and helper liveness without trace clocks. User acceptance covered login/dual-Pass flicker, latency, dual-Pass smoothness and each measurement-removal stage. Fresh publication checks passed **14/14 main + 11/11 helper** focused tests after rebuilding both production targets from this repository. Installer fixtures passed validation-only, install, preserved configuration, rollback, external-change rejection, corrupt-payload rejection and path containment. Package payload hashes and ZIP CRC passed. Historical cloud runs remain separately labelled.

The main production links CPU/queue measurement-disabled backends; the helper uses `MGPU_HELPER_NO_TRACE=1`. Legacy instrumentation stress tests still compile their original test backends and may have different environmental requirements. The functional probe/maintenance path, resource synchronization, SDK markers and output-liveness state remain active.

See [current changes](PIPELINE-UPDATE.en.md), [artifact identities](PROVENANCE.md) and `scripts/Package-Pipeline.py`. Earlier preview.1 and preview.2 records below retain their original scope.

## Build instructions and historical verification

### Preview.3 publication results

- Both complete source builds succeeded with MSVC 14.44.35207 / Windows SDK 10.0.26100.0.
- Focused regression: **14/14 main + 11/11 helper**; complete helper suite: **23/23**.
- Complete main suite: **69/86**, with **17 failures**. Several legacy tests still require timestamp calibration, active trace hooks or measurable capture spans that this release deliberately disables. The recorded failures also include asynchronous WARP drain/coverage assertions and a depth-normalizer software-adapter failure; these have not all been independently attributed to measurement removal. Full-suite validation is therefore incomplete.
- The legacy fenced-pipeline test was adapted to the new submission-result arguments, and standalone aligned-input fixtures received source/module stubs. These are test-only publication changes; production source remains identical to the validated snapshot.
- Exact test logs and a machine-readable summary are retained under [evidence/preview3](evidence/preview3). No assertions were removed or failed tests suppressed.
- Installer and rollback fixtures passed, including configuration preservation and rejection of corrupt payloads, path traversal and external modifications. Published payload hashes match the user-confirmed installed files. Historical performance figures were not resampled.

The unchanged standard CI entrypoint runs the complete main suite first, so these remaining failures can stop it before helper testing. The separate local helper result above is recorded explicitly.

Windows x64, MSVC C++17, Windows SDK 10, CMake 3.21+, and Python 3 for evidence generation. Run the commands in an x64 Developer PowerShell.

```powershell
.\scripts\Fetch-Dependencies.ps1
.\scripts\Build.ps1
```

The first script fetches pinned ReShade/NGX headers and checks their hashes against `scripts/dependencies.json`. Streamline headers needed by the main module retain their original snapshot in `ext/streamline/`. The helper needs the complete official Streamline 2.14.16 SDK headers, fetched separately into the ignored dependency directory. ImGui declarations are fetched by the original CMake configuration at the recorded pin.

The build script configures the main add-on and `experimental/official_fg`, then runs both CTest suites. Hidden-window tests cover SDK/resource contracts; game testing covers visible output and input. Current source matches the pipeline preview.3 lineage and retains the RTX 5080 helper check. The separate RTX 50-series candidate is not merged.

The 2026-10-31 candidate build passed **77/77 main + 20/20 helper** tests locally. Source-option coverage includes repeated On-to-Off requests, original-thread/once-only forwarding, return-code and caller-memory preservation, bounded structure-version prefixes and live guard failures. Runtime foreground sixfold GPU acceptance is still pending; see [revision details](SOURCE-FG-OFF.zh-CN.md).

[Preview.2 GitHub run 41129943140](https://github.com/wc2395082443-del/MGPU-for-NTE/actions/runs/41129943140) compiled successfully and passed **76/77 main tests**. The existing fine-capacity WARP stress case hit its asynchronous GPU-drain timeout; helper build/tests were not reached. The new source-FG option/guard and Streamline route cases passed. The failure is retained without reducing assertions or repeatedly rerunning until green. This does not replace the separate local 77+20 result.

```powershell
.\scripts\Build.ps1 -BuildDirectory _build/nte
```

Outputs: `_build/nte/main/mgpu_bridge.addon64` and `_build/nte/helper/mgpu_official_host.exe`. Preview.3 uses archived, hash-pinned installed main8ADEAA05/helper446FD260 binaries; rebuilding may produce different hashes. Original preview.1 retains its M364/M363 artifacts. Preserve all input/resource retirement contracts when changing code.

`baseline/gpu1_context.cpp` supplies the M358 negative control required by M364's dispatch regression. It is not compiled into production. Source metadata inherited under `docs/` or `history/` may describe earlier upstream milestones; this fork's current release contract is under `docs/nte/`.

For the repository layout, test-only baseline paths were changed from the local work-folder layout to `baseline/` inside the repository. Helper negative controls use the archived M351 and M362 output-window headers in the same directory. Production code is unchanged by this packaging adjustment.

## Validation scope

Archived main regression: 75 CTest cases. Archived helper non-GPU regression: 18 cases, with a separate hidden 5080 official-present test. Build, fixture and game measurements are recorded separately.

Publication validation (2026-10-29): a fresh build passed **75/75 main tests and 20/20 helper tests**, including WARP transport and window negative controls. Installer tests cover validation-only, installation, configuration preservation, rollback, external-change rejection, corrupted payloads and path traversal. Viewer checks cover desktop/mobile, 670 marks, lane toggles and data export.

Both attempts of [GitHub run 40768218148](https://github.com/wc2395082443-del/MGPU-for-NTE/actions/runs/40768218148) compiled the main source and passed **74/75** main cases. `mgpu_nonblocking_fine_capacity_warp_test` (512 operations) hit its 20-second asynchronous drain timeout, taking 22.16/22.31 seconds overall. The job stopped before the helper build. The cause remains under investigation; the release uses the original M364/M363 binaries.

`python scripts/render-evidence.py` regenerates the SVG and self-contained five-frame HTML from the published evidence JSON. No GPU trace is acquired by rendering these artifacts.

`python scripts/render-comparison.py` regenerates the before/after comparison SVGs and self-contained HTML, including the separately labelled M364 instrumented window. `python scripts/test_comparison.py` checks actual five-cycle boundaries, equal plot scales, COPY overlap, coverage caveats and the absence of fabricated display lanes. Browser controls and desktop/mobile rendering were separately verified.
