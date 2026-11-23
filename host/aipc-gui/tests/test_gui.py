"""The main window against a real scipd, end to end on localhost."""

from __future__ import annotations

import pytest

from aipc_gui import media
from aipc_gui.main_window import MainWindow, parse_target
from aipc_gui.protocol import AipcClient
from aipc_gui.widgets.io import FROM_PC, PATTERN

from .conftest import free_udp_port


@pytest.fixture
def window(qtbot, scipd):
    window = MainWindow()
    qtbot.addWidget(window)
    window.connect_to(scipd.host, scipd.port)
    qtbot.waitUntil(lambda: bool(window.pipeline.functions and window.target_info), timeout=5000)
    yield window
    window.close()


def test_parse_target():
    assert parse_target("10.0.0.2") == ("10.0.0.2", 5600)
    assert parse_target("10.0.0.2:5700 (board)") == ("10.0.0.2", 5700)


def test_builds_and_edits_the_pipeline(qtbot, window, scipd):
    assert "pytest-target" in window.target_label.text()
    window.pipeline.add_function("gray")
    window.pipeline.add_function("blur")
    qtbot.waitUntil(lambda: window.pipeline.list.count() == 2, timeout=3000)

    # The new stage is selected and its parameters shown.
    assert window.pipeline.editor.fields.keys() == {"ksize", "sigma"}
    window.pipeline.editor.fields["ksize"].spin.setValue(7)
    with AipcClient(scipd.host, scipd.port) as other:
        qtbot.waitUntil(lambda: "ksize=7" in other.call("pipeline.get")["text"], timeout=3000)
        # A change by another client shows up here (event.config).
        other.call("pipeline.clear")
    qtbot.waitUntil(lambda: window.pipeline.list.count() == 0, timeout=3000)


def test_runs_a_pattern_and_shows_stats(qtbot, window):
    window.inputs.kind.setCurrentIndex(PATTERN)
    window.inputs.pattern_size.setCurrentText("64x48")
    window.inputs.apply()
    qtbot.waitUntil(lambda: window.last_stats.get("state") == "running", timeout=5000)
    assert "64x48" in window.stats.labels["input"].text()


@pytest.mark.skipif(not media.available(), reason="no GStreamer for Python")
def test_streams_a_file_to_the_target_and_shows_the_result(qtbot, window, clip):
    to_target, back = free_udp_port(), free_udp_port()
    window.pipeline.text.setText("gray ! invert")
    window.pipeline.text.returnPressed.emit()

    window.inputs.kind.setCurrentIndex(FROM_PC)
    window.inputs.pc_port.setValue(to_target)
    window.inputs.apply()
    window.outputs.to_pc_port.setValue(back)
    window.outputs.to_pc.setChecked(True)
    window.outputs.apply()

    assert window.player.open(str(clip))
    window.player.loop.setChecked(True)
    window.player.toggle()  # play
    qtbot.waitUntil(lambda: window.result_view.image is not None, timeout=8000)
    assert window.sent_view.image is not None
    # Statistics arrive every 500 ms, so they may lag the first frames.
    qtbot.waitUntil(lambda: window.last_stats.get("state") == "running", timeout=3000)
    assert window.last_stats["output_info"]["format"] == "gray8"
    assert window.receiver.port == back


@pytest.mark.skipif(not media.has_element("gdppay"), reason="needs gdppay")
def test_streams_raw_frames_to_the_target(qtbot, window, clip):
    to_target, back = free_udp_port(), free_udp_port()
    window.inputs.kind.setCurrentIndex(FROM_PC)
    window.inputs.pc_port.setValue(to_target)
    window.inputs.transport.setCurrentIndex(1)  # raw, lossless
    assert "Mbit/s" in window.inputs.stream_estimate.text()
    window.inputs.apply()
    qtbot.waitUntil(lambda: window.inputs.current_uri == f"tcp://:{to_target}", timeout=3000)
    window.outputs.to_pc_port.setValue(back)
    window.outputs.to_pc.setChecked(True)
    window.outputs.apply()

    assert window.player.open(str(clip))
    assert "raw I420" in window.player.route_label.text()
    window.player.toggle()  # play
    qtbot.waitUntil(lambda: window.result_view.image is not None, timeout=8000)
    qtbot.waitUntil(lambda: window.last_stats.get("state") == "running", timeout=3000)
    assert window.last_stats["input"]["info"]["width"] == 320

    # Back to H.264 while playing: the target's input follows by itself.
    window.inputs.transport.setCurrentIndex(0)
    qtbot.waitUntil(lambda: window.inputs.current_uri == f"rtp://:{to_target}", timeout=3000)
    qtbot.waitUntil(lambda: window.last_stats.get("state") == "running", timeout=5000)


@pytest.mark.skipif(not media.available(), reason="no GStreamer for Python")
def test_controls_a_file_the_target_plays(qtbot, window, clip):
    """Here the target is scipd on this PC, so the clip is a file "on the target"."""
    from aipc_gui.widgets.files import TargetFileDialog

    dialog = TargetFileDialog(window.client)
    qtbot.addWidget(dialog)
    dialog.show_folder(str(clip.parent))
    qtbot.waitUntil(lambda: dialog.list.count() > 0, timeout=3000)
    assert dialog.list.item(0).text() == clip.name

    window.outputs.to_pc_port.setValue(free_udp_port())
    window.outputs.input_port.setValue(free_udp_port())
    window.outputs.to_pc.setChecked(True)
    window.outputs.apply()
    window.inputs.open_target_file(str(clip))
    qtbot.waitUntil(lambda: window.player.on_target, timeout=3000)
    assert window.player.title.text() == "Video on the target"
    remote = window.player.remote
    qtbot.waitUntil(lambda: remote.info is not None and remote.info.width == 320, timeout=3000)
    assert remote.state == "paused"  # a new video starts paused
    assert remote.duration() == pytest.approx(3.0, abs=0.1)

    window.player.toggle()  # play, on the target
    qtbot.waitUntil(lambda: window.last_stats.get("state") == "running", timeout=3000)
    # Both views: the target's input on the left, its result on the right.
    qtbot.waitUntil(
        lambda: window.sent_view.image is not None and window.result_view.image is not None,
        timeout=8000,
    )
    assert window.sent_view.title == "Input of the target"

    window.player.toggle()  # pause
    qtbot.waitUntil(lambda: window.last_stats.get("state") == "paused", timeout=3000)
    window.player.slider.jumped.emit(2000)  # click at 2 s
    qtbot.waitUntil(
        lambda: abs((window.last_stats.get("playback") or {}).get("position", 0) - 2.0) < 0.1,
        timeout=3000,
    )
    assert remote.position() == pytest.approx(2.0, abs=0.1)

    # Back to streaming from this PC: the player follows.
    window.inputs.kind.setCurrentIndex(FROM_PC)
    window.inputs.apply()
    qtbot.waitUntil(lambda: not window.player.on_target, timeout=3000)
