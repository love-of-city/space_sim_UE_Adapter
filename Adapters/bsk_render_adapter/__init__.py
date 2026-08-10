"""Public API for the renderer-neutral Basilisk visualization adapter."""

__version__ = "0.2.0"

from .bridge import BasiliskRenderBridge
from .descriptors import (
    CameraVisual,
    CelestialBodyVisual,
    GeometryVisual,
    SceneSettings,
    VisualElement,
)
from .protocol import (
    PROTOCOL_V2,
    RenderPublisher,
    decode_packet,
    encode_packet,
)
from .mjcf_assets import load_asset_catalog, parse_mjcf_geometry_metadata, parse_mjcf_scene_metadata
from .recording import BskRecordingReader, BskRecordingWriter
from .ue_support import (
    enable_unreal_visualization,
    enableUnrealVisualization,
    setActuatorGuiSetting,
    setInstrumentGuiSetting,
)

__all__ = [
    "BasiliskRenderBridge",
    "BskRecordingReader",
    "BskRecordingWriter",
    "CameraVisual",
    "CelestialBodyVisual",
    "GeometryVisual",
    "PROTOCOL_V2",
    "RenderPublisher",
    "SceneSettings",
    "VisualElement",
    "__version__",
    "decode_packet",
    "enable_unreal_visualization",
    "enableUnrealVisualization",
    "encode_packet",
    "load_asset_catalog",
    "parse_mjcf_geometry_metadata",
    "parse_mjcf_scene_metadata",
    "setActuatorGuiSetting",
    "setInstrumentGuiSetting",
]
