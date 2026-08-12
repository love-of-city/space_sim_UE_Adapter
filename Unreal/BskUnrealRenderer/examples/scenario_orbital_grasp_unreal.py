"""Earth-orbit rendezvous and grasp with native MJScene wheels and thrust."""

from __future__ import annotations

import argparse
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import xml.etree.ElementTree as ET

import numpy as np


RW_DEFINITIONS = (
    {
        "body_name": "orbit_rw_x",
        "joint_name": "orbit_rw_x_spin",
        "actuator_name": "orbit_rw_x_motor",
        "position_body_m": (0.0, 0.132, 0.0),
        "axis_body": (1.0, 0.0, 0.0),
        "color_rgba": (0.18, 0.45, 1.0, 1.0),
        "label": "RW-X",
    },
    {
        "body_name": "orbit_rw_y",
        "joint_name": "orbit_rw_y_spin",
        "actuator_name": "orbit_rw_y_motor",
        "position_body_m": (-0.132, 0.0, 0.0),
        "axis_body": (0.0, 1.0, 0.0),
        "color_rgba": (0.15, 0.9, 0.38, 1.0),
        "label": "RW-Y",
    },
    {
        "body_name": "orbit_rw_z",
        "joint_name": "orbit_rw_z_spin",
        "actuator_name": "orbit_rw_z_motor",
        "position_body_m": (0.0, 0.0, -0.188),
        "axis_body": (0.0, 0.0, 1.0),
        "color_rgba": (1.0, 0.22, 0.15, 1.0),
        "label": "RW-Z",
    },
)

RW_MASS_KG = 0.08
RW_SPIN_INERTIA = 2.5e-5
RW_TRANSVERSE_INERTIA = 1.65e-5
RW_TORQUE_LIMIT_NM = 0.003
RW_SPEED_LIMIT_RAD_S = 6000.0 * 2.0 * math.pi / 60.0
MISSION_TIME_STEP_S = 0.002
ORBIT_ALTITUDE_M = 500_000.0
RENDEZVOUS_INITIAL_OFFSET_M = 0.75
DOCKING_CAMERA_POSITION_BODY_M = np.array([0.06, -0.08, 0.19])
# Unit vector from the body-mounted camera to the validated final grasp point.
# The rendezvous translation and camera boresight intentionally share it.
DOCKING_AXIS_BODY = np.array([0.79852285, 0.19236641, 0.57040023])
RENDEZVOUS_START_S = 2.0
RENDEZVOUS_STOP_S = 20.0
STATION_KEEP_STOP_S = 22.0
GRASP_TIME_OFFSET_S = STATION_KEEP_STOP_S
RENDEZVOUS_CONTROL_STOP_S = GRASP_TIME_OFFSET_S + 4.5
GRASP_SEQUENCE_DURATION_S = 12.0
MINIMUM_MISSION_DURATION_S = GRASP_TIME_OFFSET_S + GRASP_SEQUENCE_DURATION_S
RENDEZVOUS_MAX_THRUST_N = 0.60
RENDEZVOUS_ESTIMATED_MASS_KG = 24.984
RENDEZVOUS_POSITION_GAIN_N_PER_M = 6.0
RENDEZVOUS_VELOCITY_GAIN_N_PER_MPS = 20.0
TIGHT_GRIP_JOINT_RAD = -0.16
# A conservative half-step toward the inverse-kinematic solution that places
# the gripper reference on the handle while retaining the original tool angle.
EXTENDED_ALIGNED_ARM = np.array(
    [0.0, 0.10916789, -0.19346612, 0.08429824, 0.0, 0.4]
)
# Retract the captured object by about 3 cm along the docking axis while
# changing tool orientation by about one degree.
RETRACTED_GRASP_ARM = np.array(
    [0.01690432, -0.11100908, 0.19918142, -0.08815929, -0.00059566, -0.16]
)


def _quintic_rendezvous_reference(seconds: float) -> tuple[float, float, float]:
    """Return approach distance, speed and acceleration along relative +Y."""

    if seconds <= RENDEZVOUS_START_S:
        return 0.0, 0.0, 0.0
    if seconds >= RENDEZVOUS_STOP_S:
        return RENDEZVOUS_INITIAL_OFFSET_M, 0.0, 0.0
    duration = RENDEZVOUS_STOP_S - RENDEZVOUS_START_S
    phase = (seconds - RENDEZVOUS_START_S) / duration
    blend = 10.0 * phase**3 - 15.0 * phase**4 + 6.0 * phase**5
    blend_rate = (30.0 * phase**2 - 60.0 * phase**3 + 30.0 * phase**4) / duration
    blend_acceleration = (
        60.0 * phase - 180.0 * phase**2 + 120.0 * phase**3
    ) / duration**2
    return (
        RENDEZVOUS_INITIAL_OFFSET_M * blend,
        RENDEZVOUS_INITIAL_OFFSET_M * blend_rate,
        RENDEZVOUS_INITIAL_OFFSET_M * blend_acceleration,
    )


