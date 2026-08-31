# CubeSat + SO-101 MJCF

This directory contains a generated free-floating 12U CubeSat with the
MuJoCo Menagerie SO-101 arm attached to its positive Z face. The generated
model is intended as a structural and dynamics baseline, not a flight-quality
spacecraft model.

## Files

- `cubesat_bus.xml`: editable source model for the placeholder 12U bus.
- `build_model.py`: parameterized MjSpec composition and validation tool.
- `cubesat_so101.xml`: generated MJCF consumed by MuJoCo and Basilisk.

Do not edit `cubesat_so101.xml` directly. Change the bus source or generator
arguments and regenerate it.

## Generate and check

The generator is pinned to the same MuJoCo version as the local Basilisk
build:

```bash
uv run --no-project --with mujoco==3.11.0 \
  python assets/cubesat_so101/build_model.py

uv run --no-project --with mujoco==3.11.0 \
  python assets/cubesat_so101/build_model.py --check
```

The default bus has full dimensions `0.2263 x 0.2263 x 0.3405 m`, mass
`24 kg`, a centered center of mass, and uniform-box inertia. The SO-101 base
is mounted at `[0, 0, 0.17025] m` with its positive Z axis pointing away from
the bus. These values are placeholders and must be replaced with measured
spacecraft properties before engineering analysis.

For example, a custom bus and mount can be generated with:

```bash
uv run --no-project --with mujoco==3.11.0 \
  python assets/cubesat_so101/build_model.py \
  --bus-size 0.23 0.23 0.36 \
  --bus-mass 22 \
  --mount-pos 0.02 0 0.18 \
  --mount-quat 1 0 0 0
```

Use `--bus-fullinertia IXX IYY IZZ IXY IXZ IYZ` to replace the uniform-box
inertia. Append `--help` to the pinned `uv run` command for the full interface.

## Stable model interface

The combined model has one free joint, `cubesat_free`, and six direct torque
actuators:

```text
so101_shoulder_pan_motor
so101_shoulder_lift_motor
so101_elbow_flex_motor
so101_wrist_flex_motor
so101_wrist_roll_motor
so101_gripper_motor
```

Each actuator input is joint torque in `N*m`, with a control and actuator-force
limit of `[-2.94, 2.94] N*m`. MuJoCo serializes these motor-equivalent
actuators as `<general>` elements with unit gain, no bias, and no activation
dynamics. There is no built-in position controller.

Useful sites are `cubesat_origin`, `so101_mount`, `so101_baseframe`, and
`so101_gripperframe`. The upstream fixed wrist camera is retained as
`so101_wrist_cam`.

## Basilisk loading

Basilisk must receive the STL files through its MuJoCo virtual file system:

```python
from pathlib import Path

from Basilisk.simulation import mujoco

model_dir = Path("assets/cubesat_so101").resolve()
so101_dir = model_dir.parent / "robotstudio_so101"
mesh_files = [
    str(path.resolve()) for path in sorted((so101_dir / "assets").glob("*.stl"))
]

scene = mujoco.MJScene.fromFile(
    str(model_dir / "cubesat_so101.xml"),
    files=mesh_files,
)
```

Retrieve each torque input with `scene.getSingleActuator(name)` and connect a
`SingleActuatorMsg`. Joint control, spacecraft actuators, target objects,
grasping logic, and camera rendering are intentionally outside this model
package.

## Provenance and license

The unmodified upstream model and meshes are in `../robotstudio_so101/`. They
come from MuJoCo Menagerie and are licensed under Apache-2.0; see the upstream
`README.md` and `LICENSE`. The generated XML records that it is a modified,
composite model and references the same license.
