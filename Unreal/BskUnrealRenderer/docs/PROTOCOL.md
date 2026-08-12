# BSK render wire protocol

The transport is a four-byte unsigned payload length in network byte order,
followed by exactly that many UTF-8 bytes containing one JSON object. Packets
larger than 32 MiB and zero-length packets are rejected.

UE accepts the legacy `bsk-isaac-render/1` and `bsk-render/1` frame formats.
New integrations use `bsk-render/2`.

## Version 2 session

The sender opens a TCP connection to the UE listener and sends:

1. `hello` with the session ID, coordinate contract, and capabilities;
2. the retained `scene_manifest`;
3. `frame` state messages and optional bounded `event` messages.

The retained hello and manifest are resent after every reconnect. Frames use a
one-slot latest-frame queue on both sides and never back-pressure Basilisk.
Events use a bounded queue; overflow requests a manifest resend rather than
allowing unbounded memory growth.

All physical values use SI units. Wire coordinates are right-handed local
render-frame components. Positions are relative to `origin_N_m`. Quaternions
are active parent-from-child rotations in `(w,x,y,z)` order. Object state
transforms are world transforms even when a semantic parent is present;
geometry, visual, and camera installation transforms are body-local.

JSON cannot exactly represent every 64-bit integer through a double-based
parser, so frame IDs, revisions, sequences, and nanosecond timestamps are sent
as decimal strings.

## Message examples

```json
{
  "protocol": "bsk-render/2",
  "type": "hello",
  "session_id": "b95e...",
  "capabilities": ["scene_manifest", "events", "recording", "multi_camera", "typed_visual_channels", "bidirectional_commands"],
  "required_capabilities": ["scene_manifest"],
  "coordinates": {
    "length_unit": "m",
    "handedness": "right",
    "position_frame": "L",
    "quaternion_order": "wxyz",
    "rotation_semantics": "active_parent_from_child"
  }
}
```

Mesh geometries use the same body-local transform and add packaged-asset metadata:

```json
{
  "geometry_id": "ur5e/base/geom/0",
  "shape": "mesh",
  "dimensions_m": [1.0, 1.0, 1.0],
  "position_body_m": [0.0, 0.0, 0.0],
  "orientation_body_from_geometry_wxyz": [1.0, 0.0, 0.0, 0.0],
  "asset_type": "static_mesh",
  "asset_path": "/Game/BSK/Generated/UR5e/base_0.base_0",
  "scale": [100.0, 100.0, 100.0],
  "render_role": "visual",
  "asset_key": "base_0",
  "material_name": "black",
  "material_specular": 0.5,
  "material_shininess": 0.25,
  "material_reflectance": 0.0,
  "material_emission": 0.0,
  "material_texture": "",
  "material_texture_asset_path": "",
  "use_asset_materials": false,
  "material_texture_repeat": [1.0, 1.0]
}
```

`scale` is dimensionless and renderer-asset-specific. `render_role=collision`
geometries are hidden when a visual mesh loaded successfully and become the
fallback when it did not. Runtime consumers must not interpret `asset_key` as
an external file to load. Material fields preserve MJCF semantics. UE maps the
numeric properties onto a packaged Default Lit material; a texture name is
metadata until a renderer-specific packaged texture mapping is supplied.

MJCF lighting is represented by `kind=light` visual definitions. Its
`properties` object supports `light_type`, `target_id`, `diffuse_rgb`,
`specular_rgb`, `intensity`, `cutoff_deg`, and `cast_shadows`. World lights may
use an empty `parent_id`; body-mounted lights retain the normal parent rule.
Scene settings additionally carry `use_scene_lighting`, `headlight_enabled`,
and the headlight diffuse/ambient/specular RGB triples.

```json
{
  "protocol": "bsk-render/2",
  "type": "scene_manifest",
  "session_id": "b95e...",
  "revision": "4",
  "objects": [{
    "object_id": "primary/panel_p1",
    "display_name": "panel_p1",
    "parent_id": "primary/hub",
    "transform_space": "world",
    "semantic_label": "spacecraft_part",
    "geometries": [{
      "geometry_id": "primary/panel_p1/geom/1",
      "shape": "box",
      "dimensions_m": [2.0, 0.1, 2.0],
      "position_body_m": [0.0, 0.0, 0.0],
      "orientation_body_from_geometry_wxyz": [1.0, 0.0, 0.0, 0.0],
      "color_rgba": [0.0, 1.0, 0.0, 1.0]
    }]
  }],
  "celestial_bodies": [],
  "visuals": [{
    "visual_id": "primary/reaction_wheel/0",
    "kind": "reaction_wheel",
    "parent_id": "primary",
    "position_body_m": [0.0, 0.0, 0.0],
    "normal_body": [0.0, 0.0, 1.0],
    "channel_schema": {
      "angle_rad": {"type": "number", "unit": "rad"},
      "omega_rad_s": {"type": "number", "unit": "rad/s"},
      "saturated": {"type": "boolean"}
    }
  }],
  "cameras": [{
    "camera_id": "primary/camera/wrist",
    "display_name": "Wrist Camera",
    "parent_id": "primary/wrist",
    "position_body_m": [0.0, 0.055, -0.045],
    "orientation_body_from_camera_wxyz": [0.3392525, -0.6204095, 0.6204095, 0.3392525],
    "field_of_view_rad": 1.34948,
    "resolution": [480, 270],
    "semantic_label": "mjcf_camera",
    "picture_in_picture": true,
    "capture_rate_hz": 15.0,
    "picture_in_picture_slot": 2,
    "capture_products": ["rgb", "depth", "segmentation"]
  }],
  "settings": {"origin_object_id": "primary/hub"}
}
```

