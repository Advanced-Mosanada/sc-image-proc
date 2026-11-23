"""Build the target's pipeline: pick functions, order them, tune their parameters.

Every change is one AIPC call; the panel then redraws from the answer, so it
always shows what the target really runs (and several GUIs stay in sync via
event.config).
"""

from __future__ import annotations

from typing import Any

from PySide6.QtCore import QRectF, QSize, Qt, QTimer
from PySide6.QtGui import QAction, QColor, QFont, QGuiApplication, QKeySequence, QPainter
from PySide6.QtWidgets import (
    QAbstractItemView,
    QLabel,
    QLineEdit,
    QListWidget,
    QListWidgetItem,
    QMenu,
    QScrollArea,
    QSplitter,
    QStyledItemDelegate,
    QTreeWidget,
    QTreeWidgetItem,
    QVBoxLayout,
    QWidget,
)

from .. import theme
from ..icons import icon
from ..qt_client import QtAipcClient
from .common import Card, tool_button
from .params import ParamEditor

ID_ROLE = Qt.ItemDataRole.UserRole
BADGE_ROLE = Qt.ItemDataRole.UserRole + 1  # (text, kind): kind "time" | "error" | "off"
EDIT_DELAY_MS = 120  # a slider sends at most ~8 updates per second


class FunctionList(QWidget):
    """The functions the target offers, by category, with a search box."""

    def __init__(self, on_add):
        super().__init__()
        self.on_add = on_add
        self.functions: list[dict[str, Any]] = []
        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        self.filter = QLineEdit(placeholderText="Search functions")
        self.filter.setClearButtonEnabled(True)
        self.search_icon = self.filter.addAction(
            icon("search", theme.current().muted), QLineEdit.ActionPosition.LeadingPosition
        )
        self.filter.textChanged.connect(self._apply_filter)
        self.tree = QTreeWidget()
        self.tree.setHeaderHidden(True)
        self.tree.setColumnCount(2)
        self.tree.setRootIsDecorated(False)
        self.tree.setIndentation(8)
        self.tree.itemDoubleClicked.connect(lambda item, _: self._add(item))
        layout.addWidget(self.filter)
        layout.addWidget(self.tree, 1)
        theme.follow(self, self._recolor)

    def _recolor(self, t: theme.Theme) -> None:
        self.search_icon.setIcon(icon("search", t.muted))
        self.set_functions(self.functions)

    def set_functions(self, functions: list[dict[str, Any]]) -> None:
        self.functions = functions
        t = theme.current()
        self.tree.clear()
        groups: dict[str, QTreeWidgetItem] = {}
        for function in sorted(functions, key=lambda f: f["name"]):
            category = function.get("category") or "other"
            if function.get("backend") == "gstreamer":
                category = f"{category} · gstreamer"
            group = groups.get(category)
            if group is None:
                group = groups[category] = QTreeWidgetItem([category.upper()])
                group.setFlags(Qt.ItemFlag.ItemIsEnabled)
                font = QFont(self.font())
                font.setPointSizeF(font.pointSizeF() - 1.5)
                font.setWeight(QFont.Weight.DemiBold)
                group.setFont(0, font)
                group.setForeground(0, QColor(t.muted))
                group.setFirstColumnSpanned(True)
            item = QTreeWidgetItem(group, [function["name"], function.get("title", "")])
            item.setData(0, ID_ROLE, function["name"])
            item.setForeground(1, QColor(t.muted))
            tip = (
                f"{function.get('title', '')}\n{function.get('description', '')}\n\n"
                "Double-click to add it to the pipeline."
            )
            item.setToolTip(0, tip)
            item.setToolTip(1, tip)
        for name in sorted(groups):
            self.tree.addTopLevelItem(groups[name])
            groups[name].setFirstColumnSpanned(True)
        self.tree.expandAll()
        self.tree.resizeColumnToContents(0)
        self._apply_filter(self.filter.text())

    def current_name(self) -> str | None:
        item = self.tree.currentItem()
        return item.data(0, ID_ROLE) if item else None

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


