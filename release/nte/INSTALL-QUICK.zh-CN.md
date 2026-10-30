# MGPU for NTE：源端FG关闭版安装

本包让4080S负责游戏渲染并关闭源端FG，5080负责NR及其后的NVIDIA官方FG，最多请求六倍。当前helper限定RTX 5080。

1. 正常退出游戏及helper。先准备API20 ReShade、NR运行库/模型及匹配的官方Streamline运行库。NVIDIA模型/运行库不含在此包。
2. 在解压目录打开PowerShell，路径换成实际HTGame.exe目录：

```powershell
.\Install.ps1 -GameDirectory 'D:\YourGame\HT\Binaries\Win64' -ValidateOnly
.\Install.ps1 -GameDirectory 'D:\YourGame\HT\Binaries\Win64'
```

3. 新安装从自己合法取得的匹配runtime目录运行`Import-Runtime.ps1`。升级且运行库已匹配时无需重复导入。
4. 按[完整安装说明](docs/nte/INSTALL.zh-CN.md)合并Engine.ini的ForceTagging/Reflex等键。安装器保留已有配置，请核对`SecondaryFG=1`、`FGMultiplier=6`和NR输出模式。
5. 从原启动器进入场景，核对源`slDLSSGSetOptions installed=1`、`SOURCE-FG-OFF action=2 result=0`，同时确认NR输出和helper可见六倍。Ctrl＋Alt＋F7切换input/output。

安装器保存备份并输出回滚receipt。正常退出后恢复：

```powershell
.\Install.ps1 -RollbackReceipt '游戏目录\mgpu-nte-backups\TIMESTAMP\receipt.json'
```

[依赖](docs/nte/DEPENDENCIES.md) · [新版改动和验证范围](docs/nte/SOURCE-FG-OFF.zh-CN.md) · [二进制来源](docs/nte/PROVENANCE.md)
