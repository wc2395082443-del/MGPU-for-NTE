# 安装双NR节拍与低延迟版 preview.4

下载`MGPU-for-NTE-lifecycle-preview.4.zip`。使用已验收主模块B09C＋helper E172：**4080S关闭源端FG，5080在NR后运行官方FG，最多请求六倍**。这是保留测量入口的诊断版；原去测量preview.3保留为独立旧Release。其他50系副卡见独立[兼容候选说明](RTX50-COMPAT.zh-CN.md)，本版helper仍限定RTX 5080。

## 准备

1. 正常退出异环及helper。从原启动器运行游戏，不更改或禁用反作弊。
2. 需要双卡、兼容NVIDIA驱动、Windows x64、VC++ x64运行库。当前helper只接受**RTX 5080**；已测组合为4080 SUPER游戏渲染＋5080神经输出。显示器/Windows GPU选择应使两卡角色正确，启动后查看日志中的实际adapter，不能只按编号猜测。
3. 在异环真实`HTGame.exe`目录安装支持add-on API20的ReShade。本次支持的EXE SHA256为`64ED3CAE0B5FC61F3B56CDD933B4807CB23F0EC7913590DB60D1A8642DE2DFEC`，其他构建需重新适配，安装器拒绝未知EXE。
4. 按上游文档准备自己的NR运行库/模型，`nvngx_dlssnr.dll`放在exe旁的`mgpu/`子目录，**不要放exe同级**。

## 安装本包

解压Release ZIP，在PowerShell执行（路径换成自己的）：

```powershell
.\Install.ps1 -GameDirectory 'D:\YourGame\HT\Binaries\Win64' -ValidateOnly
.\Install.ps1 -GameDirectory 'D:\YourGame\HT\Binaries\Win64'
```

安装脚本先校验包哈希并检查游戏/helper均退出，备份将替换的文件。仅安装主add-on、helper、模板shader等本项目文件，既有`mgpu.ini`、`gpu1.ini`、`ReShade2.ini`默认保留；新安装使用包内示例。配置不同可能导致功能差异，请按下面核对，不能以文件没覆盖等同于适配完成。

本包**不含 NVIDIA helper 运行库**。从你自己已有、合法取得的匹配运行库目录导入：

```powershell
.\Import-Runtime.ps1 -RuntimeSource 'D:\ExistingMGPU\mgpu-official\runtime' `
  -GameDirectory 'D:\YourGame\HT\Binaries\Win64'
```

它验证文件名、SHA256及NVIDIA签名，全部匹配后才复制，既有文件会备份；不能用源码头文件或任意新版DLL代替。没有匹配依赖时，当前首发不能保证直接运行。参见[依赖说明](DEPENDENCIES.md)。

## 游戏设置与 Engine.ini

本次节拍测试采用双NR、两遍强度2、风格2、自动蒙版、最大六倍；包内新安装模板与此对应，游戏超分和分辨率仍由自己的游戏设置决定。旧单NR历史记录保留原条件。升级默认保留已有配置，若要核对同一双NR条件，在面板确认Passes=2及两遍强度；不要同时开启游戏源端FG。模板NRPreset=4沿用验收时的创建hint，实际权重切换未确认；0对应运行库默认，更改后需重新初始化NR。

备份`%LOCALAPPDATA%\HT\Saved\Config\Windows\Engine.ini`，在已有`[SystemSettings]`里加入/核对：

```ini
r.Streamline.ForceTagging=1
t.Streamline.Reflex.Enable=1
t.Streamline.Reflex.Mode=1
t.Streamline.Reflex.Auto=1
r.OneFrameThreadLag=1
```

ForceTagging保留原生资源标签入口，Reflex用于当前集成，`OneFrameThreadLag`采用测试配置值。安装脚本保留游戏设置和此文件的现有属性；请手动合并这些键，并在启动日志中核对生效情况。保留文件内其他游戏数据。

本版不再使用插件主动设置原生Reflex CVar，游戏仍可依据自己的设置启用Reflex。源端FG包装不改变Reflex选项。helper六倍在`mgpu.ini`中对应`SecondaryFG=1`、`FGMultiplier=6`；输出选`Present=nr`，也可进入场景后用Ctrl＋Alt＋F7切换。已有配置由安装器保留，需要自行核对。

游戏原生FG保持关闭；无需开启4080S原生插帧来给5080提供引导。源端保护校验要求ForceTagging生效，且只对本次支持的游戏构建启用。具体见[源端FG关闭说明](SOURCE-FG-OFF.zh-CN.md)。

## 启动核对

从原启动器进入场景。发布身份以本包SHA256为准；内部版本字符串沿用`nte-NR-preset-20261111`，不能单凭字符串区分后续候选。日志应出现正确的游戏/神经adapter、颜色/深度/MV标签、NR输出和helper初始化。源`slDLSSGSetOptions`应`installed=1`，`SOURCE-FG-OFF`中On请求对应`action=2 result=0`。只有实际调用与源SDK Off持续成立，才能确认该入口已生效；原生CVar仍On不代表SDK仍On。`color_overlap=1`/`guide_overlap=1`表示分阶段COPY路径启用。

helper仍请求官方六倍动态MFG，目标200 FPS；这是SDK输出目标，实际比例随负载/可见状态变化。确认helper输出可见及SDK正常，后台每源帧报告1张不能当成六倍成功或失败的前台证据。

- Ctrl＋Alt＋F7切换input/output，对比NR；Home为ReShade面板。
- helper使用独立输出覆盖层，输入仍属于原游戏。按钮/滚轮/转视角和Alt＋Tab都应核对。
- RTSS只对`mgpu_official_host.exe`开启检测；对游戏及启动链保持排除。配置文件由RTSS管理，本包不会全局改动Afterburner/RTSS。
- 六倍只是官方SDK请求上限，检查真实源帧率与显示节奏，不以浮层数字独立判断画质/延迟。

出现启动异常、画面闪烁、缺失引导或无法正常输入时，退出后回退，保留日志；不要通过删fence或禁用保护继续运行。

preview.4保留GPU计时、主模块CPU/队列采集入口及helper追踪和FG计时能力，纯NR诊断像素关闭。详细FG采集默认不启动，显式触发一次最多10秒/4096次评估；其他时间查询、状态和日志仍有开销。测量版已获当前场景验收；不能称其为去测量版本。详见[本次更新](LIFECYCLE-UPDATE.zh-CN.md)。

运行日志应包含`three lifecycle credits before native Reflex Sleep`，以及`submitted`、`reserved`、`recovering`。正常场景应看到`ready=1`且`completed`继续推进。存在一次采样前超时后恢复的已知边界，载入/菜单还需验证；长期ready=0或完成不推进应保留日志排查。

## 回退

安装结束会显示receipt路径。游戏退出后执行：

```powershell
.\Install.ps1 -RollbackReceipt 'D:\YourGame\HT\Binaries\Win64\mgpu-nte-backups\TIMESTAMP\receipt.json'
```

脚本只在当前文件仍匹配该次安装后的哈希时恢复/移除，外部改过的文件拒绝覆盖。你手动更改的Engine/游戏设置需用自己的备份处理，本脚本不会把本机或旧配置强加给别人。
