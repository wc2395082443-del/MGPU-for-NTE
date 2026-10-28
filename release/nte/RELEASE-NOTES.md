# MGPU for NTE — M364 + M363 preview

First NTE adaptation release: same-frame stability fixes, staged multi-GPU COPY transport, RTX 5080 official Streamline frame generation, and isolated mouse-passthrough output helper.

The ZIP contains our hash-pinned deployed add-on/helper binaries, depth shader, example configuration, integrity manifest, reversible installer, and local NVIDIA runtime importer. It excludes NVIDIA model/runtime binaries, game files and ReShade. Follow the repository's install/dependency instructions; matching runtimes are required.

Historical unique NR completions increased 26.699→33.451/s (~25.3%) across M339c to M340+ForceTagging, with multiple conditions changing. Full transport elapsed decreased 7.073→6.013→5.502ms. This is not a controlled single-change FPS comparison or a promise of current 34 FPS. Later base rates remained near 28; detailed M364 instrumentation recorded 22.739/s and must not be compared directly with uninstrumented runs.

Sixfold generation configuration is supported by the tested helper/runtime; API counts do not establish physical displayed FPS or sixfold input response. Pacing/latency and remaining source dependencies are documented. No post-FG six-image buffering is included. M365 was withdrawn; M366/M367 experiments are excluded.

The exact helper is currently RTX 5080-only. Tested game SHA256:
`032EB661CE65E0669FB643A2E87E727D5574BBA832CD6FE34FF0AB1D134CD788`.

No anti-cheat bypass, automatic driver tuning, game launch, model download or game-setting replacement is provided.

## Validation limitation / 验证限制

Local publication build: 75/75 main tests and 20/20 helper tests passed; package install/rollback, integrity and viewer checks passed. GitHub-hosted source compilation passed, but both CI attempts passed only 74/75 main cases: `mgpu_nonblocking_fine_capacity_warp_test` timed out during asynchronous drain, before cloud helper tests were reached. The cause is not established. The test was not skipped or weakened, and the archived M364/M363 release binaries are unchanged. [CI evidence](https://github.com/wc2395082443-del/MGPU-for-NTE/actions/runs/40768218148).

本地源码75＋20项通过；云端两次均为主工程74/75，采集压力测试排空超时，helper云端测试未执行。未降低断言或反复重试刷绿。请把首发当作带已知诊断测试限制的预览版，不视为所有环境稳定性保证。
