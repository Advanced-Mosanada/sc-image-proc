"""Parameter widgets generated from ParamSpecs."""

from __future__ import annotations

from aipc_gui.widgets.params import NumberField, ParamEditor

BLUR = {
    "name": "blur",
    "title": "Gaussian blur",
    "description": "Smooth the image.",
    "backend": "native",
    "params": [
        {
            "name": "ksize",
            "type": "int",
            "default": 3,
            "min": 3,
            "max": 31,
            "step": 2,
            "description": "kernel size",
        },
        {
            "name": "sigma",
            "type": "float",
            "default": 0.0,
            "min": 0.0,
            "max": 20.0,
            "description": "0 = from ksize",
        },
    ],
}
THRESHOLD = {
    "name": "threshold",
    "params": [
        {"name": "mode", "type": "enum", "default": "fixed", "choices": ["fixed", "otsu"]},
        {"name": "invert", "type": "bool", "default": False},
        {"name": "label", "type": "string", "default": ""},
    ],
}


def test_builds_fields_with_current_values(qtbot):
    editor = ParamEditor()
    qtbot.addWidget(editor)
    editor.show_function(BLUR, {"ksize": 7})
    assert editor.values() == {"ksize": 7, "sigma": 0.0}
    assert isinstance(editor.fields["ksize"], NumberField)
    assert editor.fields["ksize"].slider is not None


def test_emits_only_user_edits_snapped_to_the_step(qtbot):
    editor = ParamEditor()
    qtbot.addWidget(editor)
    editor.show_function(BLUR, {})
    edits = []
    editor.changed.connect(edits.append)

    editor.update_values({"ksize": 9})  # from the target: no echo back
    assert edits == []

    editor.fields["ksize"].spin.setValue(10)  # not 3 + n*2
    assert edits[-1] == {"ksize": 11}
    editor.fields["sigma"].slider.setValue(500)  # middle of 0..20
    assert edits[-1] == {"sigma": 10.0}


def test_other_types_and_reset(qtbot):
    editor = ParamEditor()
    qtbot.addWidget(editor)
    editor.show_function(THRESHOLD, {"mode": "otsu", "invert": True})
    assert editor.values() == {"mode": "otsu", "invert": True, "label": ""}
    edits = []
    editor.changed.connect(edits.append)
    editor.fields["mode"].combo.setCurrentText("fixed")
    assert edits[-1] == {"mode": "fixed"}
    editor.reset.click()
    assert edits[-1] == {"mode": "fixed", "invert": False, "label": ""}
    assert editor.values()["invert"] is False
