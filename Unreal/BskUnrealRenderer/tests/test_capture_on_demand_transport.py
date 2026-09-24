"""Tagged strict frames survive preview replacement and capture stop."""
import json
import socket
from unittest.mock import patch

import pytest

from bsk_render_adapter.protocol import RenderPublisher, HEADER
from bsk_render_adapter import BasiliskRenderBridge


def unpack(packet): return json.loads(packet[4:])


@pytest.mark.parametrize("legacy_reliable", [False, True])
def test_explicit_idle_overrides_legacy_capture_and_stop_preserves_strict(legacy_reliable):
    publisher = RenderPublisher(reliable_frames=legacy_reliable)
    with patch.object(publisher, "start"):
        for i in range(8): publisher.publish_frame(dict(frame_id=str(i), capture_episode_id=""))
        assert publisher._strict_frames.empty()
        assert publisher._preview_frames.qsize() == 1
        for i in (8, 9): publisher.publish_frame(dict(frame_id=str(i), capture_episode_id="episode-A"))
        assert publisher._preview_frames.empty()
        for i in range(10, 1000): publisher.publish_frame(dict(frame_id=str(i), capture_episode_id=""))
        assert [unpack(publisher._strict_frames.get_nowait())["frame_id"] for _ in range(2)] == ["8", "9"]
        assert unpack(publisher._preview_frames.get_nowait())["frame_id"] == "999"
        assert publisher.stats.frames_queued - publisher.stats.frames_dropped == 3


def test_worker_drains_strict_frames_before_latest_post_stop_preview():
    listener = socket.socket()
    listener.bind(("127.0.0.1", 0)); listener.listen(); listener.settimeout(3)
    publisher = RenderPublisher(port=listener.getsockname()[1])
    connection = None
    try:
        with patch.object(publisher, "start"):
            publisher.publish_frame(dict(frame_id="0", capture_episode_id=""))
            for i in (1, 2): publisher.publish_frame(dict(frame_id=str(i), capture_episode_id="episode-A"))
            for i in (3, 4): publisher.publish_frame(dict(frame_id=str(i), capture_episode_id=""))
        publisher.start()
        connection, _ = listener.accept(); connection.settimeout(3)
        def read_exact(n):
            body = b""
            while len(body) < n:
                chunk = connection.recv(n - len(body))
                assert chunk
                body += chunk
            return body
        received = []
        for _ in range(3):
            size, = HEADER.unpack(read_exact(4))
            received.append(json.loads(read_exact(size))["frame_id"])
        assert received == ["1", "2", "4"]
    finally:
        publisher.close()
        if connection: connection.close()
        listener.close()


class Publisher:
    def retain_hello(self, message): pass
    def retain_manifest(self, message): pass
    def publish_frame(self, message): self.frame = message
    def publish_event(self, message): return True
    def close(self): pass


def test_bridge_freezes_capture_mode_once_per_frame_for_observation_pairing():
    state = {"capture_episode_id": "", "capture_request_id": "idle"}
    calls = []
    def capture_state():
        calls.append(1)
        return dict(state)
    publisher = Publisher()
    bridge = BasiliskRenderBridge(publisher=publisher, capture_state_provider=capture_state)
    try:
        bridge.Reset(0)
        bridge.UpdateState(0)
        assert publisher.frame["capture_episode_id"] == ""
        state.update(capture_episode_id="episode-A", capture_request_id="start")
        # Network receipt alone cannot change the last observation snapshot.
        assert bridge.last_capture_state["capture_episode_id"] == ""
        bridge.UpdateState(1)
        assert publisher.frame["capture_episode_id"] == "episode-A"
        assert bridge.last_capture_state == state
        state.update(capture_episode_id="", capture_request_id="stop")
        assert bridge.last_capture_state["capture_episode_id"] == "episode-A"
        bridge.UpdateState(2)
        assert publisher.frame["capture_episode_id"] == ""
        assert bridge.last_capture_state["capture_request_id"] == "stop"
        assert len(calls) == 3
    finally: bridge.close()


def test_capture_hook_cannot_override_authoritative_payload():
    publisher = Publisher()
    bridge = BasiliskRenderBridge(publisher=publisher, capture_state_provider=lambda: {
        "capture_episode_id": "", "frame_id": "999", "sim_time_ns": "999", "objects": ["bad"]})
    try:
        bridge.Reset(0)
        bridge.UpdateState(0)
        assert publisher.frame["frame_id"] == publisher.frame["sim_time_ns"] == "0"
        assert publisher.frame["objects"] == []
        assert bridge.last_capture_state == {"capture_episode_id": "", "capture_request_id": ""}
    finally: bridge.close()
