"""Light and dark themes: one palette of named colours, applied as a Qt palette
(for what the Fusion style draws itself) plus one stylesheet (for the rest).

    manager = ThemeManager(app)       # follows the desktop by default
    manager.set_mode("dark")          # "system", "light" or "dark"
    follow(widget, callback)          # widgets that paint themselves repaint
    current()                         # the active Theme, from anywhere

Widgets never hard-code colours: they use object names / properties that the
stylesheet styles (`card`, `pill`, `primary`...) or read `current()`.
"""

from __future__ import annotations

import contextlib
from collections.abc import Callable
from dataclasses import dataclass

from PySide6.QtCore import QObject, Qt, Signal
from PySide6.QtGui import QColor, QGuiApplication, QPalette
from PySide6.QtWidgets import QApplication

MODES = ("system", "light", "dark")


@dataclass(frozen=True)
class Theme:
    name: str
    dark: bool
    window: str  # behind everything
    surface: str  # cards, panels
    surface_alt: str  # inputs, lists, table rows
    hover: str
    border: str
    text: str
    muted: str
    accent: str
    accent_hover: str
    accent_text: str
    success: str
    warning: str
    danger: str
    video: str  # behind the video

    def state_color(self, state: str) -> str:
        return {
            "running": self.success,
            "waiting": self.warning,
            "error": self.danger,
            "connected": self.success,
            "connecting": self.warning,
        }.get(state, self.muted)


DARK = Theme(
    name="dark",
    dark=True,
    window="#0e1116",
    surface="#161a21",
    surface_alt="#1d222b",
    hover="#262c37",
    border="#2a303c",
    text="#e6e9ef",
    muted="#8d97a8",
    accent="#4d8dff",
    accent_hover="#6aa0ff",
    accent_text="#ffffff",
    success="#3fb950",
    warning="#d29922",
    danger="#f85149",
    video="#07090c",
)

LIGHT = Theme(
    name="light",
    dark=False,
    window="#eef1f5",
    surface="#ffffff",
    surface_alt="#f4f6f9",
    hover="#e6ebf2",
    border="#d9dee7",
    text="#1b2230",
    muted="#5e697b",
    accent="#2f6fed",
    accent_hover="#1f5fdc",
    accent_text="#ffffff",
    success="#1a7f37",
    warning="#9a6700",
    danger="#cf222e",
    video="#0c0e12",
)

_current = DARK


def current() -> Theme:
    return _current


def with_alpha(color: str, alpha: float) -> str:
    c = QColor(color)
    return f"rgba({c.red()}, {c.green()}, {c.blue()}, {alpha:.2f})"


