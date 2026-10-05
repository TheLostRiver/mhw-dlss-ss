# 研究工具使用说明

本仓库没有自动安装器。编译产物是用途不同的研究工具；不要一次性把所有 DLL 放进游戏目录。现有代码的固定哈希与 RVA 只对应[记录的构建](RESEARCH.md#兼容标识)。

## 原生采集环境

研究采用游戏 DX12、TAA 入口与能正常工作的 MHWSS 1.0.2。MHWSS 的文件按其发布包结构位于游戏根目录，研究时使用的 Steam 启动项为：

```text
cmd /c start MHWSSLauncher.exe & rem %command%
```

本项目不分发 MHWSS 或游戏插件加载器。部分历史数据通过临时启用 MHWSS DLAA 入口采集；最终 SR 方案不因此要求额外 DLAA。

| 工具 | 行为 | 控制或输出 |
|---|---|---|
| `MhwSrProbe.dll` | 串联 NGX 参数调用，观察三个上游资源阶段 | DLL 同目录 `MhwSrProbe-<PID>.jsonl` |
| `MhwViewportProbe_v2.dll` | 观察 raster state 与 draw/dispatch | `tools/viewport_control.ps1` |
| `MhwTextureProbe_v2.dll` | 增加纹理回读复制；使用同队列 fence 确认 GPU 完成 | `tools/texture_control.ps1` |
| `MhwPassProbe_v4.dll` | 追踪候选资源、描述符、管线与着色器 | `tools/pass_control.ps1` |
| `MhwScreenProbe_v4.dll` | 读取已定位的 CPU 可见 CBScreen 数据 | `tools/screen_control.ps1` |
| `MhwScalePilot_v4.dll` | 短暂改变内部比例并恢复 | `tools/scale_control.ps1` |
| `MhwQuadPilot.dll` | 比例脉冲，同时记录特定后处理取样矩形 | 同上，`-Observer MhwQuadPilot` |
| `MhwSrBridge.dll` | 当帧 TAA 输入、后 TAA 顶点采集、尺寸查询和可选短时抖动控制 | 同上，`-Observer MhwSrBridge`；未执行 SR |
| `MhwD3D12Methods.exe` | 在进程外取得本机 D3D12 方法信息 | 为捕获配置提供方法 RVA、签名与适配器信息 |
| `MhwProbeInjector.exe` | 显式指定目标游戏 PID 与 DLL 路径的加载工具 | 不会自动选择或启动游戏 |

部分探针依赖由配套脚本生成的当前 PID、方法签名和已存在钩子配置。保留前一轮配置并换一个进程号并不足以证明兼容。具体输入参数可查看脚本的 `--help`，PowerShell 工具则查看文件开头的 `param`。

## 比例观察器控制

以下是已核对环境中的操作格式，不能绕过源码中的兼容检查。需要已有可用的 DLL 插件加载方式；随游戏启动加载后，程序仍等待场景信号。

同目录 `MhwQuadPilot.ini` 或 `MhwSrBridge.ini`：

```ini
[Experiment]
Enabled=1
```

桥接前置采集可包含以下选项；`NgxProxySha256` 必须替换为已核对的实际代理 SHA-256，不能直接使用占位文字：

以下是普通观察模式。需要实际执行 SR 的实验版本另见 [质量档原型说明](QUALITY-PROTOTYPE.md)，其配置模板、部署和失败边界不同，不能只把代理切成 DLSS 就视为已接通。

```ini
[Experiment]
Enabled=1
RequireFrameGenOff=1
ApplyScalePulse=1
GameHooks=1
TracePostTaa=0
PulseJitter=0
TraceTextures=0

[Compatibility]
NgxProxySha256=<已核对的64位十六进制SHA256>
```

默认要求现有 `OptiScaler.ini` 的 `[FrameGen] Enabled=false`。如果采集期间图像质量、MHWSS 选项或这项帧生成配置发生变化，观察器请求恢复内部比例。

研究 TAA 输入时，必须确认**游戏内**原生抗锯齿已设为 TAA；磁盘配置不一定反映尚未保存的运行设置。MHWSS 的 Upscaler 仍可保持 None，不需要启用 DLAA。

只读输入窗口使用 `GameHooks=0`，此时不设置内部比例。高／低内部尺寸现已分别得到实际 TAA CBV 验证，但这不代表完整时域验证。

`PulseJitter=1` 要求 `GameHooks=1` 且 `ApplyScalePulse=1`，只在约 3 秒脉冲中启用已核对的 MHWSS 投影抖动开关，并自动恢复。它不是 DLAA/SR 开关。None 模式的基线记录可能合法地全为零；`camera_valid` 不代表抖动非零，使用汇总脚本核对实际数值。

`TracePostTaa=1` 增加后 TAA 绘制、顶点绑定、有限资源发现和引擎顶点资源关联观察。此路径额外绑定已核对的代理绘制入口和游戏代码版本。它读取少量上传缓冲区字节，不修改绘制或执行 SR；详细边界见 [桥接说明](BRIDGE.md)。

`TraceTextures=1` 还要求 `GameHooks=1` 和 `TracePostTaa=1`，增加实际描述符、RTV、复制、scissor、图形常量、着色器容器及 MHWSS 深度准备观察。当前合计 32 个已核对入口。每种输入尺寸只保留前四帧的详细操作顺序，其他记录去重；容器写入 DLL 同目录 `shaders/`，原始数据仅用于本地研究。此选项不执行 SR，也不改变资源状态或复制参数。

进入可移动场景后，在 PowerShell 使用实际 PID：

```powershell
$gameProcessId = 12345 # 替换为当前游戏 PID
./tools/scale_control.ps1 -GameProcessId $gameProcessId -Observer MhwQuadPilot -Action Start
./tools/scale_control.ps1 -GameProcessId $gameProcessId -Observer MhwQuadPilot -Action Cancel
```

`Cancel` 是取消操作示例，不需要紧跟 `Start` 执行。正常完成后检查比例恢复及 `owned_hooks_restored`。这些观察器是一次性工作线程，结束后不会自动重复；内存中的模块保留至进程退出。正常退出游戏后再更换加载中的 DLL。

## 独立回放

需要自己持有经 fence 确认完成的 Low 纹理采集，以及现代 `nvngx_dlss.dll`。仓库不包含这些文件。

```powershell
python tools/prepare_sr_replay.py evidence/my-low-capture C:/local-dlss-runtime evidence/my-replay
./build/bin/Release/MhwSrReplay.exe evidence/my-replay/replay.ini evidence/my-replay
python tools/decode_texture_capture.py evidence/my-replay
```

准备脚本目前只处理已研究的 75% Low 区域和对应格式/footprint。它裁取真实低分辨率区域，不把原生完整画面缩小。回放是单帧 Reset=1，不模拟真实连续帧运动。

## 分析脚本

- `inspect_pe.py`、`trace_pe.py`、`extract_shaders.py`：针对使用者自行提供的二进制做静态分析。
- `inspect_*runtime.py`、`inspect_command_list.py` 等：只读当前进程资料，输出留在本地。
- `summarize_*.py`、`compare_capture_stages.py`：汇总保存的参数和采集日志。
- `summarize_bridge.py`：汇总实际输入矩形、零/非零抖动、相邻历史连续性、顶点 UV 和恢复状态；不把参数/CPU 顶点观察解释为 SR 已可用。
- `summarize_texture_graph.py`：根据保存的着色器哈希与同帧绑定/复制记录汇总颜色链、占位深度和模糊参数；未知着色器不推测角色。
- `decode_texture_capture.py`、`analyze_low_roi.py`：分析自己的原始纹理；预览图是诊断可视化。
- `analyze_jitter_coverage.py`：可用 `--high`、`--low` 指定参数记录，离线比对抖动样本。
- `analyze_bridge_sites.py`：读取 `inspect_pe.py` 生成的证据 JSON；可用 `--evidence` 指定文件。
- `disassemble_pass_shaders.py`：DXIL 回退反汇编可用 `--dxc C:/path/to/dxc.exe`，或将 DXC 放入 PATH。

`evidence/` 默认被 Git 忽略。原始进程日志可能包含本机路径和地址；这些文件是本地分析输入，仓库只发布整理后的结论。
