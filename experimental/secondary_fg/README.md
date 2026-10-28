# Secondary-GPU NVIDIA DLSS FG prototype

Status: isolated development code, NOT a working game plug-in. Nothing here is
installed into Neverness To Everness. The existing MGPU installation is unchanged.

Target pipeline:

    RTX 4080 SUPER: game rendering -> game's own DLSS SR
    cross-adapter copy: real colour + matching depth/MV + camera metadata
    RTX 5080: MGPU NR -> NVIDIA DLSS FG -> paced interpolated/real presentation

This is NVIDIA FrameGeneration feature 11, not LSFG, FSR, a duplicate-frame
counter, or an attempt to migrate the game's existing FG handle across devices.
The initial target is 2x (one generated frame), not multi-frame generation.

## Pinned inputs

- MGPU source snapshot: `6bb54cf0d006b1d4e8f18eaba9fca891e352f75d`.
- NVIDIA DLSS headers: `a291cc7d2cc642a51566f3dfd5376f635cd1b284`, matching MGPU CI.
- The 64 upstream `src/` files have been retrieved and verified against pinned
  Git blob hashes. The eight pinned ReShade headers are present. Dependency
  acquisition and the native Windows build are still incomplete; there is no
  buildable replacement MGPU add-on yet.
- NVIDIA headers retain their upstream license notices. No NVIDIA runtime DLL
  is bundled or modified by this prototype.

## Implemented

- Dedicated FG parameter block, helper-style feature creation without a generic
  scratch query, and an NVIDIA FG evaluate call through typed NGX entry points.
  The evaluate path exists in source but has not been exercised on a GPU.
- Adapter, texture extent/format, output aliasing, and UAV flag checks.
- Explicit transition/restoration of caller-owned resources and output UAV barriers.
- Same-frame colour/depth/MV/camera contract, startup/discontinuity resets, and
  rejection of already frame-generated input.
- Submission-fence tracking before reusing or releasing FG state.
- A CPU presentation-pair policy: interpolated first, current real second,
  with a half-frame deadline and no buffer overwrite while the pair is pending.
- CPU-only tests. A passing test does not demonstrate working NVIDIA inference.
- An opt-in, separate-process NVIDIA requirements query and optional 1280x720
  FG creation test, selecting exactly one RTX 5080 by DXGI identity. The wrapper
  verifies official DriverStore signatures, preserves JSON reports, and limits
  the process to 30 seconds. A Windows job contains runtime children.
- A small MSVC-ABI parameter bridge compiled separately from the MinGW probe.
  All six parameter methods used by the probe and stage were checked against the
  pinned official SDK static library. A fake MSVC-ABI parameter object tests
  overload dispatch and transferred values. No vtable offsets are hardcoded.

## Required integration work, still NOT implemented

1. Finish dependency acquisition and the complete native MGPU build.
2. Integrate the independently tested official FG session into MGPU on the 5080.
   Do not reuse NR's snippet functions or singleton capability parameter block.
   The stage borrows that session; the separate probe initializes one. Both now
   follow the helper-style creation sequence without a generic scratch query.
3. Capture real, unjittered camera constants and prove their frame identities.
   Current MGPU depth/MV counters do not establish this contract. Do not invent
   identity matrices or camera planes to make the input gate pass.
   This is the prototype's conservative policy, not an unconditional NVIDIA SDK
   requirement: `NGX_D3D12_EVALUATE_DLSSG` accepts a null optional-camera block.
   Omitting that block does not establish correct defaults or picture quality.
4. Normalize motion vectors using measured engine units, not NR's scale copied
   unmodified. NVIDIA FG expects vectors normalized to [-1, 1]. NR changes to
   silhouettes also need quality validation against the unchanged engine guides.
5. Allocate persistent output texture and a >=4-byte UAV disable-interpolation
   buffer on the 5080. Keep all caller resources alive until the tracked fence
   completes. Clear the hint conservatively before each evaluation and read it
   back after the fence; an absent/unknown hint must suppress generated display.
6. Record FG after NR with valid guide states, submit on the same ordered queue,
   signal its fence, call `track_submission`, then `complete` after GPU completion.
   Creation also records commands and needs a tracked submission before use or
   release, even when CreateFeature returns an error. Discarded/unsubmitted
   command-list recovery needs a separate integration path before deployment.
7. Replace the worker's consume-all/latest-only behavior while a pair is pending.
   Present the generated texture, wait until the real-frame deadline with a
   bounded, shutdown-aware wait, present the corresponding real texture, and
   only then consume another NR frame. Do not present both immediately and call
   it 2x. Measure actual compositor acceptance rather than counting Present calls.
8. Keep in-game native FG OFF for this path, without turning off game SR.
9. Add full feature-lifecycle tests beyond the parameter-ABI and invalid-call tests,
   then a separately authorized inference test. No evaluation or game test has run.

## Build CPU tests

    cmake -S experimental/secondary_fg -B build/secondary-fg
    cmake --build build/secondary-fg --config Release
    ctest --test-dir build/secondary-fg -C Release --output-on-failure

The Windows stage uses the official SDK headers under `ext/ngx`. The portable
frame-contract test needs only a C++17 compiler. Building does not start the game,
touch either GPU, alter a driver, or install an add-on.

## Local validation, 2026-10-15

- Portable compiler installed only under `devtools/ziglang`: Zig 0.13.0.
- `frame_contract_test`: 36 checks passed, no GPU use.
- `stage_api_test`: compiled/linked with `-Wall -Wextra -Werror`; all six typed
  MSVC-ABI parameter bridge calls and invalid-call guards passed. No runtime
  loaded, initialized, or evaluated.
