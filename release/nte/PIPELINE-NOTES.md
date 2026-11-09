# MGPU for NTE — Pipeline & Low Latency Preview 3

更新为已安装并逐阶段确认正常的 **主模块 8ADEAA05＋helper 446FD260**。

4080S 负责游戏渲染与超分，关闭源端 FG；5080 负责 NR 及其后的 **NVIDIA 官方 DLSS 多帧生成，最多请求六倍**。

## 更新内容

- 更新新版异环 ForceTagging／源 FG 保护绑定。
- 修复拍照／菜单后源标记短暂中断造成 helper 退出、FG 消失的路径。
- 过期输出隐藏、新结果恢复，处理场景切换的旧画面覆盖。
- 改进输入环槽回收，保护完整待处理同帧资源。
- NR 慢于渲染时使用三额度并行背压；容量满才等最老任务，保留渲染／COPY／NR 跨帧重叠。
- 在原生 Reflex Sleep 前等待容量，减少排队输入延迟，保留提交后的安全保护。
- 双 Pass 接入已有异步流水线，两遍 NR 和画质参数保留。
- 分三阶段关闭 GPU 计时／诊断像素、主 CPU／队列详细采集与纯统计、helper 追踪及纯速率统计。

用户确认登录界面与双 Pass 不闪、延迟低、双 Pass 异步后体验良好，并确认三个测量清理阶段运行正常。必要同步、输入启动、源身份、Reflex、官方 SDK、heartbeat、输出存活保护和部分状态统计保留。面板部分速率字段停止更新，RTSS 可继续监控 helper。

The release retains three-credit cross-frame concurrency, waits before native Reflex Sleep when capacity is full, enables asynchronous official dual-Pass NR, recovers across source-marker gaps, expires stale overlays, and disables optional instrumentation in three separately validated stages.

## 下载

- `MGPU-for-NTE-pipeline-preview.3.zip`：主模块、helper、配置模板、shader、安装／备份／回退及本地运行库导入工具。
- `MGPU-for-NTE-source-preview.3.zip`：本次 tag 对应的源码。
- `SHA256SUMS.txt`：下载校验。

helper 仍限定 RTX 5080；安装器支持本次 EXE 哈希。其他 50 系、赛博朋克实验与失败的整段移除测量候选未合入。NVIDIA 运行库／模型、ReShade、游戏文件不随包分发。旧 Release 保留，升级保留已有配置。

## 验证与性能

发布副本完整编译通过，相关回归 **14/14 主测试＋11/11 helper**、完整 helper **23/23**通过，安装／回退、ZIP及文件哈希校验通过。完整主测试为 **69/86**：17项旧测量／WARP测试失败，包含关闭测量后的预期冲突、排空／覆盖断言和软件深度测试失败；详细原始日志及未确定的归因保留在仓库构建说明。本版继续作为预览发布。

历史基础帧 28→34、双卡质量＋官方六倍 160 FPS 以上等记录保留原测试条件，本版没有新一轮同条件 FPS 对照。

[安装](https://github.com/wc2395082443-del/MGPU-for-NTE/blob/mgpu-for-nte/docs/nte/INSTALL.zh-CN.md) · [详细改动](https://github.com/wc2395082443-del/MGPU-for-NTE/blob/mgpu-for-nte/docs/nte/PIPELINE-UPDATE.zh-CN.md) · [English](https://github.com/wc2395082443-del/MGPU-for-NTE/blob/mgpu-for-nte/docs/nte/PIPELINE-UPDATE.en.md)

Main SHA256: `8ADEAA052261EA7C89DC6AB3137496B13A5A57D3D7B54F3CE98C776737987E48`

Helper SHA256: `446FD26095FCB21EEB446D06F67E18485BCEE3D4C432D6B7A3BBB841C9DE6933`
