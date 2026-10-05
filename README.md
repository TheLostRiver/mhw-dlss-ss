# MHW DLSS Super Resolution Research

为《怪物猎人：世界 / Monster Hunter: World》研究现代 **DLSS Super Resolution（超分辨率）** 接入。

**当前是研究代码和诊断工具，尚未实现可用的游戏内 DLSS SR。没有已验证的帧率提升，也没有可用的档位 UI。** 请不要把编译得到的探针当成即装即用的超分模组。

目标是在保持游戏输出分辨率不变的情况下，让引擎按所选 DLSS 档位真正减少内部渲染像素，再用颜色、深度、运动矢量和抖动信息重建完整输出。目标包含质量、平衡、性能、超高性能档；最终不叠加单独的 DLAA pass，也不包含帧生成、Neural Rendering 或 Ray Reconstruction。

游戏原有 DLSS 1.x 接口不能通过直接替换 DLL 变成现代 DLSS SR。本项目从已有 MHWSS 的渲染入口与实际游戏资源入手，研究引擎内部比例和后处理衔接。

## 当前进度

截至 2026-10-06：

| 项目 | 已取得的结果 | 边界 |
|---|---|---|
| 输入数据 | 已取得颜色、深度、运动矢量和非零抖动参数，并回读实际纹理 | SR 所需的逐帧时域一致性仍待验证 |
| 真实低分辨率输入 | Low 档有效区域为 1920×1080，纹理分配仍为 2560×1440 | 分配尺寸不等于实际渲染尺寸 |
| 独立 SR 回放 | 用 DLSS DLL **310.9.1.0** 将真实 1920×1080 输入重建到 2560×1440 | 仅一帧 `Reset=1`；不是游戏内接入或性能验证 |
| 引擎比例控制 | 工具调用游戏自身 setter，得到 1696×954 内部视口，输出保持 2560×1440，随后自动恢复 | 已验证短时控制，尚未与 SR 联动 |
| 后 TAA 放大 | 已测得低分辨率源矩形到完整输出视口的取样步骤 | SR 写回后仍须协调此处取样范围 |
| 桥接前置版本 | 已在实际 TAA 调用处取得高／低内部尺寸；最新窗口 530 份有效记录、0 份无效 | 尚未执行游戏内 SR |
| 独立投影抖动 | 保持 MHWSS=None，324 个低内部尺寸帧取得非零抖动，529 对相邻记录的历史抖动连续 | 只短暂开启投影抖动开关，不启用 DLAA；尚非时域画质验证 |
| 后处理 GPU 取样 | 直接读取真实顶点缓冲区，324 次全屏三角形 UV 均匹配低分辨率区域到完整输出 | 尚未改为完整 SR 输出的取样 |
| 实际颜色链 | 已逐帧对应 TAA 输出、局部复制、动态模糊重建、色调映射和最终放大，并取得对应着色器哈希 | 全尺寸 SR 输出需要同步处理复制区域、后处理视口/裁剪和 UV，不能只改最后一次取样 |
| 深度来源 | 原生 TAA 的 t2 是 1×1 占位；已找到 MHWSS 的独立 R32 深度准备路径，269 次 TAA 与本命令列表记录周期中的深度准备对应 | 尚未用连续帧 SR 验证其像素内容、同步及遮挡解除 |
| 档位和 UI | 已有尺寸查询、范围检查及质量模式参数适配代码 | 档位切换和 UI 尚未接通；超高性能还受引擎 0.5 比例下限限制 |

上述尺寸是研究过程中使用的实例；设计应从运行时输出目标取尺寸，不绑定 2K、4K 或桌面分辨率。详细数据、接口位置和限制见 [研究记录](docs/RESEARCH.md)、[桥接设计](docs/BRIDGE.md) 和 [真实纹理链](docs/TEXTURE-GRAPH.md)。

首版 SR 的产品默认策略为关闭动态模糊。游戏的 Off 选项仍会提交重建绘制，实测普通快门为 0、毛发快门参数仍为 0.4；完整关闭该路径属于接入工作，当前研究观察器不会暗中修改这些着色器参数。

