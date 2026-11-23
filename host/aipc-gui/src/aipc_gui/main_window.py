"""The main window: connects to a target and wires the panels to it.

header    brand, target (discovered or typed), Discover, Connect, theme
left      functions and pipeline, parameters of the selected stage
center    video sent from this PC | result from the target, player
right     input, outputs, live statistics, target information
status    messages, the target's state and frame rate
"""

from __future__ import annotations

from typing import Any

from PySide6.QtCore import QByteArray, Qt, QTimer
from PySide6.QtGui import QAction, QActionGroup, QKeySequence, QShortcut
from PySide6.QtWidgets import (
    QAbstractButton,
    QComboBox,
    QFrame,
    QHBoxLayout,
    QLabel,
    QMainWindow,
    QMenu,
    QMessageBox,
    QPushButton,
    QSplitter,
    QTabWidget,
    QToolButton,
    QVBoxLayout,
    QWidget,
)

from . import media, settings, theme
from .icons import icon, set_icon
from .protocol import DEFAULT_PORT, PROTOCOL_VERSION, Target
from .qt_client import QtAipcClient, QtDiscovery
from .widgets.common import Pill, tool_button
from .widgets.io import InputPanel, OutputPanel
from .widgets.pipeline import PipelinePanel
from .widgets.player import PlayerPanel
from .widgets.status import InfoPanel, StatsPanel
from .widgets.video import VideoView

STATS_INTERVAL_MS = 500
TITLE = "AIPC - AMC Image Proc Control"
TABS = (("Input", "camera"), ("Outputs", "send"), ("Statistics", "chart"), ("Target", "info"))
SHORTCUTS = (
    ("Space", "Play or pause the video from this PC"),
    ("← / →", "Back or forward 5 s"),
    ("Home", "Back to the start"),
    ("Ctrl+O", "Open a video"),
    ("Ctrl+D", "Look for targets"),
    ("Ctrl+K", "Connect or disconnect"),
    ("Ctrl+F", "Search the functions"),
    ("Ctrl+T", "Dark or light theme"),
    ("Delete", "Remove the selected stage"),
)


def parse_target(text: str) -> tuple[str, int]:
    """'192.168.50.2', '192.168.50.2:5600' or a discovered entry's label."""
    text = text.strip().split(" ")[0]
    host, _, port = text.partition(":")
    return host, int(port) if port.isdigit() else DEFAULT_PORT


def repolish(widget: QWidget) -> None:
    """Re-apply the style sheet after an object name change."""
    widget.style().unpolish(widget)
    widget.style().polish(widget)


