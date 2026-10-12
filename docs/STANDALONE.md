# 独立运行入口

目标是由本项目独立完成启动、D3D12 数据接入、抖动、DLSS SR、完整输出和档位控制。已有的质量档 GPU 执行与引擎内部比例代码会继续使用。

当前运行组件：

- `MhwSrLauncher.exe`：启动游戏并尽早加载本项目的原生输入模块。失败时保留游戏进程，不强制退出。
- `MhwNativeHost.dll`：通过 D3D12 资源创建、计算管线创建、PSO/CBV 绑定、Reset 和 Dispatch 接口，独立发现常量缓冲和 TAA，并读取相机与屏幕参数。v2 增加可选的短时原生投影抖动，v3 修复启动阶段的投影入口等待。
- `MhwSrPanel.exe`：独立控制面板，默认 End 显示／隐藏，在界面中改键并保存。DLSS 页提供总开关、四档、锐化及 TAA／动态模糊的可保存占位预设，当前尚未连接渲染；实际 SR 状态单独显示，详见 [UI 说明](UI.md)。

**v1 已完成独立输入采集；v2 首次启动在投影指令检查处拒绝接入；v3 已编译，尚待游戏内采集。当前独立版本都不执行 SR，不改变输出或内部渲染分辨率。** 只有显式触发并通过基线匹配后才短暂修改投影；不能把编译成功当成已完成独立超分。

2026-10-06 的 v1 场景记录取得 1084 份有效 TAA 输入，1084 次相机／屏幕 CBV 均已刷新，输入和输出均为 2560×1440。进程没有加载 MHWSS，D3D12 来自系统目录；原生上传缓冲和 TAA PSO 均由本模块独立识别。当前和历史投影抖动全为零，结束后自身钩子已恢复。这证明了输入发现路径可用，尚不能证明非零抖动、独立 SR 或帧率收益。

## v3 启动等待

v2 的首次启动日志记录 `Native projection site differs`，随后停用自己的 Enabled 开关。文件哈希仍匹配，但该 RVA 在磁盘文件中的字节与此前可移动场景中记录的指令不同。该证据支持“过早检查运行时指令”的判断，并不意味着可以跳过校验或直接修改不匹配的代码。

v3 的顺序是：

1. 在启动早期只预留邻近钩子内存，不读取或修改尚未匹配的投影入口。
2. 接好原生 D3D12 观察器，以继续发现启动时创建的上传资源与计算管线。
3. 核对完整游戏文件哈希，等投影地址变为可执行、可读取且连续三次完全匹配已知签名，再创建并启用钩子。最长等待 60 秒；未匹配时记录拒绝并恢复本模块的观察器。
4. 控制事件创建完成后才报告 `Ready`，继续等待 F8 或显式 Start，初始化本身不写投影。

