"""Shared building blocks: cards, section titles, status pills, metric tiles."""

from __future__ import annotations

from PySide6.QtWidgets import (
    QFrame,
    QHBoxLayout,
    QLabel,
    QSizePolicy,
    QToolButton,
    QVBoxLayout,
    QWidget,
)

from .. import theme
from ..icons import set_icon


class Card(QFrame):
    """A rounded panel with an optional title row (title, hint, actions)."""

    def __init__(self, title: str = "", hint: str = "", parent: QWidget | None = None):
        super().__init__(parent)
        self.setObjectName("card")
        outer = QVBoxLayout(self)
        outer.setContentsMargins(14, 12, 14, 14)
        outer.setSpacing(10)
        self.header = QHBoxLayout()
        self.header.setSpacing(6)
        self.title = QLabel(title)
        self.title.setObjectName("cardTitle")
        self.header.addWidget(self.title)
        self.header.addStretch()
        if title:
            outer.addLayout(self.header)
        self.hint = QLabel(hint)
        self.hint.setObjectName("cardHint")
        self.hint.setWordWrap(True)
        self.hint.setVisible(bool(hint))
        outer.addWidget(self.hint)
        self.body = QVBoxLayout()
        self.body.setSpacing(8)
        outer.addLayout(self.body, 1)

    def add_action(self, button: QToolButton) -> QToolButton:
        self.header.addWidget(button)
        return button


def tool_button(icon_name: str, tip: str, slot=None, checkable: bool = False) -> QToolButton:
    """A flat icon button for card headers and toolbars."""
    button = QToolButton()
    button.setObjectName("flat")
    button.setToolTip(tip)
    button.setCheckable(checkable)
    set_icon(button, icon_name, size=18)
    if slot is not None:
        button.clicked.connect(slot)
    return button


def section_label(text: str) -> QLabel:
    label = QLabel(text.upper())
    label.setObjectName("sectionLabel")
    return label


class Pill(QLabel):
    """A small rounded status label: 'running', 'connected'..."""

    def __init__(self, text: str = "", state: str = "", parent: QWidget | None = None):
        super().__init__(parent)
        self.setObjectName("pill")
        self._state = state
        self.set(text, state)
        theme.follow(self, lambda _t: self._paint())

    def set(self, text: str, state: str = "") -> None:
        self._state = state or text
        self.setText(f"●  {text}")
        self._paint()

    def _paint(self) -> None:
        color = theme.current().state_color(self._state)
        self.setStyleSheet(f"color: {color}; background: {theme.with_alpha(color, 0.14)};")


class Metric(QFrame):
    """A tile with a big number and a caption."""

    def __init__(self, title: str, parent: QWidget | None = None):
        super().__init__(parent)
        self.setObjectName("metric")
        layout = QVBoxLayout(self)
        layout.setContentsMargins(12, 8, 12, 8)
        layout.setSpacing(0)
        self.value = QLabel("-")
        self.value.setObjectName("metricValue")
        self.caption = QLabel(title)
        self.caption.setObjectName("metricTitle")
        layout.addWidget(self.value)
        layout.addWidget(self.caption)
        self.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Fixed)

    def set(self, value: str, color: str | None = None) -> None:
        self.value.setText(value)
        self.value.setStyleSheet(f"color: {color};" if color else "")


def row(*widgets, stretch_last: bool = False, spacing: int = 6) -> QHBoxLayout:
    layout = QHBoxLayout()
    layout.setSpacing(spacing)
    for i, widget in enumerate(widgets):
        if widget is None:
            layout.addStretch()
        else:
            layout.addWidget(widget, 1 if stretch_last and i == len(widgets) - 1 else 0)
    return layout
