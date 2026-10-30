# 安装源端FG关闭版 preview.2

下载`MGPU-for-NTE-source-fg-off-preview.2.zip`。本版使用主模块20CF7B73＋helper118BBCBA：**4080S关闭源端FG，5080在NR后运行官方FG，最多请求六倍**。其他50系副卡见独立[兼容候选说明](RTX50-COMPAT.zh-CN.md)，本版helper仍限定RTX 5080。旧首发包保留。

## 准备

1. 正常退出异环及helper。从原启动器运行游戏，不更改或禁用反作弊。
2. 需要双卡、兼容NVIDIA驱动、Windows x64、VC++ x64运行库。当前helper只接受**RTX 5080**；已测组合为4080 SUPER游戏渲染＋5080神经输出。显示器/Windows GPU选择应使两卡角色正确，启动后查看日志中的实际adapter，不能只按编号猜测。
3. 在异环真实`HTGame.exe`目录安装支持add-on API20的ReShade。已测历史EXE SHA256为`032EB661CE65E0669FB643A2E87E727D5574BBA832CD6FE34FF0AB1D134CD788`，更新版兼容性未验证。
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

历史测试使用4K输出、游戏DLSS质量，单次NR、各强度2、风格1、自动蒙版，帧生成由5080 helper提供。不要同时把游戏原生FG打开当作本包测试条件。

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

从原启动器进入场景。日志应出现`source-fg-off-20261031`版本、正确的游戏/神经adapter、颜色/深度/MV标签、NR输出和helper初始化。源`slDLSSGSetOptions`应`installed=1`，`SOURCE-FG-OFF`中On请求对应`action=2 result=0`。只有实际调用与源SDK Off持续成立，才能确认该入口已生效；原生CVar仍On不代表SDK仍On。`color_overlap=1`/`guide_overlap=1`表示分阶段COPY路径启用。

helper仍请求官方六倍动态MFG，目标200 FPS；这是SDK输出目标，实际比例随负载/可见状态变化。确认helper输出可见及SDK正常，后台每源帧报告1张不能当成六倍成功或失败的前台证据。

- Ctrl＋Alt＋F7切换input/output，对比NR；Home为ReShade面板。
- helper使用独立输出覆盖层，输入仍属于原游戏。按钮/滚轮/转视角和Alt＋Tab都应核对。
- RTSS只对`mgpu_official_host.exe`开启检测；对游戏及启动链保持排除。配置文件由RTSS管理，本包不会全局改动Afterburner/RTSS。
- 六倍只是官方SDK请求上限，检查真实源帧率与显示节奏，不以浮层数字独立判断画质/延迟。

出现启动异常、画面闪烁、缺失引导或无法正常输入时，退出后回退，保留日志；不要通过删fence或禁用保护继续运行。

## 回退

安装结束会显示receipt路径。游戏退出后执行：

```powershell
.\Install.ps1 -RollbackReceipt 'D:\YourGame\HT\Binaries\Win64\mgpu-nte-backups\TIMESTAMP\receipt.json'
```

脚本只在当前文件仍匹配该次安装后的哈希时恢复/移除，外部改过的文件拒绝覆盖。你手动更改的Engine/游戏设置需用自己的备份处理，本脚本不会把本机或旧配置强加给别人。