class MainWindow(QMainWindow):
    def __init__(self, parent: QWidget | None = None):
        super().__init__(parent)
        self.setWindowTitle(TITLE)
        self.resize(1560, 940)
        self.client = QtAipcClient(self)
        self.discovery = QtDiscovery(self)
        self.receiver: media.RtpReceiver | None = None
        self.target_info: dict[str, Any] = {}
        self.last_stats: dict[str, Any] = {}
        self._auto_connect = False

        # Panels.
        self.pipeline = PipelinePanel(self.client)
        self.sent_view = VideoView("Sent from this PC", icon="film")
        self.sent_view.accept_files("Open a video (Ctrl+O) or drop it here")
        self.sent_view.dropped.connect(self.open_video)
        self.result_view = VideoView("Result from the target", icon="sliders")
        self.player = PlayerPanel(on_frame=self.sent_view.show_frame)
        self.player.message.connect(self.show_message)
        self.inputs = InputPanel(self.client)
        self.inputs.streaming_from_pc.connect(self._streaming_from_pc)
        self.inputs.stream_settings_changed.connect(self.player.configure)
        self.player.opened.connect(lambda _path: self.inputs.set_media(self.player.streamer.info))
        self.outputs = OutputPanel(self.client)
        self.outputs.preview_port.connect(self._set_preview)
        self.stats = StatsPanel()
        self.info = InfoPanel()

        central = QWidget()
        central.setObjectName("central")
        layout = QVBoxLayout(central)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(0)
        layout.addWidget(self._header())

        self.videos = QSplitter(Qt.Orientation.Horizontal)
        self.videos.addWidget(self.sent_view)
        self.videos.addWidget(self.result_view)
        center = QWidget()
        center_layout = QVBoxLayout(center)
        center_layout.setContentsMargins(0, 0, 0, 0)
        center_layout.setSpacing(10)
        center_layout.addWidget(self.videos, 1)
        center_layout.addWidget(self.player)

        self.tabs = QTabWidget()
        self.tabs.setDocumentMode(True)
        self.tabs.setMinimumWidth(400)
        for panel, (title, _) in zip(
            (self.inputs, self.outputs, self.stats, self.info), TABS, strict=True
        ):
            self.tabs.addTab(panel, title)
        self._tab_icons()

        self.splitter = QSplitter(Qt.Orientation.Horizontal)
        self.splitter.addWidget(self.pipeline)
        self.splitter.addWidget(center)
        self.splitter.addWidget(self.tabs)
        self.splitter.setStretchFactor(1, 1)
        self.splitter.setSizes([370, 770, 420])
        body = QWidget()
        body_layout = QVBoxLayout(body)
        body_layout.setContentsMargins(12, 12, 12, 10)
        body_layout.addWidget(self.splitter)
        layout.addWidget(body, 1)
        self.setCentralWidget(central)

        # Status bar: messages on the left, the target's state on the right.
        self.state_pill = Pill("offline", "offline")
        self.rate_label = QLabel()
        self.rate_label.setObjectName("muted")
        self.statusBar().addPermanentWidget(self.rate_label)
        self.statusBar().addPermanentWidget(self.state_pill)
        self.statusBar().setSizeGripEnabled(False)

        # Keys go to the player unless a control uses them (typing, lists...):
        # the buttons and videos do not keep the focus.
        for button in self.player.findChildren(QAbstractButton):
            button.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        for view in (self.sent_view, self.result_view):
            view.setFocusPolicy(Qt.FocusPolicy.ClickFocus)
        for keys, slot in (
            ("Ctrl+O", self.player.choose_file),
            ("Ctrl+D", lambda: self.discover()),
            ("Ctrl+K", self.toggle_connection),
            ("Ctrl+T", self.toggle_theme),
            ("Ctrl+F", lambda: self.pipeline.catalogue.filter.setFocus()),
        ):
            QShortcut(QKeySequence(keys), self, activated=slot)

        self.client.connected.connect(self._connected)
        self.client.disconnected.connect(self._disconnected)
        self.client.event.connect(self._event)
        self.client.failed.connect(self.show_message)
        self.discovery.found.connect(self._found)
        self.discovery.finished.connect(self._discovery_finished)
        theme.follow(self, self._theme_changed)

        self.poll_timer = QTimer(self, interval=15)  # > 60 fps
        self.poll_timer.timeout.connect(self._poll)
        self.poll_timer.start()
        self._set_connected(False)
        self._restore()

    # --- header -----------------------------------------------------------------

    def _header(self) -> QFrame:
        header = QFrame()
        header.setObjectName("header")
        row = QHBoxLayout(header)
        row.setContentsMargins(16, 10, 12, 10)
        row.setSpacing(8)
        brand = QLabel("AIPC")
        brand.setObjectName("brand")
        brand.setToolTip("AMC Image Proc Control")
        row.addWidget(brand)
        subtitle = QLabel("Image Proc Control")
        subtitle.setObjectName("muted")
        row.addWidget(subtitle)
        row.addSpacing(18)

        self.target = QComboBox(editable=True)
        self.target.setMinimumWidth(320)
        self.target.lineEdit().setPlaceholderText("Target address[:port], or Discover")
        self.target.lineEdit().returnPressed.connect(self.toggle_connection)
        self.target.setToolTip("A discovered target, or an address typed here")
        row.addWidget(self.target)
        self.discover_button = QPushButton("Discover")
        set_icon(self.discover_button, "radar", size=16)
        self.discover_button.setToolTip("Look for targets on the network (Ctrl+D)")
        self.discover_button.clicked.connect(lambda: self.discover())
        row.addWidget(self.discover_button)
        self.connect_button = QPushButton("Connect")
        self.connect_button.setToolTip("Connect or disconnect (Ctrl+K)")
        self.connect_button.setMinimumWidth(124)
        self.connect_button.clicked.connect(self.toggle_connection)
        row.addWidget(self.connect_button)
        row.addSpacing(8)
        self.connection_pill = Pill("offline", "offline")
        row.addWidget(self.connection_pill)
        self.target_label = QLabel()
        self.target_label.setObjectName("muted")
        self.target_label.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
        row.addWidget(self.target_label, 1)

        self.theme_button = tool_button("moon", "Dark or light theme (Ctrl+T)", self.toggle_theme)
        self.theme_button.setPopupMode(QToolButton.ToolButtonPopupMode.MenuButtonPopup)
        menu = QMenu(self.theme_button)
        group = QActionGroup(menu)
        self.theme_actions: dict[str, QAction] = {}
        for mode, title in (("system", "Like the desktop"), ("light", "Light"), ("dark", "Dark")):
            action = QAction(title, menu, checkable=True)
            action.triggered.connect(lambda _=False, m=mode: self.set_theme(m))
            group.addAction(action)
            menu.addAction(action)
            self.theme_actions[mode] = action
        self.theme_button.setMenu(menu)
        row.addWidget(self.theme_button)
        row.addWidget(tool_button("info", "Keyboard shortcuts", self.show_shortcuts))
        return header

    def _tab_icons(self) -> None:
        for index, (_, name) in enumerate(TABS):
            self.tabs.setTabIcon(index, icon(name, theme.current().muted))

    # --- theme ------------------------------------------------------------------

    def set_theme(self, mode: str) -> None:
        manager = theme.manager()
        if manager is not None:
            manager.set_mode(mode)
            settings.put("theme", mode)
            self._theme_changed(theme.current())

    def toggle_theme(self) -> None:
        self.set_theme("light" if theme.current().dark else "dark")

    def _theme_changed(self, t: theme.Theme) -> None:
        manager = theme.manager()
        mode = manager.mode if manager is not None else "system"
        if mode in self.theme_actions:
            self.theme_actions[mode].setChecked(True)
        set_icon(self.theme_button, "sun" if t.dark else "moon", size=18)
        self._tab_icons()
        if self.target_info:
            self._set_info(self.target_info)

    def show_shortcuts(self) -> None:
        rows = "".join(
            f"<tr><td style='padding:3px 16px 3px 0'><b>{keys}</b></td><td>{what}</td></tr>"
            for keys, what in SHORTCUTS
        )
        QMessageBox.information(self, "Keyboard shortcuts", f"<table>{rows}</table>")

    # --- keys -------------------------------------------------------------------

    def keyPressEvent(self, event) -> None:
        """Keys no control used: they drive the player."""
        key = event.key()
        if event.modifiers() & ~Qt.KeyboardModifier.KeypadModifier:
            super().keyPressEvent(event)
        elif key == Qt.Key.Key_Space:
            self.player.toggle()
        elif key == Qt.Key.Key_Left:
            self.player.skip(-5)
        elif key == Qt.Key.Key_Right:
            self.player.skip(5)
        elif key == Qt.Key.Key_Home:
            self.player.restart()
        else:
            super().keyPressEvent(event)

    # --- video from this PC -------------------------------------------------------

    def open_video(self, path: str) -> None:
        if self.player.open(path):
            self.show_message(f"opened {path}")

    # --- connection ---------------------------------------------------------

    def discover(self, auto_connect: bool = False) -> None:
        self._auto_connect = auto_connect
        self.discover_button.setEnabled(False)
        self.discover_button.setText("Looking...")
        self.show_message("looking for targets...")
        self.discovery.start()

    def connect_to(self, host: str, port: int = DEFAULT_PORT) -> None:
        self.show_message(f"connecting to {host}:{port}...")
        self.connect_button.setEnabled(False)
        self.connect_button.setText("Connecting...")
        self.connection_pill.set("connecting", "connecting")
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
            else:
                self.target.setFocus()
                self.show_message("type a target address, or Discover")

    def _found(self, target: Target) -> None:
        if self.target.findData((target.address, target.port)) < 0:
            self.target.addItem(
                icon("monitor", theme.current().muted),
                target.label(),
                (target.address, target.port),
            )

    def _discovery_finished(self) -> None:
        self.discover_button.setEnabled(True)
        self.discover_button.setText("Discover")
        count = self.target.count()
        self.show_message(f"{count} target(s) found" if count else "no target answered")
        if count and self._auto_connect and not self.client.is_connected:
            # Prefer the target used last time.
            last = parse_target(settings.get("target", "", str))[0]
            index = next(
                (i for i in range(count) if (self.target.itemData(i) or ("",))[0] == last), 0
            )
            self.target.setCurrentIndex(index)
            self.toggle_connection()

    def _connected(self) -> None:
        self._set_connected(True)
        self.show_message(f"connected to {self.client.host}:{self.client.port}")
        settings.put("target", f"{self.client.host}:{self.client.port}")
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
        self.connect_button.setObjectName("" if connected else "primary")
        set_icon(
            self.connect_button,
            "close" if connected else "power",
            role="text" if connected else "accent_text",
            size=16,
        )
        repolish(self.connect_button)
        self.target.setEnabled(not connected)
        self.discover_button.setEnabled(not connected and self.discover_button.text() == "Discover")
        self.connection_pill.set(
            "connected" if connected else "offline", "connected" if connected else "offline"
        )
        for panel in (self.pipeline, self.inputs, self.outputs):
            panel.setEnabled(connected)
        if not connected:
            self.target_info = {}
            self.target_label.setText("")
            self.setWindowTitle(TITLE)
            self.state_pill.set("offline", "offline")
            self.rate_label.setText("")
            self.result_view.clear("Not connected", "Connect to a target to see its result")

    def _set_info(self, info: dict[str, Any]) -> None:
        self.target_info = info
        self.info.set_info(info)
        t = theme.current()
        device, server = info["device"], info["server"]
        version = info.get("protocol", {}).get("version", "?")
        warning = (
            ""
            if version.split(".")[0] == PROTOCOL_VERSION.split(".")[0]
            else (
                f"  <b style='color:{t.danger}'>protocol {version}, this GUI speaks "
                f"{PROTOCOL_VERSION}</b>"
            )
        )
        board = f"  ·  {device['board']}" if device.get("board") else ""
        self.target_label.setText(
            f"<b style='color:{t.text}'>{device['name']}</b>{board}"
            f"  ·  scipd {server['describe']}{warning}"
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
            state = params.get("state", "?")
            self.state_pill.set(state, state)
            timing = params.get("timing", {})
            self.rate_label.setText(
                f"{params.get('fps', 0):.1f} fps  ·  {timing.get('process_ms', 0):.1f} ms"
                if state == "running"
                else ""
            )
        elif method == "event.state":
            message = f": {params['message']}" if params.get("message") else ""
            self.state_pill.set(params["state"], params["state"])
            self.show_message(f"target {params['state']}{message}")
        elif method == "event.config":
            self.refresh(params.get("what", ""))

    # --- video ------------------------------------------------------------------

    def _streaming_from_pc(self, on: bool, port: int) -> None:
        if on:
            # Destination first: new settings then connect straight to it.
            self.player.set_destination(self.client.host, port)
            self.player.configure(self.inputs.stream_settings())

    def _set_preview(self, port: int) -> None:
        """Receive the target's result on `port` (0 = stop)."""
        if self.receiver is not None and self.receiver.port == port:
            return
        if self.receiver is not None:
            self.receiver.stop()
            self.receiver = None
        if port and media.available():
            self.receiver = media.RtpReceiver(port, on_frame=self.result_view.show_frame)
            self.receiver.start()
            self.result_view.clear("Waiting for the result", f"on udp port {port}")
        elif port:
            self.result_view.clear("No preview", "needs GStreamer for Python")
        elif self.client.is_connected:
            self.result_view.clear("The result is not sent to this PC", "Tick “This PC” in Outputs")

    def _poll(self) -> None:
        if self.receiver is not None:
            self.receiver.poll()
            if self.receiver.error:
                self.show_message(f"preview: {self.receiver.error}")
                self.result_view.clear("No preview", self.receiver.error)
                self.receiver = None

    def show_message(self, text: str) -> None:
        self.statusBar().showMessage(text, 10000)

    # --- what is remembered -----------------------------------------------------

    def _restore(self) -> None:
        last = settings.get("target", "", str)
        if last:
            self.target.setEditText(last)
        for key, widget in (("window/splitter", self.splitter), ("window/videos", self.videos)):
            state = settings.get(key)
            if isinstance(state, QByteArray):
                widget.restoreState(state)
        geometry = settings.get("window/geometry")
        if isinstance(geometry, QByteArray):
            self.restoreGeometry(geometry)
        self.tabs.setCurrentIndex(settings.get("window/tab", 0, int))
        self._theme_changed(theme.current())

    def _save(self) -> None:
        settings.put("window/geometry", self.saveGeometry())
        settings.put("window/splitter", self.splitter.saveState())
        settings.put("window/videos", self.videos.saveState())
        settings.put("window/tab", self.tabs.currentIndex())

    def closeEvent(self, event) -> None:
        self._save()
        self._set_preview(0)
        self.player.close()
        self.client.close()
        super().closeEvent(event)
