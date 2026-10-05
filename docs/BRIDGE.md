# 桥接前置版本

核心源码：[`MhwSrBridge.cpp`](../control/MhwSrBridge.cpp)、[`BridgePreflight.inl`](../control/BridgePreflight.inl)、[`BridgeScreenInputs.inl`](../control/BridgeScreenInputs.inl)、[`BridgePostTaaTrace.inl`](../control/BridgePostTaaTrace.inl)、[`SrParameterAdapter.h`](../control/SrParameterAdapter.h)。

**默认启动入口仍做前置观察。** `QualityPrototype=1` 启用 [质量档渲染原型](QUALITY-PROTOTYPE.md)，已完成首轮 269 次游戏内 SR 调用及输出链，画面与性能尚待验证。下文记录观察器证据；原型独立管理 SR 特征，复用原分发链，不改写 MHWSS 的 Create/Evaluate 函数槽。

## 原有渲染入口

针对已记录的 MHWSS 1.0.2 构建，分析发现其上采样路径可将原生 TAA PSO 替换为旁路 PSO，在 TAA dispatch 后整理输入并调用上采样入口。当前原型在 TAA 处记录原始颜色和抖动，在随后的原生模糊入口获取实际深度/MV 并执行 SR，保持 MHWSS=None。原生 TAA 暂作回退，其结果不作为 SR 颜色输入。

| MHWSS 接口 | RVA |
|---|---|
| 管线回调 | `0xf0b40` |
| Dispatch 回调 | `0xf1360` |
| SubmitPostTAACommands | `0xefb90` |
| 上采样入口 | `0x117a20` |
| NGX CreateFeature 槽 | `0x559af8` |
| NGX EvaluateFeature 槽 | `0x559b00` |
| NGX ReleaseFeature 槽 | `0x559b08` |
| NGX AllocateParameters 槽 | `0x559b18` |
| NGX GetCapabilityParameters 槽 | `0x559b20` |
| NGX DestroyParameters 槽 | `0x559b28` |

接口由固定构建的加载器和访问指令定位，部分指令已做运行时只读核对。已有参数探针与代理链必须保留，不能盲目覆盖未知目标。

## 参数适配

`QueryPlan` 从已经初始化的 NGX 环境取得 capability parameters，查询档位的 optimal/min/max，并检查引擎的比例下限和宽度对齐。它不重新初始化 NGX，也不创建特征。

质量模式候选创建参数使用查询得到的 optimal 输入尺寸、原输出尺寸、Quality=2、preset K 和已有 flags=0x4B。临时参数在调用结束后恢复，特征仍由原调用者负责生命周期。

候选 evaluate 使用实际输入子矩形，换算抖动和 MV 的像素单位，并保留原 reset。它要求已知纹理格式、分配尺寸、零起点和明确的 `frameAssociationVerified`。当前尚无已验证的运行端数据生产者为这个条件赋值。

## 当前观察器（版本 3 及其诊断扩展）

1. 启动早期预留比例更新、完整场景绘制、quad 和场景视口设置四个游戏函数钩子，等待显式 Start 信号。
2. 要求 DX12、图像质量 High、MHWSS Upscaler=None 稳定后开始；默认还要求 OptiScaler 配置中的帧生成关闭。
3. 记录原生 TAA PSO、dispatch、命令列表 Reset/ClearState 代次、最近视口和场景调用序号。独立追踪实际 compute CBV 绑定，在 TAA dispatch 前读取 CBScreen、相机投影抖动与少量附加常量。
4. 在场景视口设置调用 `0x23c391a → 0x23d0330` 直接读取游戏本次计算出的输入矩形，并读取改动前的输出矩形；与后 TAA quad 对照。推算矩形只作为辅助信息，不替代这条实际参数记录。在渲染回调内查询 NGX 推荐尺寸。
5. 复用已验证的 3 秒内部比例脉冲，随后恢复比例和自身钩子，并关闭自己的 INI。默认十个钩子；启用 `TracePostTaa` 后共十六个，同时启用 `TraceTextures` 后共三十二个。诊断开销不能用作 SR 性能测量。

游戏场景函数使用线程局部作用域记录本次调用的上下文，支持嵌套调用后的恢复。实测 TAA 的命令记录没有落在这份作用域内，不能据此把 CPU 侧的最新比例传给 TAA。版本 3 因此增加独立的命令列表 CBV 追踪；相机与屏幕绑定必须在上次 TAA 后刷新，Reset/ClearState 会清除记录。

低比例脉冲窗口已取得 594 份有效 CBScreen，0 份无效：350 份实际输入为 1696×954，其余 244 份为 2560×1440，输出均保持 2560×1440。所有尺寸均在识别出的 TAA dispatch 处读取，不使用稍后读取的全局比例来冒充当帧参数。

该窗口的 `camera_valid=true` 只说明字段可读且为范围内有限值；进一步分析发现当前及上一帧抖动全部为零。不能把这个字段解释为已经取得有效的时域抖动。

`GameHooks=0` 可用于只读 TAA 输入观察，避免依赖游戏附近的跳板空间；此模式强制不执行比例脉冲。`ApplyScalePulse=0` 可在保留游戏钩子的情况下跳过比例修改。

## 独立投影抖动脉冲

[`BridgeJitterPulse.inl`](../control/BridgeJitterPulse.inl) 提供默认关闭的 `PulseJitter=1` 诊断选项。已核对构建中，MHWSS 配置更新会同时控制 `0x54eb78` 的投影抖动开关与 `0x54ebe0..0x54ebe3` 的渲染开关。`InlGetJitter` 在前者为零时返回零，因此 `Upscaler=None` 下的零抖动不能仅靠游戏菜单选择 TAA 解决。

