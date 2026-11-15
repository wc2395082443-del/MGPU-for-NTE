# MGPU for NTE

[中文](README.md) · [Install](docs/nte/INSTALL.zh-CN.md) · [Measurements](docs/nte/PERFORMANCE.zh-CN.md) · [Build](docs/nte/BUILD.md)

An experimental Neverness to Everness adaptation of [Neural Coprocessor](https://github.com/maohgad-web/Neural-coprocessor). The initial release pins **M364 bridge + M363 official-FG helper**.

The RTX 4080 SUPER renders the game and runs its game-side DLSS super resolution. The RTX 5080 receives matched color/depth/motion/camera inputs, runs neural rendering, and uses an independent Streamline/Reflex/PCL process for frame generation and presentation. This release targets the tested NTE build and **GeForce RTX 5080 helper**.

The current [lifecycle preview.4](https://github.com/wc2395082443-del/MGPU-for-NTE/releases/tag/nte-lifecycle-preview.4), dated2026-11-16, ships accepted **B09C main + E172 helper**. RTX4080 SUPER renders with source FG disabled; RTX5080 performs NR and NVIDIA official FG, requesting up to6x. See [changes and five-frame measurements](docs/nte/LIFECYCLE-UPDATE.en.md).

Input admission reserves credits before render submission, uses a three-frame lifecycle budget and recovers after timeout when real NR completion releases capacity. The user confirms retained smoothness and noticeably lower latency. Versus AE5C, simulation→first display in the associated batch mean age falls **136.80→111.15ms**, with display interval P95 **9.52→9.68ms**. This is frame age, not hardware input-to-photon latency; camera actions/load differ, and measurement overhead is included.

![Measured five-frame NR/FG pipeline](docs/nte/figures/lifecycle-five-frames.svg)

[Offline viewer](docs/nte/lifecycle-five-frame-timeline.html) · [Three-version data](docs/nte/evidence/preview4/comparison.json). This release retains diagnostic code; measurement-disabled preview.3 remains available. One pre-capture1s timeout recovered, and loading/menu cases still need improvement. The separate other50-series candidate remains excluded; the helper accepts RTX5080.

The previous preview.3 recovery, asynchronous dual-Pass and instrumentation-removal history remains in [its change record](docs/nte/PIPELINE-UPDATE.en.md). Historical FPS figures below retain their original scope.

**Sixfold frame generation uses NVIDIA's official DLSS Frame Generation runtime through Streamline/DLSS-G, rather than Lossless Scaling. The user's latest feedback describes the visual result as excellent.** This project connects the NR image and guides to that runtime and manages the output window.

## Performance: single GPU, dual-GPU optimization, then sixfold generation

The user's NTE gameplay and NR-stage figures:

### 1. Single-GPU and workload references

| GPU | Workload | User-reported result |
| --- | --- | --- |
| RTX 4080 SUPER | Game rendering, neural rendering disabled | **Approximately 40 FPS** |
| RTX 4080 SUPER | Game + NR, DLSS Quality, maximum settings and full ray tracing | **Approximately 22 FPS** |
| RTX 5080 | NR stage only | **Approximately 50 NR frames/s** |

The 4080S records approximately 40 FPS with neural rendering disabled and 22 FPS while running the game and NR together. The 5080-only NR figure describes processing throughput for the neural stage.

### 2. Dual-GPU DLSS neural rendering: base-frame improvement

| GPU assignment | Before optimization | After optimization |
| --- | ---: | ---: |
| **4080S game rendering + 5080 NR** | **Approximately 28 FPS** | **Approximately 34 FPS** |

These optimization-stage figures show an approximately **21.4%** base-frame increase.

### 3. NVIDIA official sixfold frame generation

| Setup | Game GPU | NR/FG GPU | Game DLSS mode | FG request | User-reported display FPS |
| --- | --- | --- | --- | --- | --- |
| Single RTX 5080 | RTX 5080 | None | Ultra Performance | 6x | Approximately 120 |
| **Dual-GPU setup** | **RTX 4080 SUPER** | **RTX 5080** | **Quality** | 6x | **Stable above 160** |

Figures were supplied by the user on 2026-10-29. This setup demonstrates a non-50-series primary GPU. The first-release helper supports RTX 5080; other GPU combinations require adaptation and testing.

## Helper responsibilities and FG overhead

The helper is the RTX 5080's independent official-FG/display endpoint. It validates same-frame NR/depth/motion/camera tuples, manages shared/private resource retirement, relays real source Reflex/PCL markers, submits the official proxy Present and maintains an isolated mouse-passthrough output window and diagnostics. Game rendering and NR complete upstream.

FG runs on the secondary GPU, allowing base throughput to stay approximately unchanged when that GPU has headroom. Historical windows recorded **33.02 unique NR frames/s without FG (M343)** and **33.05 real frames/s with a sixfold request (M351), SDK count 194.85/s**. Base throughput stayed close in these samples. NR and FG share the 5080; copies, synchronization and presentation feedback also use resources.

[Detailed helper documentation](docs/nte/HELPER.zh-CN.md) covers threads, fences, official/manual responsibilities, window/input isolation, rates, compatibility and current limitations. Completed six-image post-FG buffering is not implemented.

## Changes

- Stabilized same-frame input ownership, frame identities, snapshots and fence-based retirement, including skipped-frame ring-slot alignment.
- Added staged color/depth/motion COPY handoffs so receiving can begin before the complete tuple arrives, while inference still requires all inputs.
- Integrated NVIDIA's official frame-generation runtime in the output helper, with input queue isolation and layered mouse passthrough.

![Historical performance](docs/nte/figures/performance.svg)

Historical unique neural completions increased from **26.699/s to 33.451/s (~25.3%)** between M339c and M340 + ForceTagging, alongside changes to COPY, tagging and resource submission. Transport elapsed decreased **7.073 → 6.013 → 5.502ms**. M341's additional transfer reduction left overall FPS approximately unchanged. Later runs reported around 28 base FPS. The measurement page records versions, sampling conditions and rate definitions.

Sixfold requests up to five generated frames per real frame. Visible pacing and latency remain areas for improvement. Post-generation buffering of six completed images is planned. M365 was withdrawn; M366/M367 experiments remain separate.

Preview.4 publication passes13 focused main +12 focused helper cases and24/24 complete helper cases. The full main build stops on the historical GPU-measurement-disabled static assertion; no complete-main pass is claimed. [Build records](docs/nte/BUILD.md) retain that failure. This release keeps timing and trace diagnostics; hardware input-to-photon latency remains unmeasured.

![Before and after, five measured cycles on one scale](docs/nte/figures/five-frame-comparison.svg)

Five source-ready cycles span **190.184ms before (M339c) versus 151.709ms after (M340 + ForceTagging)**, on the same millisecond scale. The figure uses two historical captures. Both overlap neural rendering with source DIRECT work.

![Staged COPY handoff detail](docs/nte/figures/copy-handoff-comparison.svg)

The transport change allows color reception while the producer is still sending guides. [Interactive comparison](docs/nte/five-frame-comparison.html) (download and open locally) includes exact tooltips, lane controls, JSON export and a separate M364 + M363 window with detailed collection enabled. See [data selection](docs/nte/COMPARISON.zh-CN.md).

## Distribution

The release ZIP contains our bridge/helper binaries, example configuration, shader, integrity checks, backup/rollback installer and local runtime import tool. NVIDIA runtime/model binaries, ReShade and game assets are **not redistributed**. Supply the matching dependencies from your own installation; see [dependencies](docs/nte/DEPENDENCIES.md).

Tested game EXE SHA256: `64ED3CAE0B5FC61F3B56CDD933B4807CB23F0EC7913590DB60D1A8642DE2DFEC`. Some integration routes use build-specific guards. The project loads through ReShade; compatibility depends on the game build and its anti-cheat policy.

MIT license and upstream attribution are retained. Maintained by [wc2395082443-del](https://github.com/wc2395082443-del). Exact source/binary provenance and test limits are documented in [PROVENANCE.md](docs/nte/PROVENANCE.md).
