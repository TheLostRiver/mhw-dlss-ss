# 质量档渲染原型

最新 10 秒质量档窗口完成 **1032 次成功 SR 调用及对应完整输出，其中 1031 帧跳过原生 TAA dispatch**。一次成功 SR 用于准备后启用旁路；窗口结束后比例、投影抖动和钩子恢复，GPU 资源经 fence 完成后释放。用户反馈画面与帧数仍没有明显区别，不能把本轮当作画质或性能收益结论。

2026-10-06 的游戏内短窗口首次完成实际 SR 执行：**269 次 NGX Evaluate 返回成功，模糊绕过、完整尺寸调色、最终复制也各完成 269 次**。代理日志确认 DLSS 310.9.1 后端及 Quality=2；内部输入 1696×954，游戏输出保持 2560×1440。结束后内部比例恢复 1，投影抖动恢复关闭，钩子恢复，最后提交的 GPU fence 完成后释放特征与资源。

随后两轮 10 秒窗口分别完成 **1010 / 1016 次成功 SR 调用**，各轮的模糊绕过、完整调色和最终复制次数均与成功调用一致，均自动恢复并释放资源。第二轮由用户在游戏前台按 F8 触发；第一轮用户未注意到画面变化，不能计为画面确认。F8 这一轮用户反馈画面正常、与原生没有明显区别，帧数也没有明显变化；这是单场景短时观感，尚未获得性能收益或完整时域画质的证据。

减少详细记录并启用 GPU 时间戳、尚未跳过 TAA 的上一轮完成 **953 次成功 SR 调用**，对应完整输出次数一致；约 10 秒后比例、抖动和钩子恢复，查询及 SR 资源均在 fence 完成后释放。用户反馈画面和帧数依然没有明显区别，953 次调用也不能直接与前轮次数比较得出优化收益。

**这些结果证明调用和输出路径已接通，不等于画面、时域或帧率验证完成。** 尚无档位 UI，也不是常驻可用版本。首轮窗口只有约 3 秒，结束后已恢复原生比例，不能把结束后的画面当成质量档。现在提供显式触发、默认约 10 秒的对比窗口。

## 当前执行路径

1. 在实际 TAA 回调处记录该次命令列表的新 CBScreen、相机当前/上一帧抖动、原始 HDR 颜色 A、打包 MV 和 TAA 输出 T。
2. 原生 TAA 在准备阶段正常执行。`BypassNativeTaa=1` 且低分辨率 SR 链路已成功后，把当前原始颜色 A 的有效区域复制到 T，并通过原有回调返回值跳过 TAA dispatch；游戏仍执行 T→B。B 是当帧原始颜色回退，**不作为 SR 颜色输入**；A 本身不变。
3. 在已匹配的模糊重建绘制入口，核对命令列表、绘制顺序、视口、顶点、目标 A，以及该次实际绑定的深度 t1、MV t2。用这些已经处于合法像素着色器读取状态的输入，绘制到自有 R32 深度和 R16G16 MV 纹理。
4. MV 解包/去抖动使用第 1 步记录的抖动。每次使用独立的 256 字节上传区，GPU 完成前不重复写同一地址；不要求两个阶段复用同一个常量缓冲地址。
5. 用原始 A、自有深度/MV 执行一次 Quality SR，成功后把完整输出写回 A，并跳过整次原生模糊绘制，包括残留毛发模糊。MHWSS 保持 **None**，不创建额外 DLAA、帧生成或 RR 特征。
6. 保留游戏调色着色器，修正它的 viewport、scissor、UV，以及最终全屏复制的取样；随后恢复绑定。

因此 `expanded_copies=0` 是当前路径的正常结果：原有 T→B 复制保持原样，SR 在更晚的入口写回 A。第一个成功 SR 帧仍用于确认路径，其 `native_taa_bypassed=false` 不能代表整轮；随后是否跳过 TAA 看 `quality_native_taa_bypass_started` 及汇总中的 `native_taa_bypasses`。窗口结束后恢复原生 TAA。

