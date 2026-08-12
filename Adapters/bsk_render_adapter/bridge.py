"""Basilisk ``SysModel`` that publishes renderer-neutral scene and state data."""

from __future__ import annotations

import os
import sys
import time
import uuid
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Iterable, Mapping, Sequence

import numpy as np

from .descriptors import CameraVisual, CelestialBodyVisual, GeometryVisual, SceneSettings, VisualElement
from .frames import FrameConverter, vector
from .mjcf_assets import (
    load_asset_catalog,
    load_texture_catalog,
    parse_mjcf_geometry_metadata,
    parse_mjcf_body_parents,
    parse_mjcf_scene_metadata,
    resolve_asset,
    resolve_texture,
)
from .protocol import PROTOCOL_V2, RenderPublisher
from .recording import BskRecordingWriter


_conda_dll_directory = None
if os.name == "nt" and hasattr(os, "add_dll_directory"):
    _conda_library_bin = Path(sys.prefix) / "Library" / "bin"
    if _conda_library_bin.is_dir():
        _conda_dll_directory = os.add_dll_directory(str(_conda_library_bin))

try:
    from Basilisk.architecture import messaging, sysModel
except ImportError:
    messaging = None
    sysModel = None


class _MissingBasiliskBase:
    def __init__(self, *_: Any, **__: Any) -> None:
        raise RuntimeError("Basilisk is unavailable; use the mujoco-dev Conda environment")


_BridgeBase = sysModel.SysModel if sysModel is not None else _MissingBasiliskBase


@dataclass
class _ObjectBinding:
    object_id: str
    display_name: str
    reader: Any
    parent_id: str = ""
    asset_path: str = ""
    semantic_label: str = "spacecraft"
    geometries: list[GeometryVisual] = field(default_factory=list)


@dataclass
class _CelestialBinding:
    visual: CelestialBodyVisual
    reader: Any


_MJGEOM_SPHERE = 2
_MJGEOM_CAPSULE = 3
_MJGEOM_ELLIPSOID = 4
_MJGEOM_CYLINDER = 5
_MJGEOM_BOX = 6


def _safe_id(value: str) -> str:
    result = value.strip().replace("\\", "/").strip("/")
    if not result or ".." in result.split("/"):
        raise ValueError(f"invalid stable identifier: {value!r}")
    return result


def _rgba(values: Iterable[float]) -> list[float]:
    result = vector(values, 4, "rgba")
    return np.clip(result, 0.0, 1.0).tolist()


def _mj_geometry(geom: Any, geometry_id: str) -> GeometryVisual | None:
    size = vector(geom.size, 3, "geom.size")
    geom_type = int(geom.type)
    if geom_type == _MJGEOM_BOX:
        shape = "box"
        dimensions = 2.0 * size
    elif geom_type == _MJGEOM_SPHERE:
        shape = "sphere"
        dimensions = np.repeat(2.0 * size[0], 3)
    elif geom_type == _MJGEOM_CYLINDER:
        shape = "cylinder"
        dimensions = np.array([2.0 * size[0], 2.0 * size[0], 2.0 * size[1]])
    elif geom_type == _MJGEOM_CAPSULE:
        shape = "capsule"
        dimensions = np.array([2.0 * size[0], 2.0 * size[0], 2.0 * (size[1] + size[0])])
    elif geom_type == _MJGEOM_ELLIPSOID:
        shape = "ellipsoid"
        dimensions = 2.0 * size
    else:
        return None
    return GeometryVisual(
        geometry_id=geometry_id,
        shape=shape,
        dimensions_m=dimensions.tolist(),
        position_body_m=vector(geom.pos, 3, "geom.pos").tolist(),
        orientation_body_from_geometry_wxyz=vector(geom.quat, 4, "geom.quat").tolist(),
        color_rgba=_rgba(geom.rgba),
    )


