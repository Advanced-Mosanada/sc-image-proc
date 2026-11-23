"""What the target is (system.info) and how it is doing (event.stats)."""

from __future__ import annotations

from typing import Any

from PySide6.QtWidgets import (
    QFormLayout,
    QLabel,
    QTableWidget,
    QTableWidgetItem,
    QTreeWidget,
    QTreeWidgetItem,
    QVBoxLayout,
    QWidget,
)

STATE_COLORS = {"running": "#2e7d32", "waiting": "#f9a825", "idle": "#757575", "error": "#c62828"}


def size_text(info: dict[str, Any]) -> str:
    if not info or not info.get("width"):
        return "-"
    return f"{info['width']}x{info['height']} {info['format']}"


class StatsPanel(QWidget):
    def __init__(self, parent: QWidget | None = None):
        super().__init__(parent)
        layout = QVBoxLayout(self)
        form = QFormLayout()
        self.labels: dict[str, QLabel] = {}
        for key, title in (
            ("state", "State"),
            ("input", "Input"),
            ("output", "Output frames"),
            ("fps", "Frames per second"),
            ("frames", "Frames"),
            ("read", "Read (wait + decode)"),
            ("process", "Processing"),
            ("write", "Outputs"),
        ):
            label = QLabel("-")
            label.setWordWrap(True)
            self.labels[key] = label
            form.addRow(title, label)
        layout.addLayout(form)
        self.table = QTableWidget(0, 4)
        self.table.setHorizontalHeaderLabels(["Stage", "mean ms", "p95 ms", "Status"])
        self.table.horizontalHeader().setStretchLastSection(True)
        self.table.verticalHeader().setVisible(False)
        layout.addWidget(self.table, 1)

    def update_stats(self, stats: dict[str, Any]) -> None:
        state = stats.get("state", "?")
        color = STATE_COLORS.get(state, "black")
        message = f" - {stats['message']}" if stats.get("message") else ""
        self.labels["state"].setText(f"<b style='color:{color}'>{state}</b>{message}")
        source = stats.get("input", {})
        live = ", live" if source.get("live") else ""
        self.labels["input"].setText(
            f"{source.get('uri') or '-'}  {size_text(source.get('info', {}))}"
            f"  {source.get('fps', 0):g} fps{live}"
        )
        self.labels["output"].setText(size_text(stats.get("output_info", {})))
        self.labels["fps"].setText(f"{stats.get('fps', 0):.1f}")
        self.labels["frames"].setText(str(stats.get("frames", 0)))
        timing = stats.get("timing", {})
        self.labels["read"].setText(f"{timing.get('read_ms', 0):.2f} ms")
        self.labels["process"].setText(
            f"{timing.get('process_ms', 0):.2f} ms (p95 {timing.get('process_p95_ms', 0):.2f})"
        )
        self.labels["write"].setText(f"{timing.get('write_ms', 0):.2f} ms")

        stages = stats.get("stages", [])
        self.table.setRowCount(len(stages))
        for row, stage in enumerate(stages):
            if stage["error"]:
                status = f"bypassed: {stage['error']}"
            elif not stage["enabled"]:
                status = "disabled"
            else:
                status = "running" if stage["active"] else "-"
            for column, value in enumerate(
                (stage["text"], f"{stage['mean_ms']:.2f}", f"{stage['p95_ms']:.2f}", status)
            ):
                self.table.setItem(row, column, QTableWidgetItem(value))
        self.table.resizeColumnToContents(0)


class InfoPanel(QTreeWidget):
    """system.info as a tree."""

    def __init__(self, parent: QWidget | None = None):
        super().__init__(parent)
        self.setHeaderLabels(["Field", "Value"])

    def set_info(self, info: dict[str, Any]) -> None:
        self.clear()
        self._add(self.invisibleRootItem(), info)
        self.expandAll()
        self.resizeColumnToContents(0)

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
