# Build and verify

Windows x64, MSVC C++17, Windows SDK 10, CMake 3.21+, and Python 3 for evidence generation. Run the commands in an x64 Developer PowerShell.

```powershell
.\scripts\Fetch-Dependencies.ps1
.\scripts\Build.ps1
```

The first script fetches pinned ReShade/NGX headers and checks their hashes against `scripts/dependencies.json`. Streamline headers needed by the main module retain their original snapshot in `ext/streamline/`. The helper needs the complete official Streamline 2.14.16 SDK headers, fetched separately into the ignored dependency directory. ImGui declarations are fetched by the original CMake configuration at the recorded pin.

The build script configures the main add-on and `experimental/official_fg`, then runs both CTest suites. Hidden-window tests cover SDK/resource contracts; game testing covers visible output and input. Current source matches the source-FG-off preview.2 lineage and retains the RTX 5080 helper check. The separate RTX 50-series candidate is not merged.

The 2026-10-31 candidate build passed **77/77 main + 20/20 helper** tests locally. Source-option coverage includes repeated On-to-Off requests, original-thread/once-only forwarding, return-code and caller-memory preservation, bounded structure-version prefixes and live guard failures. Runtime foreground sixfold GPU acceptance is still pending; see [revision details](SOURCE-FG-OFF.zh-CN.md).

```powershell
.\scripts\Build.ps1 -BuildDirectory _build/nte
```

Outputs: `_build/nte/main/mgpu_bridge.addon64` and `_build/nte/helper/mgpu_official_host.exe`. Preview.2 uses archived, hash-pinned installed main20CF7B73/helper118BBCBA binaries; rebuilding may produce different hashes. Original preview.1 retains its M364/M363 artifacts. Preserve all input/resource retirement contracts when changing code.

`baseline/gpu1_context.cpp` supplies the M358 negative control required by M364's dispatch regression. It is not compiled into production. Source metadata inherited under `docs/` or `history/` may describe earlier upstream milestones; this fork's current release contract is under `docs/nte/`.

For the repository layout, test-only baseline paths were changed from the local work-folder layout to `baseline/` inside the repository. Helper negative controls use the archived M351 and M362 output-window headers in the same directory. Production code is unchanged by this packaging adjustment.

## Validation scope

Archived main regression: 75 CTest cases. Archived helper non-GPU regression: 18 cases, with a separate hidden 5080 official-present test. Build, fixture and game measurements are recorded separately.

Publication validation (2026-10-29): a fresh build passed **75/75 main tests and 20/20 helper tests**, including WARP transport and window negative controls. Installer tests cover validation-only, installation, configuration preservation, rollback, external-change rejection, corrupted payloads and path traversal. Viewer checks cover desktop/mobile, 670 marks, lane toggles and data export.

Both attempts of [GitHub run 40768218148](https://github.com/wc2395082443-del/MGPU-for-NTE/actions/runs/40768218148) compiled the main source and passed **74/75** main cases. `mgpu_nonblocking_fine_capacity_warp_test` (512 operations) hit its 20-second asynchronous drain timeout, taking 22.16/22.31 seconds overall. The job stopped before the helper build. The cause remains under investigation; the release uses the original M364/M363 binaries.

`python scripts/render-evidence.py` regenerates the SVG and self-contained five-frame HTML from the published evidence JSON. No GPU trace is acquired by rendering these artifacts.

`python scripts/render-comparison.py` regenerates the before/after comparison SVGs and self-contained HTML, including the separately labelled M364 instrumented window. `python scripts/test_comparison.py` checks actual five-cycle boundaries, equal plot scales, COPY overlap, coverage caveats and the absence of fabricated display lanes. Browser controls and desktop/mobile rendering were separately verified.
