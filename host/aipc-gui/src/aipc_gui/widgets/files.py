"""Choose a video on the target (files.list), like a file dialog."""

from __future__ import annotations

from typing import Any

from PySide6.QtCore import Qt
from PySide6.QtWidgets import (
    QDialog,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QListWidget,
    QListWidgetItem,
    QPushButton,
    QVBoxLayout,
    QWidget,
)

from .. import settings, theme
from ..icons import icon
from ..qt_client import QtAipcClient
from .common import tool_button

KIND_ICONS = {"folder": "folder", "video": "film", "image": "pattern"}


def size_text(size: int) -> str:
    for unit in ("bytes", "KB", "MB", "GB"):
        if size < 1024 or unit == "GB":
            return f"{size:.0f} {unit}" if unit == "bytes" else f"{size:.1f} {unit}"
        size /= 1024
    return ""


class TargetFileDialog(QDialog):
    """`chosen` is the path picked on the target, after exec() returns Accepted."""

    def __init__(self, client: QtAipcClient, parent: QWidget | None = None):
        super().__init__(parent)
        self.setWindowTitle("Open a video on the target")
        self.resize(620, 480)
        self.client = client
        self.chosen = ""
        self.folder = ""
        self.parent_folder = ""

        layout = QVBoxLayout(self)
        layout.setSpacing(10)
        bar = QHBoxLayout()
        self.up = tool_button("up", "Up one folder", lambda: self.show_folder(self.parent_folder))
        self.path = QLineEdit()
        self.path.setPlaceholderText("folder on the target")
        self.path.returnPressed.connect(lambda: self.show_folder(self.path.text().strip()))
        bar.addWidget(self.up)
        bar.addWidget(self.path, 1)
        bar.addWidget(
            tool_button("refresh", "Read the folder again", lambda: self.show_folder(self.folder))
        )
        layout.addLayout(bar)

        self.list = QListWidget()
        self.list.itemDoubleClicked.connect(self._activate)
        self.list.currentItemChanged.connect(lambda *_: self._update_buttons())
        layout.addWidget(self.list, 1)
        self.status = QLabel()
        self.status.setObjectName("muted")
        layout.addWidget(self.status)

        buttons = QHBoxLayout()
        hint = QLabel("Videos play on the target; this PC only controls them.")
        hint.setObjectName("muted")
        buttons.addWidget(hint, 1)
        cancel = QPushButton("Cancel")
        cancel.clicked.connect(self.reject)
        self.open_button = QPushButton("Open")
        self.open_button.setObjectName("primary")
        self.open_button.setDefault(True)
        self.open_button.clicked.connect(lambda: self._activate(self.list.currentItem()))
        buttons.addWidget(cancel)
        buttons.addWidget(self.open_button)
        layout.addLayout(buttons)

        self.show_folder(settings.get("target/folder", "", str))

    def show_folder(self, folder: str) -> None:
        """List a folder of the target ("" = where the target suggests)."""
        self.status.setText("reading...")
        self.client.call(
            "files.list",
            {"path": folder} if folder else {},
            on_result=self._show,
            on_error=lambda e: self.status.setText(
                f"<span style='color:{theme.current().danger}'>{e.message}</span>"
            ),
        )

    def _show(self, listing: dict[str, Any]) -> None:
        self.folder, self.parent_folder = listing["path"], listing.get("parent", "")
        self.path.setText(self.folder)
        self.up.setEnabled(bool(self.parent_folder))
        self.list.clear()
        muted = theme.current().muted
        for entry in listing["entries"]:
            kind = entry["kind"]
            text = entry["name"] + ("/" if kind == "folder" else "")
            item = QListWidgetItem(icon(KIND_ICONS.get(kind, "file"), muted), text)
            item.setData(Qt.ItemDataRole.UserRole, entry)
            if kind != "folder":
                item.setToolTip(f"{entry['path']}  ({size_text(entry['size'])})")
            self.list.addItem(item)
        videos = sum(1 for e in listing["entries"] if e["kind"] != "folder")
        self.status.setText(f"{videos} video(s) or image(s)" if videos else "no videos here")
        self._update_buttons()

    def _activate(self, item: QListWidgetItem | None) -> None:
        if item is None:
            return
        entry = item.data(Qt.ItemDataRole.UserRole)
        if entry["kind"] == "folder":
            self.show_folder(entry["path"])
            return
        self.chosen = entry["path"]
        settings.put("target/folder", self.folder)
        self.accept()

    def _update_buttons(self) -> None:
        item = self.list.currentItem()
        self.open_button.setEnabled(item is not None)
        if item is not None:
            folder = item.data(Qt.ItemDataRole.UserRole)["kind"] == "folder"
            self.open_button.setText("Enter" if folder else "Open")
