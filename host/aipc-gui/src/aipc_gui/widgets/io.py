"""Where the target's video comes from and where its result goes."""

from __future__ import annotations

from typing import Any

from PySide6.QtCore import Signal
from PySide6.QtWidgets import (
    QCheckBox,
    QComboBox,
    QFormLayout,
    QGroupBox,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QListWidget,
    QPushButton,
    QSpinBox,
    QStackedWidget,
    QVBoxLayout,
    QWidget,
)

from ..protocol import VIDEO_FROM_TARGET_PORT, VIDEO_TO_TARGET_PORT
from ..qt_client import QtAipcClient

# Input kinds, in the order of the combo box.
FROM_PC, CAMERA, TARGET_FILE, NETWORK, PATTERN = range(5)
KIND_LABELS = [
    "Video file streamed from this PC",
    "Camera on the target",
    "File or URL on the target",
    "RTP stream from another sender",
    "Test pattern",
]


def port_box(value: int) -> QSpinBox:
    box = QSpinBox(minimum=1, maximum=65535, value=value)
    box.setMinimumWidth(90)
    return box


def describe_uri(uri: str) -> str:
    return uri or "(none)"


class InputPanel(QWidget):
    """Chooses the target's input; `streaming_from_pc` tells the player to send."""

    streaming_from_pc = Signal(bool, int)  # on, port

    def __init__(self, client: QtAipcClient, parent: QWidget | None = None):
        super().__init__(parent)
        self.client = client
        self.cameras: list[dict[str, Any]] = []
        layout = QVBoxLayout(self)

        self.kind = QComboBox()
        self.kind.addItems(KIND_LABELS)
        self.kind.currentIndexChanged.connect(lambda i: self.pages.setCurrentIndex(i))
        layout.addWidget(self.kind)

        self.pages = QStackedWidget()
        layout.addWidget(self.pages)

        # From this PC: the player sends to rtp://:PORT on the target.
        page = QWidget()
        form = QFormLayout(page)
        self.pc_port = port_box(VIDEO_TO_TARGET_PORT)
        form.addRow("RTP port on the target", self.pc_port)
        form.addRow(QLabel("Open a file in the player and press play."))
        self.pages.addWidget(page)

        # Camera on the target.
        page = QWidget()
        form = QFormLayout(page)
        self.camera = QComboBox()
        self.camera.currentIndexChanged.connect(self._fill_modes)
        self.mode = QComboBox()
        refresh = QPushButton("Refresh")
        refresh.clicked.connect(self.refresh_devices)
        row = QHBoxLayout()
        row.addWidget(self.camera, 1)
        row.addWidget(refresh)
        form.addRow("Camera", row)
        form.addRow("Mode", self.mode)
        self.pages.addWidget(page)

        # File or URL on the target.
        page = QWidget()
        form = QFormLayout(page)
        self.target_uri = QLineEdit(placeholderText="/home/root/videos/clip.mp4 or rtsp://...")
        self.realtime = QCheckBox("At its frame rate (else as fast as possible)", checked=True)
        self.target_loop = QCheckBox("Loop")
        form.addRow("Path / URL", self.target_uri)
        form.addRow(self.realtime)
        form.addRow(self.target_loop)
        self.pages.addWidget(page)

        # RTP from another sender (a camera, ffmpeg, another target).
        page = QWidget()
        form = QFormLayout(page)
        self.network_port = port_box(VIDEO_TO_TARGET_PORT)
        form.addRow("RTP port on the target", self.network_port)
        self.pages.addWidget(page)

        # Pattern.
        page = QWidget()
        form = QFormLayout(page)
        self.pattern_size = QComboBox(editable=True)
        self.pattern_size.addItems(["640x480", "1280x720", "1920x1080", "3840x2160"])
        self.pattern_size.setCurrentText("1280x720")
        self.pattern_fps = QSpinBox(minimum=1, maximum=1000, value=30)
        form.addRow("Size", self.pattern_size)
        form.addRow("Frames per second", self.pattern_fps)
        self.pages.addWidget(page)

        self.gray = QCheckBox("Decode to gray (cheaper when the pipeline starts with gray)")
        layout.addWidget(self.gray)
        buttons = QHBoxLayout()
        apply = QPushButton("Use this input")
        apply.clicked.connect(self.apply)
        stop = QPushButton("No input")
        stop.clicked.connect(lambda: self._set({"uri": ""}))
        buttons.addWidget(apply)
        buttons.addWidget(stop)
        layout.addLayout(buttons)
        self.current = QLabel()
        self.current.setWordWrap(True)
        layout.addWidget(self.current)
        layout.addStretch()

    # --- from the target ----------------------------------------------------

    def refresh_devices(self) -> None:
        self.client.call("input.devices", on_result=self.set_devices)

    def set_devices(self, devices: dict[str, Any]) -> None:
        self.cameras = devices.get("cameras", [])
        self.camera.blockSignals(True)
        self.camera.clear()
        for camera in self.cameras:
            self.camera.addItem(f"{camera['name']} ({camera['path']})")
        self.camera.blockSignals(False)
        self._fill_modes()

    def set_input(self, current: dict[str, Any]) -> None:
        """Show the target's input (also when another GUI changed it)."""
        uri = current.get("uri", "")
        self.current.setText(f"Current input: <b>{describe_uri(uri)}</b>")
        self.gray.setChecked(bool(current.get("gray")))
        port = self._rtp_port(uri)
        if port is not None:
            if self.kind.currentIndex() != NETWORK:
                self.kind.setCurrentIndex(FROM_PC)
            (self.pc_port if self.kind.currentIndex() == FROM_PC else self.network_port).setValue(
                port
            )
        elif uri.startswith("camera:"):
            self.kind.setCurrentIndex(CAMERA)
            index = self.mode.findData(uri)
            if index >= 0:
                self.mode.setCurrentIndex(index)
        elif uri.startswith("pattern:"):
            self.kind.setCurrentIndex(PATTERN)
            self.pattern_size.setCurrentText(uri.split(":")[1])
            if current.get("max_fps"):
                self.pattern_fps.setValue(int(current["max_fps"]))
        elif uri:
            self.kind.setCurrentIndex(TARGET_FILE)
            self.target_uri.setText(uri)
            self.realtime.setChecked(bool(current.get("realtime")))
            self.target_loop.setChecked(bool(current.get("loop")))
        from_pc = port is not None and self.kind.currentIndex() == FROM_PC
        self.streaming_from_pc.emit(from_pc, port or 0)

    # --- user -----------------------------------------------------------------

    def apply(self) -> None:
        kind = self.kind.currentIndex()
        params: dict[str, Any] = {"gray": self.gray.isChecked()}
        if kind in (FROM_PC, NETWORK):
            port = self.pc_port if kind == FROM_PC else self.network_port
            params["uri"] = f"rtp://:{port.value()}"
        elif kind == CAMERA:
            mode = self.mode.currentData()
            if not mode:
                self.current.setText("No camera on the target.")
                return
            params["uri"] = mode
        elif kind == TARGET_FILE:
            params.update(
                uri=self.target_uri.text().strip(),
                realtime=self.realtime.isChecked(),
                loop=self.target_loop.isChecked(),
            )
        else:
            params.update(
                uri=f"pattern:{self.pattern_size.currentText().strip()}",
                max_fps=self.pattern_fps.value(),
            )
        self._set(params)

    def _set(self, params: dict[str, Any]) -> None:
        self.client.call(
            "input.set",
            params,
            on_result=self.set_input,
            on_error=lambda e: self.current.setText(
                f"<span style='color:#c62828'>{e.message}</span>"
            ),
        )

    def _fill_modes(self) -> None:
        self.mode.clear()
        index = self.camera.currentIndex()
        if not 0 <= index < len(self.cameras):
            return
        camera = self.cameras[index]
        best = 0  # preselect the largest mode that still runs at 25 fps or more
        for i, mode in enumerate(camera.get("modes", [])):
            self.mode.addItem(
                f"{mode['width']}x{mode['height']} @ {mode['fps']:g} fps", mode["uri"]
            )
            if mode["fps"] >= 25 and not best:
                best = i
        self.mode.addItem("camera default (largest)", camera["uri"])
        self.mode.setCurrentIndex(best)

    @staticmethod
    def _rtp_port(uri: str) -> int | None:
        if uri.startswith("rtp://") and ":" in uri[6:]:
            try:
                return int(uri.rsplit(":", 1)[1])
            except ValueError:
                return None
        return None


