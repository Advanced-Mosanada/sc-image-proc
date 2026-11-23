"""Player for the video this PC streams to the target: open, play, pause,
rewind, seek, speed, loop, A-B loop."""

from __future__ import annotations

from pathlib import Path

from PySide6.QtCore import Qt, QTimer, Signal
from PySide6.QtWidgets import (
    QCheckBox,
    QComboBox,
    QFileDialog,
    QHBoxLayout,
    QLabel,
    QPushButton,
    QSlider,
    QStyle,
    QVBoxLayout,
    QWidget,
)

from .. import media

SPEEDS = [0.25, 0.5, 1.0, 1.5, 2.0, 4.0]
SKIP_SECONDS = 5.0
POLL_MS = 15  # delivers the preview frames too: faster than 60 fps
VIDEO_FILTER = "Videos (*.mp4 *.mkv *.mov *.avi *.webm *.ts *.h264);;All files (*)"


def clock(seconds: float | None) -> str:
    if seconds is None:
        return "--:--"
    minutes, rest = divmod(max(0.0, seconds), 60)
    return f"{int(minutes):02d}:{rest:05.2f}"


class PlayerPanel(QWidget):
    """Owns the Streamer. `frame` gets the preview frames of what is sent."""

    message = Signal(str)  # for the status bar

    def __init__(self, on_frame=None, parent: QWidget | None = None):
        super().__init__(parent)
        self.streamer: media.Streamer | None = None
        self._seeking = False
        self._on_frame = on_frame
        layout = QVBoxLayout(self)

        if not media.available():
            layout.addWidget(QLabel(f"Streaming needs GStreamer for Python: {media.GST_ERROR}"))
            self.setEnabled(False)
            return
        self.streamer = media.Streamer(on_frame=on_frame, on_error=self.message.emit)

        top = QHBoxLayout()
        self.open_button = QPushButton("Open video...")
        self.open_button.clicked.connect(self.choose_file)
        self.file_label = QLabel("<i>no file</i>")
        self.file_label.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
        top.addWidget(self.open_button)
        top.addWidget(self.file_label, 1)
        layout.addLayout(top)

        self.slider = QSlider(Qt.Orientation.Horizontal)
        self.slider.setRange(0, 0)
        self.slider.sliderPressed.connect(lambda: setattr(self, "_seeking", True))
        self.slider.sliderReleased.connect(self._slider_released)
        self.slider.sliderMoved.connect(self._slider_moved)
        self.time_label = QLabel(clock(0) + " / " + clock(None))
        position = QHBoxLayout()
        position.addWidget(self.slider, 1)
        position.addWidget(self.time_label)
        layout.addLayout(position)

        buttons = QHBoxLayout()
        self.restart_button = self._button(
            QStyle.StandardPixmap.SP_MediaSkipBackward, "Back to the start", self.restart
        )
        self.rewind_button = self._button(
            QStyle.StandardPixmap.SP_MediaSeekBackward,
            f"Rewind {SKIP_SECONDS:g} s",
            lambda: self._streamer_do("skip", -SKIP_SECONDS),
        )
        self.play_button = self._button(
            QStyle.StandardPixmap.SP_MediaPlay, "Play / pause (Space)", self.toggle
        )
        self.play_button.setShortcut(Qt.Key.Key_Space)
        self.forward_button = self._button(
            QStyle.StandardPixmap.SP_MediaSeekForward,
            f"Forward {SKIP_SECONDS:g} s",
            lambda: self._streamer_do("skip", SKIP_SECONDS),
        )
        self.stop_button = self._button(
            QStyle.StandardPixmap.SP_MediaStop, "Stop (pause at the start)", self.stop
        )
        for button in (
            self.restart_button,
            self.rewind_button,
            self.play_button,
            self.forward_button,
            self.stop_button,
        ):
            buttons.addWidget(button)
        buttons.addSpacing(12)

        self.speed = QComboBox()
        self.speed.addItems([f"{s:g}x" for s in SPEEDS])
        self.speed.setCurrentIndex(SPEEDS.index(1.0))
        self.speed.setToolTip("Playback speed")
        self.speed.currentIndexChanged.connect(lambda i: self._streamer_do("set_rate", SPEEDS[i]))
        buttons.addWidget(self.speed)

        self.loop = QCheckBox("Loop")
        self.loop.toggled.connect(lambda on: self._streamer_do("set_loop", on))
        buttons.addWidget(self.loop)
        buttons.addSpacing(12)

        self.a_button = QPushButton("A")
        self.a_button.setToolTip("Loop range starts here")
        self.a_button.clicked.connect(lambda: self._mark("a"))
        self.b_button = QPushButton("B")
        self.b_button.setToolTip("Loop range ends here")
        self.b_button.clicked.connect(lambda: self._mark("b"))
        self.ab_clear = QPushButton("Clear A-B")
        self.ab_clear.clicked.connect(self.clear_ab)
        self.ab_label = QLabel("A-B: off")
        for widget in (self.a_button, self.b_button, self.ab_clear, self.ab_label):
            buttons.addWidget(widget)
        buttons.addStretch()
        layout.addLayout(buttons)

        self._marks: dict[str, float | None] = {"a": None, "b": None}
        self.timer = QTimer(self, interval=POLL_MS)
        self.timer.timeout.connect(self._tick)
        self.timer.start()
        self._update_controls()

    # --- public ---------------------------------------------------------------

    def set_destination(self, host: str, port: int) -> None:
        if self.streamer:
            self.streamer.set_destination(host, port)

    def choose_file(self) -> None:
        path, _ = QFileDialog.getOpenFileName(self, "Video to stream", "", VIDEO_FILTER)
        if path:
            self.open(path)

    def open(self, path: str) -> bool:
        if not self.streamer:
            return False
        try:
            info = self.streamer.open(path)
        except (OSError, RuntimeError) as error:
            self.message.emit(f"cannot open {path}: {error}")
            return False
        self.streamer.set_rate(SPEEDS[self.speed.currentIndex()])
        self.streamer.set_loop(self.loop.isChecked())
        self._marks = {"a": None, "b": None}
        self.slider.setRange(0, int(info.duration * 1000))
        details = f"{info.width}x{info.height}, {info.fps:.3g} fps, {clock(info.duration)}"
        self.file_label.setText(f"<b>{Path(path).name}</b>  {details}")
        self.file_label.setToolTip(path)
        self._update_controls()
        return True

    def toggle(self) -> None:
        self._streamer_do("toggle")

    def stop(self) -> None:
        self._streamer_do("stop")

    def restart(self) -> None:
        if self.streamer:
            start = self._marks["a"] if self.streamer.a is not None else 0.0
            self.streamer.seek(start or 0.0)

    def clear_ab(self) -> None:
        self._marks = {"a": None, "b": None}
        self._streamer_do("set_ab", None, None)

    def close(self) -> bool:
        if self.streamer:
            self.streamer.close()
        return super().close()

    # --- internals ------------------------------------------------------------

    def _button(self, icon, tip, slot) -> QPushButton:
        button = QPushButton(self.style().standardIcon(icon), "")
        button.setToolTip(tip)
        button.clicked.connect(slot)
        return button

    def _streamer_do(self, name: str, *args) -> None:
        if not self.streamer or self.streamer.state == "closed":
            return
        try:
            getattr(self.streamer, name)(*args)
        except ValueError as error:
            self.message.emit(str(error))
        self._update_controls()

    def _mark(self, which: str) -> None:
        if not self.streamer or self.streamer.state == "closed":
            return
        self._marks[which] = self.streamer.position()
        a, b = self._marks["a"], self._marks["b"]
        if a is not None and b is not None:
            if b <= a:
                self.message.emit("B must be after A")
                self._marks[which] = None
            else:
                self._streamer_do("set_ab", a, b)
        self._update_controls()

    def _slider_moved(self, value: int) -> None:
        self.time_label.setText(f"{clock(value / 1000)} / {clock(self._duration())}")

    def _slider_released(self) -> None:
        self._seeking = False
        self._streamer_do("seek", self.slider.value() / 1000)

    def _duration(self) -> float | None:
        return self.streamer.duration() if self.streamer and self.streamer.info else None

    def _tick(self) -> None:
        if not self.streamer:
            return
        self.streamer.poll()
        if self.streamer.state == "closed":
            return
        if not self._seeking:
            position = self.streamer.position()
            self.slider.blockSignals(True)
            self.slider.setValue(int(position * 1000))
            self.slider.blockSignals(False)
            self.time_label.setText(f"{clock(position)} / {clock(self._duration())}")
        self._update_controls()

    def _update_controls(self) -> None:
        if not self.streamer:
            return
        opened = self.streamer.state != "closed"
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
        ):
            widget.setEnabled(opened)
        playing = self.streamer.state == "playing"
        icon = (
            QStyle.StandardPixmap.SP_MediaPause if playing else QStyle.StandardPixmap.SP_MediaPlay
        )
        self.play_button.setIcon(self.style().standardIcon(icon))
        a, b = self._marks["a"], self._marks["b"]
        if self.streamer.a is not None:
            self.ab_label.setText(f"A-B: {clock(a)} - {clock(b)}")
        elif a is not None or b is not None:
            self.ab_label.setText(f"A-B: {clock(a)} - {clock(b)} (set both)")
        else:
            self.ab_label.setText("A-B: off")