def _mj_mesh_geometry(
    geom: Any,
    geometry_id: str,
    metadata: Any,
    asset_catalog: Mapping[str, Any],
    texture_catalog: Mapping[str, str],
) -> GeometryVisual:
    asset = resolve_asset(asset_catalog, metadata.source_path)
    catalog_scale = asset.component_scale if asset else (1.0, 1.0, 1.0)
    scale = [float(metadata.mesh_scale[index]) * float(catalog_scale[index]) for index in range(3)]
    return GeometryVisual(
        geometry_id=geometry_id,
        shape="mesh",
        dimensions_m=(1.0, 1.0, 1.0),
        position_body_m=list(metadata.position_body_m),
        orientation_body_from_geometry_wxyz=list(metadata.orientation_body_from_geometry_wxyz),
        color_rgba=list(metadata.material_rgba) if metadata.material_rgba else _rgba(geom.rgba),
        asset_type=asset.asset_type if asset else "static_mesh",
        asset_path=asset.asset_path if asset else "",
        scale=scale,
        render_role=metadata.role,
        asset_key=metadata.mesh_name,
        material_name=metadata.material_name,
        material_specular=metadata.material_specular,
        material_shininess=metadata.material_shininess,
        material_reflectance=metadata.material_reflectance,
        material_emission=metadata.material_emission,
        material_texture=metadata.material_texture,
        material_texture_asset_path=resolve_texture(texture_catalog, metadata.material_texture_source),
        material_texture_repeat=metadata.material_texture_repeat,
        use_asset_materials=metadata.use_asset_materials,
    )


