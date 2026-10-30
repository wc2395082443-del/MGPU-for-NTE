# preview.2：4080S关闭源端FG，5080执行NR后官方FG

2026-10-31。发布组合：主模块`source-fg-off-20261031`（SHA前缀20CF7B73）＋官方输出helper（118BBCBA）。完整安装包支持新安装及旧首发升级，已有配置默认保留；旧preview.1仍可下载回退。

## 两张卡各做什么

| 显卡 | 工作 |
| --- | --- |
| RTX 4080 SUPER | 游戏渲染、游戏DLSS超分/重构、同帧输入捕获及出站COPY；关闭游戏进程里的源端DLSS-G帧生成请求 |
| RTX 5080 | 接收颜色/深度/MV和相机，执行NR；独立helper再用NVIDIA官方Streamline/DLSS-G生成和呈现，最多请求六倍 |

**关闭的是4080S源端多余FG，保留的是5080在NR画面之后的官方FG。** 六倍采用NVIDIA官方实现，非Lossless Scaling。游戏菜单原生FG保持关闭；此前观察到的内部请求与用户菜单不同，不能解释为用户开启了原生插帧。

## 为什么要改

旧运行连续五帧中，4080S出站COPY约4.8–5.0ms，主DIRECT查询空档约7.1–7.3ms。下一批DIRECT已提前25–27ms提交，却有Wait等待同卡COMPUTE信号。五个COMPUTE任务的NGX句柄均对应同进程feature11/frame generation；线程为源游戏内的`nv.sl.dlss_g.thread.dlssg`。20个唯一内核等待包与解除事件匹配，解除后约24–35µs主DIRECT继续。

官方D3D12默认会阻塞客户端呈现队列直到DLSS-G完成，`eBlockNoClientQueues`目前只支持Vulkan，D3D客户端设置后也会回退默认模式。因此本版处理源端FG请求，保留资源保护依赖。

## 如何关闭与保留保护

主模块接游戏已缓存的源`slDLSSGSetOptions`唯一入口及后续动态解析入口，在游戏原调用线程同步转发一次，只将有效参数的mode改成Off。重复On/Auto/Dynamic请求继续改Off，保留返回码、版本、其余字段及调用者原始内存。

保护条件包括支持的异环构建/ForceTagging接口签名、官方helper路线、ForceTagging游戏和渲染副本同时为1、viewport1、已知结构GUID/v1–v5且无未知扩展链。未知输入原样转发，日志区分原样/本来Off/已改写。这里不使用M366的一次性CVar写0，不提高CVar优先级，不循环抢写，不调用会联动Reflex的整套游戏模式设置接口。

主IO分阶段COPY、6槽预算、同帧引导/快照、全部资源退休fence、诊断钩子和M357校准修复保留。helper沿用M363输入队列隔离和分层鼠标穿透，动态六倍目标200 FPS；200是官方输出目标，生成比例随负载和可见条件变化。插件不再主动接管游戏原生Reflex CVar，原始Sleep和真实标记照常中继。

## 已验证与待验证

| 项目 | 结果 |
| --- | --- |
| 本地源码构建 | 主工程77/77、helper20/20通过；含源参数边界、重复On、原线程与一次调用、保护失效及真实包装入口测试 |
| 游戏实际入口 | 缓存入口installed1，On请求action2、SDK返回eOk；前台只读源SDK为Off |
| 引导与输出 | 标签/相机/深度/MV和NR持续，采集前15.50秒16/16前台，源29.61/s、SDK176.00/s，多数一秒窗比例6 |
| 新GPU短窗口 | 已封存公共8秒，COPY/DIRECT查询交集约58.17%；旧约7ms源FG句柄区间未记录到 |
| 新GPU窗口的条件限制 | helper245帧全部visible0、SDK每源帧1张；输入观察器迟到未与GPU对齐，主流丢1、7个细分任务截断 |
| 前台六倍完整GPU与性能对照 | **仍待完成**；上述58.17%窗口条件与旧前台六倍不同 |

新窗口出站COPY均值7.759ms、入站4.569ms、NR14.654ms。COPY查询区间可能含调度与带宽争用；这些并行区间不相加成一帧成本。历史约28→34基础帧、160+显示帧体验保留为旧条件记录，本次不新增固定提帧承诺。输入观察器失败和缺测标记保存在本地事故记录，公开摘要保留关键条件。

源SDK Off后已创建的代理swapchain仍可能有离屏复制和同步成本。官方指南18.0建议随FG开关重建交换链；本版尚未适配游戏交换链重建，仍需测残留成本。

## 使用与核对

正常退出游戏及helper后安装完整包。已有`mgpu.ini`、`gpu1.ini`、`ReShade2.ini`默认保留；自行核对`SecondaryFG=1`、`FGMultiplier=6`，输出为NR。按[安装说明](INSTALL.zh-CN.md)保留ForceTagging和当前已测试的Engine键。

启动日志应为`source-fg-off-20261031`，源`slDLSSGSetOptions installed=1`，有效On请求`action=2 result=0`。包装只响应游戏自己的SetOptions调用；没有后续调用、保护失效或入口绕行时，单看安装日志不代表源FG已停。实际关闭后继续检查NR引导和helper可见六倍状态。

当前副卡限定RTX 5080，已测渲染卡为4080 SUPER；其他50系识别候选独立保存，未纳入此包。游戏EXE仍要求`032EB661CE65E0669FB643A2E87E727D5574BBA832CD6FE34FF0AB1D134CD788`。

完整二进制身份及源码来源见[PROVENANCE.md](PROVENANCE.md)，回退使用安装器生成的receipt。
