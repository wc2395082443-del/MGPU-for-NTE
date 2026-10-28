# MGPU for NTE — M364 + M363 preview

First NTE adaptation release: same-frame stability fixes, staged multi-GPU COPY transport, RTX 5080 official Streamline frame generation, and isolated mouse-passthrough output helper.

The ZIP contains our hash-pinned deployed add-on/helper binaries, depth shader, example configuration, integrity manifest, reversible installer, and local NVIDIA runtime importer. It excludes NVIDIA model/runtime binaries, game files and ReShade. Follow the repository's install/dependency instructions; matching runtimes are required.

Historical unique NR completions increased 26.699→33.451/s (~25.3%) across M339c to M340+ForceTagging, with multiple conditions changing. Full transport elapsed decreased 7.073→6.013→5.502ms. This is not a controlled single-change FPS comparison or a promise of current 34 FPS. Later base rates remained near 28; detailed M364 instrumentation recorded 22.739/s and must not be compared directly with uninstrumented runs.

Sixfold generation configuration is supported by the tested helper/runtime; API counts do not establish physical displayed FPS or sixfold input response. Pacing/latency and remaining source dependencies are documented. No post-FG six-image buffering is included. M365 was withdrawn; M366/M367 experiments are excluded.

The exact helper is currently RTX 5080-only. Tested game SHA256:
`032EB661CE65E0669FB643A2E87E727D5574BBA832CD6FE34FF0AB1D134CD788`.

No anti-cheat bypass, automatic driver tuning, game launch, model download or game-setting replacement is provided.
