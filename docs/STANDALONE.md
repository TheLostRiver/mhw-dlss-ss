# 独立运行入口

目标是由本项目独立完成启动、D3D12 数据接入、抖动、DLSS SR、完整输出和档位控制。已有的质量档 GPU 执行与引擎内部比例代码会继续使用。

当前新增两个程序：

- `MhwSrLauncher.exe`：启动游戏并尽早加载本项目的原生输入模块。失败时保留游戏进程，不强制退出。
- `MhwNativeHost.dll`：通过 D3D12 资源创建、计算管线创建、PSO/CBV 绑定、Reset 和 Dispatch 接口，独立发现常量缓冲和 TAA，并读取相机与屏幕参数。

**这一阶段只采集输入，不执行 SR，不改变分辨率、投影或游戏绘制。** 它用来确认全新进程中的原生输入，不能把编译成功当成已完成独立超分。

## 当前边界

入口按已记录的游戏版本、原生 D3D12Core 方法校准和 TAA shader SHA-256 匹配。缓冲区必须是实际 API 返回的 48 MiB 上传资源；相机与屏幕 CBV 必须位于已持有和映射的资源内，并在本轮 TAA 前刷新。不会搜索内存中的疑似 COM 对象。

模块要求进程未加载 `MHWSS.dll`。测试独立入口时需暂时停用现有 D3D12 代理及历史采集插件，避免原生方法已被改写。安装脚本只移动清单中哈希匹配的具体文件，保留恢复记录。

进入场景后按 F8，采集约 10 秒；高音开始、低音结束。也可通过现有控制脚本发送 `-Observer MhwNativeHost -Action Start` 或 `Cancel`。采集结束后恢复自身钩子，关闭自己的 Enabled 开关；不是常驻 SR 版本。

## 构建与准备

```powershell
cmake --build build --config Release --target MhwNativeHost MhwSrLauncher MhwD3D12Methods --parallel 2
build/bin/Release/MhwD3D12Methods.exe 0 "<暂存目录>/MhwNativeMethods.ini"
```

方法校准器创建自己的 D3D12 对象，只写入方法地址、代码标识和适配器信息，不打开游戏进程。暂存包还需包含启动器、输入模块、启用的 INI、MinHook 许可和逐文件 SHA-256 清单。

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

保持 DX12、游戏 TAA、图像质量高、动态模糊关闭、旧版 DLSS 关闭及原输出分辨率。独立输入确认后再接上 SR；当前 F8 不会开启超分。

需要恢复旧环境时，正常退出游戏后执行：

```powershell
powershell -ExecutionPolicy Bypass -File tools/install_standalone_capture.ps1 `
  -RestoreBackup "<安装时生成的 backup-* 目录>"
```

恢复过程保留当前独立版本文件，不递归删除目录；Steam 启动项需要按原值恢复。原始图像、日志、二进制分析和预编译运行库不进入源码仓库。
