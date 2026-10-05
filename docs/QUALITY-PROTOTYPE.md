# 质量档渲染原型

截至 2026-10-06，这份代码已编译，**尚未在游戏中执行或验证成功**。它不是常驻 SR 模组；仍为显式触发、约 3 秒内部比例窗口，然后恢复的实验版本。尚无可用的档位 UI，也没有帧率提升数据。

## 接入方式

`QualityPrototype=1` 时，在同一次原生 TAA 回调中读取新绑定的 CBScreen、当前/上一帧投影抖动、实际 t0 颜色、t4 打包运动矢量及 u0 输出。深度来自同一命令列表、同一 Reset 周期里 MHWSS 深度准备调用的真实 R32 **源纹理**，不使用 TAA 的 1×1 t2，也不读可能被其他列表覆盖的共享准备结果。源纹理状态还必须由本次命令列表中的真实 barrier 确认。

原型复用 MHWSS 已初始化的 NGX 分发链和参数探针，独立创建一个 Quality SR 特征。它不改 NGX 函数槽、不初始化第二个上下文、不调用全局 Shutdown，也不修改 MHWSS 的模式或 TAA 旁路开关。MHWSS 保持 **None**；只沿用已验证的独立投影抖动脉冲。

在高档基线阶段查询本后端的最佳输入和动态范围、准备资源并创建特征；此时不执行原生分辨率的 DLAA。随后按查询结果请求引擎内部比例，逐帧要求实测有效矩形与引擎对齐后的尺寸一致。示例输出 2560×1440 的候选有效输入是 1696×954；运行逻辑不依赖桌面分辨率。

自有计算着色器解包当前帧 MV，并保留已研究的 MHWSS 分配像素单位。NGX 的 jitter、MV.Scale 换算到实际输入像素，创建参数为 Quality=2、preset K=11、flags=0x4B。运动方向、单位、遮挡解除和时域画质仍需连续帧验证。

模式尺寸和 jitter 单位依据 [NVIDIA SDK 参数辅助接口](https://github.com/NVIDIA/DLSS/blob/main/include/nvsdk_ngx_helpers_d3d.h)；这份接口说明不构成游戏输入正确性的证明。

## 完整输出路径

只有短窗口中已经观察到正确的低分辨率后处理链、输入关联完整且 SR 执行成功时，才写回完整 TAA 输出并跳过该次原生 TAA dispatch：

1. 扩大已匹配 TAA 输出 → 场景颜色副本的复制 box。
2. 把已匹配的动态模糊重建绘制替换为完整颜色复制，连同残留毛发模糊一起跳过。
3. 保留游戏色调映射/调色着色器，使用完整 viewport、scissor 和独立不可变的全 UV 顶点。
4. 修正最终全屏复制三角形的取样；每次绘制后恢复原顶点绑定、视口和 scissor。

匹配包含着色器哈希、输入和目标纹理、绘制顺序、原始顶点、拓扑及矩形。代码不修改原游戏共享上传缓冲区。

## 失败与生命周期

- NGX 创建/执行失败不会把旧或未初始化的 SR 输出复制给游戏；原生 TAA 继续执行，并请求恢复比例。
- 输入、输出、模式或队列变化会停止实验。若在 SR 已写回后才发现后续图形路径与上一帧不同，代码会停止下一帧 SR；**当前帧仍可能出现取样错误**，目前不具备任意后处理变化的无缝回退保证。
- 渲染队列必须与基线实际提交的 Direct 队列一致。所有自有 GPU 资源及特征，必须等最后一条已记录命令列表实际提交、随后 Signal 的 fence 完成后才能释放。未观察到提交、队列变化、设备移除或 fence 不确定时，整份会话保留到进程退出，不凭超时释放。
- 解码与 NGX 调用期间隔离内部 API 记录，再恢复游戏的堆、根签名、描述符表、CBV、PSO、视口、scissor、顶点绑定和拓扑。这里按已研究游戏管线的绑定形式实现，不是任意 D3D12 应用的通用状态保存器。

GPU 生命周期原则参考 [Microsoft 多引擎同步说明](https://learn.microsoft.com/en-us/windows/win32/direct3d12/user-mode-heap-synchronization)。单次 CPU 回调完成不代表 GPU 完成。

## 准备与运行

构建 `MhwSrBridge`。在私人暂存目录放入 `MhwSrBridge.dll`、从 [配置模板](../config/MhwSrBridge-quality.ini) 复制的 `MhwSrBridge.ini`，以及含两者 SHA-256 的 `manifest.json`（字段 `bridge_sha256`、`ini_sha256`）。

正常退出游戏后，用 [安装脚本](../tools/install_quality_prototype.ps1) 部署。脚本核对固定游戏/MHWSS/代理/现代 SR DLL 哈希，备份原桥接 DLL、桥接 INI 和 OptiScaler INI，选择 `Dx12Upscaler=dlss`、关闭帧生成及比例/额外输出处理覆盖。资源 barrier 覆盖恢复为 `auto`，按 [OptiScaler 配置说明](https://github.com/optiscaler/OptiScaler/blob/master/Config.md) 关闭额外状态校正；本原型自己把输入转换到 NON_PIXEL_SHADER_RESOURCE、输出保持 UAV，不能沿用另一条路径的 192 初始状态覆盖。脚本不更换代理或 SR DLL，不启动游戏。

```powershell
powershell -ExecutionPolicy Bypass -File tools/install_quality_prototype.ps1 `
  -GameDirectory "<游戏根目录>" -StageDirectory "<已核对的暂存目录>"
```

按原 Steam/MHWSS 方式启动，保持游戏 TAA、图像质量高、动态模糊关闭、旧版 NVIDIA DLSS 关闭和 MHWSS=None。进入可移动场景后，通过已有 `scale_control.ps1 -Observer MhwSrBridge -Action Start` 显式触发；不用切低档。只创建特征不会自动降低游戏内部比例，必须等基线条件满足。取消使用同工具的 `-Action Cancel`。

完整成功需要同时检查 `quality_summary` 中的成功执行、扩大复制、模糊绕过、全尺寸调色及最终复制计数，以及实际画面和 GPU 同步。`quality_wait_count` 表示未满足的前提；仅 DLL 加载或 NGX 返回成功不足以证明画面正确。诊断钩子和日志开销尚未移除，**本版不能用于测量最终 SR 性能**。

需回退时先正常退出游戏，再从该次暂存目录的 `backup-*` 恢复三份对应文件。不要在 DLL 仍加载时覆盖，也不要把配置恢复误当成当前渲染已恢复。
