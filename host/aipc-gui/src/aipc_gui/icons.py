"""Small line icons drawn as SVG and coloured with the theme.

    set_icon(button, "play")                 # recoloured when the theme changes
    set_icon(button, "play", role="accent_text")

Drawn for this project on a 24x24 grid, 2 px strokes, round ends.
"""

from __future__ import annotations

from PySide6.QtCore import QByteArray, QRectF, QSize, Qt
from PySide6.QtGui import QIcon, QPainter, QPixmap
from PySide6.QtSvg import QSvgRenderer
from PySide6.QtWidgets import QAbstractButton

from . import theme

# Shapes inside <svg viewBox="0 0 24 24">; {c} is the colour.
_STROKE = 'fill="none" stroke="{c}" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"'
_FILL = 'fill="{c}" stroke="{c}" stroke-width="1.5" stroke-linejoin="round"'

SHAPES: dict[str, str] = {
    "play": f'<path {_FILL} d="M8 5.5v13l10.5-6.5z"/>',
    "pause": f'<rect {_FILL} x="6.5" y="5.5" width="3.5" height="13" rx="1"/>'
    f'<rect {_FILL} x="14" y="5.5" width="3.5" height="13" rx="1"/>',
    "stop": f'<rect {_FILL} x="6.5" y="6.5" width="11" height="11" rx="2"/>',
    "to-start": f'<path {_STROKE} d="M6 5v14"/><path {_FILL} d="M18 5.5v13L9 12z"/>',
    "rewind": f'<path {_FILL} d="M11.5 6.5v11L4 12z"/><path {_FILL} d="M20 6.5v11L12.5 12z"/>',
    "forward": f'<path {_FILL} d="M12.5 6.5v11L20 12z"/><path {_FILL} d="M4 6.5v11L11.5 12z"/>',
    "loop": f'<path {_STROKE} d="M17 3l3 3-3 3"/><path {_STROKE} d="M4 12V10a4 4 0 0 1 4-4h12"/>'
    f'<path {_STROKE} d="M7 21l-3-3 3-3"/><path {_STROKE} d="M20 12v2a4 4 0 0 1-4 4H4"/>',
    "search": f'<circle {_STROKE} cx="11" cy="11" r="6.5"/><path {_STROKE} d="M16 16l4.5 4.5"/>',
    "power": f'<path {_STROKE} d="M7.5 6.5a7.5 7.5 0 1 0 9 0"/><path {_STROKE} d="M12 3v8"/>',
    "radar": f'<circle {_FILL} cx="12" cy="12" r="1.6"/>'
    f'<path {_STROKE} d="M8.5 15.5a5 5 0 0 1 0-7M15.5 8.5a5 5 0 0 1 0 7"/>'
    f'<path {_STROKE} d="M5.6 18.4a9 9 0 0 1 0-12.8M18.4 5.6a9 9 0 0 1 0 12.8"/>',
    "sun": f'<circle {_STROKE} cx="12" cy="12" r="4"/>'
    f'<path {_STROKE} d="M12 2.5v2M12 19.5v2M2.5 12h2M19.5 12h2M5.3 5.3l1.4 1.4'
    'M17.3 17.3l1.4 1.4M5.3 18.7l1.4-1.4M17.3 6.7l1.4-1.4"/>',
    "moon": f'<path {_STROKE} d="M20 14.5A8.5 8.5 0 0 1 9.5 4 8.5 8.5 0 1 0 20 14.5z"/>',
    "monitor": f'<rect {_STROKE} x="3" y="4" width="18" height="12" rx="2"/>'
    f'<path {_STROKE} d="M8 20h8M12 16v4"/>',
    "plus": f'<path {_STROKE} d="M12 5v14M5 12h14"/>',
    "close": f'<path {_STROKE} d="M6 6l12 12M18 6L6 18"/>',
    "trash": f'<path {_STROKE} d="M4 7h16M10 11v6M14 11v6M9 7V4.5h6V7"/>'
    f'<path {_STROKE} d="M6 7l1 13h10l1-13"/>',
    "up": f'<path {_STROKE} d="M6 15l6-6 6 6"/>',
    "down": f'<path {_STROKE} d="M6 9l6 6 6-6"/>',
    "folder": f'<path {_STROKE} d="M3 7.5A2 2 0 0 1 5 5.5h4l2 2h8a2 2 0 0 1 2 2V17a2 2 0 0 1-2 '
    '2H5a2 2 0 0 1-2-2z"/>',
    "copy": f'<rect {_STROKE} x="8" y="8" width="12" height="12" rx="2"/>'
    f'<path {_STROKE} d="M16 8V6a2 2 0 0 0-2-2H6a2 2 0 0 0-2 2v8a2 2 0 0 0 2 2h2"/>',
    "reset": f'<path {_STROKE} d="M8 5L4 9l4 4"/><path {_STROKE} d="M4 9h10.5a5.5 5.5 0 0 1 0 '
    '11H9"/>',
    "refresh": f'<path {_STROKE} d="M20 11a8 8 0 1 0-2.3 5.7"/>'
    f'<path {_STROKE} d="M20 4.5V11h-6.5"/>',
    "send": f'<path {_STROKE} d="M21 3L10 14"/><path {_STROKE} d="M21 3l-7 18-4-7-7-4z"/>',
    "record": f'<circle {_STROKE} cx="12" cy="12" r="8"/><circle {_FILL} cx="12" cy="12" r="3.5"/>',
    "film": f'<rect {_STROKE} x="3" y="4" width="18" height="16" rx="2"/>'
    f'<path {_STROKE} d="M7 4v16M17 4v16M3 9h4M3 15h4M17 9h4M17 15h4"/>',
    "camera": f'<path {_STROKE} d="M3 8a2 2 0 0 1 2-2h2.5L9 4h6l1.5 2H19a2 2 0 0 1 2 2v9a2 2 0 0 '
    '1-2 2H5a2 2 0 0 1-2-2z"/>'
    f'<circle {_STROKE} cx="12" cy="12.5" r="3.5"/>',
    "pattern": f'<rect {_STROKE} x="3.5" y="3.5" width="17" height="17" rx="2"/>'
    f'<path {_FILL} d="M3.5 3.5h8.5v8.5H3.5zM12 12h8.5v8.5H12z"/>',
    "network": f'<circle {_STROKE} cx="12" cy="12" r="9"/>'
    f'<path {_STROKE} d="M3 12h18M12 3a14 14 0 0 1 0 18M12 3a14 14 0 0 0 0 18"/>',
    "file": f'<path {_STROKE} d="M14 3H7a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h10a2 2 0 0 0 2-2V8z"/>'
    f'<path {_STROKE} d="M14 3v5h5"/>',
    "info": f'<circle {_STROKE} cx="12" cy="12" r="9"/><path {_STROKE} d="M12 11v6"/>'
    f'<circle {_FILL} cx="12" cy="7.5" r="0.6"/>',
    "sliders": f'<path {_STROKE} d="M4 6h10M18 6h2M4 12h4M12 12h8M4 18h12M20 18h0"/>'
    f'<circle {_STROKE} cx="16" cy="6" r="2"/><circle {_STROKE} cx="10" cy="12" r="2"/>'
    f'<circle {_STROKE} cx="18" cy="18" r="2"/>',
    "chart": f'<path {_STROKE} d="M4 20V10M10 20V4M16 20v-7M22 20H2"/>',
}


