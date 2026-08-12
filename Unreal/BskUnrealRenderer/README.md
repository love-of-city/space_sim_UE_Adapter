# BskUnrealRenderer

MJCF 的 ASCII/Binary STL 离线导入说明见 [docs/STL_MESHES.md](docs/STL_MESHES.md)。

通用 MJCF mesh 与 UR5e 实时示例见 [docs/MJCF_MESHES.md](docs/MJCF_MESHES.md)。

UE 5.6 运行时航天可视化端。Basilisk 和可选 MJScene 始终负责轨道、姿态、多刚体、接触与控制动力学；UE 只负责画面、相机、资产、特效和任务 UI。

本机验证环境：

- Unreal Engine 5.6.1：`E:\UE5.6\UE_5.6`
- Visual Studio 2022、MSVC 14.38、Windows SDK 10.0.22621
- Conda 环境：`mujoco-dev`
- RTX 4060 Laptop 8 GB / 16 GB RAM 的中低负载默认设置

## 直接运行

普通双航天器 mock：

```powershell
Set-Location E:\mujoco_demo\space_sim_UE_adapter\Unreal\BskUnrealRenderer
.\scripts\run_demo.ps1
```

真实 Basilisk 双航天器：

```powershell
.\scripts\run_demo.ps1 -Sender Basilisk -Duration 60
```

Demo 8（两套 MJScene、普通 Spacecraft、8 块展开太阳翼、CSS、星敏、通信机和 Earth/Moon/Sun）：

```powershell
.\scripts\run_demo8.ps1 -BasiliskRoot E:\mujoco_demo\basilisk
```

该命令先使用原 `scenarioMJSceneVizard.py` 的动力学生成完整 `.bskrec`，再以默认 120×仿真时间在 UE 回放。BSK 的 4 秒任务周期对应约 30 Hz 墙钟画面。保留 UE 窗口：

```powershell
.\scripts\run_demo8.ps1 -KeepRendererOpen
```

复用已有录制，跳过 BSK 计算：

```powershell
.\scripts\run_demo8.ps1 -ReuseRecording -KeepRendererOpen
```

Demo 8 也可以在线实时流式运行。此时 UE 先监听，Basilisk/MJScene 随后边计算边以非阻塞 TCP 发送最新状态，不生成中间回放：

```powershell
.\scripts\run_demo8.ps1 -Live -BasiliskRoot E:\mujoco_demo\basilisk
```

实时模式默认 `-LiveRate 120`，表示每 1 秒墙钟推进 120 秒仿真时间。原 Demo 8 的任务周期为 4 秒，因此对应约 30 Hz 墙钟状态输入。可自行调整，例如：

```powershell
.\scripts\run_demo8.ps1 -Live -LiveRate 60
.\scripts\run_demo8.ps1 -Live -LiveRate 1 -KeepRendererOpen
```

`-LiveRate 1` 是严格的 1×墙钟节奏，但原场景每 4 秒仿真时间才产生一帧，因此画面只有 0.25 Hz，而且完整的 0.75 圈轨道约需 71 分钟。交互演示推荐使用默认 120×。这里的“实时”是指 BSK/MJScene 与 UE 同时在线运行；无论倍率多少，Basilisk/MJScene 始终是唯一动力学权威，限速器也不会等待 UE 回应。

分窗口调试：

```powershell
.\scripts\start_renderer.ps1
.\scripts\run_bsk.ps1 -Duration 60
.\scripts\stop_renderer.ps1
```

直接回放：

```powershell
.\scripts\start_renderer.ps1 -ReplayPath .\Saved\Recordings\demo8.bskrec -ReplayRate 120
```

相机：`W/S` 前后、`A/D` 左右、`Q/E` 上下、鼠标观察。辅助视锥默认隐藏；按 `1` 切换 CSS、`2` 切换通用传感器（Demo 8 中为星敏）、`3` 切换通信机视场。C++/Blueprint 可通过 `SetVisualKindVisible`、`ToggleVisualKindVisible` 和 `IsVisualKindVisible` 控制任意 visual kind，也支持按对象 ID 进入环绕或跟随模式。

默认显示策略可在 `Config/bsk_unreal_scene.json` 中覆盖：

```json
"visuals": {
  "visibility_by_kind": {
    "css": false,
    "generic_sensor": false,
    "transceiver": false
  }
}
```

## 构建与测试

```powershell
Set-Location E:\mujoco_demo\space_sim_UE_adapter\Unreal\BskUnrealRenderer

.\scripts\build.ps1
.\scripts\build.ps1 -Game
.\scripts\test.ps1
.\scripts\smoke_e2e.ps1
.\scripts\smoke_e2e.ps1 -Sender Basilisk
.\scripts\test_demo8.ps1
.\scripts\package.ps1
```

`test_demo8.ps1` 检查 1063 个 4 秒状态样本、12 个动态刚体、11 个 MJ geoms、8 块太阳翼姿态变化、±15 m 编队距离、6 个仪器可视元素和 3 个天体。

UR5e 示例默认让六个关节依次平滑转动 20 度，每个关节运动 2 秒、停顿 0.75 秒，最终保持目标姿态：

```powershell
.\scripts\run_ur5e.ps1 -Duration 22 -SimulationRate 1 -JointAngleDegrees 20
```

## 架构

```text
Basilisk / MJScene（唯一动力学权威）
        |
        | BasiliskRenderBridge
        | hello + retained scene_manifest + latest frame + bounded event
        | uint32-BE length + UTF-8 JSON, bsk-render/2
        v
FBskTcpReceiver（FRunnable 网络线程）或 FBskReplaySource
        |
        | POD 消息；manifest/event/frame 分离
        v
UBskRenderWorldSubsystem + ABskSceneController（Game Thread）
        |
        +-- 一个刚体 Actor、多个 body-local geom components
        +-- world-space 父子刚体状态
        +-- Earth/Moon/Sun、轨道线、仪器视锥、多相机
        +-- 两帧位置插值与四元数 SLERP
```