Camera installation transforms are parent-body local. The renderer-neutral
camera basis is `+X` forward, `+Y` left, and `+Z` up. `picture_in_picture` and
the related rate/slot fields are optional renderer hints; omitting them keeps
the camera registered without adding an on-screen view. UE clamps capture
resolution and rate to bounded runtime limits.

`capture_products` is strict: the only version-2 values are `rgb`, `depth`,
and `segmentation`; an unknown value rejects the manifest with a clear error.
The sender requests products, while the UE host retains authority over local
disk paths and capture-network destinations.

`settings.orbit_lines` and `settings.trajectory_history` are independent
renderer hints. UE currently implements the osculating orbit-line switch; the
trajectory-history value remains reserved for the persistent history provider.

Native MJScene reaction wheels use the ordinary `reaction_wheel` visual kind.
Their `angle_rad`, `omega_rad_s`, and `torque_Nm` channels are sampled directly
from the MuJoCo hinge joint and final motor command. The render protocol does
not integrate or command a second wheel model.

```json
{
  "protocol": "bsk-render/2",
  "type": "frame",
  "session_id": "b95e...",
  "manifest_revision": "4",
  "frame_id": "42",
  "sim_time_ns": "1400000000",
  "wall_time_ns": "1785760000000000000",
  "origin_N_m": [7000000.0, 0.0, 0.0],
  "c_LN": [1,0,0,0,1,0,0,0,1],
  "objects": [{
    "object_id": "primary/panel_p1",
    "position_m": [0.0, 1.0, 0.0],
    "orientation_wxyz": [0.9238795, 0.0, 0.0, 0.3826834],
    "velocity_mps": [0.0, 0.0, 0.0],
    "angular_velocity_B_radps": [0.0, 0.0, 0.05]
  }],
  "celestial_bodies": [],
  "visual_states": [{
    "visual_id": "primary/reaction_wheel/0",
    "visible": true,
    "channels": {
      "angle_rad": 1.2,
      "omega_rad_s": 42.0,
      "saturated": false
    }
  }]
}
```

Visual channel definitions are declared once in `channel_schema`. Channel
values may be JSON numbers, booleans, or strings. Current standard device
channels include absolute RW angle and speed, thruster force and throttle, CSS
signal and validity, and generic device enable state. Receivers ignore unknown
channels, allowing device adapters to evolve without changing the transport.

Unknown optional fields are ignored. A hello containing an unsupported entry
in `required_capabilities` is rejected with a protocol error. A frame whose
session or manifest revision does not match the active manifest is ignored.

## Events, mission UI, and commands

The manifest may declare allow-listed mission controls under
`settings.ui.commands`:

```json
{
  "command": "mission.pause",
  "label": "Pause simulation",
  "target_id": "",
  "payload": {},
  "requires_confirmation": false
}
```

UE never invents a BSK actuator command from an unknown module. It only renders
buttons declared by the active manifest and sends a `command` on the existing
TCP session:

```json
{
  "protocol": "bsk-render/2",
  "type": "command",
  "session_id": "b95e...",
  "command_id": "ue-12",
  "command": "mission.pause",
  "target_id": "",
  "requested_sim_time_ns": "22000000000",
  "payload": {}
}
```

The UE outbound queue is bounded to 64 commands and rejects new commands when
full or disconnected. The Python publisher receives commands on its network
thread, places them in another bounded queue, and the bridge invokes only
explicitly registered handlers on the Basilisk simulation thread. Unknown
commands, session mismatches, invalid payloads, handler exceptions, and queue
overflow produce explicit `command_result` error events. UE marks commands
pending and emits a visible timeout after 10 seconds without a result.

Event payloads should provide `severity`, `message`, and `sim_time_ns`. UE keeps
the latest 128 events for the runtime timeline. `requires_confirmation` causes
the built-in HUD to require a second click within four seconds; it is a UI
safety affordance and does not replace authorization in the BSK handler.

## Recordings and extension points

`.bskrec` starts with ASCII `BSKREC2\n`, followed by the same length-prefixed
packets. Replay therefore exercises the same parser and scene application path
as live TCP.

`IBskMessageSource` permits future UDP and binary transports. The built-in
`IBskCaptureProvider` produces RGB PNG, camera-Z depth as little-endian float32
PFM in metres, and 24-bit instance segmentation PNG. Instance ID zero is
background; metadata maps every nonzero ID to `object_id` and
`semantic_label`.

Camera products use a separate `bsk-capture/1` connection so image traffic can
never back-pressure BSK state input. Each packet is:

1. big-endian uint32 payload length;
2. big-endian uint32 metadata JSON length;
3. UTF-8 metadata JSON;
4. concatenated binary product blobs.

The metadata includes decimal-string capture sequence, source frame ID, BSK
simulation time, sender wall time, and UE capture wall time. It also contains a
pinhole calibration (`fx`, `fy`, `cx`, `cy`, horizontal/vertical FOV), camera
pose in the local render frame (`position_L_m`, `q_LC_wxyz`, `c_LC`), camera
pose in the inertial frame (`position_N_m`, `c_NC`), and the exact floating
origin (`origin_N_m`, `c_LN`) used for the frame. Camera axes remain `+X`
forward, `+Y` left, `+Z` up. Product blob offsets are relative to the blob-area
start and all byte lengths are decimal strings.

Network and disk output retain only the newest complete packet per camera while
their worker is behind. Disk writes run on a dedicated background thread. Render-target
readback is currently synchronous and should be configured at a modest rate;
an asynchronous GPU-readback provider can replace it through the existing
provider registry without changing the wire contract.
