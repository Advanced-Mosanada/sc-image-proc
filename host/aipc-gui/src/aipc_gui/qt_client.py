"""AIPC for the GUI: asynchronous, on Qt's event loop, never blocks the window.

    client = QtAipcClient()
    client.connected.connect(...)
    client.event.connect(lambda method, params: ...)
    client.connect_to("192.168.50.2")
    client.call("pipeline.get", on_result=show_pipeline)

Errors of requests without their own `on_error` are reported by `failed`.
"""

from __future__ import annotations

import itertools
import json
from collections.abc import Callable
from typing import Any

from PySide6.QtCore import QObject, QTimer, Signal
from PySide6.QtNetwork import (
    QAbstractSocket,
    QHostAddress,
    QNetworkInterface,
    QTcpSocket,
    QUdpSocket,
)

from .protocol import (
    DEFAULT_PORT,
    DISCOVER,
    DISCOVERY_PORT,
    AipcError,
    Target,
    encode,
    parse_announce,
    request,
)

ResultHandler = Callable[[Any], None]
ErrorHandler = Callable[[AipcError], None]

CONNECT_TIMEOUT_MS = 3000


def _plain_address(address: QHostAddress) -> str:
    """'::ffff:192.168.50.2' -> '192.168.50.2'."""
    text = address.toString()
    return text.removeprefix("::ffff:")


class QtAipcClient(QObject):
    connected = Signal()
    disconnected = Signal(str)  # reason, "" when we closed it ourselves
    event = Signal(str, object)  # method, params
    failed = Signal(str)  # a request failed and had no on_error

    def __init__(self, parent: QObject | None = None):
        super().__init__(parent)
        self.host = ""
        self.port = DEFAULT_PORT
        self._socket = QTcpSocket(self)
        self._socket.connected.connect(self._on_connected)
        self._socket.disconnected.connect(lambda: self._on_lost("connection closed"))
        self._socket.errorOccurred.connect(lambda _: self._on_lost(self._socket.errorString()))
        self._socket.readyRead.connect(self._on_ready_read)
        self._timeout = QTimer(self, singleShot=True, interval=CONNECT_TIMEOUT_MS)
        self._timeout.timeout.connect(lambda: self._on_lost("no answer (is scipd running?)"))
        self._buffer = b""
        self._ids = itertools.count(1)
        self._pending: dict[int, tuple[str, ResultHandler | None, ErrorHandler | None]] = {}
        self._closing = False

    # --- connection ---------------------------------------------------------

    def connect_to(self, host: str, port: int = DEFAULT_PORT) -> None:
        self.close()
        self.host, self.port = host, port
        self._closing = False
        self._timeout.start()
        self._socket.connectToHost(host, port)

    def close(self) -> None:
        self._closing = True
        self._timeout.stop()
        self._socket.abort()
        self._fail_pending("disconnected")
        self._buffer = b""

    @property
    def is_connected(self) -> bool:
        return self._socket.state() == QAbstractSocket.SocketState.ConnectedState

    @property
    def local_address(self) -> str:
        """Our address as the target sees it: where its video should come back to."""
        return _plain_address(self._socket.localAddress())

    # --- requests -------------------------------------------------------------

    def call(
        self,
        method: str,
        params: dict[str, Any] | None = None,
        on_result: ResultHandler | None = None,
        on_error: ErrorHandler | None = None,
    ) -> None:
        if not self.is_connected:
            self._report(method, AipcError(0, "not connected"), on_error)
            return
        id_ = next(self._ids)
        self._pending[id_] = (method, on_result, on_error)
        self._socket.write(encode(request(id_, method, params)))

    # --- internals ------------------------------------------------------------

    def _on_connected(self) -> None:
        self._timeout.stop()
        self.connected.emit()

    def _on_lost(self, reason: str) -> None:
        self._timeout.stop()
        was_closing = self._closing
        if self._socket.state() != QAbstractSocket.SocketState.UnconnectedState:
            self._closing = True
            self._socket.abort()
        self._fail_pending(reason)
        if not was_closing:
            self._closing = True
            self.disconnected.emit(reason)

    def _on_ready_read(self) -> None:
        self._buffer += bytes(self._socket.readAll().data())
        while b"\n" in self._buffer:
            line, self._buffer = self._buffer.split(b"\n", 1)
            if line.strip():
                self._handle(json.loads(line))

    def _handle(self, message: dict[str, Any]) -> None:
        if "id" not in message:
            self.event.emit(message.get("method", ""), message.get("params", {}))
            return
        pending = self._pending.pop(message["id"], None)
        if pending is None:
            return  # e.g. {"id": null, "error": ...} for a line we did not send
        method, on_result, on_error = pending
        if "error" in message:
            error = AipcError(message["error"]["code"], message["error"]["message"])
            self._report(method, error, on_error)
        elif on_result:
            on_result(message.get("result"))

    def _report(self, method: str, error: AipcError, on_error: ErrorHandler | None) -> None:
        if on_error:
            on_error(error)
        else:
            self.failed.emit(f"{method}: {error.message}")

    def _fail_pending(self, reason: str) -> None:
        pending, self._pending = self._pending, {}
        for _, _, on_error in pending.values():
            if on_error:
                on_error(AipcError(0, reason))


class QtDiscovery(QObject):
    """Find targets: one datagram to every interface's broadcast address."""

    found = Signal(object)  # Target
    finished = Signal()

    def __init__(self, parent: QObject | None = None, port: int = DISCOVERY_PORT):
        super().__init__(parent)
        self.port = port
        self._socket: QUdpSocket | None = None
        self._seen: set[tuple[str, int]] = set()

    def start(self, timeout_ms: int = 1000, extra: list[str] | None = None) -> None:
        self._stop()
        self._seen.clear()
        self._socket = QUdpSocket(self)
        self._socket.bind(QHostAddress(QHostAddress.SpecialAddress.AnyIPv4), 0)
        self._socket.readyRead.connect(self._on_ready_read)
        for address in self._addresses() | set(extra or []):
            self._socket.writeDatagram(DISCOVER, QHostAddress(address), self.port)
        QTimer.singleShot(timeout_ms, self._finish)

    @staticmethod
    def _addresses() -> set[str]:
        addresses = {"127.0.0.1"}
        for interface in QNetworkInterface.allInterfaces():
            if not interface.flags() & QNetworkInterface.InterfaceFlag.IsUp:
                continue
            for entry in interface.addressEntries():
                ipv4 = entry.ip().protocol() == QAbstractSocket.NetworkLayerProtocol.IPv4Protocol
                if ipv4 and not entry.broadcast().isNull():
                    addresses.add(entry.broadcast().toString())
        return addresses

    def _on_ready_read(self) -> None:
        while self._socket is not None and self._socket.hasPendingDatagrams():
            datagram = self._socket.receiveDatagram()
            announce = parse_announce(bytes(datagram.data().data()))
            if announce is None:
                continue
            target = Target.from_announce(_plain_address(datagram.senderAddress()), announce)
            key = (target.hostname or target.address, target.port)  # loopback and LAN = one
            if key not in self._seen:
                self._seen.add(key)
                self.found.emit(target)

    def _finish(self) -> None:
        self._stop()
        self.finished.emit()

    def _stop(self) -> None:
        if self._socket is not None:
            self._socket.close()
            self._socket.deleteLater()
            self._socket = None
