# CubeSat + SO-101 pure-contact grasp model

This package extends the generated `cubesat_so101` model with an independent
free-floating capture target. It is a calibrated regression case for approach,
contact, gripper closure, retained grasp, withdrawal, and system momentum
conservation.

The target has its own `capture_target_free` joint, mass `0.10 kg`, and a narrow
cylindrical handle. Capture uses only MuJoCo contact and friction. The model has
no equality constraint, weld, or adhesion. Its stored default target pose is
deliberately contact-free; the scenario sets the calibrated task pose after
Basilisk initialization.

## Generate and validate

```bash
uv run --no-project --with mujoco==3.11.0 \
  python assets/cubesat_so101_grasp/build_model.py

uv run --no-project --with mujoco==3.11.0 \
  python assets/cubesat_so101_grasp/build_model.py --check
```

Do not edit `cubesat_so101_grasp.xml` directly. Edit `build_model.py` and
regenerate it. The source `../cubesat_so101/cubesat_so101.xml` and vendored
Menagerie SO-101 remain unchanged.

## Run the grasp task

```bash
uv run --no-project --with mujoco==3.11.0 \
  python scenarios/scenario_cubesat_so101_grasp.py
```

The command prints JSON metrics and returns a nonzero status if a grasp or
momentum criterion fails. Use `--no-assert` to collect calibration metrics
without enforcing thresholds.

Render the recorded Basilisk state history to an H.264 video with:

```bash
uv run --no-project --with mujoco==3.11.0 \
  --with imageio-ffmpeg --with pillow \
  python scenarios/scenario_cubesat_so101_grasp.py \
  --video tmp/cubesat_so101_grasp.mp4
```

The renderer replays the generalized states produced by Basilisk; it does not
run a second MuJoCo simulation. The view follows the gripper and target and
shows the current task phase and `FREE`/`CONTACT` state. MuJoCo contact-force
glyphs are hidden by default because they can obscure the narrow target handle;
enable them explicitly with `--contact-debug` when diagnosing contact physics.

The controller is a six-channel Basilisk joint PID followed by per-actuator
torque saturation. A quintic joint-space reference performs pregrasp hold,
approach, closure, hold, withdrawal, and final verification over `10 s`.

## Stable target interface

| Type | Name |
|---|---|
| body | `capture_target` |
| free joint | `capture_target_free` |
| handle geom | `capture_target_handle` |
| main geom | `capture_target_body_geom` |
| grasp site | `capture_target_grasp` |

The full model has `nq=20`, `nv=18`, `nu=6`, two free joints, and total mass
`24.744006 kg`.

Momentum is reconstructed from every recorded Basilisk `qpos/qvel` sample with
native MuJoCo `mj_subtreeVel`. Mechanical energy is reported but is not asserted
constant because the PID actuators, damping, friction, and inelastic contact do
work or dissipate energy.
