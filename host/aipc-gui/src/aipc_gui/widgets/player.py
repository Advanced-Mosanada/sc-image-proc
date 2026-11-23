"""Player: open, play, pause, rewind, seek, speed, loop, A-B loop.

Either for the video this PC streams to the target (media.Streamer), or for a
video file the target plays itself (TargetPlayer); both have the same API,
the panel's `backend`.
"""

from __future__ import annotations

from pathlib import Path

from PySide6.QtCore import QRectF, QSize, Qt, QTimer, Signal
from PySide6.QtGui import QColor, QPainter
from PySide6.QtWidgets import (
    QComboBox,
    QFileDialog,
    QHBoxLayout,
    QLabel,
    QPushButton,
    QSlider,
    QStyle,
    QStyleOptionSlider,
    QToolButton,
    QWidget,
)

from .. import media, settings, theme
from ..icons import set_icon
from ..target_player import TargetPlayer
from .common import Card, tool_button
from .files import TargetFileDialog

SPEEDS = [0.25, 0.5, 1.0, 1.5, 2.0, 4.0]
SKIP_SECONDS = 5.0
POLL_MS = 15  # delivers the preview frames too: faster than 60 fps
VIDEO_FILTER = "Videos (*.mp4 *.mkv *.mov *.avi *.webm *.ts *.h264);;All files (*)"


def clock(seconds: float | None) -> str:
    if seconds is None:
        return "--:--"
    minutes, rest = divmod(max(0.0, seconds), 60)
    return f"{int(minutes):02d}:{rest:05.2f}"


class SeekSlider(QSlider):
    """A position slider that jumps where you click and shows the A-B range."""

    jumped = Signal(int)

    def __init__(self):
        super().__init__(Qt.Orientation.Horizontal)
        self.marks: tuple[int | None, int | None] = (None, None)

    def set_marks(self, a: int | None, b: int | None) -> None:
        self.marks = (a, b)
        self.update()

    def mousePressEvent(self, event) -> None:
        if event.button() == Qt.MouseButton.LeftButton and self.maximum() > 0:
            groove = self._groove()
            value = QStyle.sliderValueFromPosition(
                self.minimum(),
                self.maximum(),
                int(event.position().x() - groove.left()),
                int(groove.width()),
            )
            self.setValue(value)
            self.jumped.emit(value)
        super().mousePressEvent(event)

    def _groove(self) -> QRectF:
        option = QStyleOptionSlider()
        self.initStyleOption(option)
        return QRectF(
            self.style().subControlRect(
                QStyle.ComplexControl.CC_Slider, option, QStyle.SubControl.SC_SliderGroove, self
            )
        )

    def _x(self, value: int, groove: QRectF) -> float:
        span = max(1, self.maximum() - self.minimum())
        return groove.left() + groove.width() * (value - self.minimum()) / span

    def paintEvent(self, event) -> None:
        super().paintEvent(event)
        a, b = self.marks
        if a is None and b is None:
            return
        t = theme.current()
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        groove = self._groove()
        y = groove.center().y()
        color = QColor(t.warning)
        if a is not None and b is not None:
            band = QColor(color)
            band.setAlpha(90)
            painter.fillRect(
                QRectF(self._x(a, groove), y - 5, self._x(b, groove) - self._x(a, groove), 10), band
            )
        painter.setPen(Qt.PenStyle.NoPen)
        painter.setBrush(color)
        for mark in (a, b):
            if mark is not None:
                painter.drawRoundedRect(QRectF(self._x(mark, groove) - 1.5, y - 8, 3, 16), 1.5, 1.5)


