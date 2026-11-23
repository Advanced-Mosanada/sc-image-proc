"""aipc-gui: control scipd targets over AIPC."""

from __future__ import annotations

import argparse
import signal
import sys

from PySide6.QtWidgets import QApplication

from . import __version__
from .main_window import MainWindow, parse_target


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="aipc-gui", description=__doc__)
    parser.add_argument("--target", help="connect to ADDRESS[:PORT] (else: discover)")
    parser.add_argument("--video", help="open this file in the player")
    parser.add_argument("--version", action="version", version=f"aipc-gui {__version__}")
    args, qt_args = parser.parse_known_args(argv)

    app = QApplication([sys.argv[0], *qt_args])
    app.setApplicationName("aipc-gui")
    signal.signal(signal.SIGINT, signal.SIG_DFL)  # Ctrl+C in the terminal quits

    window = MainWindow()
    window.show()
    if args.video:
        window.player.open(args.video)
    if args.target:
        window.target.setEditText(args.target)
        window.connect_to(*parse_target(args.target))
    else:
        window.discover(auto_connect=True)
    return app.exec()