def build_orbital_mjcf(source: Path, output: Path, mesh_directory: Path) -> Path:
    """Create a derived MJCF without modifying the external model source."""

    root = ET.parse(source).getroot()
    compiler = root.find("./compiler")
    if compiler is None:
        compiler = ET.Element("compiler")
        root.insert(0, compiler)
    compiler.set("meshdir", mesh_directory.resolve().as_posix())

    bus = root.find(".//body[@name='cubesat_bus']")
    if bus is None:
        raise ValueError("cubesat_bus was not found in the source MJCF")
    # This is a physical, body-mounted navigation camera rather than a UE
    # spectator view.  Keeping the installation transform in MJCF makes it
    # portable to every renderer and guarantees that it follows the bus.
    ET.SubElement(
        bus,
        "camera",
        {
            "name": "cubesat_docking_camera",
            "pos": "0.06 -0.08 0.19",
            # MuJoCo camera convention: forward -Z, up +Y.  This is the MJCF
            # equivalent of the previously validated bus overview transform.
            "quat": "0.38279638 0.65706406 -0.59453085 -0.26127919",
            "fovy": "60",
            "resolution": "1280 720",
        },
    )
    wheel_xml = (
        ("orbit_rw_x", "orbit_rw_x_spin", "0 0.132 0", "1 0 0", "0.7071067812 0 0.7071067812 0"),
        ("orbit_rw_y", "orbit_rw_y_spin", "-0.132 0 0", "0 1 0", "0.7071067812 -0.7071067812 0 0"),
        ("orbit_rw_z", "orbit_rw_z_spin", "0 0 -0.188", "0 0 1", "1 0 0 0"),
    )
    for name, joint_name, position, axis, quaternion in wheel_xml:
        body = ET.SubElement(bus, "body", {"name": name, "pos": position})
        ET.SubElement(
            body,
            "joint",
            {
                "name": joint_name,
                "type": "hinge",
                "axis": axis,
                "limited": "false",
                "damping": "0.000002",
            },
        )
        inertia = (
            f"{RW_SPIN_INERTIA} {RW_TRANSVERSE_INERTIA} {RW_TRANSVERSE_INERTIA}"
            if axis == "1 0 0"
            else f"{RW_TRANSVERSE_INERTIA} {RW_SPIN_INERTIA} {RW_TRANSVERSE_INERTIA}"
            if axis == "0 1 0"
            else f"{RW_TRANSVERSE_INERTIA} {RW_TRANSVERSE_INERTIA} {RW_SPIN_INERTIA}"
        )
        ET.SubElement(
            body,
            "inertial",
            {"pos": "0 0 0", "mass": str(RW_MASS_KG), "diaginertia": inertia},
        )
        # The authoritative wheel is a MuJoCo rigid body. Its transparent geom
        # supplies dimensions to MuJoCo while the renderer-neutral RW visual
        # supplies telemetry-driven colour and animation in UE.
        ET.SubElement(
            body,
            "geom",
            {
                "type": "cylinder",
                "size": "0.025 0.012",
                "quat": quaternion,
                "mass": "0",
                "contype": "0",
                "conaffinity": "0",
                "group": "2",
                "rgba": "0.2 0.5 1 0",
            },
        )

    actuators = root.find("./actuator")
    if actuators is None:
        actuators = ET.SubElement(root, "actuator")
    for definition in RW_DEFINITIONS:
        ET.SubElement(
            actuators,
            "motor",
            {
                "name": definition["actuator_name"],
                "joint": definition["joint_name"],
            },
        )
    ET.SubElement(
        actuators,
        "general",
        {
            "name": "docking_approach_thruster",
            "site": "cubesat_origin",
            "gear": "0.79852285 0.19236641 0.57040023 0 0 0",
        },
    )
    ET.SubElement(
        actuators,
        "general",
        {
            "name": "docking_braking_thruster",
            "site": "cubesat_origin",
            "gear": "-0.79852285 -0.19236641 -0.57040023 0 0 0",
        },
    )
    output.parent.mkdir(parents=True, exist_ok=True)
    ET.ElementTree(root).write(output, encoding="utf-8", xml_declaration=True)
    return output


def _specific_orbit(position: np.ndarray, velocity: np.ndarray, mu: float) -> tuple[float, float]:
    radius = float(np.linalg.norm(position))
    energy = 0.5 * float(np.dot(velocity, velocity)) - mu / radius
    return radius, -mu / (2.0 * energy)


