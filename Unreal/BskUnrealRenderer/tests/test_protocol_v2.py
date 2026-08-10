import json
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

from bsk_render_adapter import (
    BasiliskRenderBridge,
    BskRecordingReader,
    BskRecordingWriter,
    VisualElement,
    enableUnrealVisualization,
)
from bsk_render_adapter.protocol import PROTOCOL_V2, decode_packet, encode_packet
from bsk_render_adapter.protocol import RenderPublisher
from bsk_render_adapter.mjcf_assets import (
    load_asset_catalog,
    parse_mjcf_geometry_metadata,
    parse_mjcf_scene_metadata,
    resolve_asset,
)
from Basilisk.architecture import messaging


class _Publisher:
    def __init__(self):
        self.hello = None
        self.manifest = None
        self.frames = []
        self.events = []

    def retain_hello(self, message):
        self.hello = message

    def retain_manifest(self, message):
        self.manifest = message

    def publish_frame(self, message):
        self.frames.append(message)

    def publish_event(self, message):
        self.events.append(message)
        return True

    def close(self):
        pass


class _Origin:
    def __init__(self, message):
        self.stateOutMsg = message


class _Body:
    def __init__(self, message):
        self._origin = _Origin(message)

    def getOrigin(self):
        return self._origin


class _Scene:
    ModelTag = "primary"

    def __init__(self, messages):
        self._messages = messages
        self._geometries = [
            SimpleNamespace(
                bodyName="hub",
                type=6,
                size=[1.0, 0.5, 0.25],
                pos=[0.0, 0.0, 0.0],
                quat=[1.0, 0.0, 0.0, 0.0],
                rgba=[1.0, 0.0, 0.0, 0.5],
            ),
            SimpleNamespace(
                bodyName="panel",
                type=5,
                size=[0.1, 1.0, 0.0],
                pos=[1.0, 0.0, 0.0],
                quat=[1.0, 0.0, 0.0, 0.0],
                rgba=[0.0, 1.0, 0.0, 1.0],
            ),
            SimpleNamespace(bodyName="hub", type=2, size=[0.5, 0.0, 0.0], pos=[0, 0, 0], quat=[1, 0, 0, 0], rgba=[1, 1, 1, 1]),
            SimpleNamespace(bodyName="hub", type=3, size=[0.2, 0.8, 0.0], pos=[0, 0, 0], quat=[1, 0, 0, 0], rgba=[1, 1, 1, 1]),
            SimpleNamespace(bodyName="hub", type=4, size=[0.5, 0.25, 0.1], pos=[0, 0, 0], quat=[1, 0, 0, 0], rgba=[1, 1, 1, 1]),
        ]

    def getBodyNames(self):
        return ["hub", "panel"]

    def getBodyParentName(self, name):
        return "world" if name == "hub" else "hub"

    def getBody(self, name):
        return _Body(self._messages[name])

    def getGeomInfos(self):
        return self._geometries


class _Simulation:
    def __init__(self):
        self.models = []

    def AddModelToTask(self, task_name, model, priority=0):
        self.models.append((task_name, model, priority))


