"""Shows frames scaled to fit, in a rounded frame, with overlaid captions."""

from __future__ import annotations

import time

from PySide6.QtCore import QRectF, Qt, Signal
from PySide6.QtGui import QColor, QFont, QImage, QPainter, QPainterPath
from PySide6.QtWidgets import QSizePolicy, QWidget

from .. import theme
from ..icons import pixmap
from ..media import Frame

RADIUS = 12.0


class VideoView(QWidget):
    """`dropped` carries the path of a file dropped on the view (if accepting)."""

    dropped = Signal(str)

    def __init__(self, title: str, icon: str = "film", parent: QWidget | None = None):
        super().__init__(parent)
        self.title = title
        self.icon = icon
        self.image: QImage | None = None
        self.placeholder = "no video"
        self.hint = ""
        self._times: list[float] = []
        self.setMinimumSize(240, 160)
        self.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Expanding)
        theme.follow(self, lambda _t: self.update())

    # --- frames -----------------------------------------------------------------

    def show_frame(self, frame: Frame) -> None:
        image = QImage(
            frame.data, frame.width, frame.height, frame.stride, QImage.Format.Format_RGBX8888
        )
        self.image = image.copy()  # own the pixels; `frame.data` goes away
        now = time.monotonic()
        self._times = [t for t in self._times if now - t < 1.0] + [now]
        self.update()

    def clear(self, placeholder: str = "no video", hint: str = "") -> None:
        self.image = None
        self.placeholder = placeholder
        self.hint = hint
        self._times.clear()
        self.update()

    @property
    def fps(self) -> int:
        now = time.monotonic()
        return sum(1 for t in self._times if now - t < 1.0)

    @property
    def live(self) -> bool:
        return bool(self._times) and time.monotonic() - self._times[-1] < 0.5

    # --- drag and drop ---------------------------------------------------------

    def accept_files(self, hint: str) -> None:
        """Let the user drop a file here; `hint` is shown while empty."""
        self.setAcceptDrops(True)
        self.hint = hint
        self.update()

    def dragEnterEvent(self, event) -> None:
        if event.mimeData().hasUrls():
            event.acceptProposedAction()

    def dropEvent(self, event) -> None:
        for url in event.mimeData().urls():
            if url.isLocalFile():
                self.dropped.emit(url.toLocalFile())
                break

    # --- painting -----------------------------------------------------------------

    def paintEvent(self, _event) -> None:
        t = theme.current()
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        frame = QRectF(self.rect()).adjusted(0.5, 0.5, -0.5, -0.5)
        clip = QPainterPath()
        clip.addRoundedRect(frame, RADIUS, RADIUS)
        painter.setClipPath(clip)
        painter.fillRect(self.rect(), QColor(t.video))

        if self.image is not None:
            size = self.image.size().scaled(self.size(), Qt.AspectRatioMode.KeepAspectRatio)
            x = (self.width() - size.width()) / 2
            y = (self.height() - size.height()) / 2
            painter.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform)
            painter.drawImage(QRectF(x, y, size.width(), size.height()), self.image)
        else:
            self._paint_empty(painter, t)

        painter.setClipping(False)
        self._pill(painter, 12, 12, self.title, left=True)
        if self.image is not None:
            details = f"{self.image.width()}×{self.image.height()}   {self.fps} fps"
            self._pill(
                painter,
                self.width() - 12,
                12,
                details,
                left=False,
                dot=t.success if self.live else t.muted,
            )
        painter.setPen(QColor(t.border))
        painter.setBrush(Qt.BrushStyle.NoBrush)
        painter.drawRoundedRect(frame, RADIUS, RADIUS)

    def _paint_empty(self, painter: QPainter, t: theme.Theme) -> None:
        center_y = self.height() / 2
        painter.setOpacity(0.55)
        icon = pixmap(self.icon, "#9aa3b2", 40)
        painter.drawPixmap(int(self.width() / 2 - 20), int(center_y - 48), icon)
        painter.setOpacity(1.0)
        painter.setPen(QColor("#c9ced8"))
        font = QFont(self.font())
        font.setPointSizeF(font.pointSizeF() + 1)
        font.setWeight(QFont.Weight.DemiBold)
        painter.setFont(font)
        painter.drawText(
            QRectF(0, center_y, self.width(), 24), Qt.AlignmentFlag.AlignCenter, self.placeholder
        )
        if self.hint:
            painter.setPen(QColor("#8a93a3"))
            painter.setFont(self.font())
            painter.drawText(
                QRectF(20, center_y + 24, self.width() - 40, 40),
                Qt.AlignmentFlag.AlignHCenter | Qt.TextFlag.TextWordWrap,
                self.hint,
            )

    def _pill(
        self, painter: QPainter, x: float, y: float, text: str, left: bool, dot: str | None = None
    ) -> None:
        font = QFont(self.font())
        font.setPointSizeF(max(7.5, font.pointSizeF() - 1))
        font.setWeight(QFont.Weight.DemiBold)
        painter.setFont(font)
        metrics = painter.fontMetrics()
        pad = 9
        dot_space = 12 if dot else 0
        width = metrics.horizontalAdvance(text) + 2 * pad + dot_space
        height = metrics.height() + 8
        box = QRectF(x if left else x - width, y, width, height)
        painter.setPen(Qt.PenStyle.NoPen)
        painter.setBrush(QColor(0, 0, 0, 150))
        painter.drawRoundedRect(box, height / 2, height / 2)
        if dot:
            painter.setBrush(QColor(dot))
            painter.drawEllipse(QRectF(box.left() + pad, box.center().y() - 3.5, 7, 7))
        painter.setPen(QColor("#f1f3f6"))
        painter.drawText(
            box.adjusted(pad + dot_space, 0, -pad, 0),
            Qt.AlignmentFlag.AlignVCenter | Qt.AlignmentFlag.AlignLeft,
            text,
        )
