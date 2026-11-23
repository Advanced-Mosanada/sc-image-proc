"""Build the target's pipeline: pick functions, order them, tune their parameters.

Every change is one AIPC call; the panel then redraws from the answer, so it
always shows what the target really runs (and several GUIs stay in sync via
event.config).
"""

from __future__ import annotations

from typing import Any

from PySide6.QtCore import Qt, QTimer
from PySide6.QtGui import QBrush, QColor
from PySide6.QtWidgets import (
    QAbstractItemView,
    QGroupBox,
    QHBoxLayout,
    QLineEdit,
    QListWidget,
    QListWidgetItem,
    QPushButton,
    QScrollArea,
    QSplitter,
    QTreeWidget,
    QTreeWidgetItem,
    QVBoxLayout,
    QWidget,
)

from ..qt_client import QtAipcClient
from .params import ParamEditor

ID_ROLE = Qt.ItemDataRole.UserRole
EDIT_DELAY_MS = 120  # a slider sends at most ~8 updates per second


class FunctionList(QWidget):
    """The functions the target offers, by category."""

    def __init__(self, on_add):
        super().__init__()
        self.on_add = on_add
        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        self.filter = QLineEdit(placeholderText="Filter functions...")
        self.filter.textChanged.connect(self._apply_filter)
        self.tree = QTreeWidget()
        self.tree.setHeaderLabels(["Function", "Title"])
        self.tree.itemDoubleClicked.connect(lambda item, _: self._add(item))
        self.add_button = QPushButton("Add to pipeline")
        self.add_button.clicked.connect(lambda: self._add(self.tree.currentItem()))
        layout.addWidget(self.filter)
        layout.addWidget(self.tree, 1)
        layout.addWidget(self.add_button)

    def set_functions(self, functions: list[dict[str, Any]]) -> None:
        self.tree.clear()
        groups: dict[str, QTreeWidgetItem] = {}
        for function in functions:
            category = function.get("category") or "other"
            if function.get("backend") == "gstreamer":
                category = f"{category} (GStreamer)"
            group = groups.get(category)
            if group is None:
                group = groups[category] = QTreeWidgetItem(self.tree, [category])
                group.setFlags(Qt.ItemFlag.ItemIsEnabled)
            item = QTreeWidgetItem(group, [function["name"], function.get("title", "")])
            item.setData(0, ID_ROLE, function["name"])
            item.setToolTip(0, function.get("description", ""))
            item.setToolTip(1, function.get("description", ""))
        self.tree.sortItems(0, Qt.SortOrder.AscendingOrder)
        self.tree.expandAll()
        self.tree.resizeColumnToContents(0)
        self._apply_filter(self.filter.text())

    def _apply_filter(self, text: str) -> None:
        text = text.lower()
        for i in range(self.tree.topLevelItemCount()):
            group = self.tree.topLevelItem(i)
            visible = 0
            for j in range(group.childCount()):
                item = group.child(j)
                match = text in item.text(0).lower() or text in item.text(1).lower()
                item.setHidden(not match)
                visible += match
            group.setHidden(visible == 0)

    def _add(self, item: QTreeWidgetItem | None) -> None:
        name = item.data(0, ID_ROLE) if item else None
        if name:
            self.on_add(name)


