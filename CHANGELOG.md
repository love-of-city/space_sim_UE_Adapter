# Changelog

## Unreleased

- 支持 MJCF 引用的 ASCII/Binary STL：确定性缓存转换、完整 LOD0、源指纹重导入、角度感知法线和基础 UV。
- MJCF 元数据遍历支持 `<frame>` 安装层级，可发现 MjSpec 组合模型中的嵌套 mesh。
- 增加自由漂浮 CubeSat + SO-101 一键实时示例，并支持在高频动力学任务中约 30 Hz 限频采样。
- 增加原生 SO-101 PID/纯接触抓取的 UE 实时包装，并规避 Windows Python MuJoCo/Basilisk MuJoCo DLL 预加载冲突。
- UR5e 示例改为六个关节依次平滑转动固定角度并保持，默认每关节 20°。
- MJCF 动态目标灯设为 Movable，避免每帧 Mobility 警告造成 Game Thread 卡顿。

## 0.2.0 - 2026-08-10

- 建立统一的 BSK→UE5 单仓库版本基线。
- 提供渲染器无关的 `BasiliskRenderBridge` 和 `bsk-render/2` 协议。
- 提供 UE 5.6 Runtime C++ 接收插件、断线重连、最新帧、浮动原点和插值。
- 支持 Spacecraft、MJScene 刚体/几何、天体、航天器仪器可视元素和录制回放。
- 提供双航天器、Demo 8 和 UR5e 示例。
- UR5e Static Mesh 使用完整标准 LOD0、关闭 Nanite，并在构建阶段完成米到厘米转换。
