# MGPU for NTE：双NR节拍与低延迟版 preview.4 安装

本包使用已验收的B09C主模块＋E172 helper，修复三帧生命周期额度与超时后恢复；保留诊断采集入口及其开销。4080S负责游戏和超分、关闭源端FG，5080负责双NR及NVIDIA官方最大六倍FG。helper限定RTX5080，支持EXE哈希以包清单为准。

1. 正常退出游戏及helper。先准备API20 ReShade、NR运行库/模型及匹配的官方Streamline运行库。NVIDIA模型/运行库不含在此包。
2. 在解压目录打开PowerShell，路径换成实际HTGame.exe目录：

```powershell
.\Install.ps1 -GameDirectory 'D:\YourGame\HT\Binaries\Win64' -ValidateOnly
.\Install.ps1 -GameDirectory 'D:\YourGame\HT\Binaries\Win64'
```

3. 新安装从自己合法取得的匹配runtime目录运行`Import-Runtime.ps1`。升级且运行库已匹配时无需重复导入。
4. 按[完整安装说明](docs/nte/INSTALL.zh-CN.md)合并Engine.ini的ForceTagging/Reflex等键。安装器保留已有配置。核对`SecondaryFG=1`、`FGMultiplier=6`和`Present=nr`；本次双NR测量使用`Passes=2`、两遍强度2。新安装模板对应此条件，升级不自动覆盖原参数。
5. 从原启动器进入场景，核对源`slDLSSGSetOptions installed=1`、`SOURCE-FG-OFF action=2 result=0`，同时确认NR输出和helper可见六倍。Ctrl＋Alt＋F7切换input/output。

安装器保存备份并输出回滚receipt。正常退出后恢复：

```powershell
.\Install.ps1 -RollbackReceipt '游戏目录\mgpu-nte-backups\TIMESTAMP\receipt.json'
```

[依赖](docs/nte/DEPENDENCIES.md) · [本次改动与五帧测量](docs/nte/LIFECYCLE-UPDATE.zh-CN.md) · [二进制来源](docs/nte/PROVENANCE.md)