## 源码目录

| 目录 | 用途 |
|---|---|
| `control/` | 内部比例控制、后处理矩形采集、SR 参数适配和桥接前置观察器 |
| `probe/` | MHWSS / NGX 参数探针与 CMake 构建定义 |
| `viewport/` | 视口观察器、显式指定进程的探针加载工具 |
| `readback/` | D3D12 方法定位、带 GPU fence 的纹理回读 |
| `pipeline/` | 后处理资源、描述符与管线追踪 |
| `screen/` | CBScreen 参数观察 |
| `replay/` | 独立 D3D12 / DLSS SR 单帧回放器 |
| `shaders/` | 根据已分析格式重建的运动矢量解码说明代码 |
| `tools/` | 采集控制、PE 分析、日志汇总及纹理解码脚本 |

## 构建

需要 Windows x64、Visual Studio 2022 C++ 工具、Windows SDK、CMake 3.22+ 和 Git。分析脚本使用 Python 3.10+。

```powershell
git clone https://github.com/TheLostRiver/mhw-dlss-ss.git
cd mhw-dlss-ss

# 从官方仓库取得固定版本依赖；不会改游戏目录。
powershell -ExecutionPolicy Bypass -File tools/fetch_dependencies.ps1

cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel 2
```

产物在 `build/bin/Release/`。只编译桥接前置版本可增加 `--target MhwSrBridge`。不需要独立回放器时，在配置命令中增加 `-DMHW_SR_BUILD_REPLAY=OFF`。

已有依赖也可以直接指定：

```powershell
cmake -S . -B build -A x64 `
  -DMHW_SR_MINHOOK_DIR="C:/deps/minhook" `
  -DMHW_SR_NGX_SDK_DIR="C:/deps/DLSS"
```

Python 分析环境：

```powershell
python -m venv .venv
.\.venv\Scripts\python -m pip install -r requirements.txt
```

采集工具的行为、启动条件及本地数据准备见 [工具使用说明](docs/TOOLS.md)。**构建不会安装、注入、启动游戏或运行采集。**

## 兼容范围

已研究的环境是 DX12、MHWSS 1.0.2 和 RTX 3070 Laptop。代码包含特定游戏、MHWSS、D3D12Core 的哈希、RVA 和指令签名检查，不是通用版本适配。

部分观察器还绑定研究时探针或 OptiScaler 代理的哈希。重新构建、更换代理或游戏更新后，需要重新核对这些约束；编译成功不代表可以直接在任意环境加载。运行中的 DLL 不应被覆盖，恢复与退出由各工具的控制流程处理。

## 后续工作

1. 将已对应的实际纹理链接入质量模式 SR，保留正确的独立深度准备路径。
2. 协调 SR 后的完整尺寸复制、关闭模糊的路径、色调映射及最终取样。
3. 完成特征生命周期、尺寸变化、历史重置和安全恢复。
4. 验证运动矢量、投影抖动、遮挡变化与连续帧画质，再测量 GPU 帧时间。
5. 接通质量／平衡／性能档位和 UI；另行解决超高性能所需的引擎比例下限。

## 依赖与仓库内容

依赖使用固定版本的 [MinHook](https://github.com/TsudaKageyu/minhook) 和 [NVIDIA DLSS SDK](https://github.com/NVIDIA/DLSS)，详见 [第三方依赖说明](THIRD_PARTY.md)。MHWSS 与游戏需要使用者自行提供。

仓库只收录研究源码、脚本及整理后的观察结果；不包含游戏/MHWSS/OptiScaler/NVIDIA 运行库、原始纹理、内存快照、反汇编产物、个人路径日志或预编译模组。

---

Experimental research for modern DLSS Super Resolution in Monster Hunter: World. **Not a working in-game SR mod yet.** The standalone replay, engine render-scale experiment, and post-TAA sampling observations are separate results; they do not establish in-game temporal quality or FPS gains.
