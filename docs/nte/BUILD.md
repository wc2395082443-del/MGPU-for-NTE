# Build and verify

Windows x64, MSVC C++17, Windows SDK 10, CMake 3.21+, and Python 3 for evidence generation. Use an x64 Developer PowerShell, not a machine-specific absolute compiler path.

```powershell
.\scripts\Fetch-Dependencies.ps1
.\scripts\Build.ps1
```

The first script fetches pinned ReShade/NGX headers and checks their hashes against `scripts/dependencies.json`. Streamline headers needed by the main module retain their original snapshot in `ext/streamline/`. The helper needs the complete official Streamline 2.14.16 SDK headers, fetched separately into the ignored dependency directory. ImGui declarations are fetched by the original CMake configuration at the recorded pin.

The build script configures the main add-on with tests and separately configures M363 `experimental/official_fg`, then runs both CTest suites. The hidden window/adapter-specific hardware fixture is not automatically treated as a game acceptance test. The original helper intentionally rejects non-5080 hardware.

```powershell
.\scripts\Build.ps1 -BuildDirectory _build/nte
```

Outputs: `_build/nte/main/mgpu_bridge.addon64` and `_build/nte/helper/mgpu_official_host.exe`. The release ZIP uses archived, hash-pinned deployed M364/M363 binaries; rebuilding may produce different hashes. Preserve all input/resource retirement contracts when changing code.

`baseline/gpu1_context.cpp` supplies the M358 negative control required by M364's dispatch regression. It is not compiled into production. Source metadata inherited under `docs/` or `history/` may describe earlier upstream milestones; this fork's current release contract is under `docs/nte/`.

For the repository layout, test-only baseline paths were changed from the local work-folder layout to `baseline/` inside the repository. Helper negative controls use the archived M351 and M362 output-window headers in the same directory. Production code is unchanged by this packaging adjustment.

## Validation scope

Archived main regression: 75 CTest cases. Archived helper non-GPU regression: 18 cases plus the separately recorded hidden 5080 official-present test. New packaging/build and fixture results are recorded with the release; compilation/tests alone never establish FPS gains, visible uniform sixfold presentation or input latency.

Publication validation (2026-10-29): a fresh build of this repository passed **75/75 main tests and 20/20 helper tests**, including WARP transport and the window negative controls. Installer fixture tests cover validation-only, installation, preserved configuration, rollback, rejecting externally modified files, corrupted payloads and path traversal. The five-frame viewer passed desktop/mobile, 670-mark count, lane toggles and data export checks. This is not a fresh game capture.

`python scripts/render-evidence.py` regenerates the SVG and self-contained five-frame HTML from the published evidence JSON. No GPU trace is acquired by rendering these artifacts.