内存预留调用固定版本 [MinHook 的分配接口](https://github.com/TsudaKageyu/minhook/blob/8af6b4acae5a9388fd742b56fa79ece89d96f823/src/buffer.h)，由本模块唯一的管理线程与钩子操作串行执行。占用的槽仅用于维持邻近内存块，永不执行；未修改第三方库源码。实际钩子仍通过 MinHook 正常建立和恢复。

每个进程写入独立的 `MhwNativeHost-<PID>.state.ini`，包含进程创建时间、初始化阶段与失败原因。面板核对进程创建时间，避免旧文件因 PID 重用显示为当前状态；未启用时也明确记录。初始化失败不再覆写 Enabled，显式 Cancel 或完成一次性采集才自动写入 `Enabled=0`；手动设为 0 仍会停止。

## 投影采集

默认配置见 [MhwNativeHost.ini](../standalone/MhwNativeHost.ini)。在可移动场景按一次 F8 后：

1. 先采集约 1.5 秒未经修改的输入，将 TAA 的完整投影矩阵与原生相机视图匹配。
2. 必须匹配到唯一的全尺寸透视视图，并确认 getter 返回的是调用者栈上的矩阵、原始抖动为零、输入连续有效。任一条件不符时只记录，不写投影。
3. 高音后进入约 10 秒抖动窗口。仅修改该 getter 返回的临时矩阵，由游戏继续生成后续渲染参数。不会在几何渲染之后修改上传常量来伪造抖动。
4. 低音后停止写入，继续采集约 1 秒恢复记录，核对当前和历史投影是否回到零。

当前脉冲使用居中的八相位 Halton(2,3)，按实际输入宽高换算到游戏投影矩阵，随识别出的 TAA 调用推进相位。选用 Halton 的通用依据见 [NVIDIA DLSS 编程指南](https://github.com/NVIDIA/DLSS/blob/main/doc/DLSS_Programming_Guide_Release.pdf)；游戏中的投影方向、传递和相位对应仍须以本轮采集核实。八相位仅用于本轮原生尺寸验证，不是已确定的全部 SR 档位方案。

日志包含逐帧当前／历史抖动、八相位覆盖、脉冲期间的历史匹配数、恢复记录、写入次数与拒绝原因。尺寸、主视图或数据有效性改变时停止写入；渲染回调也独立检查最长持续时间。停止写入后，由下一次原生投影更新恢复输入；`recovery_confirmed` 需要连续有效零抖动记录，不能只凭计时结束认定恢复。

## 当前边界

入口按已记录的游戏版本、原生 D3D12Core 方法校准和 TAA shader SHA-256 匹配。缓冲区必须是实际 API 返回的 48 MiB 上传资源；相机与屏幕 CBV 必须位于已持有和映射的资源内，并在本轮 TAA 前刷新。不会搜索内存中的疑似 COM 对象。

模块要求进程未加载 `MHWSS.dll`。测试独立入口时需暂时停用现有 D3D12 代理及历史采集插件，避免原生方法已被改写。安装脚本只移动清单中哈希匹配的具体文件，保留恢复记录。

`KeepArmed=1` 会在一次采集后保留启动时识别的对象和钩子，等待下一次 F8，空闲时不写投影。也可通过现有控制脚本发送 `-Observer MhwNativeHost -Action Start` 或 `Cancel`。取消或将 INI 中 `Enabled` 改为 0 后恢复自身钩子并释放引用；完全停止后再次接入仍需重启。`ProjectionPulse=0` 仅作 10 秒只读采集。两个模式开关在启动时读取。

不要同时使用内置 F8 和外部脚本的 `-WaitForF8`。同一时刻只运行一个短采集，窗口内的重复触发会被丢弃。本版保留原生 TAA，尚不旁路或重建画面，也不是常驻 SR 版本。

## 构建与准备

```powershell
cmake --build build --config Release --target MhwNativeHost MhwSrLauncher MhwD3D12Methods --parallel 2
cmake --build build --config Release --target MhwSrPanel --parallel 2
build/bin/Release/MhwD3D12Methods.exe 0 "<暂存目录>/MhwNativeMethods.ini"
```

方法校准器创建自己的 D3D12 对象，只写入方法地址、代码标识和适配器信息，不打开游戏进程。暂存包还需包含启动器、输入模块、启用的 INI、MinHook 许可和逐文件 SHA-256 清单。v3 安装清单类型为 `standalone-projection-capture-v3`，另含读取初始化状态的面板程序和保留用户偏好的面板配置，共七个文件；安装器仍兼容 v1/v2 的恢复记录。

投影接入口由固定游戏哈希、RVA 和原指令签名限制。自有 MASM 入口保留易失寄存器与标志，再跳回原指令。邻近跳转空间在启动早期预留；完整游戏哈希和运行时指令检查通过前不启用投影钩子。它不使用外部宿主的回调或抖动开关。

保存后的日志可用以下命令汇总，脚本不打开游戏进程：

```powershell
python tools/summarize_native_host.py "<采集日志.jsonl>" --output "<汇总.json>"
```

汇总区分实际计数与稀疏日志样本，并记录启动拒绝、脉冲、相邻历史抖动与恢复结果。

## 安装与恢复

保存进度并正常退出游戏后，由 [安装脚本](../tools/install_standalone_capture.ps1) 安装已核对的包；脚本拒绝在游戏运行时更改文件，也不启动游戏。

安装器另支持 `standalone-control-panel-v1` UI 更新包，仅替换面板程序、面板配置和启动器，保留当前原生输入版本及其配置。更换面板时也需先从界面或托盘菜单退出面板。

```powershell
powershell -ExecutionPolicy Bypass -File tools/install_standalone_capture.ps1 `
  -GameDirectory "<游戏目录>" -StageDirectory "<暂存目录>"
```

本轮 Steam 启动项为：

```text
cmd /c start "" MhwSrLauncher.exe & rem %command%
```

保持 DX12、游戏 TAA、图像质量高、动态模糊关闭、旧版 DLSS 关闭及原输出分辨率。本轮要求实际输入等于输出，先确认原生投影传递，再与低内部尺寸及 SR 联动；当前 F8 不会开启超分。

需要恢复旧环境时，正常退出游戏后执行：

```powershell
powershell -ExecutionPolicy Bypass -File tools/install_standalone_capture.ps1 `
  -RestoreBackup "<安装时生成的 backup-* 目录>"
```

恢复过程保留当前独立版本文件，不递归删除目录；Steam 启动项需要按原值恢复。原始图像、日志、二进制分析和预编译运行库不进入源码仓库。