class OutputPanel(QWidget):
    """The target's outputs. `preview_port` is 0 when the result is not sent here."""

    preview_port = Signal(int)

    def __init__(self, client: QtAipcClient, parent: QWidget | None = None):
        super().__init__(parent)
        self.client = client
        self.outputs: list[str] = []
        layout = QVBoxLayout(self)

        box = QGroupBox("Outputs of the target")
        form = QFormLayout(box)
        self.to_pc = QCheckBox("Send the result to this PC")
        self.to_pc.setChecked(True)
        self.to_pc_port = port_box(VIDEO_FROM_TARGET_PORT)
        row = QHBoxLayout()
        row.addWidget(self.to_pc)
        row.addWidget(QLabel("port"))
        row.addWidget(self.to_pc_port)
        form.addRow(row)

        self.display = QCheckBox("Show on the target's display")
        self.display_choice = QComboBox()
        row = QHBoxLayout()
        row.addWidget(self.display)
        row.addWidget(self.display_choice, 1)
        form.addRow(row)

        self.record = QCheckBox("Record on the target")
        self.record_path = QLineEdit("/tmp/scip-record.mp4")
        row = QHBoxLayout()
        row.addWidget(self.record)
        row.addWidget(self.record_path, 1)
        form.addRow(row)

        self.extra = QLineEdit(placeholderText="more outputs, comma separated (udp:HOST:PORT, ...)")
        form.addRow("Other", self.extra)
        layout.addWidget(box)

        apply = QPushButton("Apply outputs")
        apply.clicked.connect(self.apply)
        layout.addWidget(apply)

        self.status = QListWidget()
        layout.addWidget(QLabel("Status"))
        layout.addWidget(self.status, 1)

    # --- from the target ----------------------------------------------------

    def set_displays(self, displays: dict[str, Any]) -> None:
        current = self.display_choice.currentData()
        self.display_choice.clear()
        if displays.get("window_system"):
            self.display_choice.addItem("a window", "display")
        for d in displays.get("displays", []):
            state = f"{d['width']}x{d['height']}" if d["connected"] else "not connected"
            self.display_choice.addItem(f"{d['name']} ({state})", d["uri"])
        if self.display_choice.count() == 0:
            self.display_choice.addItem("default", "display")
        if current:
            index = self.display_choice.findData(current)
            if index >= 0:
                self.display_choice.setCurrentIndex(index)

    def set_outputs(self, result: dict[str, Any]) -> None:
        """Reflect the target's outputs in the check boxes."""
        self.outputs = result.get("outputs", [])
        here = self._to_pc_uri()
        self.to_pc.setChecked(here in self.outputs)
        displays = [o for o in self.outputs if o == "display" or o.startswith("display:")]
        self.display.setChecked(bool(displays))
        if displays:
            index = self.display_choice.findData(displays[0])
            if index >= 0:
                self.display_choice.setCurrentIndex(index)
        recordings = [o for o in self.outputs if o.endswith((".mp4", ".mkv"))]
        self.record.setChecked(bool(recordings))
        if recordings:
            self.record_path.setText(recordings[0])
        known = {here, *displays, *recordings}
        self.extra.setText(", ".join(o for o in self.outputs if o not in known))
        self.preview_port.emit(self.to_pc_port.value() if here in self.outputs else 0)

    def update_status(self, stats: dict[str, Any]) -> None:
        self.status.clear()
        for output in stats.get("outputs", []):
            if output["error"]:
                text = f"⚠ {output['uri']}: {output['error']}"
            elif output["active"]:
                text = f"✓ {output['description'] or output['uri']}"
            else:
                text = f"… {output['uri']} (opens with the first frame)"
            self.status.addItem(text)

    # --- user -----------------------------------------------------------------

    def apply(self) -> None:
        outputs = []
        if self.to_pc.isChecked():
            outputs.append(self._to_pc_uri())
        if self.display.isChecked():
            outputs.append(self.display_choice.currentData() or "display")
        if self.record.isChecked() and self.record_path.text().strip():
            outputs.append(self.record_path.text().strip())
        outputs += [o.strip() for o in self.extra.text().split(",") if o.strip()]
        self.client.call(
            "output.set",
            {"outputs": outputs},
            on_result=self.set_outputs,
            on_error=lambda e: (self.status.clear(), self.status.addItem(f"⚠ {e.message}")),
        )

    def _to_pc_uri(self) -> str:
        return f"udp:{self.client.local_address}:{self.to_pc_port.value()}"
