# RTX 50 系列副卡兼容候选

发布说明：该候选已独立归档，**未合入当前preview.2发布源码和二进制**。当前公开源端FG关闭版保留精确RTX 5080检查；本页描述的其他型号识别仍属单独候选。

本候选基于M364主模块和M363 helper，扩展helper的型号识别。该候选在本地独立归档；当前preview.2和原始preview.1均保留RTX 5080检查。不能使用当前发布源码直接重建此型号扩展，其他型号仍待独立实机验证。

## 接受的型号与能力检查

- 接受 NVIDIA GeForce RTX 5050、5060、5060 Ti、5070、5070 Ti、5080、5090、5090 D、5090 D V2 的名称，以及相应 Laptop GPU 名称。
- 为未来的同系列驱动名称保留 SUPER、Ti SUPER 后缀识别。这只决定是否进入官方能力检测。
- 排除软件适配器、其他厂商、RTX 40/60 系列和 RTX PRO。专业卡需要单独验证。
- helper 严格使用主模块导出的副卡 LUID，在这张卡上打开共享纹理并建立 D3D12/Streamline 设备。
- 官方 `slIsFeatureSupported` 仍需确认 DLSS-G、Reflex、PCL 可用；最大生成倍率和动态 MFG 检查继续生效。六倍请求需最大倍率至少六倍且支持动态 MFG，不满足时报告错误。

NR 初始化、同帧颜色/深度/MV/相机验证、三槽共享资源、fence、输入穿透及呈现逻辑沿用原实现。游戏渲染卡的选择规则保持原样。

## 验证范围

2026-10-29：候选 helper 和探针编译通过，84 项型号/模式/倍率策略检查及 20/20 helper CTest 通过，包括 WARP 进程间资源传输和输入窗口回归。尚未进行本候选的游戏实测。主模块生产源码没有修改。

RTX 4080 SUPER＋RTX 5080 是既有游戏验证组合。其他型号需要提供实机结果，特别是较小显存型号的 4K NR＋FG 分配、持续吞吐和显示节奏；同样的功能支持不意味着相同帧率。笔记本还需确认两张独立显卡及其显示/共享资源拓扑。

启动日志新增 `[RTX50-COMPAT]` 行，包含型号、VendorId、DeviceId、显存和 LUID。后续日志记录 SDK 最大倍率、动态 MFG 和实际输出状态。

## 构建和独立探测

按 [BUILD.md](BUILD.md) 构建，候选 helper 为 `_build/nte/helper/mgpu_official_host.exe`。无需更改主模块的生产源码。

独立探针支持以下通用入口，旧 `--initialize-5080` 和 `--hidden-swapchain-5080` 入口保留兼容：

```powershell
.\mgpu_streamline_probe.exe --initialize-rtx50 'D:\Runtime' 'D:\Logs'
.\mgpu_streamline_probe.exe --hidden-swapchain-rtx50 'D:\Runtime' 'D:\Logs'
```

探针使用官方 NVIDIA 签名运行库，路径需绝对且存在。运行硬件探测前正常退出游戏。独立探针遇到多张 50 系列卡时拒绝猜测；游戏 helper 使用生产者传入的 LUID，无需扫描猜测。初始化和隐藏交换链探测不执行游戏 NR 或生成帧测试。

其他型号的验收需覆盖：NR 连续输出、同帧引导、请求倍率、实际显示帧、转镜头与按钮/滚轮、切换 input/output、长时间运行及显存占用，并保留型号/驱动/分辨率/配置与日志。