def svg(name: str, color: str) -> bytes:
    shapes = SHAPES[name].replace("{c}", color)
    return f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24">{shapes}</svg>'.encode()


def pixmap(name: str, color: str, size: int, ratio: float = 2.0) -> QPixmap:
    renderer = QSvgRenderer(QByteArray(svg(name, color)))
    image = QPixmap(int(size * ratio), int(size * ratio))
    image.fill(Qt.GlobalColor.transparent)
    painter = QPainter(image)
    renderer.render(painter, QRectF(0, 0, image.width(), image.height()))
    painter.end()
    image.setDevicePixelRatio(ratio)
    return image


def icon(name: str, color: str | None = None) -> QIcon:
    t = theme.current()
    result = QIcon()
    for size in (16, 20, 24, 32):
        result.addPixmap(pixmap(name, color or t.text, size), QIcon.Mode.Normal)
        result.addPixmap(pixmap(name, t.muted, size), QIcon.Mode.Disabled)
    return result


def set_icon(button: QAbstractButton, name: str, role: str = "text", size: int = 18) -> None:
    """Give `button` an icon in the theme colour `role`, kept up to date."""
    button.setProperty("_icon", (name, role))
    button.setIcon(icon(name, getattr(theme.current(), role)))
    button.setIconSize(QSize(size, size))
    manager = theme.manager()
    if manager is not None and not button.property("_iconBound"):
        button.setProperty("_iconBound", True)
        theme.follow(button, lambda _t, b=button: _refresh(b))


def _refresh(button: QAbstractButton) -> None:
    try:
        name, role = button.property("_icon")
    except (RuntimeError, TypeError):  # deleted, or icon removed
        return
    button.setIcon(icon(name, getattr(theme.current(), role)))