class PipelinePanel(QWidget):
    def __init__(self, client: QtAipcClient, parent: QWidget | None = None):
        super().__init__(parent)
        self.client = client
        self.functions: dict[str, dict[str, Any]] = {}
        self.stages: list[dict[str, Any]] = []
        self.status: dict[int, dict[str, Any]] = {}  # stage id -> stats
        self._pending_params: dict[str, Any] = {}
        self._edit_timer = QTimer(self, singleShot=True, interval=EDIT_DELAY_MS)
        self._edit_timer.timeout.connect(self._send_params)

        self.catalogue = FunctionList(self.add_function)

        self.list = QListWidget()
        self.list.setDragDropMode(QAbstractItemView.DragDropMode.InternalMove)
        self.list.model().rowsMoved.connect(self._rows_moved)
        self.list.itemChanged.connect(self._item_changed)
        self.list.currentItemChanged.connect(lambda *_: self._show_selected())
        self.text = QLineEdit(placeholderText="gray ! blur ksize=5   (Enter to set)")
        self.text.returnPressed.connect(
            lambda: self.call("pipeline.set", {"text": self.text.text()})
        )
        buttons = QHBoxLayout()
        for label, slot in (
            ("Up", lambda: self._move_selected(-1)),
            ("Down", lambda: self._move_selected(+1)),
            ("Remove", self._remove_selected),
            ("Clear", lambda: self.call("pipeline.clear")),
        ):
            button = QPushButton(label)
            button.clicked.connect(slot)
            buttons.addWidget(button)
        stages_box = QGroupBox("Pipeline (check = enabled, drag to reorder)")
        stages_layout = QVBoxLayout(stages_box)
        stages_layout.addWidget(self.list, 1)
        stages_layout.addLayout(buttons)
        stages_layout.addWidget(self.text)

        self.editor = ParamEditor()
        self.editor.changed.connect(self._param_edited)
        editor_scroll = QScrollArea(widgetResizable=True)
        editor_scroll.setWidget(self.editor)
        editor_box = QGroupBox("Parameters")
        QVBoxLayout(editor_box).addWidget(editor_scroll)

        functions_box = QGroupBox("Functions on the target")
        QVBoxLayout(functions_box).addWidget(self.catalogue)

        splitter = QSplitter(Qt.Orientation.Vertical)
        splitter.addWidget(functions_box)
        splitter.addWidget(stages_box)
        splitter.addWidget(editor_box)
        splitter.setSizes([250, 200, 250])
        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.addWidget(splitter)

    # --- from the target ----------------------------------------------------

    def set_functions(self, functions: list[dict[str, Any]]) -> None:
        self.functions = {f["name"]: f for f in functions}
        self.catalogue.set_functions(functions)
        self._show_selected()

    def set_pipeline(self, pipeline: dict[str, Any]) -> None:
        """Redraw from a Pipeline object (protocol section 4)."""
        selected = self.selected_id()
        previous_ids = [s["id"] for s in self.stages]
        self.stages = pipeline.get("stages", [])
        self.list.blockSignals(True)
        self.list.clear()
        for stage in self.stages:
            item = QListWidgetItem()
            item.setData(ID_ROLE, stage["id"])
            item.setFlags(item.flags() | Qt.ItemFlag.ItemIsUserCheckable)
            item.setCheckState(
                Qt.CheckState.Checked if stage["enabled"] else Qt.CheckState.Unchecked
            )
            self.list.addItem(item)
            self._decorate(item)
            if stage["id"] == selected:
                self.list.setCurrentItem(item)
        self.list.blockSignals(False)
        if not self.text.hasFocus():
            self.text.setText(pipeline.get("text", ""))
        new_ids = [s["id"] for s in self.stages if s["id"] not in previous_ids]
        if len(new_ids) == 1:  # just added: edit it
            self.select(new_ids[0])
        elif self.selected_id() == selected and selected is not None:
            self.editor.update_values(self._stage(selected)["params"])
        else:
            self._show_selected()

    def update_status(self, stats: dict[str, Any]) -> None:
        """Per-stage timing and errors from event.stats."""
        self.status = {s["id"]: s for s in stats.get("stages", [])}
        for i in range(self.list.count()):
            self._decorate(self.list.item(i))

    # --- user actions ---------------------------------------------------------

    def call(self, method: str, params: dict[str, Any] | None = None) -> None:
        self.client.call(method, params, on_result=self.set_pipeline)

    def add_function(self, name: str) -> None:
        index = self.list.currentRow() + 1 if self.list.currentItem() else -1
        self.client.call(
            "pipeline.add", {"function": name, "index": index}, on_result=self.set_pipeline
        )

    def selected_id(self) -> int | None:
        item = self.list.currentItem()
        return item.data(ID_ROLE) if item else None

    def select(self, stage_id: int) -> None:
        for i in range(self.list.count()):
            if self.list.item(i).data(ID_ROLE) == stage_id:
                self.list.setCurrentRow(i)
                return

    def _stage(self, stage_id: int | None) -> dict[str, Any] | None:
        return next((s for s in self.stages if s["id"] == stage_id), None)

    def _show_selected(self) -> None:
        self._send_params()  # finish edits of the previous stage first
        stage = self._stage(self.selected_id())
        function = self.functions.get(stage["function"]) if stage else None
        self.editor.show_function(function, stage["params"] if stage else {})

    def _param_edited(self, values: dict[str, Any]) -> None:
        self._pending_params.update(values)
        self._edit_timer.start()

    def _send_params(self) -> None:
        stage_id = self.selected_id()
        if self._pending_params and stage_id is not None:
            self.call("pipeline.update", {"id": stage_id, "params": self._pending_params})
        self._pending_params = {}
        self._edit_timer.stop()

    def _item_changed(self, item: QListWidgetItem) -> None:
        enabled = item.checkState() == Qt.CheckState.Checked
        stage = self._stage(item.data(ID_ROLE))
        if stage is not None and stage["enabled"] != enabled:
            self.call("pipeline.update", {"id": stage["id"], "enabled": enabled})

    def _rows_moved(self, _parent, start, _end, _dest, row) -> None:
        index = row if row < start else row - 1
        item = self.list.item(index)
        if item is not None:
            self.call("pipeline.move", {"id": item.data(ID_ROLE), "index": index})

    def _move_selected(self, delta: int) -> None:
        stage_id = self.selected_id()
        if stage_id is not None:
            index = max(0, self.list.currentRow() + delta)
            self.call("pipeline.move", {"id": stage_id, "index": index})

    def _remove_selected(self) -> None:
        stage_id = self.selected_id()
        if stage_id is not None:
            self.call("pipeline.remove", {"id": stage_id})

    def _decorate(self, item: QListWidgetItem) -> None:
        stage = self._stage(item.data(ID_ROLE))
        if stage is None:
            return
        status = self.status.get(stage["id"], {})
        text = stage["text"]
        if status.get("active"):
            text += f"    {status.get('mean_ms', 0):.1f} ms"
        if status.get("error"):
            text = f"⚠ {text}"
            item.setForeground(QBrush(QColor("#c62828")))
            item.setToolTip(f"bypassed: {status['error']}")
        else:
            item.setForeground(QBrush())
            item.setToolTip(self.functions.get(stage["function"], {}).get("description", ""))
        if not stage["enabled"]:
            item.setForeground(QBrush(QColor("gray")))
        self.list.blockSignals(True)
        item.setText(text)
        self.list.blockSignals(False)
