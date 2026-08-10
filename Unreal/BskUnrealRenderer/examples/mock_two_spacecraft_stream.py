"""Send a smooth two-spacecraft rendezvous without requiring Basilisk."""

from __future__ import annotations

import argparse
import math
import time
import uuid

from bsk_render_adapter import PROTOCOL_V2, RenderPublisher


def hello_message(session_id: str) -> dict:
    return {
        "protocol": PROTOCOL_V2,
        "type": "hello",
        "session_id": session_id,
        "capabilities": ["scene_manifest"],
        "required_capabilities": ["scene_manifest"],
        "coordinates": {
            "length_unit": "m",
            "handedness": "right",
            "position_frame": "L",
            "quaternion_order": "wxyz",
            "rotation_semantics": "active_parent_from_child",
        },
    }


def manifest_message(session_id: str) -> dict:
    def object_definition(object_id: str, color: list[float]) -> dict:
        return {
            "object_id": object_id,
            "display_name": object_id,
            "parent_id": "",
            "transform_space": "world",
            "asset_path": "",
            "semantic_label": object_id,
            "geometries": [
                {
                    "geometry_id": f"{object_id}/placeholder",
                    "shape": "box",
                    "dimensions_m": [1.5, 1.0, 0.8],
                    "position_body_m": [0.0, 0.0, 0.0],
                    "orientation_body_from_geometry_wxyz": [1.0, 0.0, 0.0, 0.0],
                    "color_rgba": color,
                    "scale": [1.0, 1.0, 1.0],
                    "render_role": "visual",
                }
            ],
        }

    return {
        "protocol": PROTOCOL_V2,
        "type": "scene_manifest",
        "session_id": session_id,
        "revision": "1",
        "objects": [
            object_definition("chaser", [0.15, 0.55, 1.0, 1.0]),
            object_definition("target", [1.0, 0.35, 0.1, 1.0]),
        ],
        "celestial_bodies": [],
        "visuals": [],
        "cameras": [],
        "settings": {
            "origin_object_id": "chaser",
            "skybox": "black",
            "default_camera_target": "chaser",
            "default_camera_distance_m": 25.0,
            "interpolation_delay_ms": 100.0,
            "max_extrapolation_ms": 100.0,
        },
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=5558)
    parser.add_argument("--rate", type=float, default=30.0)
    parser.add_argument("--duration", type=float, default=60.0)
    args = parser.parse_args()
    if args.rate <= 0.0:
        parser.error("--rate must be positive")

    session_id = str(uuid.uuid4())
    publisher = RenderPublisher(args.host, args.port)
    publisher.retain_hello(hello_message(session_id))
    publisher.retain_manifest(manifest_message(session_id))
    publisher.start()
    started = time.monotonic()
    deadline = started
    frame_id = 0
    try:
        while time.monotonic() - started < args.duration:
            sim_time = time.monotonic() - started
            angle = 0.35 * sim_time
            range_m = max(2.5, 12.0 - 0.12 * sim_time)
            objects = [
                {
                    "object_id": "chaser",
                    "position_m": [0.0, 0.0, 0.0],
                    "orientation_wxyz": [1.0, 0.0, 0.0, 0.0],
                    "velocity_mps": [0.0, 0.0, 0.0],
                    "angular_velocity_B_radps": [0.0, 0.0, 0.0],
                },
                {
                    "object_id": "target",
                    "position_m": [0.8 * math.sin(0.2 * sim_time), range_m, 0.5 * math.cos(0.2 * sim_time)],
                    "orientation_wxyz": [math.cos(angle / 2.0), 0.0, 0.0, math.sin(angle / 2.0)],
                    "velocity_mps": [0.16 * math.cos(0.2 * sim_time), -0.12, -0.1 * math.sin(0.2 * sim_time)],
                    "angular_velocity_B_radps": [0.0, 0.0, 0.35],
                },
            ]
            publisher.publish_frame(
                {
                    "protocol": PROTOCOL_V2,
                    "type": "frame",
                    "session_id": session_id,
                    "manifest_revision": "1",
                    "frame_id": str(frame_id),
                    "sim_time_ns": str(int(sim_time * 1e9)),
                    "wall_time_ns": str(time.time_ns()),
                    "origin_N_m": [7_000_000.0, 0.0, 0.0],
                    "c_LN": [1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0],
                    "objects": objects,
                    "celestial_bodies": [],
                    "visual_states": [],
                }
            )
            frame_id += 1
            deadline += 1.0 / args.rate
            time.sleep(max(0.0, deadline - time.monotonic()))
    except KeyboardInterrupt:
        pass
    finally:
        publisher.close()
    print(f"queued={publisher.stats.queued} sent={publisher.stats.sent} dropped={publisher.stats.dropped}")


if __name__ == "__main__":
    main()
