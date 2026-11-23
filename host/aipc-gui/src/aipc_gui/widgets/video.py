"""Shows frames scaled to fit, with a caption."""

from __future__ import annotations

import time

from PySide6.QtCore import QRectF, Qt
from PySide6.QtGui import QColor, QImage, QPainter
from PySide6.QtWidgets import QSizePolicy, QWidget

from ..media import Frame


class VideoView(QWidget):
    def __init__(self, title: str, parent: QWidget | None = None):
        super().__init__(parent)
        self.title = title
        self.image: QImage | None = None
        self.placeholder = "no video"
        self._times: list[float] = []
        self.setMinimumSize(240, 160)
        self.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Expanding)

    def show_frame(self, frame: Frame) -> None:
        image = QImage(
            frame.data, frame.width, frame.height, frame.stride, QImage.Format.Format_RGBX8888
        )
        self.image = image.copy()  # own the pixels; `frame.data` goes away
        now = time.monotonic()
        self._times = [t for t in self._times if now - t < 1.0] + [now]
        self.update()

    def clear(self, placeholder: str = "no video") -> None:
        self.image = None
        self.placeholder = placeholder
        self._times.clear()
        self.update()

    @property
    def fps(self) -> int:
        now = time.monotonic()
        return sum(1 for t in self._times if now - t < 1.0)

    def paintEvent(self, _event) -> None:
        painter = QPainter(self)
        painter.fillRect(self.rect(), QColor("#111"))
        if self.image is not None:
            size = self.image.size().scaled(self.size(), Qt.AspectRatioMode.KeepAspectRatio)
            x = (self.width() - size.width()) / 2
            y = (self.height() - size.height()) / 2
            painter.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform)
            painter.drawImage(QRectF(x, y, size.width(), size.height()), self.image)
            caption = f"{self.title}  {self.image.width()}x{self.image.height()}  {self.fps} fps"
        else:
            painter.setPen(QColor("#888"))
            painter.drawText(self.rect(), Qt.AlignmentFlag.AlignCenter, self.placeholder)
            caption = self.title
        painter.setPen(QColor("#eee"))
        painter.drawText(8, 18, caption)
