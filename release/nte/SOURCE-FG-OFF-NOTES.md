# MGPU for NTE — Source FG Off Preview 2

**4080S关闭源端帧生成，5080在NR后运行NVIDIA官方帧生成，最多请求六倍。**

**The RTX 4080 SUPER's game-process FG is switched off; the RTX 5080 performs official NVIDIA FG after neural rendering, with up to a sixfold request.**

本版对应本机安装的精确组合：主模块`20CF7B73`＋helper`118BBCBA`。主模块在游戏原线程将源`slDLSSGSetOptions`的有效mode转为Off，避免上游反复On请求恢复源FG；保留ForceTagging、同帧颜色/深度/MV/相机、原生Reflex调用、IO优化和全部资源同步。5080 helper保留输入隔离、分层鼠标穿透、官方六倍动态MFG及200 FPS输出目标。

Source options are rewritten only behind game-build, live-tagging, viewport and version guards. Caller data and SDK return codes are preserved. The helper is a separate process and retains its official FG path. No one-shot CVar forcing, fence removal, custom presentation timer or swapchain teardown is added.

## Downloads / 下载

- `MGPU-for-NTE-source-fg-off-preview.2.zip`: 完整安装/升级包，包含主模块、helper、示例配置、深度shader、备份/回退及本机运行库导入工具。
- `MGPU-for-NTE-source-preview.2.zip`: 与本次tag对应的源码。
- `SHA256SUMS.txt`: 文件校验值。

NVIDIA runtime/model binaries、ReShade和游戏文件不随包分发。仍需从自己的合法本机运行库导入精确匹配的依赖；安装保留现有配置，需核对ForceTagging、SecondaryFG和NR输出模式。helper目前限定RTX 5080；其他50系独立候选未合入此版本。旧M364＋M363首发仍保留。

## Validation / 验证

- Local build: **77/77 main + 20/20 helper** tests passed. Installer fixtures and payload hashes are verified separately.
- 已在游戏前台读到源SDK Off，实际On→Off包装SDK返回eOk，资源标签与NR继续；前台15.50秒源约29.61/s、SDK约176.00/s。
- 新8秒GPU窗口出现COPY/DIRECT查询交集约58.17%，但该窗口helper visible0、SDK每源帧1张；输入观察器迟到、main丢1条和7个任务细分截断均保留。**前台六倍完整GPU及性能对照仍待完成。**
- The source-Off foreground check and GPU window are separate observations. The latter does not establish foreground sixfold performance gains. Existing proxy-swapchain copies/synchronization may remain after source FG is disabled.
- Historical 28→34 base FPS and 160+ displayed-FPS user observations remain historical, rather than new measurements of this revision. No fixed FPS uplift is promised.
- [Preview.2 hosted CI](https://github.com/wc2395082443-del/MGPU-for-NTE/actions/runs/41129943140) compiled successfully and passed **76/77 main tests**. The existing fine-capacity WARP stress fixture hit its asynchronous GPU-drain timeout, stopping before helper tests. New source-FG option/guard/route cases passed. Local results remain 77 main + 20 helper; the preview keeps this known limitation without weakening tests.

[安装说明 / Install](https://github.com/wc2395082443-del/MGPU-for-NTE/blob/mgpu-for-nte/docs/nte/INSTALL.zh-CN.md) · [新版实现与证据](https://github.com/wc2395082443-del/MGPU-for-NTE/blob/mgpu-for-nte/docs/nte/SOURCE-FG-OFF.zh-CN.md)

Main SHA256: `20CF7B73A4014DB20A7139A0ECBA84D58606E42B8DE16F103F27DAA617DDEDD4`

Helper SHA256: `118BBCBAA15A76B9F1208489BF2EDE9B7EA3685D574FF557DCD4FC6EC6A33162`
