# Pipeline and low-latency preview.3

Released 2026-11-09. This release packages the installed, user-confirmed main `8ADEAA05` and helper `446FD260` artifacts. The 4080 SUPER renders and performs game-side DLSS super resolution with source FG switched off. The 5080 performs NR followed by NVIDIA official frame generation, with up to a sixfold request.

- Refresh guarded ForceTagging/source-FG binding for the supported NTE build.
- Keep the helper alive across temporary source-marker gaps after photo/menu operations, and recover on valid input.
- Hide stale output overlays and resume when fresh completions return.
- Reclaim retired capture slots while protecting complete pending tuples.
- Bound source lead with three in-flight credits. When full, wait for the oldest task to free capacity, retaining cross-frame render/COPY/NR overlap.
- Wait for capacity before the original Reflex Sleep, retaining post-submit protection.
- Allow official dual-Pass NR through the existing three-slot asynchronous path, preserving both evaluations, settings and barriers.
- Disable optional GPU timing/pixel samples, main detailed CPU/queue capture and pure counters, then helper trace allocation/events/writer/files/stage timings and pure rate statistics.

Functional locks, fences, source identities, Reflex/PCL, SDK completion state, heartbeat, timeout and output-liveness checks remain. MetaProbe, PresentAudit, PanelStats, some counters/logs and legacy conditional readbacks remain. Some panel diagnostic/rate fields no longer update; external RTSS can still monitor the helper.

The user confirmed both login and dual-Pass flicker resolved, low latency after moving the wait, smoother dual-Pass behavior, and normal operation after each measurement-removal stage. These are user observations; this revision has no new controlled FPS or input-to-photon measurement. Historical 28→34 base FPS and 160+ displayed FPS retain their original conditions.

The validated snapshot passed 14 main and 11 helper focused tests. Publication build/package checks are recorded in [BUILD.md](BUILD.md). Exact artifacts and supported game hash are in [PROVENANCE.md](PROVENANCE.md). The helper still targets RTX 5080; other 50-series and Cyberpunk experiments are excluded.

Use `MGPU-for-NTE-pipeline-preview.3.zip` and the [installer](INSTALL.zh-CN.md). Existing configurations are preserved and replaced files are backed up. NVIDIA runtime/model assets, ReShade and game files are supplied separately. Earlier releases remain available.
