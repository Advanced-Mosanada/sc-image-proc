"""Dark and light themes: switching, remembering, icons that follow."""

from __future__ import annotations

from aipc_gui import settings, theme
from aipc_gui.main_window import MainWindow


def test_switches_and_remembers_the_theme(qtbot):
    window = MainWindow()
    qtbot.addWidget(window)
    assert theme.current().dark  # the tests start dark

    window.set_theme("light")
    assert not theme.current().dark
    assert settings.get("theme") == "light"
    assert window.theme_actions["light"].isChecked()
    assert window.theme_button.property("_icon")[0] == "moon"  # offers the other theme

    window.toggle_theme()
    assert theme.current().dark
    assert settings.get("theme") == "dark"
    assert window.theme_button.property("_icon")[0] == "sun"


def test_every_theme_has_a_colour_for_each_state():
    for t in (theme.DARK, theme.LIGHT):
        assert t.state_color("running") == t.success
        assert t.state_color("error") == t.danger
        assert t.state_color("anything else") == t.muted
        assert "QFrame#card" in theme.stylesheet(t)
