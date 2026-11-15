# MGPU for NTE — 双NR节拍与低延迟 Preview 4

2026-11-16，发布已验收的 **B09C主模块＋E172官方helper**。用户反馈：保持流畅，延迟明显降低。

4080S负责游戏与超分、关闭源端FG；5080负责NR和NVIDIA官方DLSS多帧生成，最大请求六倍。

## 优化内容

- 输入获准即预留额度，将尚未提交的渲染帧计入在途。
- 三帧预算覆盖输入到NR完成；提交确认同一额度，完成释放额度，保留跨帧并行和资源fence。
- 超时后由真实NR完成恢复门控，避免永久失效。显式关闭不会被晚到完成重新开启。
- 保留原生Reflex Sleep之前的容量检查；失败的B7D3后置检查实验未保留。
- 增加真实NGX FrameGeneration GPU起止时间、生成序号、输入fence与六次Present的逐批关联。

## 本轮测量

| 指标 | AE5C（保护已关闭） | B09C |
| --- | ---: | ---: |
| 源帧/s | 25.23 | 25.61 |
| SDK呈现计数/s | 151.36 | 153.65 |
| 显示间隔P95 | 9.52ms | 9.68ms |
| 模拟→同批首次显示平均年龄 | 136.80ms | 111.15ms |

保持相近流畅度时，画面年龄降低约25.65ms；这是源模拟到关联显示批次的年龄，不是硬件输入延迟。转视角动作与场景负载非严格一致，测量开销未剔除。

相对失败的B7D3，显示P95从30.71降到9.68ms，20ms以上显示空档85→3。B09C正式窗口205批均六次Present、1025次FG评估关联有效。图中按生成1–5分行显示真实GPU查询，按Present1–6分行显示系统事件；两类编号不能未经验证逐张对应。

## 下载

- `MGPU-for-NTE-lifecycle-preview.4.zip`：精确验收的主DLL/helper、配置模板、shader、安装/备份/回退、运行库本地导入及说明。
- `MGPU-for-NTE-five-frame-evidence-preview.4.zip`：B7D3、AE5C、B09C三版完整离线图与五帧数据。
- GitHub自动生成本tag的源码ZIP/tar.gz；`SHA256SUMS.txt`校验发布附件。

本版仍是**保留测量功能的诊断构建**。详细FG采集需显式触发，正常计数/日志和部分GPU计时仍有开销。旧去测量preview.3保留。一次采样前1秒超时已恢复，载入/菜单全场景仍需改进。主流丢失1条、细分截断21个任务、内核ETL丢失0。

发布副本重点回归13/13主测试、12/12 helper，完整helper24/24通过；安装/备份/回退及包校验通过。完整主构建被旧“GPU计时必须关闭”测试的静态断言阻止，完整主套件未运行。原断言和失败日志保留，未把重点回归通过写成全套通过。

支持EXE SHA256：`64ED3CAE0B5FC61F3B56CDD933B4807CB23F0EC7913590DB60D1A8642DE2DFEC`。helper仍限定RTX5080。NVIDIA运行库/模型、ReShade及游戏文件不分发；已有配置由安装器保留，升级不自动改游戏参数。

Main SHA256: `B09CA7B12A3B23069FFB35DDB9F08D8684239DBAFDFEBB486E78EEAFA7922BF0`

Helper SHA256: `E172D87654241079991C3D8766BDCE66B7B763235720703EAA1F3B1DD3B0A767`

[详细更新与五帧解释](https://github.com/wc2395082443-del/MGPU-for-NTE/blob/mgpu-for-nte/docs/nte/LIFECYCLE-UPDATE.zh-CN.md) · [English](https://github.com/wc2395082443-del/MGPU-for-NTE/blob/mgpu-for-nte/docs/nte/LIFECYCLE-UPDATE.en.md)