通用发送端位于仓库的 `Adapters/bsk_render_adapter`。它提供 `BasiliskRenderBridge`、MJScene 自动发现、天体/仪器/相机描述、非阻塞发送和 `.bskrec`。没有修改 Basilisk 上游源码。

### UE Runtime 扩展接口

项目插件可以实现 `IBskRenderExtension`，而不必修改 `ABskSceneController`。扩展在 Game Thread 上按优先级调用，可以选择接管对象、天体、Visual 或相机 Actor 的创建；返回 `nullptr` 会继续尝试下一个扩展，并最终使用内置工厂。

```cpp
class FMyBskExtension final : public IBskRenderExtension
{
public:
    virtual FName GetExtensionName() const override { return TEXT("MyProject.Renderer"); }
    virtual AActor* TrySpawnVisual(
        const FBskRenderSpawnContext& Context,
        const FBskVisualDefinition& Definition) override;
    virtual void OnFrameApplied(const FBskRenderFrame& Frame) override;
};

auto Extension = MakeShared<FMyBskExtension>();
World->GetSubsystem<UBskRenderWorldSubsystem>()
    ->RegisterRenderExtension(Extension, 100);
```

`UBskRenderWorldSubsystem` 还提供：

- 已接收和已渲染最新帧的独立缓存；
- manifest/event/frame 的 Game Thread 广播；
- `IBskCaptureProvider` 注册、相机登记和捕获请求路由；
- 会话 ID、manifest revision、仿真时间和最新 frame ID 查询。

这些接口只消费 BSK 状态，不允许扩展在 UE 中推进或覆盖权威动力学。

### Vizard 风格接入

新场景可使用与 `vizSupport.enableUnityVisualization()` 参数形状一致的入口：

```python
from bsk_render_adapter import ue_support as ueSupport

renderer = ueSupport.enableUnrealVisualization(
    scSim,
    "simTask",
    spacecraftList,
    rwEffectorList=rwEffectors,
    thrEffectorList=thrusterEffectors,
    cssList=cssDevices,
    genericSensorList=genericSensors,
    transceiverList=transceivers,
)
```

RW、推力器和 CSS 会从 Basilisk 公共配置及标准输出消息自动发现。RW 发送绝对转角、轮速、转矩和饱和状态；推力器发送推力和节流率；CSS 发送信号、归一化信号及有效状态。运行时动画使用仿真状态并参与回放插值，不依赖 UE 帧率积分。旧的手工 `VisualElement` 注册方式仍然兼容。

协议字段见 [docs/PROTOCOL.md](docs/PROTOCOL.md)。

## 场景与资产

默认启用 UE 5.6 自带的 Epic `Celestial Vault` 资源，但不使用其星历计算：

- `MI_CelestialVault` 与 `SM_CelestialVault` 提供银河背景；
- `MI_Stars` 与官方 billboard mesh 渲染低成本实例化恒星；
- `MI_Moon` 渲染月球盘；
- `SkyAtmosphere` 以 BSK Earth 的位置和半径生成地球大气；
- Earth/Moon/Sun 的位置、姿态和太阳光方向仍全部来自 BSK 帧。

这些选项位于 `Config/bsk_unreal_scene.json`：

```json
"use_official_celestial_assets": true,
"use_earth_sky_atmosphere": true,
"celestial_vault_radius_km": 500000.0,
"celestial_background_intensity": 0.12
```

官方插件资产加载失败时会自动回退到黑色背景、实例化白色星点和单色天体，不影响网络接收或动力学回放。

配置文件为 `Config/bsk_unreal_scene.json`。本地映射优先于 manifest 资产；随后依次尝试 UE Static/Skeletal Mesh、可选 USD、自动 MJ primitive，最后使用按名称着色的占位体。

外部磁盘资产默认禁止。需要时显式设置：

```json
"assets": {"allow_external_files": true}
```

生产打包更推荐先把 USD 导入 `/Game`，再使用 UE 软对象路径。运行时不依赖 Unreal Python。

## 坐标、尺度和线程约束

发送端计算：

```text
r_L[m] = C_LN (r_N - origin_N)
C_LB   = C_LN C_BN^T
```

线上的姿态是主动 body-to-local `(w,x,y,z)`。UE 再执行右手系到左手 Z-up 的 Y 镜像，并把米转换为厘米。协议状态使用 `FVector3d/FQuat4d` 和 UE LWC；绝对惯性原点仍保留在帧中。

网络线程负责 accept、分包和 JSON→POD，绝不访问 UObject。Actor、组件、材质、附着和 Transform 只在 Game Thread 创建或更新。发送端和接收端各保留一个最新动态帧，断线时保持最后画面并继续监听。

## 已交付与后续接口

已交付：

- v1 兼容和 `bsk-render/2`
- reliable manifest、bounded event、latest-frame state
- Spacecraft/MJScene/geom/父子层级/天体/常用仪器
- Vizard 风格场景入口及 RW、推力器、CSS 自动发现
- 带类型动态通道、RW 绝对角度、推力羽流和 CSS 状态动画
- 录制回放、暂停/倍速/单步/定位 API
- 多相机 manifest 和运行时 CameraActor
- `IBskCaptureProvider` 扩展接口
- 基础 HUD、自由/环绕/跟随相机和轨道线

后续实现而非首版功能：RGB/深度/分割产品写出、UDP、二进制编码、严格锁步、完整 Vizard 事件面板和 OpNav 图像闭环。