此选项要求游戏钩子和比例脉冲同时启用，在原始抖动开关为零、四个渲染开关均为零、归一化尺寸匹配输出时，仅将投影抖动开关暂置一；三个条件任一不符即拒绝。它不更改上采样器选择，不启用 TAA 旁路或 NGX 执行。比例恢复时一起恢复抖动；若用户切换了 MHWSS 模式，则保留较新的模式。

最新联合窗口中，324 个 1696×954 输入帧全部取得非零当前抖动，观察到八个不同位置；529 对相邻 TAA 记录的上一帧抖动均等于前一记录的当前抖动。`mhwss_handled_taa=0`。恢复后的当前抖动回到零，所有钩子、内部比例和投影抖动开关均已恢复。这证明了独立控制路径与相机记录的连续性，尚未证明 DLSS 的 MV 解码或时域画质正确。

## 后 TAA 顶点观察

`TracePostTaa=1` 观察同一命令列表最近一次有效 TAA 后的前 32 次 `DrawInstanced`，结合最近视口、顶点绑定、输入/输出尺寸识别放大全屏三角形。实际绘制只在低内部比例期间出现，位于观察窗口中的第三次该类绘制。

游戏复用初始化时保存的顶点地址，后来的 `GetGPUVirtualAddress`、`Map` 和 `CopyBufferRegion` 调用没有暴露这些缓冲区。沿实际绘制栈定位到游戏 `0x259e470`，再由其顶点输入找到已验证字段中的资源对象，解决了这个问题。MHWSS 命令列表包装对象必须通过经核对的 `+8` 转发关系换成底层对象，才能与 API 观察结果对应。

[`BridgeEngineVertices.inl`](../control/BridgeEngineVertices.inl) 校验固定代码签名、4 MiB 独立资源布局、资源虚表入口、GPU 地址和描述尺寸；[`BridgeVertexReader.inl`](../control/BridgeVertexReader.inl) 持有引用及嵌套映射，在结束时释放。它不扫描进程寻找疑似 COM 对象。映射生命周期依据 [D3D12 Map 文档](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12resource-map)；上传堆 CPU 读取只用于这个短诊断窗口，不能当成正式渲染路径的性能方案。

实测三个 4 MiB 上传缓冲区，324 次匹配读取、0 次读取失败。顶点为：

```text
position       UV
(-1,  1)       (0, 0)
(-1, -3)       (0, 1.32430565)
( 3,  1)       (1.3246094, 0)
```

UV 对应 `2 × (实际输入尺寸 - 0.5) / 输出尺寸`，目标视口为 2560×1440。日志中的三角形条目按类型去重，出现一条并不表示只发生一次绘制。

该版本不改顶点、绘制参数、着色器或资源状态，不创建 SR 特征。确认 UV 不等于确认被取样的颜色纹理身份；后续 `TraceTextures` 扩展已补充实际资源对应关系，见 [真实纹理链](TEXTURE-GRAPH.md)。它仍未连接 NGX 参数适配器。

## 纹理和深度追踪

[`BridgeTextureBindings.inl`](../control/BridgeTextureBindings.inl) 读取游戏现有的图形/计算绑定包，并在已核对的描述符复制调用、描述符堆、根签名和根表绑定处交叉核对。记录必须来自同一个命令列表和当前管线绑定，不接受旧描述符地址碰巧复用。计算管线的 SRV 从混合表偏移 9 开始，着色器寄存器和表偏移分别记录。

它还观察 RTV、scissor、图形常量与现有着色器容器。只保存有限数量、经过 DXBC 头和长度检查的字节副本，写盘在采集停止后完成。原始容器和反汇编仅保存在本地。

[`BridgeTextureCopies.inl`](../control/BridgeTextureCopies.inl) 记录实际纹理复制的源/目标与 box；每种输入尺寸的前四个 TAA 记录保留详细操作顺序，其余记录去重。完整例子证明，TAA 后先将有效区域复制到场景纹理，然后经过模糊重建及色调映射，才发生全屏放大。

[`BridgePreparedDepth.inl`](../control/BridgePreparedDepth.inl) 观察 MHWSS 现有 `0x115940` 深度准备函数，记录源和 `RRenderer+0x78` 的目标纹理。它不调用额外的深度复制。高档只读窗口中，270 次 TAA 有 269 次匹配同一命令列表的 Reset 代次；首个接入中的记录未看到 Reset，因此拒绝把它计为已关联。另一个命令列表也会写相同的准备纹理，这份 CPU 命令顺序证据不能替代后续 GPU 同步与像素验证。

当前 OptiScaler 代理的尺寸查询与独立 NVIDIA SDK 回放存在差异（例如 Balanced 为 1505×847，而独立回放为 1485×835）。`bridge_plan` 表示该条代理调用链返回的设置，不能自动等同于最终 NVIDIA 后端的设置。

## 仍需实现

游戏内验证新增原型的 SR 调用和完整后处理链；随后完善常驻特征重建、尺寸变化及场景切换恢复、投影/MV/历史一致性，以及档位 UI。已编译的调用代码不等于已经取得可用的游戏内结果。

当前版本拒绝与部署配置不符的代理哈希。可在 `MhwSrBridge.ini` 的 `[Compatibility] NgxProxySha256` 指定经过部署检查的文件标识；只更新这项并不意味着已经验证代理最终传给 NVIDIA 的 SR 参数。版本 2 会将观察到的哈希和匹配结果写入日志。

近距离跳板空间在游戏启动后可能耗尽，游戏函数钩子因此需要随启动预留；这也是不能把任意新版本热加载视为可靠接入方式的原因。