class BasiliskRenderBridge(_BridgeBase):
    """Publish Basilisk state without making rendering simulation-critical."""

    def __init__(
        self,
        host: str = "127.0.0.1",
        port: int = 5558,
        *,
        origin_object: str | None = None,
        c_l_n: Iterable[Iterable[float]] | None = None,
        publisher: RenderPublisher | Any | None = None,
        recording_path: str | Path | None = None,
        frame_period_ns: int | None = None,
    ) -> None:
        super().__init__()
        if frame_period_ns is not None and int(frame_period_ns) <= 0:
            raise ValueError("frame_period_ns must be positive when provided")
        self.ModelTag = "BasiliskRenderBridge"
        self.session_id = str(uuid.uuid4())
        self.origin_object = origin_object
        self.converter = FrameConverter(c_l_n)
        self.publisher = publisher or RenderPublisher(host, port)
        self.recorder = BskRecordingWriter(recording_path) if recording_path else None
        self._objects: list[_ObjectBinding] = []
        self._celestial: list[_CelestialBinding] = []
        self._visuals: list[VisualElement] = []
        self._cameras: list[CameraVisual] = []
        self._settings = SceneSettings(origin_object_id=origin_object or "")
        self._ui_settings: dict[str, list[dict[str, Any]]] = {
            "actuators": [],
            "instruments": [],
        }
        self._frame_id = 0
        self._manifest_revision = 1
        self.frame_period_ns = int(frame_period_ns) if frame_period_ns is not None else None
        self._next_frame_ns = 0

    def add_object(
        self,
        object_id: str,
        state_message: Any,
        *,
        display_name: str | None = None,
        parent_id: str = "",
        asset_path: str = "",
        semantic_label: str = "spacecraft",
        geometries: Sequence[GeometryVisual] = (),
    ) -> str:
        """Register one ``SCStates`` compatible output message."""

        if messaging is None:
            raise RuntimeError("Basilisk messaging is unavailable")
        stable_id = _safe_id(object_id)
        if any(binding.object_id == stable_id for binding in self._objects):
            raise ValueError(f"duplicate object id: {stable_id}")
        reader = messaging.SCStatesMsgReader()
        reader.subscribeTo(state_message)
        self._objects.append(
            _ObjectBinding(
                stable_id,
                display_name or stable_id.rsplit("/", 1)[-1],
                reader,
                _safe_id(parent_id) if parent_id else "",
                asset_path,
                semantic_label,
                list(geometries),
            )
        )
        self._manifest_revision += 1
        return stable_id

    def add_spacecraft(
        self,
        spacecraft: Any,
        *,
        object_id: str | None = None,
        name: str | None = None,
        asset_path: str = "",
        semantic_label: str = "spacecraft",
    ) -> str:
        """Register a regular Basilisk ``Spacecraft``."""

        stable_id = object_id or name or spacecraft.ModelTag
        return self.add_object(
            stable_id,
            spacecraft.scStateOutMsg,
            display_name=name or spacecraft.ModelTag,
            asset_path=asset_path,
            semantic_label=semantic_label,
        )

    def add_mj_scene(
        self,
        scene: Any,
        *,
        namespace: str | None = None,
        asset_map: Mapping[str, str] | None = None,
        source_path: str | Path | None = None,
        mesh_asset_catalog: str | Path | Mapping[str, Any] | None = None,
        semantic_label: str = "spacecraft_part",
    ) -> dict[str, str]:
        """Auto-register MJScene bodies, hierarchy, geometry, and state messages."""

        prefix = _safe_id(namespace or getattr(scene, "ModelTag", "mjscene"))
        body_names = list(scene.getBodyNames())
        ids = {name: f"{prefix}/{name}" for name in body_names}
        geometry_by_body: dict[str, list[GeometryVisual]] = {name: [] for name in body_names}
        geom_infos = scene.getGeomInfos()
        source_metadata = parse_mjcf_geometry_metadata(source_path) if source_path else []
        source_parents = parse_mjcf_body_parents(source_path) if source_path else {}
        catalog = load_asset_catalog(mesh_asset_catalog)
        texture_catalog = load_texture_catalog(mesh_asset_catalog)
        for index in range(len(geom_infos)):
            geom = geom_infos[index]
            body_name = str(geom.bodyName)
            if body_name not in geometry_by_body:
                continue
            metadata = source_metadata[index] if index < len(source_metadata) else None
            if metadata is not None and metadata.body_name == body_name and metadata.mesh_name:
                geometry = _mj_mesh_geometry(
                    geom,
                    f"{ids[body_name]}/geom/{index}",
                    metadata,
                    catalog,
                    texture_catalog,
                )
            else:
                geometry = _mj_geometry(geom, f"{ids[body_name]}/geom/{index}")
                if geometry is not None and metadata is not None:
                    geometry.render_role = metadata.role
                    geometry.material_name = metadata.material_name
                    geometry.material_specular = metadata.material_specular
                    geometry.material_shininess = metadata.material_shininess
                    geometry.material_reflectance = metadata.material_reflectance
                    geometry.material_emission = metadata.material_emission
                    geometry.material_texture = metadata.material_texture
                    geometry.material_texture_repeat = metadata.material_texture_repeat
                    if metadata.material_rgba:
                        geometry.color_rgba = list(metadata.material_rgba)
            if geometry is not None:
                geometry_by_body[body_name].append(geometry)
        assets = dict(asset_map or {})
        for body_name in body_names:
            # Basilisk's SWIG getBodyParentName can access-violate for bodies
            # attached through an MjSpec <frame>.  Prefer the renderer metadata
            # source when available and keep the API fallback for source-less
            # scenes and existing integrations.
            parent_name = source_parents.get(body_name)
            if parent_name is None:
                parent_name = str(scene.getBodyParentName(body_name))
            self.add_object(
                ids[body_name],
                scene.getBody(body_name).getOrigin().stateOutMsg,
                display_name=body_name,
                parent_id="" if parent_name == "world" else ids[parent_name],
                asset_path=assets.get(body_name, ""),
                semantic_label=semantic_label,
                geometries=geometry_by_body[body_name],
            )
        if source_path:
            scene_metadata = parse_mjcf_scene_metadata(source_path, prefix)
            if scene_metadata.headlight_enabled or scene_metadata.lights:
                self._settings.use_scene_lighting = True
            if scene_metadata.headlight_enabled:
                self._settings.headlight_enabled = True
                self._settings.headlight_diffuse_rgb = scene_metadata.headlight_diffuse_rgb
                self._settings.headlight_ambient_rgb = scene_metadata.headlight_ambient_rgb
                self._settings.headlight_specular_rgb = scene_metadata.headlight_specular_rgb
            for light in scene_metadata.lights:
                self.add_visual(
                    VisualElement(
                        visual_id=light["visual_id"],
                        kind="light",
                        parent_id=light["parent_id"],
                        position_body_m=light["position_body_m"],
                        normal_body=light["normal_body"],
                        color_rgba=light["color_rgba"],
                        properties=light["properties"],
                    )
                )
        return ids

    def add_celestial_bodies(self, bodies: Iterable[Any]) -> None:
        """Register Basilisk gravity bodies and their linked ephemeris readers."""

        for body in bodies:
            display_name = str(getattr(body, "displayName", "") or getattr(body, "planetName", "body"))
            body_id = _safe_id(display_name.casefold())
            if any(binding.visual.body_id == body_id for binding in self._celestial):
                continue
            visual = CelestialBodyVisual(
                body_id=body_id,
                display_name=display_name,
                mu_m3_s2=float(body.mu),
                equatorial_radius_m=float(body.radEquator),
                polar_radius_ratio=float(getattr(body, "radiusRatio", 1.0)),
                asset_path=str(getattr(body, "modelDictionaryKey", "")),
                luminous=display_name.casefold() == "sun",
            )
            self._celestial.append(_CelestialBinding(visual, body.planetBodyInMsg))
        self._manifest_revision += 1

    def add_visual(self, visual: VisualElement) -> str:
        """Register a renderer-neutral visual element."""

        visual.visual_id = _safe_id(visual.visual_id)
        visual.parent_id = _safe_id(visual.parent_id) if visual.parent_id else ""
        if any(item.visual_id == visual.visual_id for item in self._visuals):
            raise ValueError(f"duplicate visual id: {visual.visual_id}")
        self._visuals.append(visual)
        self._manifest_revision += 1
        return visual.visual_id

    def _add_visual_kind(self, kind: str, visuals: VisualElement | Iterable[VisualElement]) -> None:
        items = [visuals] if isinstance(visuals, VisualElement) else list(visuals)
        for item in items:
            item.kind = kind
            self.add_visual(item)

    def add_reaction_wheels(
        self,
        visuals_or_effector: Any,
        *,
        parent_id: str | None = None,
        prefix: str | None = None,
    ) -> None:
        """Register explicit visuals or auto-discover a Basilisk RW effector."""

        if isinstance(visuals_or_effector, VisualElement) or (
            isinstance(visuals_or_effector, (list, tuple))
            and all(isinstance(item, VisualElement) for item in visuals_or_effector)
        ):
            self._add_visual_kind("reaction_wheel", visuals_or_effector)
            return
        if not parent_id:
            raise ValueError("parent_id is required when auto-discovering reaction wheels")
        from .device_adapters import reaction_wheel_visuals

        self._add_visual_kind(
            "reaction_wheel",
            reaction_wheel_visuals(visuals_or_effector, _safe_id(parent_id), prefix),
        )

    def add_thrusters(
        self,
        visuals_or_effectors: Any,
        *,
        parent_id: str | None = None,
        colors: Any = None,
        prefix: str | None = None,
    ) -> None:
        """Register explicit visuals or auto-discover thruster effectors."""

        if isinstance(visuals_or_effectors, VisualElement) or (
            isinstance(visuals_or_effectors, (list, tuple))
            and all(isinstance(item, VisualElement) for item in visuals_or_effectors)
        ):
            self._add_visual_kind("thruster", visuals_or_effectors)
            return
        if not parent_id:
            raise ValueError("parent_id is required when auto-discovering thrusters")
        from .device_adapters import thruster_visuals

        self._add_visual_kind(
            "thruster",
            thruster_visuals(
                visuals_or_effectors,
                _safe_id(parent_id),
                colors=colors,
                prefix=prefix,
            ),
        )

    def add_css(
        self,
        visuals_or_devices: Any,
        *,
        parent_id: str | None = None,
        prefix: str | None = None,
    ) -> None:
        """Register explicit visuals or auto-discover coarse sun sensors."""

        if isinstance(visuals_or_devices, VisualElement) or (
            isinstance(visuals_or_devices, (list, tuple))
            and all(isinstance(item, VisualElement) for item in visuals_or_devices)
        ):
            self._add_visual_kind("css", visuals_or_devices)
            return
        if not parent_id:
            raise ValueError("parent_id is required when auto-discovering CSS devices")
        from .device_adapters import css_visuals

        self._add_visual_kind(
            "css",
            css_visuals(visuals_or_devices, _safe_id(parent_id), prefix),
        )

    def add_generic_sensors(self, visuals: VisualElement | Iterable[VisualElement]) -> None:
        self._add_visual_kind("generic_sensor", visuals)

    def add_transceivers(self, visuals: VisualElement | Iterable[VisualElement]) -> None:
        self._add_visual_kind("transceiver", visuals)

    def add_lights(self, visuals: VisualElement | Iterable[VisualElement]) -> None:
        self._add_visual_kind("light", visuals)

    def add_storage(self, visuals: VisualElement | Iterable[VisualElement]) -> None:
        self._add_visual_kind("storage", visuals)

    def add_camera(self, camera: CameraVisual) -> str:
        """Register one camera descriptor."""

        camera.camera_id = _safe_id(camera.camera_id)
        if camera.parent_id:
            camera.parent_id = _safe_id(camera.parent_id)
        self._cameras.append(camera)
        self._manifest_revision += 1
        return camera.camera_id

    def set_scene_settings(self, settings: SceneSettings | Mapping[str, Any]) -> None:
        """Replace renderer hints without changing dynamics."""

        self._settings = settings if isinstance(settings, SceneSettings) else SceneSettings(**settings)
        self._manifest_revision += 1

    def add_ui_setting(self, category: str, setting: Mapping[str, Any]) -> None:
        """Store renderer UI hints; unknown consumers may safely ignore them."""

        if category not in self._ui_settings:
            raise ValueError(f"unknown UI setting category: {category}")
        self._ui_settings[category].append(dict(setting))
        self._manifest_revision += 1

    def _hello_message(self) -> dict[str, Any]:
        return {
            "protocol": PROTOCOL_V2,
            "type": "hello",
            "session_id": self.session_id,
            "capabilities": [
                "scene_manifest",
                "events",
                "recording",
                "multi_camera",
                "typed_visual_channels",
            ],
            "required_capabilities": ["scene_manifest"],
            "coordinates": {
                "length_unit": "m",
                "handedness": "right",
                "position_frame": "L",
                "quaternion_order": "wxyz",
                "rotation_semantics": "active_parent_from_child",
            },
        }

    def _manifest_message(self) -> dict[str, Any]:
        return {
            "protocol": PROTOCOL_V2,
            "type": "scene_manifest",
            "session_id": self.session_id,
            "revision": str(self._manifest_revision),
            "objects": [
                {
                    "object_id": item.object_id,
                    "display_name": item.display_name,
                    "parent_id": item.parent_id,
                    "transform_space": "world",
                    "asset_path": item.asset_path,
                    "semantic_label": item.semantic_label,
                    "geometries": [geometry.to_payload() for geometry in item.geometries],
                }
                for item in self._objects
            ],
            "celestial_bodies": [item.visual.to_payload() for item in self._celestial],
            "visuals": [item.to_payload() for item in self._visuals],
            "cameras": [item.to_payload() for item in self._cameras],
            "settings": {
                **self._settings.to_payload(),
                "ui": {key: list(value) for key, value in self._ui_settings.items()},
            },
        }

    def _record_and_retain_static(self) -> None:
        hello = self._hello_message()
        manifest = self._manifest_message()
        self.publisher.retain_hello(hello)
        self.publisher.retain_manifest(manifest)
        if self.recorder:
            self.recorder.write(hello)
            self.recorder.write(manifest)

    def Reset(self, CurrentSimNanos: int) -> None:
        """Start a new stream session and publish retained scene data."""

        self._frame_id = 0
        self._next_frame_ns = int(CurrentSimNanos)
        self._record_and_retain_static()

    def _origin(self, raw_objects: list[dict[str, Any]]) -> np.ndarray:
        if not raw_objects:
            return np.zeros(3)
        if not self.origin_object:
            return np.asarray(raw_objects[0]["position_N_m"], dtype=np.float64)
        candidates = [self.origin_object]
        candidates.extend(item.object_id for item in self._objects if item.display_name == self.origin_object)
        for item in raw_objects:
            if item["object_id"] in candidates:
                return np.asarray(item["position_N_m"], dtype=np.float64)
        raise ValueError(f"origin object is not registered: {self.origin_object}")

    def UpdateState(self, CurrentSimNanos: int) -> None:
        """Sample registered messages and enqueue the newest render frame."""

        current_sim_ns = int(CurrentSimNanos)
        if self.frame_period_ns is not None and current_sim_ns < self._next_frame_ns:
            return

        raw_objects: list[dict[str, Any]] = []
        for binding in self._objects:
            state = binding.reader()
            raw_objects.append(
                {
                    "object_id": binding.object_id,
                    "position_N_m": list(state.r_BN_N),
                    "velocity_N_mps": list(state.v_BN_N),
                    "sigma_BN": list(state.sigma_BN),
                    "angular_velocity_B_radps": list(state.omega_BN_B),
                }
            )
        origin_n = self._origin(raw_objects)
        objects = [
            {
                "object_id": item["object_id"],
                "position_m": self.converter.position(item["position_N_m"], origin_n).tolist(),
                "orientation_wxyz": self.converter.body_orientation(item["sigma_BN"]).tolist(),
                "velocity_mps": self.converter.velocity(item["velocity_N_mps"]).tolist(),
                "angular_velocity_B_radps": vector(item["angular_velocity_B_radps"], 3, "omega_BN_B").tolist(),
            }
            for item in raw_objects
        ]
        celestial = []
        for binding in self._celestial:
            state = binding.reader()
            celestial.append(
                {
                    "body_id": binding.visual.body_id,
                    "position_m": self.converter.position(state.PositionVector, origin_n).tolist(),
                    "velocity_mps": self.converter.velocity(state.VelocityVector).tolist(),
                    "orientation_wxyz": self.converter.fixed_orientation(
                        np.asarray(state.J20002Pfix, dtype=np.float64).reshape((3, 3))
                    ).tolist(),
                }
            )
        visual_states = []
        for visual in self._visuals:
            if visual.state_provider is not None:
                if visual.properties.get("state_provider_accepts_sim_time", False):
                    state = dict(visual.state_provider(int(CurrentSimNanos)))
                else:
                    state = dict(visual.state_provider())
                channels = state.get("channels")
                if channels is None:
                    channels = {
                        key: value
                        for key, value in state.items()
                        if key not in {"visible", "value", "status"}
                    }
                visual_states.append(
                    {
                        "visual_id": visual.visual_id,
                        **state,
                        "channels": dict(channels),
                    }
                )
        message = {
            "protocol": PROTOCOL_V2,
            "type": "frame",
            "session_id": self.session_id,
            "manifest_revision": str(self._manifest_revision),
            "frame_id": str(self._frame_id),
            "sim_time_ns": str(int(CurrentSimNanos)),
            "wall_time_ns": str(time.time_ns()),
            "origin_N_m": origin_n.tolist(),
            "c_LN": self.converter.c_l_n.reshape(9).tolist(),
            "objects": objects,
            "celestial_bodies": celestial,
            "visual_states": visual_states,
        }
        self.publisher.publish_frame(message)
        if self.recorder:
            self.recorder.write(message)
        self._frame_id += 1
        if self.frame_period_ns is not None:
            elapsed = max(0, current_sim_ns - self._next_frame_ns)
            periods = elapsed // self.frame_period_ns + 1
            self._next_frame_ns += periods * self.frame_period_ns

    def publish_event(self, event_kind: str, payload: Mapping[str, Any] | None = None) -> bool:
        """Publish one bounded reliable event."""

        message = {
            "protocol": PROTOCOL_V2,
            "type": "event",
            "session_id": self.session_id,
            "sequence": str(self._frame_id),
            "event_kind": event_kind,
            "payload": dict(payload or {}),
        }
        if self.recorder:
            self.recorder.write(message)
        return self.publisher.publish_event(message)

    def close(self) -> None:
        """Close network and recording resources."""

        self.publisher.close()
        if self.recorder:
            self.recorder.close()
