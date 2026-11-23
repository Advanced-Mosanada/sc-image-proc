"""The main window: connects to a target and wires the panels to it.

toolbar   target (discovered or typed), Discover, Connect
left      functions and pipeline, parameters of the selected stage
center    video sent from this PC | result from the target, player
right     input, outputs, live statistics, target information
"""

from __future__ import annotations

from typing import Any

from PySide6.QtCore import Qt, QTimer
from PySide6.QtWidgets import (
    QComboBox,
    QLabel,
    QMainWindow,
    QPushButton,
    QSplitter,
    QTabWidget,
    QToolBar,
    QVBoxLayout,
    QWidget,
)

from . import media
from .protocol import DEFAULT_PORT, PROTOCOL_VERSION, Target
from .qt_client import QtAipcClient, QtDiscovery
from .widgets.io import InputPanel, OutputPanel
from .widgets.pipeline import PipelinePanel
from .widgets.player import PlayerPanel
from .widgets.status import InfoPanel, StatsPanel
from .widgets.video import VideoView

STATS_INTERVAL_MS = 500


def parse_target(text: str) -> tuple[str, int]:
    """'192.168.50.2', '192.168.50.2:5600' or a discovered entry's label."""
    text = text.strip().split(" ")[0]
    host, _, port = text.partition(":")
    return host, int(port) if port.isdigit() else DEFAULT_PORT