- `capability_probe`: compiled/linked with warnings as errors using
  `build_probe.ps1`. The parameter bridge targets `x86_64-windows-msvc`, uses no
  CRT or SDK runtime library, and exposes only C entry points to the MinGW host.
- PowerShell wrapper: syntax check and opt-in rejection check passed.
- Local Windows hosts use `x86_64-windows-gnu`; both the probe and stage route all
  NGX C++ parameter-object calls through the separately compiled MSVC-ABI bridge.
  The audit covers six typed methods. This does not replace native add-on build
  validation, NVIDIA feature lifecycle testing, or a GPU evaluation test.
- No MSVC, Windows SDK, or CMake installation was found in the inspected standard
  locations/PATH. No system-wide development toolchain was installed.
- A later local-only toolchain bootstrap provided MSVC 19.44, CMake 3.31.6,
  Ninja 1.12.1 and UCRT under `devtools/portable-msvc`. The native STL compile
  passed, but Windows/DirectX SDK headers and import libraries are incomplete.
  No native add-on DLL was linked. See `build/PORTABLE_TOOLCHAIN_STATUS.md`.
  Dependency downloads have stopped; no downloader was left running. The GitHub
  dependency fetch also stopped at the public API limit before completing the
  remaining Streamline headers/license and ImGui inputs.
- Authorized independent GPU validation completed at local time
  `2026-10-15 22:53:01` (use the JSON UTC timestamp for portable provenance):
  selected 5080 LUID `00000000:00010C57`, requirements success/support flags 0,
  NGX init success, FG available 1, feature 11 creation success with a non-null
  handle, initialization fence completed, release and shutdown success,
  remaining sessions 0, process exit 0. No EvaluateFeature call was made.
- Evidence: `probe-logs/probe-20261015-225301.json` records full logs, selected
  adapter, runtime hashes and executable hash. The SDK selected its cached
  DLSSG `310.9.16` override, not necessarily the queried DriverStore feature DLL.
- Earlier attempts are retained, including one process access violation during
  cleanup. Its cause was a missing second output argument to the driver's core
  `Shutdown1`, which differs from the public SDK wrapper. The corrected path
  completed without that crash. Core `Init_ProjectID` similarly puts SDK version
  before common info; the public `Init_with_ProjectID` wrapper has the reverse
  order. These were checked against the pinned SDK object code, not guessed.
- Generic GetScratchBufferSize returned `0xBAD00002` with `Not called from NGX
  runtime`. Renaming/spoofing the executable or patching that check was not used.
  The final probe follows `NGX_D3D12_CREATE_DLSSG`, which performs no scratch query.
- No game launch, inference, frame pacing, or NR-to-FG integration is demonstrated.
  No driver, display topology, GPU enable state, or installed MGPU file was changed
  by the scripts. NVIDIA initialization itself accesses its global NGX model cache
  and logs that it updates that cache's ACL and sends initialization telemetry.

## Reproduce the isolated checks

    ./build_probe.ps1
    $env:PYTHONPATH = '<root>/devtools/pe-tools'
    python ./audit_parameter_bridge.py
    ./run_capability_probe.ps1 -NvidiaCore '<DriverStore folder>/_nvngx.dll' -AllowSdkQuery -InitializeFg

The static audit needs the pinned library under `ext/ngx` and its extracted
`nvsdk_ngx_parameters_lib.obj` under `build/sdk-audit`; extract with portable
`zig ar x --output <root>/build/sdk-audit <root>/ext/ngx/nvsdk_ngx_s.lib`.
Python audit dependencies (`lief`, `capstone`, `pefile`) are local to devtools.
The initialization wrapper pins audited core/runtime hashes, not just signatures:
after a driver upgrade, re-audit its internal init/shutdown ABI before enabling
feature creation again. A requirements-only query does not use that internal ABI.
Downloading or reviewing the programming-guide PDF was not required to establish
the helper's source-code sequence; an attempted PDF download was stopped.

## Game-side acquisition check

The inspected `HTGame.exe` imports neither Streamline nor NGX through its normal
or delay import tables. This does not prove that no other module imports them,
but the existing Streamline IAT observer cannot be assumed to receive the game's
calls. Merely resolving `slSetConstants` or `slSetTagForFrame` exports proves no
per-frame data has been captured. A source of matching real-frame guides remains
an integration blocker, in addition to the missing consumer and presentation
wiring. No new add-on has been copied into the game.

Post-test observation at local `22:54:10` through `22:54:25` (15.3 seconds):
5080 50 C, 12-13% total GPU use, 5104-5156 MiB total VRAM;
4080 SUPER 33 C, 0% GPU use, 0 MiB VRAM. These are total adapter readings,
including desktop/apps, not isolated FG benchmark measurements. No new System
4101/41/1001 event was found after `22:52`. The earlier probe child PID 39408
was confirmed exited, with zero threads and handles; its process record remained
enumerable. The installed MGPU config and add-on hashes still match below.

Existing game files inspected at the end of this development stage:

    mgpu.ini
    430DE331E87408BEC25942A520D69ED49FCDA31296CB2F19FE7CB3F3F2FD3643
    nvngx.dll_mgpu_bridge.addon64
    B2835FB9AF03C60CFBEECBC46EC65D5A27C720CAE27D626FCA2909ED0368450F

Do not copy these test executables or objects into the game directory. None is
a replacement MGPU add-on, and no working NR -> FG presentation is claimed.