def stylesheet(t: Theme) -> str:
    selection = with_alpha(t.accent, 0.28 if t.dark else 0.18)
    return f"""
* {{ outline: 0; }}
QWidget {{ color: {t.text}; font-size: 10pt; }}
QMainWindow, QDialog {{ background: {t.window}; }}
QWidget#central {{ background: {t.window}; }}
QFrame#header {{ background: {t.surface}; border-bottom: 1px solid {t.border}; }}
QToolTip {{
    background: {t.surface}; color: {t.text}; border: 1px solid {t.border};
    border-radius: 6px; padding: 6px 8px;
}}

/* Cards: the panels' building block */
QFrame#card {{ background: {t.surface}; border: 1px solid {t.border}; border-radius: 12px; }}
QLabel#cardTitle {{ font-weight: 600; font-size: 10.5pt; }}
QLabel#cardHint, QLabel#muted {{ color: {t.muted}; }}
QLabel#sectionLabel {{ color: {t.muted}; font-size: 8.5pt; font-weight: 600; letter-spacing: 1px; }}
QLabel#brand {{ font-size: 13pt; font-weight: 700; color: {t.accent}; }}
QLabel#metricValue {{ font-size: 17pt; font-weight: 600; }}
QLabel#metricTitle {{ color: {t.muted}; font-size: 8.5pt; }}
QFrame#metric {{ background: {t.surface_alt}; border: 1px solid {t.border}; border-radius: 10px; }}

/* Pills: small coloured status labels; colour set per instance */
QLabel#pill {{ border-radius: 10px; padding: 2px 10px; font-weight: 600; font-size: 9pt; }}

/* Buttons */
QPushButton, QToolButton {{
    background: {t.surface_alt}; border: 1px solid {t.border}; border-radius: 8px;
    padding: 6px 12px;
}}
QPushButton:hover, QToolButton:hover {{ background: {t.hover}; }}
QPushButton:pressed, QToolButton:pressed {{ background: {t.border}; }}
QPushButton:disabled, QToolButton:disabled {{ color: {t.muted}; background: {t.surface}; }}
QPushButton:checked, QToolButton:checked {{
    background: {selection}; border-color: {t.accent}; color: {t.text};
}}
QPushButton#primary {{
    background: {t.accent}; border-color: {t.accent}; color: {t.accent_text}; font-weight: 600;
}}
QPushButton#primary:hover {{ background: {t.accent_hover}; }}
QPushButton#primary:disabled {{
    background: {t.border}; border-color: {t.border}; color: {t.muted};
}}
QPushButton#danger {{ color: {t.danger}; }}
QToolButton#flat, QPushButton#flat {{
    background: transparent; border-color: transparent; padding: 5px;
}}
QToolButton#flat:hover, QPushButton#flat:hover {{ background: {t.hover}; }}
QToolButton#flat:checked {{ background: {selection}; border-color: transparent; }}
QPushButton#play {{
    background: {t.accent}; border: none; border-radius: 22px; padding: 0;
}}
QPushButton#play:hover {{ background: {t.accent_hover}; }}
QPushButton#play:disabled {{ background: {t.border}; }}
QToolButton::menu-indicator {{ image: none; width: 0; }}

/* Inputs */
QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox {{
    background: {t.surface_alt}; border: 1px solid {t.border}; border-radius: 8px;
    padding: 5px 8px; selection-background-color: {t.accent};
    selection-color: {t.accent_text};
}}
QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus, QComboBox:focus {{
    border-color: {t.accent};
}}
QLineEdit:disabled, QSpinBox:disabled, QDoubleSpinBox:disabled, QComboBox:disabled {{
    color: {t.muted};
}}
QComboBox::drop-down {{ border: none; width: 22px; }}
QComboBox QAbstractItemView {{
    background: {t.surface}; border: 1px solid {t.border}; border-radius: 8px;
    selection-background-color: {selection}; selection-color: {t.text}; padding: 4px;
}}
QSpinBox::up-button, QSpinBox::down-button,
QDoubleSpinBox::up-button, QDoubleSpinBox::down-button {{ border: none; width: 16px; }}
QCheckBox {{ spacing: 8px; }}

/* Sliders */
QSlider::groove:horizontal {{ height: 4px; background: {t.border}; border-radius: 2px; }}
QSlider::sub-page:horizontal {{ background: {t.accent}; border-radius: 2px; }}
QSlider::handle:horizontal {{
    background: {t.surface}; border: 2px solid {t.accent}; width: 12px; height: 12px;
    margin: -6px 0; border-radius: 8px;
}}
QSlider::handle:horizontal:hover {{ background: {t.accent}; }}
QSlider::sub-page:horizontal:disabled {{ background: {t.muted}; }}
QSlider::handle:horizontal:disabled {{ border-color: {t.muted}; }}

/* Lists, trees, tables */
QListWidget, QTreeWidget, QTableWidget {{
    background: {t.surface_alt}; border: 1px solid {t.border}; border-radius: 10px;
    padding: 4px; alternate-background-color: {t.surface};
}}
QListWidget::item, QTreeWidget::item {{ padding: 5px 6px; border-radius: 6px; }}
QListWidget::item:hover, QTreeWidget::item:hover {{ background: {t.hover}; }}
QListWidget::item:selected, QTreeWidget::item:selected,
QTableWidget::item:selected {{ background: {selection}; color: {t.text}; }}
QTableWidget {{ gridline-color: transparent; }}
QTableWidget::item {{ padding: 4px 6px; }}
QHeaderView::section {{
    background: transparent; color: {t.muted}; border: none;
    border-bottom: 1px solid {t.border}; padding: 6px; font-weight: 600;
}}

/* Tabs */
QTabWidget::pane {{ border: none; }}
QTabBar::tab {{
    background: transparent; color: {t.muted}; padding: 8px 10px; margin-right: 0;
    border: none; border-bottom: 2px solid transparent; font-weight: 600;
}}
QTabBar::tab:hover {{ color: {t.text}; }}
QTabBar::tab:selected {{ color: {t.text}; border-bottom-color: {t.accent}; }}

/* Scroll areas and bars */
QScrollArea {{ background: transparent; border: none; }}
QScrollArea > QWidget > QWidget {{ background: transparent; }}
QScrollBar:vertical {{ background: transparent; width: 10px; margin: 2px; }}
QScrollBar:horizontal {{ background: transparent; height: 10px; margin: 2px; }}
QScrollBar::handle {{
    background: {t.border}; border-radius: 4px; min-height: 24px; min-width: 24px;
}}
QScrollBar::handle:hover {{ background: {t.muted}; }}
QScrollBar::add-line, QScrollBar::sub-line {{ width: 0; height: 0; }}
QScrollBar::add-page, QScrollBar::sub-page {{ background: transparent; }}

QSplitter::handle {{ background: transparent; }}
QSplitter::handle:horizontal {{ width: 8px; }}
QSplitter::handle:vertical {{ height: 8px; }}
QStatusBar {{ background: {t.surface}; border-top: 1px solid {t.border}; color: {t.muted}; }}
QStatusBar::item {{ border: none; }}
QMenu {{ background: {t.surface}; border: 1px solid {t.border}; border-radius: 8px; padding: 4px; }}
QMenu::item {{ padding: 6px 20px 6px 12px; border-radius: 6px; }}
QMenu::item:selected {{ background: {selection}; }}
QMenu::separator {{ height: 1px; background: {t.border}; margin: 4px 6px; }}
"""


