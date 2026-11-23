"""What the GUI remembers between runs (theme, last target, window layout...).

Stored with QSettings: ~/.config/scip/aipc-gui.conf on Linux.
"""

from __future__ import annotations

from typing import Any

from PySide6.QtCore import QSettings


def settings() -> QSettings:
    return QSettings("scip", "aipc-gui")


def get(key: str, default: Any = None, type_: type | None = None) -> Any:
    value = settings().value(key, default)
    if type_ is bool and isinstance(value, str):  # INI files store booleans as text
        return value.lower() == "true"
    if type_ is not None and value is not None and not isinstance(value, type_):
        try:
            return type_(value)
        except (TypeError, ValueError):
            return default
    return value


def put(key: str, value: Any) -> None:
    settings().setValue(key, value)