游戏菜单里的 TAA 目前仍需开启，以保留获取当帧数据的渲染入口；启用上述旁路后，该入口仍工作，低内部比例下的 TAA 计算被跳过。SR 的颜色一直来自 TAA 之前，去掉重复计算不保证画面会更锐利。原始颜色回退不含抗锯齿；若旁路后 SR 或后处理检查失败，使用当前原始颜色继续原生路径并请求恢复比例。切换时的历史稳定性仍需实景确认。

## 为何选择这个入口

高档时 MHWSS 的深度准备路径能与 TAA 命令列表对应。低内部比例时，实测深度由另一条命令列表中的全屏绘制生成，原来的 CopyResource 准备回调不再出现。该绘制使用已识别的系统复制 PS、1696×954 视口及相应 UV。

当前路径改在游戏原本消费这些资源的位置取样，沿用原游戏的命令提交依赖，避免拿“最近一次深度准备”充当当帧数据。原始颜色仍由 TAA 处记录，连续操作必须符合已观察的颜色链。模糊与调色四边形的顶点顺序不同，分别按实际数据检查。

## 参数与边界

创建参数为 Quality=2、preset K=11、flags=0x4B；创建尺寸来自现有后端的 optimal 查询，执行时使用引擎对齐后的实际输入矩形。窗口中的 optimal 为 1706×960、实际为 1696×954；这不等于独立 NVIDIA SDK 查询的所有档位结果。

`WindowMs` 允许 1000～10000 毫秒，质量档默认 10000；时间从请求内部比例变化开始计算。工作线程超时同时保留额外 17 秒用于前置准备和恢复，不会沿用三秒版的固定总时限。每轮最多使用 4096 个独立常量上传区；若帧数先达到上限，提前恢复并记录 `immutable_frame_budget_exhausted`，不循环覆盖 GPU 仍可能读取的地址。

`WindowSounds=1` 时，工作线程检测到首个完整 SR 输出绘制已记录后播放高音，确认引擎比例恢复后播放低音；提示音不在渲染线程运行。日志每秒记录 `quality_window_status`、剩余时间及完整输出次数。它们表示命令路径状态，不是画面正确性、呈现时刻或帧率测量。

