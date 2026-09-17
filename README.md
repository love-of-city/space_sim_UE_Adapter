# BSK → Unreal Engine 可视化适配器

Basilisk/MJScene 状态发送端与 UE 5.6 渲染接收端。动力学由仿真端推进，UE 负责渲染与相机采集，协议为 `bsk-render/2`。

| 目录 | 用途 |
| --- | --- |
| `Adapters/bsk_render_adapter/` | 通用 Python 发送端 |
| `Unreal/BskUnrealRenderer/` | UE 项目、Runtime 插件、示例和测试 |
| `scripts/` | 仓库级入口 |
| `test/model/` | 独立 Demo 模型 |

完整遥操作平台按[服务端 README](https://github.com/love-of-city/space_sim_server#readme)安装两个同级仓库。本页是适配器独立构建与验证流程。

## 1. 环境与安装

Windows x64、PowerShell 7、Git LFS、UE 5.6、Visual Studio 2022 C++ 游戏开发工具及 Windows SDK。图形运行需要支持 UE 的 GPU/驱动。Python 3.11+；真实仿真需匹配解释器的 Basilisk/MJScene，mock 与协议测试不依赖它。

所有命令从**本仓库根目录**执行。可复用服务端的仿真环境，无需创建第二套：

```powershell
git lfs install --local
git lfs pull
. .\Unreal\BskUnrealRenderer\scripts\python_runtime.ps1
$env:SPACE_SIM_PYTHON = Resolve-SpaceSimPython -RepositoryRoot $PWD.Path
```

**uv 环境：**

```powershell
uv pip install --python $env:SPACE_SIM_PYTHON -e ".[test]"
```

uv 环境中没有 pip 是正常现象，不运行 `ensurepip`。传统 pip 管理的环境可选下面的替代命令，不必两种都执行：

```powershell
& $env:SPACE_SIM_PYTHON -m pip install -e ".[test]"
```

```powershell
& $env:SPACE_SIM_PYTHON -c "import numpy; from bsk_render_adapter import BasiliskRenderBridge; print(BasiliskRenderBridge)"
```

先激活所需环境，或在仓库/父工作区使用 `.venv`/`venv`。解释器选择顺序为 `-Python`、`SPACE_SIM_PYTHON`、已激活环境、本地环境、PATH；选中环境无效时直接报错。首次创建 uv 环境使用 `uv venv`，Python 版本须与 Basilisk 匹配；不要重建已有仿真环境。Python 包安装不会安装 UE 或 Basilisk，资产导入使用 UE 内置 Python。

## 2. 构建与测试

设置本机包含 `Engine` 的目录，下例按实际安装位置修改。依次执行，失败时修复后再继续：

```powershell
$env:UE56_ROOT = 'D:\UE\UE_5.6'
pwsh -NoProfile -File .\scripts\build.ps1
pwsh -NoProfile -File .\scripts\test.ps1 -SkipBuild
pwsh -NoProfile -File .\scripts\smoke_e2e.ps1
```

分别验证 Editor C++ 构建、Python/UE 自动化测试、mock 两次连接与重连。默认 smoke 使用 NullRHI，不验收 GPU 或 WebRTC 视频。依赖外部 Basilisk 的测试可能 skip，须检查输出。

无 UE 时可只执行 Python 部分（可选替代，不能视作完整验收）：

```powershell
pwsh -NoProfile -File .\scripts\test.ps1 -SkipUnreal
```

## 3. 独立预览

完成构建后运行 mock，60 秒后自动结束：

```powershell
pwsh -NoProfile -File .\scripts\run_demo.ps1 -Duration 60
```

C 切换自由相机，W/S、A/D、Q/E 和鼠标观察，Home 返回；M 显示任务面板。提前停止：

```powershell
pwsh -NoProfile -File .\scripts\stop_renderer.ps1
```

浏览器操作台与真实机械臂使用服务端启动入口；renderer 不会自动启动信令和网页。

## 开发与参考

- 合并前执行构建、测试和 smoke；渲染改动另做图形验收。
- 模型、纹理和 `.uasset` 使用 LFS；`Saved/`、`Intermediate/`、虚拟环境、日志和录制不提交。
- 移动路径后重新生成资产映射，不复制别人 `Saved/AssetImport` 中的 catalog。
- 发布时保持 `VERSION`、Python 包和 UE 插件版本一致。
- [Python 发送端](Adapters/README.md) / [UE 模块](Unreal/BskUnrealRenderer/README.md)
- [协议](Unreal/BskUnrealRenderer/docs/PROTOCOL.md) / [MJCF](Unreal/BskUnrealRenderer/docs/MJCF_MESHES.md) / [STL](Unreal/BskUnrealRenderer/docs/STL_MESHES.md)

Demo 8、UR5e、CubeSat、轨道抓取、录制回放与打包脚本是可选开发工具，参数见对应脚本，不是首次部署必做步骤。OpNav 闭环、异步 GPU 读回和单遍分割尚未完成。
