# 独立运行入口

目标是由本项目独立完成启动、D3D12 数据接入、抖动、DLSS SR、完整输出和档位控制。已有的质量档 GPU 执行与引擎内部比例代码会继续使用。

当前新增两个程序：

- `MhwSrLauncher.exe`：启动游戏并尽早加载本项目的原生输入模块。失败时保留游戏进程，不强制退出。
- `MhwNativeHost.dll`：通过 D3D12 资源创建、计算管线创建、PSO/CBV 绑定、Reset 和 Dispatch 接口，独立发现常量缓冲和 TAA，并读取相机与屏幕参数。v2 增加可选的短时原生投影抖动。

**v1 已完成独立输入采集；v2 已编译，尚待游戏内采集。两者都不执行 SR，不改变输出或内部渲染分辨率。** v2 仅在显式触发并通过基线匹配后，短暂修改投影；不能把编译成功当成已完成独立超分。

2026-10-06 的 v1 场景记录取得 1084 份有效 TAA 输入，1084 次相机／屏幕 CBV 均已刷新，输入和输出均为 2560×1440。进程没有加载 MHWSS，D3D12 来自系统目录；原生上传缓冲和 TAA PSO 均由本模块独立识别。当前和历史投影抖动全为零，结束后自身钩子已恢复。这证明了输入发现路径可用，尚不能证明非零抖动、独立 SR 或帧率收益。

## v2 投影采集

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

v2 的 `KeepArmed=1` 会在一次采集后保留启动时识别的对象和钩子，等待下一次 F8，空闲时不写投影。也可通过现有控制脚本发送 `-Observer MhwNativeHost -Action Start` 或 `Cancel`。取消或将 INI 中 `Enabled` 改为 0 后恢复自身钩子并释放引用；完全停止后再次接入仍需重启。`ProjectionPulse=0` 仅作 10 秒只读采集。两个模式开关在启动时读取。

不要同时使用内置 F8 和外部脚本的 `-WaitForF8`。同一时刻只运行一个短采集，窗口内的重复触发会被丢弃。本版保留原生 TAA，尚不旁路或重建画面，也不是常驻 SR 版本。

## 构建与准备

```powershell
cmake --build build --config Release --target MhwNativeHost MhwSrLauncher MhwD3D12Methods --parallel 2
build/bin/Release/MhwD3D12Methods.exe 0 "<暂存目录>/MhwNativeMethods.ini"
```

方法校准器创建自己的 D3D12 对象，只写入方法地址、代码标识和适配器信息，不打开游戏进程。暂存包还需包含启动器、输入模块、启用的 INI、MinHook 许可和逐文件 SHA-256 清单。v2 安装清单类型为 `standalone-projection-capture-v2`；安装器仍兼容 v1 的恢复记录。

投影接入口由固定游戏哈希、RVA 和原指令签名限制。自有 MASM 入口保留易失寄存器与标志，再跳回原指令。邻近跳转空间在启动早期预留；完整游戏哈希通过前不启用投影钩子。它不使用外部宿主的回调或抖动开关。

## 安装与恢复

保存进度并正常退出游戏后，由 [安装脚本](../tools/install_standalone_capture.ps1) 安装已核对的包；脚本拒绝在游戏运行时更改文件，也不启动游戏。

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