jitter 和 MV.Scale 换算到实际输入像素单位，规则参考 [NVIDIA SDK 辅助接口](https://github.com/NVIDIA/DLSS/blob/main/include/nvsdk_ngx_helpers_d3d.h)。运动方向、遮挡解除、细节稳定性和重置行为仍需画面验证。设计读取游戏输出尺寸，不读取桌面分辨率决定 SR 档位。

当前仅固定已研究的游戏、MHWSS、D3D12Core 和代理构建。资源/着色器/尺寸不匹配时停止尝试；NGX 失败不会复制旧输出，原生绘制继续。如果成功写回之后才发现后续图形路径变化，当前帧仍可能取样错误，原型尚不保证任意场景变化下无缝回退。

所有特征及自有 GPU 资源必须等最后使用的命令列表实际提交、后续 Signal 的 fence 完成后释放。提交、队列或设备状态不确定时保留到退出，不凭 CPU 超时释放。原则见 [Microsoft 同步说明](https://learn.microsoft.com/en-us/windows/win32/direct3d12/user-mode-heap-synchronization)。

## 性能记录

`VerboseDiagnostics=0` 跳过重复的纹理绑定快照、顶点/矩阵明细和四边形日志，只保留稀疏输入样本及必要的资源、描述符、状态和绘制顺序检查。它没有关闭 SR 的输入检查；TAA 旁路由独立的 `BypassNativeTaa` 控制。

`GpuTimings=1` 为同一条已确认的 DIRECT 命令列表添加独立时间戳对：`native_taa_to_copy` 从 TAA 回调末端计到其结果复制入口（包含中间同步），`sr_inputs_evaluate_copy` 包含深度/MV 准备、NGX 和完整 HDR 输出复制。按实际输入尺寸及阶段分组输出均值、中位数和 P95；这些局部耗时不等于整帧 GPU 忙碌时间，也不能单独证明 CPU 瓶颈或 FPS 提升。

TAA 旁路增加 `raw_taa_fallback_copy` 和 `taa_bypass_to_copy` 两段计时，分别量出原始颜色回退复制及跳过 dispatch 后到 T→B 入口的剩余间隔。

时间戳使用队列的频率换算毫秒，最多 8192 对，整个窗口不复用；只解析已完成的查询对，并在最后使用的 fence 完成后读取。实现依据 [Microsoft 的 D3D12 计时说明](https://learn.microsoft.com/en-us/windows/win32/direct3d12/timing) 和 [ResolveQueryData 约束](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-resolvequerydata)。

首轮时间戳结果（同一张 RTX 3070 Laptop、同一游戏输出 2560×1440）：

| 范围 | 实际输入 | 样本 | 中位耗时 | P95 |
|---|---|---:|---:|---:|
| 原生比例的 TAA 至结果复制入口 | 2560×1440 | 138 | 0.336 ms | 0.338 ms |
| 质量档比例的 TAA 至结果复制入口 | 1696×954 | 954 | 0.162 ms | 0.163 ms |
| SR 输入准备、NGX、完整输出复制 | 1696×954 | 953 | 3.186 ms | 4.431 ms |

随后启用 TAA 旁路的一轮中，1031 次原始颜色回退复制的中位耗时为 **0.045 ms**；跳过 dispatch 后到结果复制入口的剩余间隔，中位数和 P95 均在本次时间戳精度下读为 0。SR 总段的 1032 个样本中位耗时为 **3.026 ms**。这些记录配合旁路返回值计数，确认重复 TAA 计算已被跳过；各轮场景、时钟和负载未作严格对齐，不能拿调用次数或 SR 局部耗时之差当成整帧收益。

TAA 旁路只减少了较小一段开销，画面也不一定改变，因为此前 SR 就使用原始颜色。SR 总段还未拆分为输入准备、模型执行和复制，当前也未测得整帧 GPU 忙碌时间，瓶颈判断需继续细分。

## 部署与触发

构建 `MhwSrBridge`。私人暂存目录放入 `MhwSrBridge.dll`、从 [配置模板](../config/MhwSrBridge-quality.ini) 复制的 `MhwSrBridge.ini`，以及包含 `bridge_sha256`、`ini_sha256` 的 `manifest.json`。

正常退出游戏后运行 [安装脚本](../tools/install_quality_prototype.ps1)。它核对构建哈希并备份三份原文件，选择 `Dx12Upscaler=dlss`，关闭帧生成、比例覆盖和额外输出处理；资源 barrier 覆盖设为 `auto`，按 [OptiScaler 配置说明](https://github.com/optiscaler/OptiScaler/blob/master/Config.md) 关闭额外状态校正。它不启动游戏。

```powershell
powershell -ExecutionPolicy Bypass -File tools/install_quality_prototype.ps1 `
  -GameDirectory "<游戏根目录>" -StageDirectory "<已核对的暂存目录>"
```

按原 Steam/MHWSS 方式启动，保持游戏 TAA、图像质量高、动态模糊关闭、旧版游戏 DLSS 关闭和 MHWSS=None。进入场景后触发：

```powershell
powershell -ExecutionPolicy Bypass -File tools/scale_control.ps1 `
  -GameProcessId <PID> -Observer MhwSrBridge -Action Start -WaitForF8
```

`-WaitForF8` 等待游戏连续处于前台 2 秒、且用户重新按下 F8 后才发出开始信号；不会强制切换窗口或发送按键。默认等待最多 120 秒（`-ForegroundTimeoutSeconds` 可调至 300），超时不发信号。如果只需要切回游戏便触发，可以改用 `-WaitForForeground`。信号后仍有约 7 秒配置稳定检查及原生比例准备期；听到高音后观察约 10 秒，低音表示比例已恢复。配置或资源变化可能导致提前停止。取消使用 `-Action Cancel`，立即生效于控制线程，并由渲染线程执行比例恢复。不用手动切低档。

一次性线程结束后不重新启动；热加载修正版使用单独白名单别名和目录，不覆盖已加载 DLL。需要回退时先正常退出，再从该次 `backup-*` 恢复对应文件。仓库不发布原始图像、着色器、日志或预编译模组。