def _contact_replay_metrics(
    mjcf_path: Path,
    state_recorder,
    start_seconds: float,
    stop_seconds: float,
) -> dict[str, float]:
    """Reconstruct contacts in a clean Python-MuJoCo subprocess.

    Basilisk and the pip MuJoCo package load incompatible MuJoCo DLLs on this
    Windows build, so the numerical replay must not import both in one process.
    """

    replay_script = Path(__file__).with_name("orbital_grasp_contact_replay.py")
    with tempfile.TemporaryDirectory(prefix="bsk-orbital-contact-") as directory:
        state_path = Path(directory) / "states.npz"
        np.savez_compressed(
            state_path,
            qpos=np.asarray(state_recorder.qpos, dtype=float),
            qvel=np.asarray(state_recorder.qvel, dtype=float),
            times_ns=np.asarray(state_recorder.times(), dtype=np.uint64),
        )
        replay_environment = os.environ.copy()
        replay_environment.pop("MUJOCO_GL", None)
        result = subprocess.run(
            [
                sys.executable,
                str(replay_script),
                "--mjcf",
                str(mjcf_path),
                "--states",
                str(state_path),
                "--start",
                str(start_seconds),
                "--stop",
                str(stop_seconds),
            ],
            check=False,
            capture_output=True,
            text=True,
            env=replay_environment,
        )
        if result.returncode != 0:
            raise RuntimeError(
                "Python MuJoCo contact replay failed: "
                + (result.stderr.strip() or result.stdout.strip())
            )
    return json.loads(result.stdout)