class StageDelegate(QStyledItemDelegate):
    """Draws a stage's timing or error as a badge on the right of its row."""

    def sizeHint(self, option, index) -> QSize:
        size = super().sizeHint(option, index)
        return QSize(size.width(), max(size.height(), 30))

    def paint(self, painter: QPainter, option, index) -> None:
        super().paint(painter, option, index)
        badge = index.data(BADGE_ROLE)
        if not badge:
            return
        text, kind = badge
        t = theme.current()
        color = QColor({"error": t.danger, "off": t.muted}.get(kind, t.muted))
        painter.save()
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        font = QFont(option.font)
        font.setPointSizeF(font.pointSizeF() - 1)
        painter.setFont(font)
        width = painter.fontMetrics().horizontalAdvance(text) + 16
        rect = QRectF(option.rect.right() - width - 6, option.rect.center().y() - 10, width, 20)
        fill = QColor(color)
        fill.setAlpha(40)
        painter.setPen(Qt.PenStyle.NoPen)
        painter.setBrush(fill)
        painter.drawRoundedRect(rect, 10, 10)
        painter.setPen(color)
        painter.drawText(rect, Qt.AlignmentFlag.AlignCenter, text)
        painter.restore()


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

        # Functions.
        self.catalogue = FunctionList(self.add_function)
        functions_card = Card("Functions")
        functions_card.add_action(
            tool_button(
                "plus",
                "Add the selected function after the selected stage",
                lambda: (
                    self.catalogue.current_name()
                    and self.add_function(self.catalogue.current_name())
                ),
            )
        )
        functions_card.body.addWidget(self.catalogue)

        # Pipeline.
        self.list = QListWidget()
        self.list.setItemDelegate(StageDelegate(self.list))
        self.list.setDragDropMode(QAbstractItemView.DragDropMode.InternalMove)
        self.list.setContextMenuPolicy(Qt.ContextMenuPolicy.CustomContextMenu)
        self.list.customContextMenuRequested.connect(self._context_menu)
        self.list.model().rowsMoved.connect(self._rows_moved)
        self.list.itemChanged.connect(self._item_changed)
        self.list.currentItemChanged.connect(lambda *_: self._show_selected())
        remove = QAction(self.list)
        remove.setShortcut(QKeySequence.StandardKey.Delete)
        remove.setShortcutContext(Qt.ShortcutContext.WidgetShortcut)
        remove.triggered.connect(self._remove_selected)
        self.list.addAction(remove)
        self.empty = QLabel("Empty: frames pass unchanged.\nDouble-click a function to add it.")
        self.empty.setObjectName("muted")
        self.empty.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.text = QLineEdit(placeholderText="gray ! blur ksize=5   (Enter applies)")
        self.text.setToolTip("The pipeline as text, like scip-cli -p. Edit and press Enter.")
        self.text.returnPressed.connect(
            lambda: self.call("pipeline.set", {"text": self.text.text()})
        )
        pipeline_card = Card("Pipeline", "Check to enable · drag to reorder")
        for name, tip, slot in (
            ("up", "Move up", lambda: self._move_selected(-1)),
            ("down", "Move down", lambda: self._move_selected(+1)),
            ("trash", "Remove the selected stage (Del)", self._remove_selected),
            ("close", "Remove all stages", lambda: self.call("pipeline.clear")),
            ("copy", "Copy the pipeline text", self._copy_text),
        ):
            pipeline_card.add_action(tool_button(name, tip, slot))
        pipeline_card.body.addWidget(self.list, 1)
        pipeline_card.body.addWidget(self.empty)
        pipeline_card.body.addWidget(self.text)

        # Parameters.
        self.editor = ParamEditor()
        self.editor.changed.connect(self._param_edited)
        editor_scroll = QScrollArea(widgetResizable=True)
        editor_scroll.setWidget(self.editor)
        editor_card = Card("Parameters")
        editor_card.body.addWidget(editor_scroll)

        splitter = QSplitter(Qt.Orientation.Vertical)
        splitter.setChildrenCollapsible(False)
        for card in (functions_card, pipeline_card, editor_card):
            splitter.addWidget(card)
        splitter.setSizes([300, 240, 300])
        self.splitter = splitter
        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.addWidget(splitter)
        self._update_empty()
        theme.follow(self, lambda _t: self._redecorate())

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
        self._update_empty()
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
        self._redecorate()

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

    def _copy_text(self) -> None:
        QGuiApplication.clipboard().setText(self.text.text())

    def _context_menu(self, position) -> None:
        item = self.list.itemAt(position)
        if item is None:
            return
        stage = self._stage(item.data(ID_ROLE))
        menu = QMenu(self)
        toggle = menu.addAction("Disable" if stage["enabled"] else "Enable")
        menu.addSeparator()
        up = menu.addAction(icon("up"), "Move up")
        down = menu.addAction(icon("down"), "Move down")
        menu.addSeparator()
        remove = menu.addAction(icon("trash"), "Remove")
        chosen = menu.exec(self.list.viewport().mapToGlobal(position))
        if chosen is toggle:
            self.call("pipeline.update", {"id": stage["id"], "enabled": not stage["enabled"]})
        elif chosen in (up, down):
            self.list.setCurrentItem(item)
            self._move_selected(-1 if chosen is up else 1)
        elif chosen is remove:
            self.call("pipeline.remove", {"id": stage["id"]})

    def _update_empty(self) -> None:
        self.empty.setVisible(self.list.count() == 0)

    def _redecorate(self) -> None:
        for i in range(self.list.count()):
            self._decorate(self.list.item(i))

    def _decorate(self, item: QListWidgetItem) -> None:
        stage = self._stage(item.data(ID_ROLE))
        if stage is None:
            return
        t = theme.current()
        status = self.status.get(stage["id"], {})
        self.list.blockSignals(True)
        item.setText(stage["text"])
        if not stage["enabled"]:
            badge = ("off", "off")
            item.setForeground(QColor(t.muted))
            item.setToolTip("Disabled: frames pass it unchanged")
        elif status.get("error"):
            badge = ("bypassed", "error")
            item.setForeground(QColor(t.danger))
            item.setToolTip(f"Bypassed: {status['error']}")
        else:
            badge = (f"{status['mean_ms']:.1f} ms", "time") if status.get("active") else None
            item.setForeground(QColor(t.text))
            item.setToolTip(self.functions.get(stage["function"], {}).get("description", ""))
        item.setData(BADGE_ROLE, badge)
        self.list.blockSignals(False)
