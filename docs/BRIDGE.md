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

## 当前观察器

1. 启动早期预留比例更新、完整场景绘制、quad 三个游戏函数钩子，等待显式 Start 信号。
2. 要求 DX12、图像质量 High、MHWSS Upscaler=None 稳定后开始。
3. 记录原生 TAA PSO、dispatch、命令列表 reset 代次、最近视口和场景调用序号。
4. 对照同次场景调用中的候选输入矩形和后 TAA quad 矩形；在渲染回调内查询 NGX 推荐尺寸。
5. 复用已验证的 3 秒内部比例脉冲，随后恢复比例和七个钩子，并关闭自己的 INI。

游戏场景函数使用线程局部作用域记录本次调用的上下文，支持嵌套调用后的恢复。该机制已完成编译，尚未取得新一轮运行证据。CPU 调用关联也不能代替 GPU 资源和连续帧验证。

## 仍需实现

真正接通 SR 调用、在对应帧产出完整 SR 后调整后 TAA 取样、特征重建与释放、尺寸变化及场景切换恢复、投影/MV/历史一致性，以及档位 UI。

当前版本拒绝未知代理哈希。近距离跳板空间在游戏启动后可能耗尽，游戏函数钩子因此需要随启动预留；这也是不能把任意新版本热加载视为可靠接入方式的原因。