class MainWindow(QMainWindow):
    def __init__(self, parent: QWidget | None = None):
        super().__init__(parent)
        self.setWindowTitle("AIPC - AMC Image Proc Control")
        self.resize(1500, 900)
        self.client = QtAipcClient(self)
        self.discovery = QtDiscovery(self)
        self.receiver: media.RtpReceiver | None = None
        self.target_info: dict[str, Any] = {}
        self.last_stats: dict[str, Any] = {}

        # Toolbar.
        toolbar = QToolBar("Target")
        toolbar.setMovable(False)
        self.addToolBar(toolbar)
        toolbar.addWidget(QLabel(" Target "))
        self.target = QComboBox(editable=True)
        self.target.setMinimumWidth(380)
        self.target.lineEdit().setPlaceholderText("address[:port], or Discover")
        self.target.lineEdit().returnPressed.connect(self.toggle_connection)
        toolbar.addWidget(self.target)
        self.discover_button = QPushButton("Discover")
        self.discover_button.clicked.connect(self.discover)
        toolbar.addWidget(self.discover_button)
        self.connect_button = QPushButton("Connect")
        self.connect_button.clicked.connect(self.toggle_connection)
        toolbar.addWidget(self.connect_button)
        self.target_label = QLabel()
        toolbar.addWidget(self.target_label)

        # Panels.
        self.pipeline = PipelinePanel(self.client)
        self.sent_view = VideoView("Sent from this PC")
        self.result_view = VideoView("Result from the target")
        self.player = PlayerPanel(on_frame=self.sent_view.bridge.deliver)
        self.player.message.connect(self.show_message)
        self.inputs = InputPanel(self.client)
        self.inputs.streaming_from_pc.connect(self._streaming_from_pc)
        self.outputs = OutputPanel(self.client)
        self.outputs.preview_port.connect(self._set_preview)
        self.stats = StatsPanel()
        self.info = InfoPanel()

        videos = QSplitter(Qt.Orientation.Horizontal)
        videos.addWidget(self.sent_view)
        videos.addWidget(self.result_view)
        center = QWidget()
        center_layout = QVBoxLayout(center)
        center_layout.setContentsMargins(0, 0, 0, 0)
        center_layout.addWidget(videos, 1)
        center_layout.addWidget(self.player)

        tabs = QTabWidget()
        tabs.addTab(self.inputs, "Input")
        tabs.addTab(self.outputs, "Outputs")
        tabs.addTab(self.stats, "Statistics")
        tabs.addTab(self.info, "Target")

        splitter = QSplitter(Qt.Orientation.Horizontal)
        splitter.addWidget(self.pipeline)
        splitter.addWidget(center)
        splitter.addWidget(tabs)
        splitter.setSizes([380, 760, 360])
        self.setCentralWidget(splitter)

        self.client.connected.connect(self._connected)
        self.client.disconnected.connect(self._disconnected)
        self.client.event.connect(self._event)
        self.client.failed.connect(self.show_message)
        self.discovery.found.connect(self._found)
        self.discovery.finished.connect(self._discovery_finished)

        self.poll_timer = QTimer(self, interval=100)
        self.poll_timer.timeout.connect(self._poll)
        self.poll_timer.start()
        self._set_connected(False)

    # --- connection ---------------------------------------------------------

    def discover(self, auto_connect: bool = False) -> None:
        self._auto_connect = auto_connect
        self.discover_button.setEnabled(False)
        self.show_message("looking for targets...")
        self.discovery.start()

    def connect_to(self, host: str, port: int = DEFAULT_PORT) -> None:
        self.show_message(f"connecting to {host}:{port}...")
        self.connect_button.setEnabled(False)
        self.client.connect_to(host, port)

    def toggle_connection(self) -> None:
        if self.client.is_connected:
            self.client.close()
            self._disconnected("")
        else:
            data = self.target.currentData()
            if data and self.target.currentText() == self.target.itemText(
                self.target.currentIndex()
            ):
                host, port = data
            else:
                host, port = parse_target(self.target.currentText())
            if host:
                self.connect_to(host, port)

    def _found(self, target: Target) -> None:
        if self.target.findData((target.address, target.port)) < 0:
            self.target.addItem(target.label(), (target.address, target.port))

    def _discovery_finished(self) -> None:
        self.discover_button.setEnabled(True)
        count = self.target.count()
        self.show_message(f"{count} target(s) found" if count else "no target answered")
        if count and getattr(self, "_auto_connect", False) and not self.client.is_connected:
            self.target.setCurrentIndex(0)
            self.toggle_connection()

    def _connected(self) -> None:
        self._set_connected(True)
        self.show_message(f"connected to {self.client.host}:{self.client.port}")
        self.client.call("system.info", on_result=self._set_info)
        self.client.call(
            "functions.list", on_result=lambda r: self.pipeline.set_functions(r["functions"])
        )
        self.client.call("output.displays", on_result=self.outputs.set_displays)
        self.inputs.refresh_devices()
        self.refresh("pipeline")
        self.refresh("input")
        self.refresh("outputs")
        self.client.call(
            "events.subscribe",
            {"topics": ["config", "state", "stats"], "interval_ms": STATS_INTERVAL_MS},
        )

    def _disconnected(self, reason: str) -> None:
        self._set_connected(False)
        self._set_preview(0)
        self.show_message(f"disconnected: {reason}" if reason else "disconnected")

    def _set_connected(self, connected: bool) -> None:
        self.connect_button.setEnabled(True)
        self.connect_button.setText("Disconnect" if connected else "Connect")
        self.target.setEnabled(not connected)
        for panel in (self.pipeline, self.inputs, self.outputs):
            panel.setEnabled(connected)
        if not connected:
            self.target_label.setText("")
            self.setWindowTitle("AIPC - AMC Image Proc Control")

    def _set_info(self, info: dict[str, Any]) -> None:
        self.target_info = info
        self.info.set_info(info)
        device, server = info["device"], info["server"]
        version = info.get("protocol", {}).get("version", "?")
        warning = (
            ""
            if version.split(".")[0] == PROTOCOL_VERSION.split(".")[0]
            else (
                f"  <b style='color:#c62828'>protocol {version}, this GUI speaks "
                f"{PROTOCOL_VERSION}</b>"
            )
        )
        branch = f" on {server['branch']}" if server.get("branch") else ""
        self.target_label.setText(
            f"  <b>{device['name']}</b>  {device.get('board', '')}  "
            f"scipd {server['describe']}{branch}{warning}"
        )
        self.setWindowTitle(f"AIPC - {device['name']}")

    # --- target state -----------------------------------------------------------

    def refresh(self, what: str) -> None:
        if what == "pipeline":
            self.client.call("pipeline.get", on_result=self.pipeline.set_pipeline)
        elif what == "input":
            self.client.call("input.get", on_result=self.inputs.set_input)
        elif what == "outputs":
            self.client.call("output.get", on_result=self.outputs.set_outputs)

    def _event(self, method: str, params: dict[str, Any]) -> None:
        if method == "event.stats":
            self.last_stats = params
            self.stats.update_stats(params)
            self.pipeline.update_status(params)
            self.outputs.update_status(params)
        elif method == "event.state":
            message = f": {params['message']}" if params.get("message") else ""
            self.show_message(f"target {params['state']}{message}")
        elif method == "event.config":
            self.refresh(params.get("what", ""))

    # --- video ------------------------------------------------------------------

    def _streaming_from_pc(self, on: bool, port: int) -> None:
        if on:
            self.player.set_destination(self.client.host, port)

    def _set_preview(self, port: int) -> None:
        """Receive the target's result on `port` (0 = stop)."""
        if self.receiver is not None and self.receiver.port == port:
            return
        if self.receiver is not None:
            self.receiver.stop()
            self.receiver = None
            self.result_view.clear()
        if port and media.available():
            self.receiver = media.RtpReceiver(port, on_frame=self.result_view.bridge.deliver)
            self.receiver.start()
            self.result_view.clear(f"waiting for the target on udp port {port}...")
        elif port:
            self.result_view.clear("needs GStreamer for Python")

    def _poll(self) -> None:
        if self.receiver is not None:
            self.receiver.poll()
            if self.receiver.error:
                self.show_message(f"preview: {self.receiver.error}")
                self.result_view.clear(self.receiver.error)
                self.receiver = None

    def show_message(self, text: str) -> None:
        self.statusBar().showMessage(text, 10000)

    def closeEvent(self, event) -> None:
        self._set_preview(0)
        self.player.close()
        self.client.close()
        super().closeEvent(event)
