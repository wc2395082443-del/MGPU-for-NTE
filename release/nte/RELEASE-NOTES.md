# Current release: Source FG Off Preview 2

2026-10-31：**4080S关闭源端FG，5080在NR后执行NVIDIA官方FG，最多请求六倍。** 当前主模块20CF7B73＋helper118BBCBA。完整当前说明见[SOURCE-FG-OFF-NOTES.md](SOURCE-FG-OFF-NOTES.md)；以下为旧首发历史，旧tag和文件保持原样。

# Historical: MGPU for NTE — M364 + M363 preview

First NTE adaptation release: same-frame stability fixes, staged multi-GPU COPY transport, RTX 5080 official Streamline frame generation, and isolated mouse-passthrough output helper.

**Sixfold generation uses NVIDIA's official DLSS Frame Generation through Streamline/DLSS-G, rather than Lossless Scaling. The user's latest visual-quality feedback is excellent.**

**六倍采用NVIDIA官方DLSS帧生成，并非小黄鸭插帧；用户最新体验反馈画面效果非常好。**

The ZIP contains our hash-pinned deployed add-on/helper binaries, depth shader, example configuration, integrity manifest, reversible installer, and local NVIDIA runtime importer. It excludes NVIDIA model/runtime binaries, game files and ReShade. Follow the repository's install/dependency instructions; matching runtimes are required.

Historical NR throughput increased **26.699→33.451/s (~25.3%)** across M339c to M340 + ForceTagging, alongside changes to COPY, tagging and resource submission. Transport elapsed decreased **7.073→6.013→5.502ms**. Later base rates were around 28 FPS; M364 recorded 22.739 source frames/s with detailed collection enabled.

The helper uses official Streamline generation and presentation, with isolated mouse-passthrough output. Sixfold selects dynamic MFG; post-generation six-image buffering is planned. M365 was withdrawn; M366/M367 remain separate experiments.

The exact helper is currently RTX 5080-only. Tested game SHA256:
`032EB661CE65E0669FB643A2E87E727D5574BBA832CD6FE34FF0AB1D134CD788`.

No anti-cheat bypass, automatic driver tuning, game launch, model download or game-setting replacement is provided.

## Validation limitation / 验证限制

Local build: **75/75 main and 20/20 helper tests passed**, along with install/rollback, integrity and viewer checks. Both GitHub-hosted attempts passed **74/75** main cases: `mgpu_nonblocking_fine_capacity_warp_test` timed out during asynchronous drain, stopping the job before helper tests. The cause remains under investigation. [CI log](https://github.com/wc2395082443-del/MGPU-for-NTE/actions/runs/40768218148).

本地75项主工程和20项helper测试通过；云端两次均为74/75，采集压力测试排空超时，helper云端测试未执行。

## 用户体验对照 / Gameplay comparison

用户于2026-10-29提供的异环DLSS5＋全景光追体验记录：

### 1. 单卡与分工参考

| 硬件 | 工作负载 | 用户记录 |
| --- | --- | --- |
| 4080S | 游戏渲染，关闭DLSS5 | 约40FPS |
| 4080S | 游戏＋DLSS5，质量档、全景光追、最高画质 | 约22FPS |
| 5080 | 仅执行NR阶段 | 约50个NR帧/s |

### 2. 双卡DLSS5基础帧优化

| 分工 | 优化前 | 优化后 |
| --- | ---: | ---: |
| **4080S主渲染＋5080 NR** | **约28FPS** | **约34FPS** |

优化阶段基础帧提高约21.4%。

### 3. NVIDIA官方六倍帧生成

| 方案 | 游戏主卡 | NR/FG副卡 | DLSS | FG | 用户报告显示FPS |
| --- | --- | --- | --- | --- | --- |
| 单卡5080 | RTX 5080 | 无 | 超级性能 | 六倍 | 约120 |
| **双卡** | **RTX 4080 SUPER** | **RTX 5080** | **质量** | 六倍 | **稳定160以上** |

游戏渲染可使用非50系主卡，当前已测试4080S＋5080；首发helper支持5080，其他组合需要适配。

## helper与六倍性能 / Helper

helper负责同帧输入校验、共享/私有资源退休、Reflex/PCL中继、官方FG呈现、独立输出窗口、鼠标穿透与诊断。[完整功能说明](https://github.com/wc2395082443-del/MGPU-for-NTE/blob/mgpu-for-nte/docs/nte/HELPER.zh-CN.md)。

Historical samples recorded 33.02 NR frames/s with FG off (M343), and 33.05 real frames/s with sixfold requested (M351), SDK count 194.85/s. Base throughput stayed approximately unchanged in those windows while the secondary GPU handled generation.

## 五帧流水线对比 / Five-frame comparison

M339c的五周期190.184ms，M340＋ForceTagging为151.709ms。图表使用统一刻度，另有COPY交接放大与M364重采集窗口。

下载 `MGPU-for-NTE-five-frame-comparison.zip`，解压后打开 `five-frame-comparison.html`。[图表与数据说明](https://github.com/wc2395082443-del/MGPU-for-NTE/blob/mgpu-for-nte/docs/nte/COMPARISON.zh-CN.md)。
