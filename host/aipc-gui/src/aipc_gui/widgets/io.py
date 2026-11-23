"""Where the target's video comes from and where its result goes."""

from __future__ import annotations

from typing import Any

from PySide6.QtCore import QRect, QSize, Qt, Signal
from PySide6.QtGui import QColor, QFont, QPainter
from PySide6.QtWidgets import (
    QCheckBox,
    QComboBox,
    QFormLayout,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QListWidget,
    QListWidgetItem,
    QPushButton,
    QScrollArea,
    QSpinBox,
    QStyle,
    QStyledItemDelegate,
    QVBoxLayout,
    QWidget,
)

from .. import theme
from ..icons import icon, pixmap
from ..protocol import VIDEO_FROM_TARGET_PORT, VIDEO_TO_TARGET_PORT
from ..qt_client import QtAipcClient
from .common import Card, tool_button

# Input kinds, in the order of the list.
FROM_PC, CAMERA, TARGET_FILE, NETWORK, PATTERN = range(5)
KINDS = [
    ("film", "Video from this PC", "Streamed by the player below the videos"),
    ("camera", "Camera on the target", "A V4L2 camera and its mode"),
    ("file", "File or URL on the target", "A video on the target, or rtsp://, http://"),
    ("network", "RTP stream", "H.264 sent to the target by another sender"),
    ("pattern", "Test pattern", "Generated on the target"),
]
KIND_LABELS = [title for _, title, _ in KINDS]
CHOICE_HEIGHT = 46


def describe_uri(uri: str) -> str:
    return uri or "(none)"


def port_box(value: int) -> QSpinBox:
    box = QSpinBox(minimum=1, maximum=65535, value=value)
    box.setMinimumWidth(90)
    return box


class ChoiceDelegate(QStyledItemDelegate):
    """Icon, title and a muted subtitle per row."""

    def sizeHint(self, option, index) -> QSize:
        return QSize(200, CHOICE_HEIGHT)

    def paint(self, painter: QPainter, option, index) -> None:
        t = theme.current()
        painter.save()
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        rect = option.rect.adjusted(2, 2, -2, -2)
        selected = bool(option.state & QStyle.StateFlag.State_Selected)
        hovered = bool(option.state & QStyle.StateFlag.State_MouseOver)
        if selected or hovered:
            fill = QColor(t.accent if selected else t.hover)
            if selected:
                fill.setAlpha(50 if t.dark else 35)
            painter.setPen(QColor(t.accent) if selected else Qt.PenStyle.NoPen)
            painter.setBrush(fill)
            painter.drawRoundedRect(rect, 8, 8)
        name, title, subtitle = index.data(Qt.ItemDataRole.UserRole)
        painter.drawPixmap(
            rect.left() + 10,
            rect.center().y() - 10,
            pixmap(name, t.accent if selected else t.muted, 20),
        )
        left = rect.left() + 42
        font = QFont(option.font)
        font.setWeight(QFont.Weight.DemiBold)
        painter.setFont(font)
        painter.setPen(QColor(t.text))
        painter.drawText(
            QRect(left, rect.top() + 4, rect.width() - 50, 20), Qt.AlignmentFlag.AlignVCenter, title
        )
        font.setWeight(QFont.Weight.Normal)
        font.setPointSizeF(font.pointSizeF() - 1)
        painter.setFont(font)
        painter.setPen(QColor(t.muted))
        painter.drawText(
            QRect(left, rect.top() + 22, rect.width() - 50, 18),
            Qt.AlignmentFlag.AlignVCenter,
            subtitle,
        )
        painter.restore()


class ChoiceList(QListWidget):
    """A list of choices with the API of a combo box (currentIndex...)."""

    currentIndexChanged = Signal(int)

    def __init__(self, choices: list[tuple[str, str, str]]):
        super().__init__()
        self.setItemDelegate(ChoiceDelegate(self))
        self.setMouseTracking(True)
        self.setVerticalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        for choice in choices:
            item = QListWidgetItem()
            item.setData(Qt.ItemDataRole.UserRole, choice)
            self.addItem(item)
        self.setFixedHeight(len(choices) * CHOICE_HEIGHT + 12)
        self.currentRowChanged.connect(self.currentIndexChanged.emit)
        self.setCurrentRow(0)

    def currentIndex(self) -> int:
        return self.currentRow()

    def setCurrentIndex(self, index: int) -> None:
        self.setCurrentRow(index)


class Pages(QWidget):
    """Shows one page at a time and is as tall as that page.

    (A QStackedWidget keeps room for its tallest page.)
    """

    def __init__(self):
        super().__init__()
        self.layout_ = QVBoxLayout(self)
        self.layout_.setContentsMargins(0, 0, 0, 0)
        self.pages: list[QWidget] = []

    def add_form(self) -> QFormLayout:
        page = QWidget()
        form = QFormLayout(page)
        form.setContentsMargins(0, 0, 0, 0)
        form.setVerticalSpacing(8)
        page.setVisible(not self.pages)
        self.layout_.addWidget(page)
        self.pages.append(page)
        return form

    def setCurrentIndex(self, index: int) -> None:
        for i, page in enumerate(self.pages):
            page.setVisible(i == index)


