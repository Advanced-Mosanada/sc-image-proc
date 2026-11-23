"""What the target is (system.info) and how it is doing (event.stats)."""

from __future__ import annotations

import datetime
from typing import Any

from PySide6.QtCore import QRectF, Qt
from PySide6.QtGui import QColor, QPainter
from PySide6.QtWidgets import (
    QAbstractItemView,
    QFormLayout,
    QGridLayout,
    QHeaderView,
    QLabel,
    QScrollArea,
    QStyledItemDelegate,
    QTableWidget,
    QTableWidgetItem,
    QTreeWidget,
    QTreeWidgetItem,
    QVBoxLayout,
    QWidget,
)

from .. import theme
from .common import Card, Metric, Pill

BAR_ROLE = Qt.ItemDataRole.UserRole + 2  # share of the slowest stage, 0..1


def size_text(info: dict[str, Any]) -> str:
    if not info or not info.get("width"):
        return "-"
    return f"{info['width']}×{info['height']} {info['format']}"


class BarDelegate(QStyledItemDelegate):
    """A time with a bar showing how it compares to the slowest stage."""

    def paint(self, painter: QPainter, option, index) -> None:
        share = index.data(BAR_ROLE)
        if share:
            t = theme.current()
            painter.save()
            painter.setRenderHint(QPainter.RenderHint.Antialiasing)
            rect = QRectF(option.rect).adjusted(8, 0, -52, 0)
            top = rect.center().y() - 3
            painter.setPen(Qt.PenStyle.NoPen)
            track = QColor(t.border)
            painter.setBrush(track)
            painter.drawRoundedRect(QRectF(rect.left(), top, rect.width(), 6), 3, 3)
            painter.setBrush(QColor(t.accent))
            painter.drawRoundedRect(
                QRectF(rect.left(), top, max(6.0, rect.width() * share), 6), 3, 3
            )
            painter.restore()
        super().paint(painter, option, index)


