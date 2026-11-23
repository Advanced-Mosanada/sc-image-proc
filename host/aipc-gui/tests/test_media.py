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