class InputPanel(QScrollArea):
    """Chooses the target's input; `streaming_from_pc` tells the player to send."""

    streaming_from_pc = Signal(bool, int)  # on, port

    def __init__(self, client: QtAipcClient, parent: QWidget | None = None):
        super().__init__(parent)
        self.setWidgetResizable(True)
        self.client = client
        self.cameras: list[dict[str, Any]] = []
        content = QWidget()
        layout = QVBoxLayout(content)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(10)
        self.setWidget(content)

        source = Card("Source", "Where the target's frames come from")
        self.kind = ChoiceList(KINDS)
        source.body.addWidget(self.kind)
        layout.addWidget(source)

        self.settings_card = Card("Settings")
        self.pages = Pages()
        self.settings_card.body.addWidget(self.pages)
        layout.addWidget(self.settings_card)

        # From this PC: the player sends to rtp://:PORT on the target.
        form = self.pages.add_form()
        self.pc_port = port_box(VIDEO_TO_TARGET_PORT)
        form.addRow("RTP port on the target", self.pc_port)
        hint = QLabel("Then open a video in the player and press play.")
        hint.setObjectName("muted")
        form.addRow(hint)

        # Camera on the target.
        form = self.pages.add_form()
        self.camera = QComboBox()
        self.camera.currentIndexChanged.connect(self._fill_modes)
        camera_row = QHBoxLayout()
        camera_row.addWidget(self.camera, 1)
        camera_row.addWidget(tool_button("refresh", "Look for cameras again", self.refresh_devices))
        form.addRow("Camera", camera_row)
        self.mode = QComboBox()
        form.addRow("Mode", self.mode)

        # File or URL on the target.
        form = self.pages.add_form()
        self.target_uri = QLineEdit(placeholderText="/home/root/videos/clip.mp4 or rtsp://...")
        self.realtime = QCheckBox("Play at its frame rate", checked=True)
        self.realtime.setToolTip("Off: as fast as the target can (to measure)")
        self.target_loop = QCheckBox("Loop")
        form.addRow("Path or URL", self.target_uri)
        form.addRow(self.realtime)
        form.addRow(self.target_loop)

        # RTP from another sender (a camera, ffmpeg, another target).
        form = self.pages.add_form()
        self.network_port = port_box(VIDEO_TO_TARGET_PORT)
        form.addRow("RTP port on the target", self.network_port)

        # Pattern.
        form = self.pages.add_form()
        self.pattern_size = QComboBox(editable=True)
        self.pattern_size.addItems(["640x480", "1280x720", "1920x1080", "3840x2160"])
        self.pattern_size.setCurrentText("1280x720")
        self.pattern_fps = QSpinBox(minimum=1, maximum=1000, value=30)
        self.pattern_fps.setSuffix(" fps")
        form.addRow("Size", self.pattern_size)
        form.addRow("Rate", self.pattern_fps)

        self.gray = QCheckBox("Decode to gray")
        self.gray.setToolTip("Cheaper when the pipeline starts with 'gray' anyway")
        self.settings_card.body.addWidget(self.gray)
        apply = QPushButton("Use this input")
        apply.setObjectName("primary")
        apply.clicked.connect(self.apply)
        stop = QPushButton("No input")
        stop.clicked.connect(lambda: self._set({"uri": ""}))
        buttons = QHBoxLayout()
        buttons.addWidget(stop)
        buttons.addStretch()
        buttons.addWidget(apply)
        self.settings_card.body.addLayout(buttons)

        self.current = QLabel()
        self.current.setObjectName("muted")
        self.current.setWordWrap(True)
        self.current.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
        layout.addWidget(self.current)
        layout.addStretch(1)  # spare height goes here, not into the cards
        self.kind.currentIndexChanged.connect(self._kind_changed)
        self._kind_changed(0)

    def _kind_changed(self, index: int) -> None:
        self.pages.setCurrentIndex(index)
        self.settings_card.title.setText(KIND_LABELS[index])

    # --- from the target ----------------------------------------------------

    def refresh_devices(self) -> None:
        self.client.call("input.devices", on_result=self.set_devices)

    def set_devices(self, devices: dict[str, Any]) -> None:
        self.cameras = devices.get("cameras", [])
        self.camera.blockSignals(True)
        self.camera.clear()
        for camera in self.cameras:
            self.camera.addItem(f"{camera['name']}  ({camera['path']})")
        if not self.cameras:
            self.camera.addItem("no camera found")
        self.camera.blockSignals(False)
        self._fill_modes()

    def set_input(self, current: dict[str, Any]) -> None:
        """Show the target's input (also when another GUI changed it)."""
        uri = current.get("uri", "")
        self.current.setText(f"Target input: <b>{describe_uri(uri)}</b>")
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
                self._show_error("No camera on the target.")
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
            on_error=lambda e: self._show_error(e.message),
        )

    def _show_error(self, text: str) -> None:
        self.current.setText(f"<span style='color:{theme.current().danger}'>{text}</span>")

    def _fill_modes(self) -> None:
        self.mode.clear()
        index = self.camera.currentIndex()
        if not 0 <= index < len(self.cameras):
            return
        camera = self.cameras[index]
        best = 0  # preselect the largest mode that still runs at 25 fps or more
        for i, mode in enumerate(camera.get("modes", [])):
            self.mode.addItem(
                f"{mode['width']}×{mode['height']}  ·  {mode['fps']:g} fps", mode["uri"]
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


class IconLabel(QLabel):
    """A small icon in the theme's muted colour."""

    def __init__(self, name: str):
        super().__init__()
        self.name = name
        self.setFixedWidth(22)
        self._paint()
        theme.follow(self, lambda _t: self._paint())

    def _paint(self) -> None:
        self.setPixmap(pixmap(self.name, theme.current().muted, 18))


def option_row(icon_name: str, check: QCheckBox, *controls: QWidget) -> QHBoxLayout:
    """An output option: icon, check box, then its settings."""
    row = QHBoxLayout()
    row.setSpacing(8)
    row.addWidget(IconLabel(icon_name))
    row.addWidget(check)
    row.addStretch()
    for control in controls:
        row.addWidget(control)
    return row


class OutputPanel(QScrollArea):
    """The target's outputs. `preview_port` is 0 when the result is not sent here."""

    preview_port = Signal(int)

    def __init__(self, client: QtAipcClient, parent: QWidget | None = None):
        super().__init__(parent)
        self.setWidgetResizable(True)
        self.client = client
        self.outputs: list[str] = []
        content = QWidget()
        layout = QVBoxLayout(content)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(10)
        self.setWidget(content)

        card = Card("Send the result to", "Any combination, applied with the button")
        self.to_pc = QCheckBox("This PC (the right view)")
        self.to_pc.setChecked(True)
        self.to_pc_port = port_box(VIDEO_FROM_TARGET_PORT)
        self.to_pc_port.setPrefix("port ")
        card.body.addLayout(option_row("send", self.to_pc, self.to_pc_port))

        self.display = QCheckBox("The target's display")
        self.display_choice = QComboBox()
        self.display_choice.setMinimumWidth(170)
        card.body.addLayout(option_row("monitor", self.display, self.display_choice))

        self.record = QCheckBox("A recording on the target")
        card.body.addLayout(option_row("record", self.record))
        self.record_path = QLineEdit("/tmp/scip-record.mp4")
        self.record_path.setToolTip(".mp4 or .mkv; finished when the output is removed")
        card.body.addWidget(self.record_path)

        self.extra = QLineEdit(placeholderText="More outputs, comma separated: udp:HOST:PORT, ...")
        card.body.addWidget(self.extra)
        apply = QPushButton("Apply outputs")
        apply.setObjectName("primary")
        apply.clicked.connect(self.apply)
        buttons = QHBoxLayout()
        buttons.addStretch()
        buttons.addWidget(apply)
        card.body.addLayout(buttons)
        layout.addWidget(card)

        status_card = Card("Status")
        self.status = QListWidget()
        self.status.setMinimumHeight(110)
        status_card.body.addWidget(self.status)
        layout.addWidget(status_card)
        layout.addStretch()

    # --- from the target ----------------------------------------------------

    def set_displays(self, displays: dict[str, Any]) -> None:
        current = self.display_choice.currentData()
        self.display_choice.clear()
        if displays.get("window_system"):
            self.display_choice.addItem("a window", "display")
        for d in displays.get("displays", []):
            state = f"{d['width']}×{d['height']}" if d["connected"] else "not connected"
            self.display_choice.addItem(f"{d['name']}  ·  {state}", d["uri"])
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
        t = theme.current()
        self.status.clear()
        outputs = stats.get("outputs", [])
        if not outputs:
            item = QListWidgetItem("No outputs: the result goes nowhere")
            item.setForeground(QColor(t.muted))
            self.status.addItem(item)
        for output in outputs:
            if output["error"]:
                item = QListWidgetItem(
                    icon("close", t.danger), f"{output['uri']}: {output['error']}"
                )
                item.setForeground(QColor(t.danger))
            elif output["active"]:
                text = output["description"] or output["uri"]
                item = QListWidgetItem(icon("send", t.success), text)
            else:
                text = f"{output['uri']}  (opens with the first frame)"
                item = QListWidgetItem(icon("refresh", t.muted), text)
                item.setForeground(QColor(t.muted))
            item.setToolTip(item.text())
            self.status.addItem(item)

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
            on_error=self._show_error,
        )

    def _show_error(self, error) -> None:
        t = theme.current()
        self.status.clear()
        item = QListWidgetItem(icon("close", t.danger), error.message)
        item.setForeground(QColor(t.danger))
        self.status.addItem(item)

    def _to_pc_uri(self) -> str:
        return f"udp:{self.client.local_address}:{self.to_pc_port.value()}"