def palette(t: Theme) -> QPalette:
    p = QPalette()
    roles = {
        QPalette.ColorRole.Window: t.window,
        QPalette.ColorRole.WindowText: t.text,
        QPalette.ColorRole.Base: t.surface_alt,
        QPalette.ColorRole.AlternateBase: t.surface,
        QPalette.ColorRole.Text: t.text,
        QPalette.ColorRole.Button: t.surface_alt,
        QPalette.ColorRole.ButtonText: t.text,
        QPalette.ColorRole.Highlight: t.accent,
        QPalette.ColorRole.HighlightedText: t.accent_text,
        QPalette.ColorRole.ToolTipBase: t.surface,
        QPalette.ColorRole.ToolTipText: t.text,
        QPalette.ColorRole.PlaceholderText: t.muted,
        QPalette.ColorRole.Link: t.accent,
        QPalette.ColorRole.Light: t.hover,
        QPalette.ColorRole.Midlight: t.border,
        QPalette.ColorRole.Mid: t.border,
        QPalette.ColorRole.Dark: t.window,
        QPalette.ColorRole.Shadow: "#000000",
    }
    for role, color in roles.items():
        p.setColor(role, QColor(color))
    for role in (
        QPalette.ColorRole.Text,
        QPalette.ColorRole.ButtonText,
        QPalette.ColorRole.WindowText,
    ):
        p.setColor(QPalette.ColorGroup.Disabled, role, QColor(t.muted))
    return p


class ThemeManager(QObject):
    """Applies the theme to the application and follows the desktop's choice."""

    changed = Signal(object)  # Theme

    def __init__(self, app: QApplication, mode: str = "system"):
        super().__init__(app)
        self.app = app
        self.mode = mode if mode in MODES else "system"
        app.setStyle("Fusion")  # draws consistently on every platform, honours the palette
        app.styleHints().colorSchemeChanged.connect(lambda _: self._apply())
        self._apply()

    def set_mode(self, mode: str) -> None:
        if mode in MODES and mode != self.mode:
            self.mode = mode
            self._apply()

    def toggle(self) -> None:
        self.set_mode("light" if current().dark else "dark")

    def resolved(self) -> Theme:
        if self.mode == "light":
            return LIGHT
        if self.mode == "dark":
            return DARK
        scheme = QGuiApplication.styleHints().colorScheme()
        return LIGHT if scheme == Qt.ColorScheme.Light else DARK  # unknown: dark suits video

    def _apply(self) -> None:
        global _current
        _current = self.resolved()
        self.app.setPalette(palette(_current))
        self.app.setStyleSheet(stylesheet(_current))
        self.changed.emit(_current)


_signals: ThemeManager | None = None


def install(app: QApplication, mode: str = "system") -> ThemeManager:
    """Create the application's ThemeManager (once)."""
    global _signals
    if _signals is None or _signals.app is not app:
        _signals = ThemeManager(app, mode)
    else:
        _signals.set_mode(mode)
    return _signals


def manager() -> ThemeManager | None:
    return _signals


def follow(widget: QObject, callback: Callable[[Theme], None]) -> None:
    """Call `callback(theme)` on every theme change, for as long as `widget` lives.

    (A lambda connected directly would outlive the widget and touch a deleted object.)
    """
    signals = _signals
    if signals is None:
        return

    def relay(t: Theme) -> None:  # a plain function: disconnects even once `widget` is gone
        callback(t)

    signals.changed.connect(relay)

    def forget(*_args) -> None:
        with contextlib.suppress(RuntimeError, TypeError, SystemError):  # already gone
            signals.changed.disconnect(relay)

    widget.destroyed.connect(forget)
