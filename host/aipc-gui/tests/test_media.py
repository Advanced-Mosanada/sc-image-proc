"""Streamer and RtpReceiver, on localhost with a generated clip."""

from __future__ import annotations

import time

import pytest

from aipc_gui import media

from .conftest import free_udp_port, run_for

pytestmark = pytest.mark.skipif(not media.available(), reason="no GStreamer for Python")


def test_opens_a_file_paused(clip):
    streamer = media.Streamer()
    info = streamer.open(clip)
    assert (info.width, info.height) == (320, 240)
    assert info.fps == pytest.approx(30.0)
    assert info.duration == pytest.approx(3.0, abs=0.1)
    assert streamer.state == "paused"
    streamer.close()


def test_streams_to_a_receiver(clip):
    port = free_udp_port()
    sent, received = [], []
    receiver = media.RtpReceiver(port, on_frame=received.append)
    receiver.start()
    streamer = media.Streamer(on_frame=sent.append)
    streamer.set_destination("127.0.0.1", port)
    streamer.open(clip)
    streamer.play()
    run_for(1.5, streamer, receiver)
    streamer.close()
    receiver.stop()
    assert len(sent) > 20
    assert len(received) > 20
    assert (received[-1].width, received[-1].height) == (320, 240)
    assert receiver.error == ""


def test_seek_rate_and_end(clip):
    streamer = media.Streamer()
    streamer.open(clip)
    streamer.seek(2.0)
    assert streamer.position() == pytest.approx(2.0, abs=0.05)

    streamer.seek(0.0)
    streamer.set_rate(2.0)
    streamer.play()
    run_for(0.75, streamer)
    assert streamer.position() > 1.2  # ~1.5 s of video in 0.75 s

    run_for(1.5, streamer)
    assert streamer.state == "ended"
    streamer.play()  # from the start again
    run_for(0.2, streamer)
    assert streamer.state == "playing"
    assert streamer.position() < 1.5
    streamer.close()


def test_ab_loop_stays_in_range(clip):
    streamer = media.Streamer()
    streamer.open(clip)
    streamer.set_ab(1.0, 1.5)
    streamer.play()
    positions = []
    deadline = time.monotonic() + 2.0  # four times around
    while time.monotonic() < deadline:
        streamer.poll()
        positions.append(streamer.position())
        time.sleep(0.02)
    assert streamer.state == "playing"
    assert min(positions[5:]) >= 0.95
    assert max(positions) <= 1.55
    with pytest.raises(ValueError):
        streamer.set_ab(2.0, 1.0)
    streamer.set_ab(None, None)
    streamer.close()


def test_whole_file_loop_does_not_end(clip):
    streamer = media.Streamer()
    streamer.open(clip)
    streamer.set_loop(True)
    streamer.seek(2.5)
    streamer.play()
    run_for(1.0, streamer)
    assert streamer.state == "playing"
    assert streamer.position() < 1.0  # wrapped around
    streamer.close()


class GdpReceiver:
    """What the target's tcp:// input does, in Python: raw frames from GDP over TCP."""

    def __init__(self, port: int):
        self.frames: list[tuple[str, int, int]] = []  # format, width, height
        self.pipeline = media.Gst.parse_launch(
            f"tcpserversrc host=127.0.0.1 port={port} ! gdpdepay "
            "! appsink name=sink max-buffers=4 drop=true sync=false"
        )
        self.pipeline.set_state(media.Gst.State.PLAYING)

    def poll(self) -> None:
        sink = self.pipeline.get_by_name("sink")
        while (sample := sink.emit("try-pull-sample", 0)) is not None:
            s = sample.get_caps().get_structure(0)
            self.frames.append((s.get_value("format"), s.get_value("width"), s.get_value("height")))

    def stop(self) -> None:
        self.pipeline.set_state(media.Gst.State.NULL)


@pytest.mark.skipif(not media.has_element("gdppay"), reason="needs gdppay")
def test_sends_raw_frames_over_tcp(clip):
    port = free_udp_port()  # a free number; TCP and UDP ports are separate
    receiver = GdpReceiver(port)
    settings = media.StreamSettings(raw=True)
    assert settings.uri(5000) == "tcp://:5000"
    assert settings.megabits(720, 576, 25) == pytest.approx(124.4, abs=0.1)  # PAL I420
    streamer = media.Streamer(settings=settings)
    streamer.set_destination("127.0.0.1", port)
    streamer.open(clip)
    streamer.play()
    run_for(1.0, streamer, receiver)
    streamer.close()
    receiver.stop()
    assert len(receiver.frames) > 20
    assert receiver.frames[-1] == ("I420", 320, 240)  # every pixel, as decoded


def test_new_settings_keep_the_position(clip):
    port = free_udp_port()
    received = []
    receiver = media.RtpReceiver(port, on_frame=received.append)
    receiver.start()
    streamer = media.Streamer()
    streamer.set_destination("127.0.0.1", port)
    streamer.open(clip)
    streamer.seek(1.0)
    streamer.play()
    run_for(0.3, streamer, receiver)
    quality = media.StreamSettings(quality=20, preset="veryfast", keyframe_interval=10)
    assert quality.megabits(320, 240, 30) is None  # depends on the content
    assert "quality 20" in quality.describe()
    streamer.configure(quality)
    assert streamer.state == "playing"
    assert streamer.position() == pytest.approx(1.3, abs=0.25)
    received.clear()
    run_for(0.7, streamer, receiver)
    streamer.close()
    receiver.stop()
    assert len(received) > 10  # the new encoder's stream decodes
