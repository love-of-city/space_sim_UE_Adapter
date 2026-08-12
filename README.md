# BSK→UE5 通用可视化接口

这是一个统一版本管理、双运行端部署的航天可视化接口。Basilisk/MJScene 始终是唯一动力学权威；Unreal Engine 5 只接收状态并负责渲染、相机、特效与任务 UI。

## 仓库结构

```text
Adapters/bsk_render_adapter/             通用 Python/BSK 发送端
Unreal/BskUnrealRenderer/                UE 5.6 项目与 Runtime C++ 插件
test/model/arm/universal_robots_ur5e/    BSD-3-Clause UR5e 集成测试资产
scripts/                                 仓库级统一命令
```

MJCF 的 ASCII/Binary STL 资产准备说明见 [STL_MESHES.md](Unreal/BskUnrealRenderer/docs/STL_MESHES.md)。

发送端和接收端属于同一个产品与 Git 版本，但运行在不同进程中。两端通过 `bsk-render/2` 的大端 `uint32` 长度前缀 JSON 通信；动态帧采用最新帧覆盖，不反压 Basilisk。

详细协议和 UE 扩展接口见 [UE 项目说明](Unreal/BskUnrealRenderer/README.md)与 [协议文档](Unreal/BskUnrealRenderer/docs/PROTOCOL.md)。

## 环境

- Unreal Engine 5.6.1；本机默认路径为 `E:\UE5.6\UE_5.6`
- Visual Studio 2022、MSVC 与 Windows SDK
- Conda 环境 `mujoco-dev`，包含 Basilisk/MJScene 和 NumPy
- Git LFS（用于 `.uasset`、OBJ 和纹理）

UE 引擎、Conda 环境和 Basilisk 上游源码不进入本仓库。Demo 8 会读取同级工作区中的 `basilisk/examples/mujoco/scenarioMJSceneVizard.py`，但不会修改它。

## 统一命令

```powershell
Set-Location E:\mujoco_demo\space_sim_UE_adapter

# 构建与全部自动化测试
.\scripts\build.ps1
.\scripts\test.ps1
.\scripts\smoke_e2e.ps1

# 普通双航天器
.\scripts\run_demo.ps1
.\scripts\run_demo.ps1 -Sender Basilisk -Duration 60

# Demo 8：在线流式；120× 时约 30 Hz 输入
.\scripts\run_demo8.ps1 -Live -LiveRate 120 -BasiliskRoot E:\mujoco_demo\basilisk

# Demo 8：严格 1× 仿真时间
.\scripts\run_demo8.ps1 -Live -LiveRate 1 -KeepRendererOpen -BasiliskRoot E:\mujoco_demo\basilisk

# UR5e/MJScene：六个关节依次平滑转动 20 度并保持
.\scripts\run_ur5e.ps1 -NormalMode preserve -Duration 22 -SimulationRate 1 -JointAngleDegrees 20

# 外部 CubeSat + SO-101：自动准备 STL 并实时显示自由漂浮多刚体动力学
.\scripts\run_spacecraft_arm.ps1 -ModelRoot E:\mujoco_demo\test\model\spacecraft_and_arm -Duration 10 -SimulationRate 1

# 同一模型的原生 PID/纯接触抓取场景
.\scripts\run_spacecraft_arm_grasp.ps1 -ModelRoot E:\mujoco_demo\test\model\spacecraft_and_arm -Duration 10 -SimulationRate 1 -KeepRendererOpen
```

原生抓取命令会注册 CubeSat 机身总览相机和 MJCF 中定义的 SO-101 腕部相机，
两路画面均以实时画中画显示。按 `4` 或 `5` 可分别显示或隐藏；每路相机默认
以 480×270、15 Hz 渲染，与约 30 Hz 的动力学状态流相互独立。

### 完整地球轨道抓取任务

```powershell
Set-Location E:\mujoco_demo\space_sim_UE_adapter
.\scripts\run_orbital_grasp.ps1 `
  -ModelRoot E:\mujoco_demo\test\model\spacecraft_and_arm `
  -Duration 34 -SimulationRate 1 -KeepRendererOpen
```

该 Demo 只运行一套权威 Basilisk/MJScene 系统：卫星位于 500 km 地球轨道，
三个 MuJoCo 原生铰接刚体反作用轮由 Basilisk 标准姿态 FSW 驱动；航天器完成
可见的 0.75 m 闭环接近、相对制动和 2 秒定点保持，随后执行原 SO-101
纯接触抓取。接近推力轴、机身对接相机和最终抓取点共线，使目标在交会期间
保持在相机视轴附近。机械臂采用向前延伸约 7 mm 的保守末端姿态、更紧的夹爪
闭合和抓取后约 3 cm 的回撤。

对接相机和腕部相机均在生成的 MJCF/XML 中定义并由适配器自动发现；UE 同时
显示相机画面、地球、机动羽流和反作用轮遥测。数值验收数据写入
`Unreal/BskUnrealRenderer/Saved/orbital_grasp_metrics.json`。所有权威 MJScene
动力学和控制器统一运行在 500 Hz 任务上，UE 适配器仅将状态降采样为约 30 Hz
的非权威渲染流。

任务面板默认隐藏：按 `M` 显示或隐藏，按 `Tab` 在自由相机与鼠标交互模式之间
切换。在轨抓取 Demo 提供暂停、继续和状态查询按钮，并显示交会、定点保持、
抓取与回撤等任务事件。

相机数据产品可写入磁盘，也可通过独立 TCP 通道按最新帧输出：

```powershell
# RGB、米制深度和实例分割写入磁盘
.\scripts\run_orbital_grasp.ps1 -CaptureDirectory .\capture -CaptureProducts rgb,depth,segmentation -CaptureRate 2 -KeepRendererOpen

# 网络输出时先启动接收端
.\scripts\receive_camera_products.ps1 -Port 5560 -OutputDirectory .\capture-network
.\scripts\run_orbital_grasp.ps1 -CaptureProducts rgb,depth,segmentation -CaptureRate 2 -CaptureNetworkPort 5560
```

仓库级脚本只转发参数，原有 `Unreal\BskUnrealRenderer\scripts` 命令仍然可用。
`scripts` 目录同时提供 `start_renderer.ps1`、`stop_renderer.ps1`、`run_bsk.ps1`、`run_mock.ps1`、`test_demo8.ps1`、`package.ps1` 及 MJCF 资产准备入口。

## Python 接入

开发安装：

```powershell
conda run -n mujoco-dev python -m pip install -e E:\mujoco_demo\space_sim_UE_adapter
```

场景代码使用：

```python
from bsk_render_adapter import BasiliskRenderBridge
```

`bsk_unreal_adapter.BasiliskUnrealBridge` 作为旧 UE 命名兼容层继续保留。

## Git 约定

- `main` 保存可构建、可测试的基线。
- 功能开发使用短期分支，合并前运行 `.\scripts\test.ps1`。
- 发布版本同时更新根目录 `VERSION`、Python 包版本和 UE 插件 `VersionName`。
- UE/Python 生成目录、日志、录制文件和本机引擎不会提交。
- 克隆后先确认 `git lfs install` 和 `git lfs pull` 已完成。

当前基线版本为 `0.2.0`。现已具备多相机画中画与数据采集、任务事件面板和
白名单双向命令；OpNav 结果闭环、异步 GPU 读回和更多 BSK 模块专用适配器仍待完善。