def run(
    model_root: Path,
    catalog: Path,
    host: str,
    port: int,
    duration: float,
    simulation_rate: float,
    generated_mjcf: Path,
    metrics_path: Path,
) -> dict[str, float | int | bool]:
    if duration <= 0.0 or simulation_rate <= 0.0:
        raise ValueError("duration and simulation_rate must be positive")
    if duration < MINIMUM_MISSION_DURATION_S:
        raise ValueError(f"duration must be at least {MINIMUM_MISSION_DURATION_S} seconds")
    if not catalog.is_file():
        raise FileNotFoundError(f"asset catalog is missing: {catalog}")

    from scenario_spacecraft_arm_grasp_unreal import load_native_grasp_module

    native = load_native_grasp_module(model_root)
    augmented_path = build_orbital_mjcf(native.MODEL_PATH, generated_mjcf, native.MESH_DIR)
    original_model_path = native.MODEL_PATH
    original_time_step = native.TIME_STEP
    original_reference_descriptor = native.JointTrajectoryPublisher.__dict__["reference"]
    extended_closed = EXTENDED_ALIGNED_ARM.copy()
    extended_closed[-1] = TIGHT_GRIP_JOINT_RAD
    extended_withdraw = RETRACTED_GRASP_ARM.copy()

    def mission_joint_reference(seconds: float) -> tuple[np.ndarray, np.ndarray]:
        local_seconds = seconds - GRASP_TIME_OFFSET_S
        if local_seconds < 1.0:
            return native.PREGRASP.copy(), np.zeros(6)
        if local_seconds < 4.0:
            return native.JointTrajectoryPublisher._segment(
                local_seconds, 1.0, 4.0, native.PREGRASP, EXTENDED_ALIGNED_ARM
            )
        if local_seconds < 4.5:
            return EXTENDED_ALIGNED_ARM.copy(), np.zeros(6)
        if local_seconds < 6.0:
            return native.JointTrajectoryPublisher._segment(
                local_seconds, 4.5, 6.0, EXTENDED_ALIGNED_ARM, extended_closed
            )
        if local_seconds < 7.0:
            return extended_closed.copy(), np.zeros(6)
        if local_seconds < 9.0:
            return native.JointTrajectoryPublisher._segment(
                local_seconds, 7.0, 9.0, extended_closed, extended_withdraw
            )
        return extended_withdraw.copy(), np.zeros(6)

    native.MODEL_PATH = augmented_path
    # Keep the authoritative MJScene dynamics, arm PID, attitude FSW, reaction
    # wheels and rendezvous controller on one straightforward 500 Hz clock.
    # Rendering remains a read-only 30 Hz decimation of this unified task.
    native.TIME_STEP = MISSION_TIME_STEP_S
    native.JointTrajectoryPublisher.reference = classmethod(
        lambda cls, seconds: mission_joint_reference(seconds)
    )

    from Basilisk.architecture import messaging, sysModel
    from Basilisk.fswAlgorithms import attTrackingError, inertial3D, mrpFeedback, rwMotorTorque
    from Basilisk.simulation import (
        arrayMotorTorqueToSingleActuators,
        saturationSingleActuator,
        scalarJointStatesToRWSpeed,
        simpleNav,
    )
    from Basilisk.utilities import macros, orbitalMotion, simIncludeGravBody
    from bsk_render_adapter import BasiliskRenderBridge, SceneSettings, VisualElement

    class RendezvousThrustController(sysModel.SysModel):
        def __init__(self) -> None:
            super().__init__()
            self.ModelTag = "orbitalClosedLoopRendezvous"
            self.busStateInMsg = messaging.SCStatesMsgReader()
            self.targetStateInMsg = messaging.SCStatesMsgReader()
            self.approachOutMsg = messaging.SingleActuatorMsg()
            self.brakingOutMsg = messaging.SingleActuatorMsg()

        def Reset(self, CurrentSimNanos: int) -> None:
            self.UpdateState(CurrentSimNanos)

        def UpdateState(self, CurrentSimNanos: int) -> None:
            seconds = CurrentSimNanos * macros.NANO2SEC
            signed_force = 0.0
            if seconds < RENDEZVOUS_CONTROL_STOP_S:
                bus_state = self.busStateInMsg()
                target_state = self.targetStateInMsg()
                travelled, reference_speed, reference_acceleration = (
                    _quintic_rendezvous_reference(seconds)
                )
                desired_relative_axis = (
                    float(np.dot(native.TARGET_POS, DOCKING_AXIS_BODY))
                    + RENDEZVOUS_INITIAL_OFFSET_M
                    - travelled
                )
                relative_position = np.asarray(target_state.r_BN_N) - np.asarray(bus_state.r_BN_N)
                relative_velocity = np.asarray(target_state.v_BN_N) - np.asarray(bus_state.v_BN_N)
                relative_axis = float(np.dot(relative_position, DOCKING_AXIS_BODY))
                relative_speed_axis = float(np.dot(relative_velocity, DOCKING_AXIS_BODY))
                reference_speed_axis = -reference_speed
                reference_acceleration_axis = -reference_acceleration
                position_error = desired_relative_axis - relative_axis
                velocity_error = reference_speed_axis - relative_speed_axis
                # Positive actuator force accelerates the bus along the camera
                # boresight, which decreases target-minus-bus range.
                signed_force = (
                    -RENDEZVOUS_ESTIMATED_MASS_KG * reference_acceleration_axis
                    -RENDEZVOUS_POSITION_GAIN_N_PER_M * position_error
                    -RENDEZVOUS_VELOCITY_GAIN_N_PER_MPS * velocity_error
                )
                signed_force = float(np.clip(
                    signed_force,
                    -RENDEZVOUS_MAX_THRUST_N,
                    RENDEZVOUS_MAX_THRUST_N,
                ))
            approach = max(0.0, signed_force)
            braking = max(0.0, -signed_force)
            self.approachOutMsg.write(
                messaging.SingleActuatorMsgPayload(input=approach), CurrentSimNanos, self.moduleID
            )
            self.brakingOutMsg.write(
                messaging.SingleActuatorMsgPayload(input=braking), CurrentSimNanos, self.moduleID
            )

    try:
        simulation, scene, native_dynamics, native_recorders = native._build_simulation()
        # Freeze the shifted schedule on this publisher instance before the
        # external scenario class is restored for other callers.
        native_dynamics[0].reference = mission_joint_reference
    finally:
        native.MODEL_PATH = original_model_path
        native.TIME_STEP = original_time_step
        native.JointTrajectoryPublisher.reference = original_reference_descriptor
    scene.extraEoMCall = True
    bus = scene.getBody("cubesat_bus")
    target = scene.getBody("capture_target")

    gravity_factory = simIncludeGravBody.gravBodyFactory()
    earth = gravity_factory.createEarth()
    earth.isCentralBody = True
    earth_state = messaging.SpicePlanetStateMsgPayload()
    earth_state.J20002Pfix = np.identity(3)
    earth_state_message = messaging.SpicePlanetStateMsg().write(earth_state)
    earth.planetBodyInMsg.subscribeTo(earth_state_message)
    gravity_model = gravity_factory.addBodiesTo(scene)

    wheel_joints = []
    wheel_actuators = []
    for definition in RW_DEFINITIONS:
        wheel_body = scene.getBody(definition["body_name"])
        wheel_joints.append(wheel_body.getScalarJoint(definition["joint_name"]))
        wheel_actuators.append(scene.getSingleActuator(definition["actuator_name"]))

    navigation = simpleNav.SimpleNav()
    navigation.ModelTag = "orbitalGraspSimpleNav"
    navigation.scStateInMsg.subscribeTo(bus.getCenterOfMass().stateOutMsg)
    simulation.AddModelToTask("graspTask", navigation, 900)

    attitude_reference = inertial3D.inertial3D()
    attitude_reference.ModelTag = "orbitalGraspInertialReference"
    attitude_reference.sigma_R0N = [0.0, 0.0, 0.0]
    simulation.AddModelToTask("graspTask", attitude_reference, 800)

    attitude_error = attTrackingError.attTrackingError()
    attitude_error.ModelTag = "orbitalGraspAttitudeError"
    attitude_error.attNavInMsg.subscribeTo(navigation.attOutMsg)
    attitude_error.attRefInMsg.subscribeTo(attitude_reference.attRefOutMsg)
    simulation.AddModelToTask("graspTask", attitude_error, 700)

    wheel_configuration = messaging.RWArrayConfigMsgPayload()
    wheel_configuration.numRW = 3
    wheel_configuration.GsMatrix_B = [1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0]
    wheel_configuration.JsList = [RW_SPIN_INERTIA] * 3
    wheel_configuration.uMax = [RW_TORQUE_LIMIT_NM] * 3
    wheel_configuration_message = messaging.RWArrayConfigMsg().write(wheel_configuration)

    wheel_speeds = scalarJointStatesToRWSpeed.ScalarJointStatesToRWSpeed()
    wheel_speeds.ModelTag = "mjsceneWheelSpeeds"
    wheel_speeds.setNumJoints(3)
    for index, joint in enumerate(wheel_joints):
        wheel_speeds.jointStateInMsgs[index].subscribeTo(joint.stateDotOutMsg)
    simulation.AddModelToTask("graspTask", wheel_speeds, 600)

    attitude_controller = mrpFeedback.mrpFeedback()
    attitude_controller.ModelTag = "orbitalGraspMrpFeedback"
    attitude_controller.K = 0.08
    attitude_controller.Ki = -1.0
    attitude_controller.P = 0.35
    attitude_controller.integralLimit = 0.0
    attitude_controller.guidInMsg.subscribeTo(attitude_error.attGuidOutMsg)
    attitude_controller.rwParamsInMsg.subscribeTo(wheel_configuration_message)
    attitude_controller.rwSpeedsInMsg.subscribeTo(wheel_speeds.rwSpeedOutMsg)
    vehicle_configuration = messaging.VehicleConfigMsgPayload(
        ISCPntB_B=[0.36, 0.0, 0.0, 0.0, 0.36, 0.0, 0.0, 0.0, 0.23]
    )
    vehicle_configuration_message = messaging.VehicleConfigMsg().write(vehicle_configuration)
    attitude_controller.vehConfigInMsg.subscribeTo(vehicle_configuration_message)
    simulation.AddModelToTask("graspTask", attitude_controller, 500)

    torque_mapping = rwMotorTorque.rwMotorTorque()
    torque_mapping.ModelTag = "orbitalGraspWheelTorqueMapping"
    torque_mapping.rwParamsInMsg.subscribeTo(wheel_configuration_message)
    torque_mapping.vehControlInMsg.subscribeTo(attitude_controller.cmdTorqueOutMsg)
    torque_mapping.controlAxes_B = [1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0]
    simulation.AddModelToTask("graspTask", torque_mapping, 400)

    torque_splitter = arrayMotorTorqueToSingleActuators.ArrayMotorTorqueToSingleActuators()
    torque_splitter.ModelTag = "orbitalGraspWheelTorqueSplitter"
    torque_splitter.setNumActuators(3)
    torque_splitter.torqueInMsg.subscribeTo(torque_mapping.rwMotorTorqueOutMsg)
    simulation.AddModelToTask("graspTask", torque_splitter, 300)

    wheel_limiters = []
    for index, actuator in enumerate(wheel_actuators):
        limiter = saturationSingleActuator.SaturationSingleActuator()
        limiter.ModelTag = f"orbitalGraspWheel{index + 1}Limiter"
        limiter.setMinInput(-RW_TORQUE_LIMIT_NM)
        limiter.setMaxInput(RW_TORQUE_LIMIT_NM)
        limiter.actuatorInMsg.subscribeTo(torque_splitter.actuatorOutMsgs[index])
        simulation.AddModelToTask("graspTask", limiter, 200 - index)
        actuator.actuatorInMsg.subscribeTo(limiter.actuatorOutMsg)
        wheel_limiters.append(limiter)

    maneuver_command = RendezvousThrustController()
    maneuver_command.busStateInMsg.subscribeTo(bus.getOrigin().stateOutMsg)
    maneuver_command.targetStateInMsg.subscribeTo(target.getOrigin().stateOutMsg)
    scene.AddModelToDynamicsTask(maneuver_command, 6500)
    scene.getSingleActuator("docking_approach_thruster").actuatorInMsg.subscribeTo(
        maneuver_command.approachOutMsg
    )
    scene.getSingleActuator("docking_braking_thruster").actuatorInMsg.subscribeTo(
        maneuver_command.brakingOutMsg
    )

    sample_time = macros.sec2nano(1.0 / 30.0)
    bus_recorder = bus.getOrigin().stateOutMsg.recorder(sample_time)
    target_recorder = target.getOrigin().stateOutMsg.recorder(sample_time)
    attitude_recorder = attitude_error.attGuidOutMsg.recorder(sample_time)
    wheel_recorders = [joint.stateDotOutMsg.recorder(sample_time) for joint in wheel_joints]
    approach_recorder = maneuver_command.approachOutMsg.recorder(sample_time)
    braking_recorder = maneuver_command.brakingOutMsg.recorder(sample_time)
    for recorder in [
        bus_recorder,
        target_recorder,
        attitude_recorder,
        approach_recorder,
        braking_recorder,
        *wheel_recorders,
    ]:
        simulation.AddModelToTask("graspTask", recorder, -100)

    bridge = BasiliskRenderBridge(
        host=host,
        port=port,
        origin_object="orbital_grasp/cubesat_bus",
        frame_period_ns=sample_time,
    )
    try:
        body_ids = bridge.add_mj_scene(
            scene,
            namespace="orbital_grasp",
            # Parse the same derived MJCF that is authoritative for MJScene so
            # body-mounted cameras and other renderer metadata are discovered
            # from XML.  The wheel helper geoms are alpha-zero; their dedicated
            # telemetry visuals remain the only visible wheel representation.
            source_path=augmented_path,
            mesh_asset_catalog=catalog,
            semantic_label="orbital_service_vehicle",
            camera_picture_in_picture=True,
            camera_capture_rate_hz=15.0,
            camera_pip_resolution=(480, 270),
            camera_picture_in_picture_start_slot=1,
            camera_display_names={
                "cubesat_docking_camera": "CubeSat Docking Camera",
                "so101_wrist_cam": "SO-101 Wrist Camera",
            },
        )
        bridge.add_celestial_bodies([earth])
        wheel_visual_definitions = []
        for index, definition in enumerate(RW_DEFINITIONS):
            wheel_visual_definitions.append(
                {
                    **definition,
                    "command_message": wheel_limiters[index].actuatorOutMsg,
                    "omega_max_rad_s": RW_SPEED_LIMIT_RAD_S,
                    "torque_max_Nm": RW_TORQUE_LIMIT_NM,
                    "diameter_m": 0.05,
                    "thickness_m": 0.024,
                }
            )
        bridge.add_mj_reaction_wheels(
            scene,
            wheel_visual_definitions,
            parent_id=body_ids["cubesat_bus"],
            prefix="orbital_grasp/reaction_wheel",
        )

        def thrust_visual_state(message):
            force = float(message.read().input)
            return {
                "visible": force > 1.0e-9,
                "value": force / RENDEZVOUS_MAX_THRUST_N,
                "channels": {
                    "throttle": force / RENDEZVOUS_MAX_THRUST_N,
                    "thrust_N": force,
                    "enabled": force > 1.0e-9,
                },
            }

        for visual_id, label, position, normal, command_message in (
            (
                "orbital_grasp/thruster/approach",
                "Closed-loop approach engine",
                tuple(-0.15 * DOCKING_AXIS_BODY),
                tuple(-DOCKING_AXIS_BODY),
                maneuver_command.approachOutMsg,
            ),
            (
                "orbital_grasp/thruster/braking",
                "Closed-loop braking engine",
                tuple(0.15 * DOCKING_AXIS_BODY),
                tuple(DOCKING_AXIS_BODY),
                maneuver_command.brakingOutMsg,
            ),
        ):
            bridge.add_thrusters(VisualElement(
                visual_id=visual_id,
                kind="thruster",
                parent_id=body_ids["cubesat_bus"],
                position_body_m=position,
                normal_body=normal,
                field_of_view_rad=(math.radians(22.0),),
                size_m=0.03,
                range_m=0.34,
                color_rgba=(0.25, 0.62, 1.0, 0.72),
                label=label,
                channel_schema={
                    "throttle": {"type": "number", "unit": "1", "minimum": 0.0, "maximum": 1.0},
                    "thrust_N": {"type": "number", "unit": "N"},
                    "enabled": {"type": "boolean"},
                },
                state_provider=lambda message=command_message: thrust_visual_state(message),
            ))
        bridge.set_scene_settings(
            SceneSettings(
                origin_object_id=body_ids["cubesat_bus"],
                default_camera_target=body_ids["so101_gripper"],
                default_camera_distance_m=1.35,
                # The close-proximity cameras sit directly on the osculating
                # path, so a full-scale orbit line crosses their foreground.
                # Keep it off in this local task view; the protocol toggle and
                # planet-scale camera can enable it without changing dynamics.
                orbit_lines=False,
                trajectory_history=True,
                interpolation_delay_ms=67.0,
                max_extrapolation_ms=67.0,
            )
        )
        mission_control = {"paused": False}

        def mission_phase(seconds: float) -> str:
            if seconds < RENDEZVOUS_START_S:
                return "initial_orbit"
            if seconds < RENDEZVOUS_STOP_S:
                return "rendezvous"
            if seconds < STATION_KEEP_STOP_S:
                return "station_keep"
            if seconds < GRASP_TIME_OFFSET_S + 4.5:
                return "arm_approach"
            if seconds < GRASP_TIME_OFFSET_S + 7.0:
                return "capture"
            if seconds < MINIMUM_MISSION_DURATION_S:
                return "retraction"
            return "complete"

        def set_paused(value: bool):
            def handler(payload, sim_time_ns):
                mission_control["paused"] = value
                return {
                    "paused": value,
                    "phase": mission_phase(sim_time_ns * macros.NANO2SEC),
                }
            return handler

        bridge.register_command_handler(
            "mission.pause",
            set_paused(True),
            label="Pause simulation",
        )
        bridge.register_command_handler(
            "mission.resume",
            set_paused(False),
            label="Resume simulation",
        )
        bridge.register_command_handler(
            "mission.status",
            lambda payload, sim_time_ns: {
                "paused": mission_control["paused"],
                "phase": mission_phase(sim_time_ns * macros.NANO2SEC),
            },
            label="Query mission status",
        )
        simulation.AddModelToTask("graspTask", bridge, -10_000)

        simulation.InitializeSimulation()
        orbit = orbitalMotion.ClassicElements()
        orbit.a = earth.radEquator + ORBIT_ALTITUDE_M
        orbit.e = 0.0
        orbit.i = 0.0
        orbit.Omega = 0.0
        orbit.omega = 0.0
        orbit.f = math.pi
        initial_position, initial_velocity = orbitalMotion.elem2rv(earth.mu, orbit)
        bus.setPosition(initial_position)
        bus.setVelocity(initial_velocity)
        bus.setAttitude([0.0, 0.0, 0.0])
        bus.setAttitudeRate([0.0, 0.0, 0.0])
        # Start with a visually meaningful separation. The target remains on
        # its local orbital trajectory while the service vehicle closes this
        # offset under relative-state feedback, brakes, then holds position.
        rendezvous_lead = DOCKING_AXIS_BODY * RENDEZVOUS_INITIAL_OFFSET_M
        target.setPosition(initial_position + native.TARGET_POS + rendezvous_lead)
        target.setVelocity(initial_velocity)
        target.setAttitude(native._quaternion_to_mrp(native.TARGET_QUAT))
        target.setAttitudeRate([0.0, 0.0, 0.0])
        for (body_name, joint_name), position in zip(native.JOINTS, native.PREGRASP, strict=True):
            joint = scene.getBody(body_name).getScalarJoint(joint_name)
            joint.setPosition(float(position))
            joint.setVelocity(0.0)
        for joint, rpm in zip(wheel_joints, (20.0, -15.0, 10.0), strict=True):
            joint.setVelocity(rpm * 2.0 * math.pi / 60.0)

        wall_start = time.monotonic()
        frame_count = int(math.ceil(duration * 30.0))
        announced_phases: set[str] = set()
        for frame in range(1, frame_count + 1):
            sim_seconds = min(frame / 30.0, duration)
            if mission_control["paused"]:
                pause_started = time.monotonic()
                while mission_control["paused"]:
                    bridge.process_commands(macros.sec2nano(max(0.0, (frame - 1) / 30.0)))
                    time.sleep(1.0 / 30.0)
                wall_start += time.monotonic() - pause_started
            deadline = wall_start + sim_seconds / simulation_rate
            time.sleep(max(0.0, deadline - time.monotonic()))
            simulation.ConfigureStopTime(macros.sec2nano(sim_seconds))
            simulation.ExecuteSimulation()
            phase = mission_phase(sim_seconds)
            if phase not in announced_phases:
                announced_phases.add(phase)
                bridge.publish_event(
                    "mission_phase",
                    {
                        "severity": "info",
                        "message": f"Mission phase: {phase}",
                        "phase": phase,
                        "sim_time_ns": str(macros.sec2nano(sim_seconds)),
                    },
                )
        wall_elapsed_seconds = time.monotonic() - wall_start

        positions = np.asarray(bus_recorder.r_BN_N, dtype=float).reshape(-1, 3)
        velocities = np.asarray(bus_recorder.v_BN_N, dtype=float).reshape(-1, 3)
        target_positions = np.asarray(target_recorder.r_BN_N, dtype=float).reshape(-1, 3)
        target_velocities = np.asarray(target_recorder.v_BN_N, dtype=float).reshape(-1, 3)
        if len(positions) < 2:
            raise RuntimeError("orbital mission did not produce enough state samples")
        initial_radius, initial_semimajor = _specific_orbit(positions[0], velocities[0], earth.mu)
        final_radius, final_semimajor = _specific_orbit(positions[-1], velocities[-1], earth.mu)
        attitude_errors = np.linalg.norm(
            np.asarray(attitude_recorder.sigma_BR, dtype=float).reshape(-1, 3), axis=1
        )
        wheel_speeds_data = np.column_stack(
            [np.asarray(recorder.state, dtype=float).reshape(-1) for recorder in wheel_recorders]
        )
        approach_samples = np.asarray(approach_recorder.input, dtype=float).reshape(-1)
        braking_samples = np.asarray(braking_recorder.input, dtype=float).reshape(-1)
        relative_positions = target_positions - positions
        sample_seconds = np.asarray(bus_recorder.times(), dtype=float) * macros.NANO2SEC
        rendezvous_index = min(
            len(sample_seconds) - 1,
            int(np.searchsorted(sample_seconds, GRASP_TIME_OFFSET_S)),
        )
        rendezvous_error = relative_positions[rendezvous_index] - native.TARGET_POS
        handoff_attitude_error = float(attitude_errors[rendezvous_index])
        maximum_pregrasp_attitude_error = float(np.max(attitude_errors[: rendezvous_index + 1]))
        rendezvous_relative_speed = float(np.linalg.norm(
            target_velocities[rendezvous_index] - velocities[rendezvous_index]
        ))
        closure_index = min(
            len(sample_seconds) - 1,
            int(np.searchsorted(sample_seconds, RENDEZVOUS_CONTROL_STOP_S)),
        )
        closure_position_error = relative_positions[closure_index] - native.TARGET_POS
        closure_relative_speed = float(np.linalg.norm(
            target_velocities[closure_index] - velocities[closure_index]
        ))
        initial_relative_position = target_positions[0] - positions[0]
        approach_translation = float(np.linalg.norm(
            relative_positions[rendezvous_index] - initial_relative_position
        ))
        withdrawal_start = min(
            len(sample_seconds) - 1,
            int(np.searchsorted(sample_seconds, GRASP_TIME_OFFSET_S + 7.0)),
        )
        withdrawal_stop = min(
            len(sample_seconds) - 1,
            int(np.searchsorted(sample_seconds, GRASP_TIME_OFFSET_S + 9.0)),
        )
        grasp_withdrawal = float(
            np.linalg.norm(relative_positions[withdrawal_stop] - relative_positions[withdrawal_start])
        )
        contact_metrics = _contact_replay_metrics(
            augmented_path,
            native_recorders[0],
            GRASP_TIME_OFFSET_S + 7.0,
            duration,
        )
        metrics: dict[str, float | int | bool] = {
            "duration_s": duration,
            "wall_elapsed_s": wall_elapsed_seconds,
            "achieved_realtime_factor": duration / wall_elapsed_seconds,
            "dynamics_rate_hz": 1.0 / MISSION_TIME_STEP_S,
            "initial_altitude_m": initial_radius - earth.radEquator,
            "final_altitude_m": final_radius - earth.radEquator,
            "initial_semimajor_axis_m": initial_semimajor,
            "final_semimajor_axis_m": final_semimajor,
            "semimajor_axis_change_m": final_semimajor - initial_semimajor,
            "final_bus_target_distance_m": float(np.linalg.norm(target_positions[-1] - positions[-1])),
            "initial_rendezvous_offset_m": RENDEZVOUS_INITIAL_OFFSET_M,
            "approach_translation_m": approach_translation,
            "rendezvous_position_error_m": float(np.linalg.norm(rendezvous_error)),
            "rendezvous_relative_speed_m_s": rendezvous_relative_speed,
            "closure_position_error_m": float(np.linalg.norm(closure_position_error)),
            "closure_relative_speed_m_s": closure_relative_speed,
            "rendezvous_error_x_m": float(rendezvous_error[0]),
            "rendezvous_error_y_m": float(rendezvous_error[1]),
            "rendezvous_error_z_m": float(rendezvous_error[2]),
            "grasp_withdrawal_m": grasp_withdrawal,
            **contact_metrics,
            "grasp_motion_detected": bool(
                grasp_withdrawal >= 0.025
                and contact_metrics["withdrawal_grasp_site_drift_m"] <= 0.003
                and contact_metrics["final_gripper_target_relative_speed_m_s"] <= 0.005
                and contact_metrics["final_gripper_target_relative_rate_rad_s"] <= 0.08
            ),
            "maximum_attitude_error_mrp": float(np.max(attitude_errors)),
            "maximum_pregrasp_attitude_error_mrp": maximum_pregrasp_attitude_error,
            "handoff_attitude_error_mrp": handoff_attitude_error,
            "final_attitude_error_mrp": float(attitude_errors[-1]),
            "maximum_wheel_speed_rad_s": float(np.max(np.abs(wheel_speeds_data))),
            "wheel_speed_within_limit": bool(
                np.max(np.abs(wheel_speeds_data)) <= RW_SPEED_LIMIT_RAD_S
            ),
            "approach_thrust_sample_count": int(np.count_nonzero(approach_samples > 1.0e-9)),
            "braking_thrust_sample_count": int(np.count_nonzero(braking_samples > 1.0e-9)),
            "orbit_safe": bool(np.min(np.linalg.norm(positions, axis=1)) - earth.radEquator > 450_000.0),
            "rendezvous_handoff_stable": bool(
                np.linalg.norm(rendezvous_error) < 0.01
                and rendezvous_relative_speed < 0.005
                and handoff_attitude_error < 0.01
            ),
        }
        metrics_path.parent.mkdir(parents=True, exist_ok=True)
        metrics_path.write_text(json.dumps(metrics, indent=2), encoding="utf-8")
        return metrics
    finally:
        bridge.close()
        _ = (
            native_dynamics,
            native_recorders,
            gravity_model,
            gravity_factory,
            earth_state_message,
            navigation,
            attitude_reference,
            attitude_error,
            wheel_speeds,
            attitude_controller,
            torque_mapping,
            torque_splitter,
            wheel_limiters,
            maneuver_command,
        )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model-root", type=Path, required=True)
    parser.add_argument("--catalog", type=Path, required=True)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=5558)
    parser.add_argument("--duration", type=float, default=34.0)
    parser.add_argument("--simulation-rate", type=float, default=1.0)
    parser.add_argument("--generated-mjcf", type=Path, required=True)
    parser.add_argument("--metrics", type=Path, required=True)
    args = parser.parse_args()
    metrics = run(
        args.model_root.resolve(),
        args.catalog.resolve(),
        args.host,
        args.port,
        args.duration,
        args.simulation_rate,
        args.generated_mjcf.resolve(),
        args.metrics.resolve(),
    )
    print("Orbital grasp mission completed")
    print(json.dumps(metrics, indent=2))


if __name__ == "__main__":
    main()
