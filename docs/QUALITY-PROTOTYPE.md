# 质量档渲染原型

2026-10-06 的游戏内短窗口首次完成实际 SR 执行：**269 次 NGX Evaluate 返回成功，模糊绕过、完整尺寸调色、最终复制也各完成 269 次**。代理日志确认 DLSS 310.9.1 后端及 Quality=2；内部输入 1696×954，游戏输出保持 2560×1440。结束后内部比例恢复 1，投影抖动恢复关闭，钩子恢复，最后提交的 GPU fence 完成后释放特征与资源。

**这些结果证明调用和输出路径已接通，不等于画面、时域或帧率验证完成。** 画面反馈仍待确认；尚无档位 UI，也不是常驻可用版本。当前仍是显式触发、约 3 秒比例窗口的研究工具。

## 当前执行路径

1. 在实际 TAA 回调处记录该次命令列表的新 CBScreen、相机当前/上一帧抖动、原始 HDR 颜色 A、打包 MV 和 TAA 输出 T。
2. 原生 TAA 正常执行，结果 T 复制到 B。它暂时充当立即回退路径，**B 不作为 SR 颜色输入**；A 此时仍保留原始颜色。
3. 在已匹配的模糊重建绘制入口，核对命令列表、绘制顺序、视口、顶点、目标 A，以及该次实际绑定的深度 t1、MV t2。用这些已经处于合法像素着色器读取状态的输入，绘制到自有 R32 深度和 R16G16 MV 纹理。
4. MV 解包/去抖动使用第 1 步记录的抖动。每次使用独立的 256 字节上传区，GPU 完成前不重复写同一地址；不要求两个阶段复用同一个常量缓冲地址。
5. 用原始 A、自有深度/MV 执行一次 Quality SR，成功后把完整输出写回 A，并跳过整次原生模糊绘制，包括残留毛发模糊。MHWSS 保持 **None**，不创建额外 DLAA、帧生成或 RR 特征。
6. 保留游戏调色着色器，修正它的 viewport、scissor、UV，以及最终全屏复制的取样；随后恢复绑定。

因此 `expanded_copies=0` 是当前路径的正常结果：原有 T→B 复制保持原样，SR 在更晚的入口写回 A。`native_taa_bypassed=false` 也属实；删除这份回退计算、减少诊断开销是后续性能工作，不能把本版当成最终性能实现。

## 为何选择这个入口

高档时 MHWSS 的深度准备路径能与 TAA 命令列表对应。低内部比例时，实测深度由另一条命令列表中的全屏绘制生成，原来的 CopyResource 准备回调不再出现。该绘制使用已识别的系统复制 PS、1696×954 视口及相应 UV。

当前路径改在游戏原本消费这些资源的位置取样，沿用原游戏的命令提交依赖，避免拿“最近一次深度准备”充当当帧数据。原始颜色仍由 TAA 处记录，连续操作必须符合已观察的颜色链。模糊与调色四边形的顶点顺序不同，分别按实际数据检查。

## 参数与边界

创建参数为 Quality=2、preset K=11、flags=0x4B；创建尺寸来自现有后端的 optimal 查询，执行时使用引擎对齐后的实际输入矩形。窗口中的 optimal 为 1706×960、实际为 1696×954；这不等于独立 NVIDIA SDK 查询的所有档位结果。

jitter 和 MV.Scale 换算到实际输入像素单位，规则参考 [NVIDIA SDK 辅助接口](https://github.com/NVIDIA/DLSS/blob/main/include/nvsdk_ngx_helpers_d3d.h)。运动方向、遮挡解除、细节稳定性和重置行为仍需画面验证。设计读取游戏输出尺寸，不读取桌面分辨率决定 SR 档位。

当前仅固定已研究的游戏、MHWSS、D3D12Core 和代理构建。资源/着色器/尺寸不匹配时停止尝试；NGX 失败不会复制旧输出，原生绘制继续。如果成功写回之后才发现后续图形路径变化，当前帧仍可能取样错误，原型尚不保证任意场景变化下无缝回退。

所有特征及自有 GPU 资源必须等最后使用的命令列表实际提交、后续 Signal 的 fence 完成后释放。提交、队列或设备状态不确定时保留到退出，不凭 CPU 超时释放。原则见 [Microsoft 同步说明](https://learn.microsoft.com/en-us/windows/win32/direct3d12/user-mode-heap-synchronization)。

## 部署与触发

构建 `MhwSrBridge`。私人暂存目录放入 `MhwSrBridge.dll`、从 [配置模板](../config/MhwSrBridge-quality.ini) 复制的 `MhwSrBridge.ini`，以及包含 `bridge_sha256`、`ini_sha256` 的 `manifest.json`。

正常退出游戏后运行 [安装脚本](../tools/install_quality_prototype.ps1)。它核对构建哈希并备份三份原文件，选择 `Dx12Upscaler=dlss`，关闭帧生成、比例覆盖和额外输出处理；资源 barrier 覆盖设为 `auto`，按 [OptiScaler 配置说明](https://github.com/optiscaler/OptiScaler/blob/master/Config.md) 关闭额外状态校正。它不启动游戏。

```powershell
powershell -ExecutionPolicy Bypass -File tools/install_quality_prototype.ps1 `
  -GameDirectory "<游戏根目录>" -StageDirectory "<已核对的暂存目录>"
```

按原 Steam/MHWSS 方式启动，保持游戏 TAA、图像质量高、动态模糊关闭、旧版游戏 DLSS 关闭和 MHWSS=None。进入场景后用 `scale_control.ps1 -GameProcessId <PID> -Observer MhwSrBridge -Action Start` 触发，取消使用 `-Action Cancel`。不用手动切低档。

一次性线程结束后不重新启动；热加载修正版使用单独白名单别名和目录，不覆盖已加载 DLL。需要回退时先正常退出，再从该次 `backup-*` 恢复对应文件。仓库不发布原始图像、着色器、日志或预编译模组。