class StatsPanel(QScrollArea):
    def __init__(self, parent: QWidget | None = None):
        super().__init__(parent)
        self.setWidgetResizable(True)
        content = QWidget()
        layout = QVBoxLayout(content)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(10)
        self.setWidget(content)

        overview = Card("Live")
        self.state = Pill("idle", "idle")
        overview.header.addWidget(self.state)
        self.message = QLabel()
        self.message.setObjectName("muted")
        self.message.setWordWrap(True)
        overview.body.addWidget(self.message)
        grid = QGridLayout()
        grid.setSpacing(8)
        self.metrics = {
            "fps": Metric("frames per second"),
            "frames": Metric("frames"),
            "process": Metric("processing ms"),
            "read": Metric("read ms (wait + decode)"),
            "write": Metric("outputs ms"),
            "p95": Metric("processing p95 ms"),
        }
        for i, metric in enumerate(self.metrics.values()):
            grid.addWidget(metric, i // 2, i % 2)
        overview.body.addLayout(grid)
        form = QFormLayout()
        form.setVerticalSpacing(4)
        self.labels: dict[str, QLabel] = {}
        for key, title in (("input", "Input"), ("output", "Result")):
            label = QLabel("-")
            label.setWordWrap(True)
            label.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
            caption = QLabel(title)
            caption.setObjectName("muted")
            form.addRow(caption, label)
            self.labels[key] = label
        overview.body.addLayout(form)
        layout.addWidget(overview)

        stages = Card("Time per function", "Mean over the last ~120 frames")
        self.table = QTableWidget(0, 3)
        self.table.setHorizontalHeaderLabels(["Function", "mean ms", "p95"])
        header = self.table.horizontalHeader()
        header.setSectionResizeMode(0, QHeaderView.ResizeMode.Stretch)
        for column, width in ((1, 130), (2, 56)):
            header.setSectionResizeMode(column, QHeaderView.ResizeMode.Fixed)
            header.resizeSection(column, width)
        self.table.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        self.table.verticalHeader().setVisible(False)
        self.table.setShowGrid(False)
        self.table.setSelectionMode(QAbstractItemView.SelectionMode.NoSelection)
        self.table.setEditTriggers(QAbstractItemView.EditTrigger.NoEditTriggers)
        self.table.setItemDelegateForColumn(1, BarDelegate(self.table))
        self.table.setMinimumHeight(160)
        stages.body.addWidget(self.table)
        layout.addWidget(stages, 1)

    def update_stats(self, stats: dict[str, Any]) -> None:
        t = theme.current()
        state = stats.get("state", "?")
        self.state.set(state, state)
        self.message.setText(stats.get("message", ""))
        self.message.setVisible(bool(stats.get("message")))
        timing = stats.get("timing", {})
        fps = stats.get("fps", 0)
        self.metrics["fps"].set(f"{fps:.1f}", t.state_color(state) if state == "running" else None)
        self.metrics["frames"].set(f"{stats.get('frames', 0):,}")
        self.metrics["process"].set(f"{timing.get('process_ms', 0):.1f}")
        self.metrics["read"].set(f"{timing.get('read_ms', 0):.1f}")
        self.metrics["write"].set(f"{timing.get('write_ms', 0):.1f}")
        self.metrics["p95"].set(f"{timing.get('process_p95_ms', 0):.1f}")
        source = stats.get("input", {})
        live = " · live" if source.get("live") else ""
        self.labels["input"].setText(
            f"{source.get('uri') or '-'}  ·  {size_text(source.get('info', {}))}"
            f"  ·  {source.get('fps', 0):g} fps{live}"
        )
        self.labels["output"].setText(size_text(stats.get("output_info", {})))

        stages = stats.get("stages", [])
        slowest = max([s["mean_ms"] for s in stages if s.get("active")] + [0.001])
        self.table.setRowCount(len(stages))
        for row, stage in enumerate(stages):
            name = stage["text"]
            if stage["error"]:
                status, color = f"bypassed: {stage['error']}", t.danger
            elif not stage["enabled"]:
                status, color, name = "disabled", t.muted, f"{name}  (off)"
            else:
                status, color = ("running", t.text) if stage["active"] else ("idle", t.muted)
            values = (name, f"{stage['mean_ms']:.2f}", f"{stage['p95_ms']:.2f}")
            for column, value in enumerate(values):
                item = QTableWidgetItem(value)
                item.setToolTip(f"{stage['text']}: {status}")
                if column == 0:
                    item.setForeground(QColor(color))
                else:
                    item.setTextAlignment(
                        Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter
                    )
                self.table.setItem(row, column, item)
            share = stage["mean_ms"] / slowest if stage.get("active") else 0.0
            self.table.item(row, 1).setData(BAR_ROLE, share)


class InfoPanel(QScrollArea):
    """system.info: a summary, then every field."""

    SUMMARY = (
        ("Device", lambda i: f"{i['device']['name']}  ({i['device']['hostname']})"),
        ("Board", lambda i: i["device"].get("board") or "-"),
        (
            "CPU",
            lambda i: (
                f"{i['device']['cpu']['model']} × {i['device']['cpu']['cores']}"
                f"  ·  {i['device']['memory_mb']:,} MB"
            ),
        ),
        ("System", lambda i: f"{i['os']['name']}  ·  Linux {i['os']['kernel']}"),
        (
            "scipd",
            lambda i: (
                f"{i['server']['version']}  ·  {i['server']['describe']}"
                + (f"  on {i['server']['branch']}" if i["server"].get("branch") else "")
            ),
        ),
        ("Build", lambda i: f"{i['server']['build_type']}  ·  {i['server']['compiler']}"),
        (
            "Libraries",
            lambda i: (
                f"GStreamer {i['libraries']['gstreamer'] or '-'}  ·  "
                f"OpenCV {i['libraries']['opencv']}"
            ),
        ),
        (
            "Hardware",
            lambda i: "  ·  ".join(
                f"{name} {'✓' if on else '✗'}"
                for name, on in (
                    ("VPU decode", i["capabilities"]["hardware"]["vpu_decoder"]),
                    ("VPU encode", i["capabilities"]["hardware"]["vpu_encoder"]),
                    ("G2D", i["capabilities"]["hardware"]["g2d"]),
                )
            ),
        ),
        (
            "Running since",
            lambda i: datetime.datetime.fromtimestamp(i["server"]["started"]).strftime(
                "%Y-%m-%d %H:%M:%S"
            ),
        ),
        (
            "Protocol",
            lambda i: (
                f"{i['protocol']['name']} {i['protocol']['version']}  ·  "
                f"port {i['control']['port']}"
            ),
        ),
    )

    def __init__(self, parent: QWidget | None = None):
        super().__init__(parent)
        self.setWidgetResizable(True)
        content = QWidget()
        layout = QVBoxLayout(content)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(10)
        self.setWidget(content)
        summary = Card("Target")
        self.form = QFormLayout()
        self.form.setVerticalSpacing(6)
        self.form.setHorizontalSpacing(14)
        summary.body.addLayout(self.form)
        self.summary_labels: dict[str, QLabel] = {}
        for title, _ in self.SUMMARY:
            caption = QLabel(title)
            caption.setObjectName("muted")
            value = QLabel("-")
            value.setWordWrap(True)
            value.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
            self.form.addRow(caption, value)
            self.summary_labels[title] = value
        layout.addWidget(summary)
        everything = Card("All fields", "system.info as the target reports it")
        self.tree = QTreeWidget()
        self.tree.setHeaderLabels(["Field", "Value"])
        self.tree.setMinimumHeight(240)
        everything.body.addWidget(self.tree)
        layout.addWidget(everything, 1)

    def set_info(self, info: dict[str, Any]) -> None:
        for title, extract in self.SUMMARY:
            try:
                text = extract(info)
            except (KeyError, TypeError, ValueError, OSError):
                text = "-"
            self.summary_labels[title].setText(text)
        self.tree.clear()
        self._add(self.tree.invisibleRootItem(), info)
        self.tree.expandAll()
        self.tree.resizeColumnToContents(0)

    def _add(self, parent: QTreeWidgetItem, value: Any) -> None:
        if isinstance(value, dict):
            for key, child in value.items():
                item = QTreeWidgetItem(parent, [key])
                if isinstance(child, (dict, list)):
                    self._add(item, child)
                else:
                    item.setText(1, str(child))
        elif isinstance(value, list):
            parent.setText(1, ", ".join(str(v) for v in value))
