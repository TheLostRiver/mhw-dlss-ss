# 第三方依赖

依赖由使用者从各自官方仓库取得。`references/` 不纳入本仓库提交；辅助脚本不会下载 MHWSS、游戏文件或 OptiScaler。

| 依赖 | 固定提交 | 用途与许可 |
|---|---|---|
| [MinHook](https://github.com/TsudaKageyu/minhook) | `8af6b4acae5a9388fd742b56fa79ece89d96f823` | API 钩子；许可副本见 [LICENSE-Minhook.txt](probe/LICENSE-Minhook.txt)，上游许可也随依赖保留 |
| [NVIDIA DLSS SDK](https://github.com/NVIDIA/DLSS) | `374959484e79a640feaba44c93ac8cfb0a03f5b5` | 官方 NGX 头文件；独立回放器另链接 SDK 静态入口库；适用 [NVIDIA SDK 许可](https://github.com/NVIDIA/DLSS/blob/374959484e79a640feaba44c93ac8cfb0a03f5b5/LICENSE.txt) |

Python 工具按需使用 Capstone、pefile、NumPy 和 Pillow，由 `requirements.txt` 安装。没有把这些包的源代码复制进本仓库。

MHWSS 是研究时使用的外部渲染接入工具，不包含其源码或运行文件。本项目不包含游戏代码镜像、提取的游戏着色器或第三方运行库；源码中的必要接口位置、短指令签名和格式说明用于对应特定构建的互操作研究。

本仓库暂未指定自有代码的通用授权许可；第三方依赖分别适用其自身许可。