class PlayerPanel(Card):
    """Owns the Streamer (preview frames of what is sent go to `on_frame`);
    with set_target() it controls the target's player instead."""

    message = Signal(str)  # for the status bar
    opened = Signal(str)  # path of the file now loaded

    def __init__(self, on_frame=None, parent: QWidget | None = None):
        super().__init__("Video from this PC", parent=parent)
        self.streamer: media.Streamer | None = None
        self.remote: TargetPlayer | None = None  # set by the window
        self.on_target = False
        self._seeking = False
        self._marks: dict[str, float | None] = {"a": None, "b": None}

        if media.available():
            self.streamer = media.Streamer(on_frame=on_frame, on_error=self.message.emit)
        else:
            self.hint.setText(f"Streaming needs GStreamer for Python: {media.GST_ERROR}")
            self.hint.show()

        # Title row: what is loaded, and the Open button.
        self.file_label = QLabel()
        self.file_label.setObjectName("muted")
        self.file_label.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
        self.header.insertWidget(1, self.file_label, 1)
        self.route_label = QLabel()
        self.route_label.setObjectName("muted")
        self.route_label.setToolTip("How the video goes to the target (Input tab)")
        self.header.insertWidget(2, self.route_label)
        self.open_button = QPushButton("Open")
        set_icon(self.open_button, "folder", size=16)
        self.open_button.setToolTip("Open a video file (Ctrl+O), or drop one on the left view")
        self.open_button.clicked.connect(self.choose_file)
        self.header.addWidget(self.open_button)

        # Position.
        self.slider = SeekSlider()
        self.slider.setRange(0, 0)
        self.slider.sliderPressed.connect(lambda: setattr(self, "_seeking", True))
        self.slider.sliderReleased.connect(self._slider_released)
        self.slider.sliderMoved.connect(self._show_time)
        self.slider.jumped.connect(lambda v: self._streamer_do("seek", v / 1000))
        self.position_label = QLabel(clock(0))
        self.duration_label = QLabel(clock(None))
        for label in (self.position_label, self.duration_label):
            label.setObjectName("muted")
            label.setMinimumWidth(64)
        self.duration_label.setAlignment(
            Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter
        )
        position = QHBoxLayout()
        position.addWidget(self.position_label)
        position.addWidget(self.slider, 1)
        position.addWidget(self.duration_label)
        self.body.addLayout(position)

        # Controls: options | transport | A-B.
        controls = QHBoxLayout()
        controls.setSpacing(6)
        self.speed = QComboBox()
        self.speed.addItems([f"{s:g}×" for s in SPEEDS])
        self.speed.setCurrentIndex(SPEEDS.index(1.0))
        self.speed.setToolTip("Playback speed")
        self.speed.currentIndexChanged.connect(lambda i: self._streamer_do("set_rate", SPEEDS[i]))
        self.loop = tool_button("loop", "Loop the whole video", checkable=True)
        self.loop.toggled.connect(lambda on: self._streamer_do("set_loop", on))
        controls.addWidget(self.speed)
        controls.addWidget(self.loop)
        controls.addStretch()

        self.restart_button = tool_button("to-start", "Back to the start (Home)", self.restart)
        self.rewind_button = tool_button(
            "rewind", f"Back {SKIP_SECONDS:g} s (←)", lambda: self.skip(-SKIP_SECONDS)
        )
        self.play_button = QPushButton()
        self.play_button.setObjectName("play")
        self.play_button.setFixedSize(QSize(44, 44))
        self.play_button.setToolTip("Play / pause (Space)")
        self.play_button.clicked.connect(self.toggle)
        self.forward_button = tool_button(
            "forward", f"Forward {SKIP_SECONDS:g} s (→)", lambda: self.skip(SKIP_SECONDS)
        )
        self.stop_button = tool_button("stop", "Stop: pause at the start", self.stop)
        for widget in (
            self.restart_button,
            self.rewind_button,
            self.play_button,
            self.forward_button,
            self.stop_button,
        ):
            controls.addWidget(widget)
        controls.addStretch()

        self.a_button = QToolButton(text="A", checkable=True)
        self.a_button.setToolTip("Loop from here (A)")
        self.a_button.clicked.connect(lambda: self._mark("a"))
        self.b_button = QToolButton(text="B", checkable=True)
        self.b_button.setToolTip("Loop until here (B)")
        self.b_button.clicked.connect(lambda: self._mark("b"))
        self.ab_clear = tool_button("close", "Clear the A-B loop", self.clear_ab)
        self.ab_label = QLabel()
        self.ab_label.setObjectName("muted")
        for widget in (self.ab_label, self.a_button, self.b_button, self.ab_clear):
            controls.addWidget(widget)
        self.body.addLayout(controls)

        self.timer = QTimer(self, interval=POLL_MS)
        self.timer.timeout.connect(self._tick)
        self.timer.start()
        self._info: media.MediaInfo | None = None
        self._show_file(None)
        theme.follow(self, lambda _t: self._show_file(self._info))
        self._update_controls()

    # --- public ---------------------------------------------------------------

    @property
    def backend(self) -> media.Streamer | TargetPlayer | None:
        """What the controls drive."""
        return self.remote if self.on_target else self.streamer

    def set_target(self, on: bool) -> None:
        """Control the video the target plays (on), or stream from this PC."""
        if on == self.on_target:
            return
        if on and self.streamer is not None and self.streamer.state == "playing":
            self.streamer.pause()  # nobody receives it any more
        self.on_target = on
        self.title.setText("Video on the target" if on else "Video from this PC")
        self.open_button.setToolTip(
            "Choose a video on the target (Ctrl+O)"
            if on
            else "Open a video file (Ctrl+O), or drop one on the left view"
        )
        self.route_label.setVisible(not on)
        self._marks = {"a": None, "b": None}
        self._apply_info(self.backend.info if self.backend else None)
        self._update_controls()

    def set_destination(self, host: str, port: int) -> None:
        if self.streamer:
            self._guarded(self.streamer.set_destination, host, port)

    def configure(self, stream: media.StreamSettings) -> None:
        """How the video is sent (H.264 settings, or raw); applies while playing."""
        if self.streamer:
            self._guarded(self.streamer.configure, stream)
            self.route_label.setText(f"\u2192 {stream.describe()}")
            self._update_controls()

    def _guarded(self, action, *args) -> None:
        try:
            action(*args)
        except (OSError, RuntimeError, ValueError) as error:
            self.message.emit(f"player: {error}")

    def choose_file(self) -> None:
        if self.on_target:
            if self.remote is not None:
                dialog = TargetFileDialog(self.remote.client, self.window())
                if dialog.exec() and dialog.chosen:
                    self.open(dialog.chosen)
            return
        folder = settings.get("player/folder", "")
        path, _ = QFileDialog.getOpenFileName(self, "Video to stream", folder, VIDEO_FILTER)
        if path:
            self.open(path)

    def open(self, path: str) -> bool:
        """A file on this PC, or on the target in target mode."""
        backend = self.backend
        if backend is None:
            return False
        try:
            info = backend.open(path)
        except (OSError, RuntimeError) as error:
            self.message.emit(f"cannot open {path}: {error}")
            return False
        if self.on_target:
            self.speed.setCurrentIndex(SPEEDS.index(1.0))
        else:
            settings.put("player/folder", str(Path(path).parent))
            backend.set_rate(SPEEDS[self.speed.currentIndex()])
            backend.set_loop(self.loop.isChecked())
        self._marks = {"a": None, "b": None}
        self._apply_info(info)
        self._update_controls()
        self.opened.emit(path)
        return True

    def toggle(self) -> None:
        self._streamer_do("toggle")

    def skip(self, seconds: float) -> None:
        self._streamer_do("skip", seconds)

    def stop(self) -> None:
        self._streamer_do("stop")

    def restart(self) -> None:
        if self.backend:
            start = self._marks["a"] if self.backend.a is not None else 0.0
            self._streamer_do("seek", start or 0.0)

    def clear_ab(self) -> None:
        self._marks = {"a": None, "b": None}
        self._streamer_do("set_ab", None, None)

    @property
    def is_open(self) -> bool:
        return self.backend is not None and self.backend.state != "closed"

    def close(self) -> bool:
        if self.streamer:
            self.streamer.close()
        return super().close()

    # --- internals ------------------------------------------------------------

    def _apply_info(self, info: media.MediaInfo | None) -> None:
        """A new file, or new details of it (the target's arrive late)."""
        self.slider.setRange(0, int(info.duration * 1000) if info else 0)
        self._show_file(info)

    def _show_file(self, info: media.MediaInfo | None) -> None:
        self._info = info
        if info is None:
            self.file_label.setText(
                "no file: Open one on the target"
                if self.on_target
                else "no file: open one, or drop it on the left view"
            )
            self.file_label.setToolTip("")
            return
        name = Path(info.path).name
        size = f"{info.width}×{info.height} · {info.fps:.3g} fps · " if info.width else ""
        details = f"{size}{clock(info.duration)}"
        self.file_label.setText(
            f"<b style='color:{theme.current().text}'>{name}</b>&nbsp;&nbsp;{details}"
        )
        self.file_label.setToolTip(info.path)

    def _streamer_do(self, name: str, *args) -> None:
        if not self.is_open:
            return
        try:
            getattr(self.backend, name)(*args)
        except ValueError as error:
            self.message.emit(str(error))
        self._update_controls()

    def _mark(self, which: str) -> None:
        if not self.is_open:
            return
        self._marks[which] = self.backend.position()
        a, b = self._marks["a"], self._marks["b"]
        if a is not None and b is not None:
            if b <= a:
                self.message.emit("B must be after A")
                self._marks[which] = None
            else:
                self._streamer_do("set_ab", a, b)
        self._update_controls()

    def _show_time(self, value: int) -> None:
        self.position_label.setText(clock(value / 1000))

    def _slider_released(self) -> None:
        self._seeking = False
        self._streamer_do("seek", self.slider.value() / 1000)

    def _tick(self) -> None:
        backend = self.backend
        if backend is None:
            return
        backend.poll()
        if backend.info != self._info:
            self._apply_info(backend.info)
        if not self.is_open:
            return
        if self.on_target:
            self._follow_target(backend)
        if not self._seeking:
            position = backend.position()
            self.slider.blockSignals(True)
            self.slider.setValue(int(position * 1000))
            self.slider.blockSignals(False)
            self._show_time(int(position * 1000))
        self._update_controls()

    def _follow_target(self, remote: TargetPlayer) -> None:
        """Show the target's speed, loop and A-B (also when another GUI set them)."""
        if remote.rate in SPEEDS and SPEEDS[self.speed.currentIndex()] != remote.rate:
            self.speed.blockSignals(True)
            self.speed.setCurrentIndex(SPEEDS.index(remote.rate))
            self.speed.blockSignals(False)
        if self.loop.isChecked() != remote.loop:
            self.loop.blockSignals(True)
            self.loop.setChecked(remote.loop)
            self.loop.blockSignals(False)
        if remote.a is not None and remote.b is not None:
            self._marks = {"a": remote.a, "b": remote.b}
        elif all(m is not None for m in self._marks.values()):
            self._marks = {"a": None, "b": None}

    def _update_controls(self) -> None:
        backend = self.backend
        self.open_button.setEnabled(backend is not None)
        if backend is None:
            for widget in (self.slider, self.play_button, self.speed, self.loop):
                widget.setEnabled(False)
            return
        opened = self.is_open
        for widget in (
            self.slider,
            self.restart_button,
            self.rewind_button,
            self.play_button,
            self.forward_button,
            self.stop_button,
            self.a_button,
            self.b_button,
            self.ab_clear,
            self.speed,
            self.loop,
        ):
            widget.setEnabled(opened)
        playing = backend.state == "playing"
        icon = "pause" if playing else "play"
        if self.play_button.property("_icon") != (icon, "accent_text"):
            set_icon(self.play_button, icon, role="accent_text", size=20)
        self.duration_label.setText(clock(backend.duration() if opened else None))
        a, b = self._marks["a"], self._marks["b"]
        self.slider.set_marks(
            None if a is None else int(a * 1000), None if b is None else int(b * 1000)
        )
        self.a_button.setChecked(a is not None)
        self.b_button.setChecked(b is not None)
        if backend.a is not None:
            self.ab_label.setText(f"{clock(a)} – {clock(b)}")
        elif a is not None or b is not None:
            self.ab_label.setText("set A and B")
        else:
            self.ab_label.setText("A–B loop")
        self.ab_clear.setVisible(a is not None or b is not None)
