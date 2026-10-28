# MGPU for NTE

[中文](README.md) · [Install](docs/nte/INSTALL.zh-CN.md) · [Measurements](docs/nte/PERFORMANCE.zh-CN.md) · [Build](docs/nte/BUILD.md)

An experimental Neverness to Everness adaptation of [Neural Coprocessor](https://github.com/maohgad-web/Neural-coprocessor). The initial release pins **M364 bridge + M363 official-FG helper**.

The RTX 4080 SUPER renders the game and runs its game-side DLSS super resolution. The RTX 5080 receives matched color/depth/motion/camera inputs, runs neural rendering, and uses an independent Streamline/Reflex/PCL process for frame generation and presentation. This helper currently accepts **GeForce RTX 5080 only**; other GPUs and game builds are not claimed supported.

## User-reported gameplay comparison

For NTE with DLSS neural rendering and the user's full-ray-tracing setting, the user reports:

| Setup | Game GPU | NR/FG GPU | Game DLSS mode | FG request | Reported display FPS |
| --- | --- | --- | --- | --- | --- |
| Before the dual-GPU adaptation | RTX 5080 | None | Ultra Performance | 6x | Approximately 120 |
| Dual-GPU setup | **RTX 4080 SUPER (not RTX 50-series)** | **RTX 5080** | **Quality** | 6x | **User reports stable above 160** |

These are newly supplied user observations, not a new instrumented scanout measurement or equal-quality controlled A/B. Resolution, scene and driver were not independently revalidated for this comparison. The architecture demonstrates a non-50-series primary GPU; it does **not** establish arbitrary primary-GPU support. The exact first-release helper remains RTX 5080-only, with other capable RTX 50-series secondary GPUs requiring adaptation and validation.

## Helper responsibilities and FG overhead

The helper is the RTX 5080's independent official-FG/display endpoint. It validates same-frame NR/depth/motion/camera tuples, manages shared/private resource retirement, relays real source Reflex/PCL markers, submits the official proxy Present and maintains an isolated mouse-passthrough output window and diagnostics. It does not render the game scene or perform NR itself.

Because FG runs on the secondary GPU rather than re-rendering five game scenes on the primary GPU, base throughput can remain approximately unchanged when the secondary GPU has headroom. Separate historical windows recorded **33.02 unique NR frames/s without FG (M343)** and **33.05 real frames/s with a sixfold request (M351), SDK count 194.85/s**. No clear base-frame reduction was observed in those windows; this is not a controlled FG toggle experiment or a universal zero-overhead claim. NR and FG share the 5080, and copies, synchronization and presentation backpressure still cost work.

[Detailed helper documentation](docs/nte/HELPER.zh-CN.md) covers threads, fences, official/manual responsibilities, window/input isolation, rates, compatibility and current limitations. Completed six-image post-FG buffering is not implemented.

## Changes

- Stabilized same-frame input ownership, frame identities, snapshots and fence-based retirement, including skipped-frame ring-slot alignment.
- Added staged color/depth/motion COPY handoffs so receiving can begin before the complete tuple arrives, while inference still requires all inputs.
- Integrated NVIDIA's official frame-generation runtime in the output helper, with input queue isolation and layered mouse passthrough.

![Historical performance](docs/nte/figures/performance.svg)

Historical unique neural completions increased from **26.699/s to 33.451/s (~25.3%)** between M339c and M340 + ForceTagging. This is **not an isolated A/B result**: tagging/resource paths and workloads differ. Transport elapsed decreased **7.073 → 6.013 → 5.502ms**; the additional M341 reduction did **not** demonstrate an FPS gain. Later runs still reported approximately 28 base FPS. See the measurement page for the full distinction between source completions, API counts, display events and heavy-instrumentation results.

Sixfold means requesting up to five generated frames per real frame, not sixfold input responsiveness or verified physical scanout. Visible pacing and latency remain imperfect. Post-generation buffering of six completed images is not implemented. M365 was withdrawn; M366/M367 experiments are excluded.

Publication checks passed locally (75 main + 20 helper cases). Two GitHub-hosted runs each passed 74/75 main cases: the fine-capacity recording stress fixture timed out during asynchronous drain. The root cause remains unverified; cloud helper tests were not reached. This limitation is retained publicly, not hidden by skipping the test. See [CI](https://github.com/wc2395082443-del/MGPU-for-NTE/actions/runs/40768218148).

![Before and after, five measured cycles on one scale](docs/nte/figures/five-frame-comparison.svg)

Five source-ready cycles span **190.184ms before (M339c) versus 151.709ms after (historical M340 + ForceTagging)**, on the same millisecond scale. These are separate captures, not a controlled single-variable A/B or a measurement of the current release. Both already overlap neural rendering with source DIRECT work.

![Staged COPY handoff detail](docs/nte/figures/copy-handoff-comparison.svg)

The direct transport change is receiving color while the producer is still sending guides. [Interactive comparison](docs/nte/five-frame-comparison.html) (download and open locally) includes exact tooltips, lane controls, JSON export and a separately labelled M364 + M363 instrumented five-frame window. Current heavy-instrumentation results are not substituted for the historical improvement or compared as a clean FPS A/B. See [selection and limitations](docs/nte/COMPARISON.zh-CN.md).

## Distribution

The release ZIP contains our bridge/helper binaries, example configuration, shader, integrity checks, backup/rollback installer and local runtime import tool. NVIDIA runtime/model binaries, ReShade and game assets are **not redistributed**. Supply the matching dependencies from your own installation; see [dependencies](docs/nte/DEPENDENCIES.md).

Tested game EXE SHA256: `032EB661CE65E0669FB643A2E87E727D5574BBA832CD6FE34FF0AB1D134CD788`. Some integration routes use build-specific guards. This is a ReShade-based modification, not an official NVIDIA/game integration. No anti-cheat bypass is provided.

MIT license and upstream attribution are retained. Maintained by [wc2395082443-del](https://github.com/wc2395082443-del). Exact source/binary provenance and test limits are documented in [PROVENANCE.md](docs/nte/PROVENANCE.md).
