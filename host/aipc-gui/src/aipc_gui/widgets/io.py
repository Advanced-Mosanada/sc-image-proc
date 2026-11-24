"""Where the target's video comes from and where its result goes."""

from __future__ import annotations

from typing import Any

from PySide6.QtCore import QRect, QSize, Qt, QTimer, Signal
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

from .. import settings, theme
from ..icons import icon, pixmap
from ..media import X264_PRESETS, MediaInfo, StreamSettings
from ..protocol import INPUT_FROM_TARGET_PORT, VIDEO_FROM_TARGET_PORT, VIDEO_TO_TARGET_PORT
from ..qt_client import QtAipcClient
from .common import Card, tool_button
from .files import TargetFileDialog

# Input kinds, in the order of the list.
FROM_PC, CAMERA, TARGET_FILE, NETWORK, PATTERN = range(5)
KINDS = [
    ("film", "Video from this PC", "Streamed by the player below the videos"),
    ("camera", "Camera on the target", "A V4L2 camera and its mode"),
    ("file", "File or URL on the target", "Played by the target, controlled from here"),
    ("network", "RTP stream", "H.264 sent to the target by another sender"),
    ("pattern", "Test pattern", "Generated on the target"),
]
KIND_LABELS = [title for _, title, _ in KINDS]
CHOICE_HEIGHT = 46
# How the PC sends its video (the "Sent as" choice).
H264, RAW_TCP, RAW_UDP = "h264", "raw-tcp", "raw-udp"


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
    stream_settings_changed = Signal(object)  # StreamSettings, while streaming from the PC
    target_player = Signal(bool)  # the target plays a video file (the player controls it)

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

        # From this PC: the player sends H.264 to rtp://:PORT on the target, or
        # raw frames to tcp://:PORT or rtpraw://:PORT.
        form = self.pages.add_form()
        self.stream_form = form
        self.pc_port = port_box(VIDEO_TO_TARGET_PORT)
        form.addRow("Port on the target", self.pc_port)
        self.transport = QComboBox()
        self.transport.addItem("H.264, compressed (RTP over UDP)", H264)
        self.transport.addItem("Raw frames, lossless (TCP)", RAW_TCP)
        self.transport.addItem("Raw frames (RTP over UDP)", RAW_UDP)
        self.transport.setToolTip(
            "Raw: every pixel as decoded; fine for PAL-size video on a gigabit link.\n"
            "TCP: every frame arrives, late when the link is too slow.\n"
            "UDP: never late, but a frame that loses a packet is dropped."
        )
        form.addRow("Sent as", self.transport)
        self.rate_mode = QComboBox()
        self.rate_mode.addItems(["Constant bitrate", "Constant quality"])
        form.addRow("Rate control", self.rate_mode)
        self.bitrate = QSpinBox(minimum=100, maximum=200_000, singleStep=500)
        self.bitrate.setSuffix(" kbit/s")
        form.addRow("Bitrate", self.bitrate)
        self.quality = QSpinBox(minimum=1, maximum=51)
        self.quality.setToolTip("x264's CRF: lower is better. 18 looks lossless, 23 is the default")
        form.addRow("Quality (CRF)", self.quality)
        self.preset = QComboBox()
        self.preset.addItems(X264_PRESETS)
        self.preset.setToolTip(
            "Slower presets give a better picture for the same bitrate, at more CPU on this PC"
        )
        form.addRow("Encoder preset", self.preset)
        self.keyframes = QSpinBox(minimum=1, maximum=600)
        self.keyframes.setSuffix(" frames")
        self.keyframes.setToolTip("A receiver that joins late waits for the next key frame")
        form.addRow("Key frame every", self.keyframes)
        self.raw_format = QComboBox()
        self.raw_format.addItem("I420 (4:2:0, as most videos decode)", "I420")
        self.raw_format.addItem("BGR (4:4:4)", "BGR")
        form.addRow("Pixel format", self.raw_format)
        self.stream_estimate = QLabel()
        self.stream_estimate.setObjectName("muted")
        form.addRow(self.stream_estimate)
        hint = QLabel("Then open a video in the player and press play.")
        hint.setObjectName("muted")
        form.addRow(hint)
        self.media: MediaInfo | None = None
        self.current_uri = ""  # the target's input, as last reported
        self._load_stream_settings()
        # Changes apply by themselves while the PC streams, once the typing stops.
        self.stream_timer = QTimer(self, singleShot=True, interval=400)
        self.stream_timer.timeout.connect(self._push_stream_settings)
        for combo in (self.transport, self.rate_mode, self.preset, self.raw_format):
            combo.currentIndexChanged.connect(self._stream_edited)
        for box in (self.bitrate, self.quality, self.keyframes):
            box.valueChanged.connect(self._stream_edited)
        self._show_stream_rows()

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
        self.target_uri.setToolTip(
            "A file plays like in a player (the player under the videos controls it);"
            " a URL is just received"
        )
        browse = tool_button("folder", "Choose a video on the target", self.browse_target)
        uri_row = QHBoxLayout()
        uri_row.addWidget(self.target_uri, 1)
        uri_row.addWidget(browse)
        self.realtime = QCheckBox("Play at its frame rate", checked=True)
        self.realtime.setToolTip("Off: as fast as the target can (to measure)")
        self.target_loop = QCheckBox("Loop")
        form.addRow("Path or URL", uri_row)
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

    # --- how the PC streams ----------------------------------------------------

    def stream_settings(self) -> StreamSettings:
        transport = self.transport.currentData()
        return StreamSettings(
            raw=transport != H264,
            raw_udp=transport == RAW_UDP,
            raw_format=self.raw_format.currentData(),
            bitrate_kbps=self.bitrate.value(),
            quality=self.quality.value() if self.rate_mode.currentIndex() == 1 else 0,
            preset=self.preset.currentText(),
            keyframe_interval=self.keyframes.value(),
        )

    def set_media(self, info: MediaInfo | None) -> None:
        """The player's file, for the bandwidth estimate."""
        self.media = info
        self._show_stream_rows()

    def _load_stream_settings(self) -> None:
        d = StreamSettings()
        self._select_transport(
            self._transport_of(
                settings.get("stream/raw", d.raw, bool),
                settings.get("stream/raw_udp", d.raw_udp, bool),
            )
        )
        fmt = settings.get("stream/raw_format", d.raw_format, str)
        self.raw_format.setCurrentIndex(max(0, self.raw_format.findData(fmt)))
        self.bitrate.setValue(settings.get("stream/bitrate_kbps", d.bitrate_kbps, int))
        quality = settings.get("stream/quality", d.quality, int)
        self.rate_mode.setCurrentIndex(1 if quality else 0)
        self.quality.setValue(quality or 18)
        preset = settings.get("stream/preset", d.preset, str)
        self.preset.setCurrentText(preset if preset in X264_PRESETS else d.preset)
        self.keyframes.setValue(settings.get("stream/keyframe_interval", d.keyframe_interval, int))

    @staticmethod
    def _transport_of(raw: bool, raw_udp: bool) -> str:
        return (RAW_UDP if raw_udp else RAW_TCP) if raw else H264

    def _select_transport(self, transport: str) -> None:
        self.transport.setCurrentIndex(max(0, self.transport.findData(transport)))

    def _set_transport(self, transport: str) -> None:
        """Show the target's transport (also when another GUI chose it)."""
        if self.transport.currentData() != transport:
            self.transport.blockSignals(True)
            self._select_transport(transport)
            self.transport.blockSignals(False)
            self._show_stream_rows()
            settings.put("stream/raw", transport != H264)
            settings.put("stream/raw_udp", transport == RAW_UDP)

    def _show_stream_rows(self) -> None:
        st = self.stream_settings()
        form = self.stream_form
        for widget, visible in (
            (self.rate_mode, not st.raw),
            (self.bitrate, not st.raw and not st.quality),
            (self.quality, not st.raw and bool(st.quality)),
            (self.preset, not st.raw),
            (self.keyframes, not st.raw),
            (self.raw_format, st.raw),
        ):
            form.setRowVisible(widget, visible)
        if self.media and self.media.width:
            w, h, fps = self.media.width, self.media.height, self.media.fps or 30.0
            what = "this video"
        else:
            w, h, fps = 720, 576, 25.0
            what = "PAL"
        mbits = st.megabits(w, h, fps)
        size = f"{w}\u00d7{h}, {fps:g} fps"
        if mbits is None:
            text = f"Bitrate follows the picture ({what}: {size})"
        elif st.raw:
            text = f"\u2248 {mbits:.0f} Mbit/s for {what} ({size})"
        else:
            text = f"{mbits:g} Mbit/s"
        self.stream_estimate.setText(text)
        self.stream_estimate.setToolTip("Gigabit Ethernet carries about 940 Mbit/s")

    def _stream_edited(self) -> None:
        st = self.stream_settings()
        for key, value in (
            ("raw", st.raw),
            ("raw_udp", st.raw_udp),
            ("raw_format", st.raw_format),
            ("bitrate_kbps", st.bitrate_kbps),
            ("quality", st.quality),
            ("preset", st.preset),
            ("keyframe_interval", st.keyframe_interval),
        ):
            settings.put(f"stream/{key}", value)
        self._show_stream_rows()
        self.stream_timer.start()

    def _push_stream_settings(self) -> None:
        """While the PC streams: new encoder settings go to the player; a new
        transport also changes the target's input (rtp://, tcp://, rtpraw://)."""
        if self._listen_port(self.current_uri) is None or self.kind.currentIndex() != FROM_PC:
            return
        st = self.stream_settings()
        if self.current_uri == st.uri(self.pc_port.value()):
            self.stream_settings_changed.emit(st)
        else:
            self.apply()

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
        self.current_uri = uri
        port = self._listen_port(uri)
        if port is not None:
            raw = uri.startswith(("tcp://", "rtpraw://"))
            if raw or self.kind.currentIndex() != NETWORK:
                self.kind.setCurrentIndex(FROM_PC)
            if self.kind.currentIndex() == FROM_PC:
                self.pc_port.setValue(port)
                self._set_transport(self._transport_of(raw, uri.startswith("rtpraw://")))
            else:
                self.network_port.setValue(port)
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
        self.target_player.emit(bool(uri) and bool(current.get("player")))

    def browse_target(self) -> None:
        dialog = TargetFileDialog(self.client, self.window())
        if dialog.exec() and dialog.chosen:
            self.open_target_file(dialog.chosen)

    def open_target_file(self, path: str) -> None:
        """Have the target play a file of its own (the player then controls it)."""
        self.kind.setCurrentIndex(TARGET_FILE)
        self.target_uri.setText(path)
        self.realtime.setChecked(True)
        self.apply()

    # --- user -----------------------------------------------------------------

    def apply(self) -> None:
        kind = self.kind.currentIndex()
        params: dict[str, Any] = {"gray": self.gray.isChecked()}
        if kind == FROM_PC:
            params["uri"] = self.stream_settings().uri(self.pc_port.value())
        elif kind == NETWORK:
            params["uri"] = f"rtp://:{self.network_port.value()}"
        elif kind == CAMERA:
            mode = self.mode.currentData()
            if not mode:
                self._show_error("No camera on the target.")
                return
            params["uri"] = mode
        elif kind == TARGET_FILE:
            uri = self.target_uri.text().strip()
            params.update(
                uri=uri,
                realtime=self.realtime.isChecked(),
                loop=self.target_loop.isChecked(),
                player="://" not in uri,  # a file: controlled like a player
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
    def _listen_port(uri: str) -> int | None:
        """The port of an rtp://, tcp:// or rtpraw:// input."""
        for scheme in ("rtp://", "tcp://", "rtpraw://"):
            if uri.startswith(scheme) and ":" in uri[len(scheme) :]:
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
    input_preview_port = Signal(int)  # 0 when the target's input is not sent here

    def __init__(self, client: QtAipcClient, parent: QWidget | None = None):
        super().__init__(parent)
        self.setWidgetResizable(True)
        self.client = client
        self.outputs: list[str] = []
        self.input_outputs: list[str] = []
        self.input_from_pc = False  # then the left view shows the PC's own preview
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

        self.show_input = QCheckBox("Its input too (the left view)")
        self.show_input.setToolTip(
            "The target sends what it receives, before the functions: a camera, or the\n"
            "file it plays. Costs one more encoder on the target. Not needed (and not\n"
            "sent) while this PC streams the video."
        )
        self.show_input.setChecked(settings.get("outputs/show_input", True, bool))
        self.show_input.toggled.connect(lambda on: settings.put("outputs/show_input", on))
        self.show_input.toggled.connect(lambda _on: self.apply_input_outputs())
        self.input_port = port_box(INPUT_FROM_TARGET_PORT)
        self.input_port.setPrefix("port ")
        card.body.addLayout(option_row("camera", self.show_input, self.input_port))
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
        self.input_outputs = result.get("input_outputs", [])
        input_here = self._input_uri()
        self.input_preview_port.emit(
            self.input_port.value() if input_here in self.input_outputs else 0
        )

    def set_input_from_pc(self, from_pc: bool) -> None:
        """The input changed: the target's input goes to the left view unless
        this PC streams it (the PC shows its own preview then)."""
        self.input_from_pc = from_pc
        self.apply_input_outputs()

    def apply_input_outputs(self) -> None:
        """Send the target's input here, or stop, when that changed."""
        wanted = (
            [self._input_uri()] if self.show_input.isChecked() and not self.input_from_pc else []
        )
        mine = {self._input_uri()}
        others = [o for o in self.input_outputs if o not in mine]  # another GUI's
        if sorted(wanted + others) != sorted(self.input_outputs) and self.client.is_connected:
            self.client.call(
                "output.set",
                {"input_outputs": others + wanted},
                on_result=self.set_outputs,
                on_error=self._show_error,
            )

    def update_status(self, stats: dict[str, Any]) -> None:
        t = theme.current()
        self.status.clear()
        outputs = stats.get("outputs", [])
        if not outputs:
            item = QListWidgetItem("No outputs: the result goes nowhere")
            item.setForeground(QColor(t.muted))
            self.status.addItem(item)
        inputs = [dict(o, uri=f"input \u2192 {o['uri']}") for o in stats.get("input_outputs", [])]
        for output in outputs + inputs:
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
            on_result=lambda result: (self.set_outputs(result), self.apply_input_outputs()),
            on_error=self._show_error,
        )

    def _show_error(self, error) -> None:
        t = theme.current()
        self.status.clear()
        item = QListWidgetItem(icon("close", t.danger), error.message)
        item.setForeground(QColor(t.danger))
        self.status.addItem(item)

    def _input_uri(self) -> str:
        return f"udp:{self.client.local_address}:{self.input_port.value()}"

    def _to_pc_uri(self) -> str:
        return f"udp:{self.client.local_address}:{self.to_pc_port.value()}"
