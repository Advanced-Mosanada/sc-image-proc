"""Widgets generated from ParamSpecs (protocol section 4), so a function added on
the target shows up here with proper controls without touching the GUI.

    bool   -> check box
    int    -> spin box (+ slider when it has a range), snapped to min + n*step
    float  -> spin box (+ slider when it has a range)
    enum   -> combo box
    string -> text field (applied on Enter / focus out)
"""

from __future__ import annotations

import math
from typing import Any

from PySide6.QtCore import Qt, Signal
from PySide6.QtWidgets import (
    QCheckBox,
    QComboBox,
    QDoubleSpinBox,
    QFormLayout,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QPushButton,
    QSlider,
    QSpinBox,
    QVBoxLayout,
    QWidget,
)

from ..icons import set_icon

INT_LIMIT = 2**31 - 1
SLIDER_STEPS = 1000


def tooltip(spec: dict[str, Any]) -> str:
    lines = [spec.get("description", "")]
    if "min" in spec:
        lines.append(f"range {spec['min']} .. {spec['max']}")
    if "step" in spec and spec["type"] == "int":
        lines.append(f"step {spec['step']}")
    lines.append(f"default {spec.get('default')}")
    return "\n".join(line for line in lines if line)


class Field(QWidget):
    """One parameter's control. Emits `edited(value)` on user changes only."""

    edited = Signal(object)

    def __init__(self, spec: dict[str, Any]):
        super().__init__()
        self.spec = spec
        layout = QHBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        self.setToolTip(tooltip(spec))

    def value(self) -> Any:
        raise NotImplementedError

    def set_value(self, value: Any) -> None:
        """Show a value without emitting `edited`."""
        raise NotImplementedError


class BoolField(Field):
    def __init__(self, spec):
        super().__init__(spec)
        self.box = QCheckBox()
        self.box.toggled.connect(lambda on: self.edited.emit(on))
        self.layout().addWidget(self.box)
        self.layout().addStretch()

    def value(self):
        return self.box.isChecked()

    def set_value(self, value):
        self.box.blockSignals(True)
        self.box.setChecked(bool(value))
        self.box.blockSignals(False)


class EnumField(Field):
    def __init__(self, spec):
        super().__init__(spec)
        self.combo = QComboBox()
        self.combo.addItems([str(c) for c in spec.get("choices", [])])
        self.combo.currentTextChanged.connect(lambda text: self.edited.emit(text))
        self.layout().addWidget(self.combo, 1)

    def value(self):
        return self.combo.currentText()

    def set_value(self, value):
        self.combo.blockSignals(True)
        self.combo.setCurrentText(str(value))
        self.combo.blockSignals(False)


class TextField(Field):
    def __init__(self, spec):
        super().__init__(spec)
        self.line = QLineEdit()
        self.line.editingFinished.connect(self._finished)
        self.layout().addWidget(self.line, 1)
        self._last = ""

    def _finished(self):
        if self.line.text() != self._last:
            self._last = self.line.text()
            self.edited.emit(self._last)

    def value(self):
        return self.line.text()

    def set_value(self, value):
        self._last = str(value)
        self.line.setText(self._last)


