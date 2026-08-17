# Python 发送端

`bsk_render_adapter` 是渲染器无关的 Basilisk/MJScene 状态发送端。它负责对象注册、坐标和姿态转换、manifest、最新帧网络发送以及 `.bskrec` 录制，不包含 Unreal Engine 代码。任意场景都可向 `BasiliskRenderBridge` 传入 `recording_path`；离线录制可搭配公共 `RecordingOnlyPublisher`，无需启动渲染器。

从仓库根目录执行 `python -m pip install -e .` 后使用：

```python
from bsk_render_adapter import BasiliskRenderBridge
```
