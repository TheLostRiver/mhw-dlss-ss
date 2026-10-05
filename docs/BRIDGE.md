# 桥接前置版本

核心源码：[`MhwSrBridge.cpp`](../control/MhwSrBridge.cpp)、[`BridgePreflight.inl`](../control/BridgePreflight.inl)、[`SrParameterAdapter.h`](../control/SrParameterAdapter.h)、[`SrParameterAdapter.cpp`](../control/SrParameterAdapter.cpp)。

**当前启动入口只做前置观察，不执行游戏内 SR。** 参数转换函数已编译为导出函数，但没有接入 MHWSS 的 Create/Evaluate 分发表。

## 原有渲染入口

针对已记录的 MHWSS 1.0.2 构建，分析发现其上采样路径可将原生 TAA PSO 替换为旁路 PSO，在 TAA dispatch 后整理输入并调用上采样入口。目标是在这个替代路径中执行一次 SR，而不是在 SR 前后再叠加独立 DLAA。

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

## 当前观察器（版本 3）

1. 启动早期预留比例更新、完整场景绘制、quad 和场景视口设置四个游戏函数钩子，等待显式 Start 信号。
2. 要求 DX12、图像质量 High、MHWSS Upscaler=None 稳定后开始；默认还要求 OptiScaler 配置中的帧生成关闭。
3. 记录原生 TAA PSO、dispatch、命令列表 Reset/ClearState 代次、最近视口和场景调用序号。独立追踪实际 compute CBV 绑定，在 TAA dispatch 前读取 CBScreen、相机投影抖动与少量附加常量。
4. 在场景视口设置调用 `0x23c391a → 0x23d0330` 直接读取游戏本次计算出的输入矩形，并读取改动前的输出矩形；与后 TAA quad 对照。推算矩形只作为辅助信息，不替代这条实际参数记录。在渲染回调内查询 NGX 推荐尺寸。
5. 复用已验证的 3 秒内部比例脉冲，随后恢复比例和十个钩子，并关闭自己的 INI。

游戏场景函数使用线程局部作用域记录本次调用的上下文，支持嵌套调用后的恢复。实测 TAA 的命令记录没有落在这份作用域内，不能据此把 CPU 侧的最新比例传给 TAA。版本 3 因此增加独立的命令列表 CBV 追踪；相机与屏幕绑定必须在上次 TAA 后刷新，Reset/ClearState 会清除记录。

在原生 TAA 开启、MHWSS=None、High 的只读窗口中，已取得 533 份有效 CBScreen，0 份无效，视图与输出均为 2560×1440。此窗口不改变比例、不提交 GPU 命令，结束后恢复钩子并释放附加映射/引用。尚需核对低内部比例下的字段含义与投影抖动。

`GameHooks=0` 可用于只读 TAA 输入观察，避免依赖游戏附近的跳板空间；此模式强制不执行比例脉冲。`ApplyScalePulse=0` 可在保留游戏钩子的情况下跳过比例修改。

当前 OptiScaler 代理的尺寸查询与独立 NVIDIA SDK 回放存在差异（例如 Balanced 为 1505×847，而独立回放为 1485×835）。`bridge_plan` 表示该条代理调用链返回的设置，不能自动等同于最终 NVIDIA 后端的设置。

## 仍需实现

真正接通 SR 调用、在对应帧产出完整 SR 后调整后 TAA 取样、特征重建与释放、尺寸变化及场景切换恢复、投影/MV/历史一致性，以及档位 UI。

当前版本拒绝与部署配置不符的代理哈希。可在 `MhwSrBridge.ini` 的 `[Compatibility] NgxProxySha256` 指定经过部署检查的文件标识；只更新这项并不意味着已经验证代理最终传给 NVIDIA 的 SR 参数。版本 2 会将观察到的哈希和匹配结果写入日志。

近距离跳板空间在游戏启动后可能耗尽，游戏函数钩子因此需要随启动预留；这也是不能把任意新版本热加载视为可靠接入方式的原因。