class NumberField(Field):
    """Spin box, plus a slider when the value has a range."""

    def __init__(self, spec):
        super().__init__(spec)
        self.integer = spec["type"] == "int"
        self.ranged = "min" in spec and "max" in spec
        self.lo = spec.get("min", -INT_LIMIT if self.integer else -1e9)
        self.hi = spec.get("max", INT_LIMIT if self.integer else 1e9)
        self.step = spec.get("step", 0)
        if self.integer:
            self.spin = QSpinBox()
            self.spin.setRange(int(self.lo), int(self.hi))
            self.spin.setSingleStep(int(self.step) or 1)
        else:
            self.spin = QDoubleSpinBox()
            self.spin.setDecimals(3)
            self.spin.setRange(self.lo, self.hi)
            default_step = (self.hi - self.lo) / 100 if self.ranged else 0.1
            self.spin.setSingleStep(self.step or default_step)
        if spec.get("unit"):
            self.spin.setSuffix(f" {spec['unit']}")
        self.spin.setMinimumWidth(110)  # room for the value and its unit
        self.spin.setKeyboardTracking(False)  # typing "15" is one edit, not "1" then "15"
        self.spin.valueChanged.connect(self._spun)
        self.layout().addWidget(self.spin)

        self.slider = None
        if self.ranged:
            self.slider = QSlider(Qt.Orientation.Horizontal)
            self.slider.setRange(0, self._slider_steps())
            self.slider.valueChanged.connect(self._slid)
            self.layout().addWidget(self.slider, 1)

    def _slider_steps(self) -> int:
        if self.integer:
            return int((self.hi - self.lo) // (self.step or 1))
        return SLIDER_STEPS

    def _from_slider(self, position: int) -> float:
        if self.integer:
            return self.lo + position * (self.step or 1)
        return self.lo + (self.hi - self.lo) * position / SLIDER_STEPS

    def _to_slider(self, value: float) -> int:
        if self.integer:
            return round((value - self.lo) / (self.step or 1))
        return round((value - self.lo) / (self.hi - self.lo) * SLIDER_STEPS)

    def snap(self, value: float) -> float:
        """Valid values only: ksize min 3 step 2 -> 3, 5, 7..."""
        if self.integer and self.step and self.ranged:
            n = math.floor((value - self.lo) / self.step + 0.5)
            value = self.lo + n * self.step
        return min(max(value, self.lo), self.hi)

    def _spun(self, value):
        snapped = self.snap(value)
        if snapped != value:
            self.set_value(snapped)
        else:
            self._sync_slider(snapped)
        self.edited.emit(self.value())

    def _slid(self, position):
        value = self.snap(self._from_slider(position))
        self.spin.blockSignals(True)
        self.spin.setValue(value)
        self.spin.blockSignals(False)
        self.edited.emit(self.value())

    def _sync_slider(self, value):
        if self.slider is not None:
            self.slider.blockSignals(True)
            self.slider.setValue(self._to_slider(value))
            self.slider.blockSignals(False)

    def value(self):
        return int(self.spin.value()) if self.integer else float(self.spin.value())

    def set_value(self, value):
        self.spin.blockSignals(True)
        self.spin.setValue(value)
        self.spin.blockSignals(False)
        self._sync_slider(value)


FIELDS = {"bool": BoolField, "int": NumberField, "float": NumberField, "enum": EnumField}


def make_field(spec: dict[str, Any]) -> Field:
    return FIELDS.get(spec["type"], TextField)(spec)


class ParamEditor(QWidget):
    """The parameters of one stage. `changed` carries only what the user edited."""

    changed = Signal(dict)

    def __init__(self, parent: QWidget | None = None):
        super().__init__(parent)
        outer = QVBoxLayout(self)
        outer.setContentsMargins(0, 0, 0, 0)
        outer.setSpacing(6)
        self.title = QLabel()
        self.title.setObjectName("cardTitle")
        self.title.setWordWrap(True)
        self.description = QLabel()
        self.description.setObjectName("muted")
        self.description.setWordWrap(True)
        outer.addWidget(self.title)
        outer.addWidget(self.description)
        self.form = QFormLayout()
        self.form.setContentsMargins(0, 6, 0, 0)
        self.form.setHorizontalSpacing(12)
        self.form.setVerticalSpacing(8)
        self.form.setLabelAlignment(Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter)
        outer.addLayout(self.form)
        self.reset = QPushButton("Reset to defaults")
        self.reset.setObjectName("flat")
        set_icon(self.reset, "reset", size=16)
        self.reset.clicked.connect(self._reset)
        reset_row = QHBoxLayout()
        reset_row.addStretch()
        reset_row.addWidget(self.reset)
        outer.addLayout(reset_row)
        outer.addStretch()
        self.fields: dict[str, Field] = {}
        self.specs: list[dict[str, Any]] = []
        self.show_function(None, {})

    def show_function(self, function: dict[str, Any] | None, values: dict[str, Any]) -> None:
        """Rebuild for a function (from functions.list) with the stage's values."""
        while self.form.rowCount():
            self.form.removeRow(0)
        self.fields.clear()
        self.specs = function.get("params", []) if function else []
        if function is None:
            self.title.setText("No stage selected")
            self.description.setText("Select a stage in the pipeline to tune its parameters.")
        else:
            backend = "  \u00b7  GStreamer" if function.get("backend") == "gstreamer" else ""
            self.title.setText(f"{function.get('title') or function['name']}{backend}")
            self.description.setText(function.get("description", ""))
            if not self.specs:
                self.description.setText(
                    f"{function.get('description', '')}\n\nThis function has no parameters."
                )
        for spec in self.specs:
            field = make_field(spec)
            field.set_value(values.get(spec["name"], spec.get("default")))
            field.edited.connect(lambda value, name=spec["name"]: self.changed.emit({name: value}))
            label = QLabel(spec["name"])
            label.setToolTip(field.toolTip())
            self.form.addRow(label, field)
            self.fields[spec["name"]] = field
        self.reset.setVisible(bool(self.specs))

    def update_values(self, values: dict[str, Any]) -> None:
        """Show new values (from the target) without rebuilding or emitting."""
        for spec in self.specs:
            field = self.fields.get(spec["name"])
            if field is not None and not field.hasFocus():
                field.set_value(values.get(spec["name"], spec.get("default")))

    def values(self) -> dict[str, Any]:
        return {name: field.value() for name, field in self.fields.items()}

    def _reset(self) -> None:
        defaults = {spec["name"]: spec.get("default") for spec in self.specs}
        self.update_values(defaults)
        self.changed.emit(defaults)
