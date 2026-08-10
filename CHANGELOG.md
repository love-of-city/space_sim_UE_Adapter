# Changelog

## Unreleased

- UR5e 示例改为六个关节依次平滑转动固定角度并保持，默认每关节 20°。
- MJCF 动态目标灯设为 Movable，避免每帧 Mobility 警告造成 Game Thread 卡顿。

## 0.2.0 - 2026-08-10

- 建立统一的 BSK→UE5 单仓库版本基线。
- 提供渲染器无关的 `BasiliskRenderBridge` 和 `bsk-render/2` 协议。
- 提供 UE 5.6 Runtime C++ 接收插件、断线重连、最新帧、浮动原点和插值。
- 支持 Spacecraft、MJScene 刚体/几何、天体、航天器仪器可视元素和录制回放。
- 提供双航天器、Demo 8 和 UR5e 示例。
- UR5e Static Mesh 使用完整标准 LOD0、关闭 Nanite，并在构建阶段完成米到厘米转换。