class ProtocolV2Tests(unittest.TestCase):
    def test_ur5e_mjcf_mesh_catalog(self):
        workspace = Path(__file__).resolve().parents[3]
        mjcf = workspace / "test" / "model" / "arm" / "universal_robots_ur5e" / "scene.xml"
        metadata = parse_mjcf_geometry_metadata(mjcf)
        meshes = [item for item in metadata if item.mesh_name]
        self.assertEqual(len(metadata), 29)
        self.assertEqual(len(meshes), 20)
        self.assertEqual(meshes[0].body_name, "base")
        self.assertEqual(meshes[0].material_rgba, (0.033, 0.033, 0.033, 1.0))
        self.assertEqual(meshes[2].material_rgba, (0.49, 0.678, 0.8, 1.0))
        self.assertEqual(meshes[2].material_specular, 0.5)
        self.assertEqual(meshes[2].material_shininess, 0.25)
        self.assertLess(max(abs(value) for value in meshes[0].position_body_m), 1.0e-9)
        self.assertLess(max(abs(value) for value in meshes[0].orientation_body_from_geometry_wxyz[1:]), 1.0e-9)
        self.assertEqual(meshes[-1].body_name, "wrist_3_link")
        self.assertTrue(Path(meshes[0].source_path).is_file())
        catalog = load_asset_catalog(
            {
                "assets": {
                    meshes[0].source_path: {
                        "asset_path": "/Game/BSK/Test/base_0.base_0",
                        "component_scale": [100, 100, 100],
                    }
                }
            }
        )
        asset = resolve_asset(catalog, meshes[0].source_path)
        self.assertIsNotNone(asset)
        self.assertEqual(asset.asset_path, "/Game/BSK/Test/base_0.base_0")
        self.assertEqual(asset.component_scale, (100.0, 100.0, 100.0))

        scene = parse_mjcf_scene_metadata(mjcf, "ur5e")
        self.assertTrue(scene.headlight_enabled)
        self.assertEqual(scene.headlight_ambient_rgb, (0.1, 0.1, 0.1))
        self.assertEqual(len(scene.lights), 2)
        directional = next(light for light in scene.lights if light["properties"]["light_type"] == "directional")
        spotlight = next(light for light in scene.lights if light["properties"]["light_type"] == "spot")
        self.assertEqual(directional["normal_body"], (0.0, 0.0, -1.0))
        self.assertEqual(spotlight["properties"]["target_id"], "ur5e/wrist_2_link")

    def test_ur5e_ue_catalog_builds_metres_as_centimetres(self):
        project = Path(__file__).resolve().parents[1]
        catalog = json.loads((project / "Config" / "BskAssets" / "ur5e.json").read_text(encoding="utf-8"))
        assets = list(catalog["assets"].values())
        self.assertEqual(len(assets), 20)
        for asset in assets:
            self.assertEqual(asset["build_scale"], [100.0, 100.0, 100.0])
            self.assertEqual(asset["component_scale"], [1.0, 1.0, 1.0])

    def test_packet_round_trip_preserves_decimal_int64(self):
        message = {
            "protocol": PROTOCOL_V2,
            "type": "frame",
            "sim_time_ns": "1785760000000000000",
        }
        self.assertEqual(decode_packet(encode_packet(message)), message)

    def test_mjscene_manifest_and_dynamic_frame(self):
        messages = {"hub": messaging.SCStatesMsg(), "panel": messaging.SCStatesMsg()}
        hub = messaging.SCStatesMsgPayload()
        hub.r_BN_N = [7_000_000.0, 0.0, 0.0]
        messages["hub"].write(hub)
        panel = messaging.SCStatesMsgPayload()
        panel.r_BN_N = [7_000_002.0, 0.0, 0.0]
        messages["panel"].write(panel)

        publisher = _Publisher()
        bridge = BasiliskRenderBridge(publisher=publisher, origin_object="primary/hub")
        ids = bridge.add_mj_scene(_Scene(messages), namespace="primary")
        bridge.add_css(
            VisualElement(
                visual_id="primary/hub/css/0",
                kind="css",
                parent_id=ids["hub"],
                field_of_view_rad=[1.0],
            )
        )
        bridge.Reset(0)
        bridge.UpdateState(9_007_199_254_740_993)

        self.assertEqual(publisher.hello["protocol"], PROTOCOL_V2)
        manifest = publisher.manifest
        self.assertEqual([item["object_id"] for item in manifest["objects"]], ["primary/hub", "primary/panel"])
        self.assertEqual(manifest["objects"][1]["parent_id"], "primary/hub")
        self.assertEqual(manifest["objects"][0]["geometries"][0]["dimensions_m"], [2.0, 1.0, 0.5])
        self.assertEqual(manifest["objects"][1]["geometries"][0]["dimensions_m"], [0.2, 0.2, 2.0])
        self.assertEqual(
            [item["shape"] for item in manifest["objects"][0]["geometries"]],
            ["box", "sphere", "capsule", "ellipsoid"],
        )
        frame = publisher.frames[-1]
        self.assertEqual(frame["sim_time_ns"], "9007199254740993")
        self.assertEqual(frame["objects"][1]["position_m"], [2.0, 0.0, 0.0])

    def test_recording_round_trip(self):
        messages = [
            {"protocol": PROTOCOL_V2, "type": "hello", "session_id": "s"},
            {"protocol": PROTOCOL_V2, "type": "event", "event_kind": "scene_reset"},
        ]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "sample.bskrec"
            with BskRecordingWriter(path) as writer:
                for message in messages:
                    writer.write(message)
            self.assertEqual(list(BskRecordingReader(path).messages()), messages)

    def test_event_queue_is_bounded(self):
        publisher = RenderPublisher(port=1, reconnect_period_s=0.01, event_queue_size=1)
        try:
            self.assertTrue(publisher.publish_event({"protocol": PROTOCOL_V2, "type": "event", "event_kind": "one"}))
            self.assertFalse(publisher.publish_event({"protocol": PROTOCOL_V2, "type": "event", "event_kind": "two"}))
            self.assertEqual(publisher.stats.events_dropped, 1)
        finally:
            publisher.close()

    def test_device_auto_discovery_and_typed_channels(self):
        spacecraft_message = messaging.SCStatesMsg()
        spacecraft_message.write(messaging.SCStatesMsgPayload())

        rw_message = messaging.RWConfigLogMsg()
        rw_payload = messaging.RWConfigLogMsgPayload()
        rw_payload.theta = 1.25
        rw_payload.Omega = 42.0
        rw_payload.Omega_max = 100.0
        rw_payload.u_current = 0.2
        rw_payload.u_max = 0.5
        rw_message.write(rw_payload)
        rw_config = SimpleNamespace(
            rWB_B=[0.1, 0.2, 0.3],
            gsHat_B=[0.0, 0.0, 1.0],
            Omega_max=100.0,
            u_max=0.5,
            label="RW-A",
        )
        rw_effector = SimpleNamespace(ReactionWheelData=[rw_config], rwOutMsgs=[rw_message])

        thruster_message = messaging.THROutputMsg()
        thruster_payload = messaging.THROutputMsgPayload()
        thruster_payload.maxThrust = 10.0
        thruster_payload.thrustForce = 4.0
        thruster_message.write(thruster_payload)
        thruster_config = SimpleNamespace(
            thrLoc_B=[-0.5, 0.0, 0.0],
            thrDir_B=[1.0, 0.0, 0.0],
            MaxThrust=10.0,
            label="THR-A",
        )
        thruster_effector = SimpleNamespace(
            ModelTag="ACS",
            thrusterData=[thruster_config],
            thrusterOutMsgs=[thruster_message],
            r_PcP_P=[0.0, 0.0, 0.0],
        )

        css_message = messaging.CSSConfigLogMsg()
        css_payload = messaging.CSSConfigLogMsgPayload()
        css_payload.signal = 0.75
        css_payload.maxSignal = 1.0
        css_payload.minSignal = 0.0
        css_message.write(css_payload)
        css = SimpleNamespace(
            ModelTag="CSS-A",
            r_B=[0.0, 0.0, 0.5],
            nHat_B=[1.0, 0.0, 0.0],
            fov=1.0,
            maxOutput=1.0,
            cssConfigLogOutMsg=css_message,
        )

        publisher = _Publisher()
        bridge = BasiliskRenderBridge(publisher=publisher, origin_object="sat")
        bridge.add_object("sat", spacecraft_message)
        bridge.add_reaction_wheels(rw_effector, parent_id="sat")
        bridge.add_thrusters(thruster_effector, parent_id="sat")
        bridge.add_css([css], parent_id="sat")
        bridge.Reset(0)
        bridge.UpdateState(1_000_000_000)

        manifest_visuals = {item["kind"]: item for item in publisher.manifest["visuals"]}
        self.assertEqual(set(manifest_visuals), {"reaction_wheel", "thruster", "css"})
        self.assertEqual(manifest_visuals["reaction_wheel"]["channel_schema"]["omega_rad_s"]["unit"], "rad/s")
        self.assertEqual(manifest_visuals["thruster"]["normal_body"], [-1.0, -0.0, -0.0])

        states = {item["visual_id"]: item for item in publisher.frames[-1]["visual_states"]}
        rw_state = states["sat/reaction_wheel/0"]
        self.assertEqual(rw_state["channels"]["angle_rad"], 1.25)
        self.assertEqual(rw_state["channels"]["omega_rad_s"], 42.0)
        thruster_state = states["sat/thruster/0"]
        self.assertAlmostEqual(thruster_state["channels"]["throttle"], 0.4)
        self.assertTrue(thruster_state["channels"]["enabled"])
        css_state = states["sat/css/0"]
        self.assertAlmostEqual(css_state["channels"]["normalized_signal"], 0.75)

    def test_vizard_shaped_enable_unreal_visualization(self):
        state_message = messaging.SCStatesMsg()
        state_message.write(messaging.SCStatesMsgPayload())
        spacecraft = SimpleNamespace(
            ModelTag="vehicle",
            scStateOutMsg=state_message,
            gravField=SimpleNamespace(gravBodies=[]),
        )
        simulation = _Simulation()
        publisher = _Publisher()
        bridge = enableUnrealVisualization(
            simulation,
            "renderTask",
            spacecraft,
            publisher=publisher,
            liveStream=False,
        )
        self.assertIs(simulation.models[0][1], bridge)
        self.assertEqual(simulation.models[0][2], -100)
        self.assertEqual(bridge.origin_object, "vehicle")
        bridge.settings.orbitLinesOn = 2
        bridge.Reset(0)
        self.assertTrue(publisher.manifest["settings"]["orbit_lines"])


if __name__ == "__main__":
    unittest.main()
