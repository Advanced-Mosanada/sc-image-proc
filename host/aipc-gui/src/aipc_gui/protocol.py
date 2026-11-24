"""AIPC client without Qt: for scripts, tests and the `aipc` command.

The protocol is specified in docs/aipc-protocol.md at the repository root:
JSON-RPC 2.0 over TCP, one JSON object per line, plus UDP discovery.

    with AipcClient("192.168.50.2") as target:
        print(target.call("system.info")["device"])
        target.call("pipeline.set", {"text": "gray ! blur ksize=5"})

The GUI uses qt_client.QtAipcClient instead, which never blocks the window.
"""

from __future__ import annotations

import argparse
import contextlib
import itertools
import json
import socket
import sys
import time
from collections.abc import Callable
from dataclasses import dataclass, field
from typing import Any

PROTOCOL_VERSION = "1.2"
DEFAULT_PORT = 5600
DISCOVERY_PORT = 5601
VIDEO_TO_TARGET_PORT = 5000  # the target's input rtp://:5000
VIDEO_FROM_TARGET_PORT = 5002  # the target's output udp:HOST:5002
INPUT_FROM_TARGET_PORT = 5004  # the target's input output udp:HOST:5004

# Error codes (protocol section 3).
PARSE_ERROR = -32700
INVALID_REQUEST = -32600
METHOD_NOT_FOUND = -32601
INVALID_PARAMS = -32602
INTERNAL_ERROR = -32603
REJECTED = -32000
NOT_FOUND = -32001
UNAVAILABLE = -32002


class AipcError(Exception):
    """An error response from the target."""

    def __init__(self, code: int, message: str):
        super().__init__(message)
        self.code = code
        self.message = message

    def __str__(self) -> str:
        return f"{self.message} ({self.code})"


def encode(message: dict[str, Any]) -> bytes:
    """One protocol line."""
    return (json.dumps(message, separators=(",", ":")) + "\n").encode()


def request(id_: int | str, method: str, params: dict[str, Any] | None = None) -> dict[str, Any]:
    message: dict[str, Any] = {"jsonrpc": "2.0", "id": id_, "method": method}
    if params:
        message["params"] = params
    return message


class AipcClient:
    """Blocking client. Events (after events.subscribe) go to `on_event`."""

    def __init__(
        self,
        host: str,
        port: int = DEFAULT_PORT,
        timeout: float = 5.0,
        on_event: Callable[[str, dict[str, Any]], None] | None = None,
    ):
        self.host = host
        self.port = port
        self.on_event = on_event
        self._ids = itertools.count(1)
        self._sock = socket.create_connection((host, port), timeout=timeout)
        self._sock.settimeout(timeout)
        self._buffer = b""

    def __enter__(self) -> AipcClient:
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()

    def close(self) -> None:
        self._sock.close()

    @property
    def local_address(self) -> str:
        """Our address as the target sees it: where to send its video back to."""
        return self._sock.getsockname()[0]

    def call(self, method: str, params: dict[str, Any] | None = None) -> Any:
        """Send a request and wait for its result; raises AipcError."""
        id_ = next(self._ids)
        self._sock.sendall(encode(request(id_, method, params)))
        while True:
            message = self._read_message()
            if message.get("id") == id_:
                if "error" in message:
                    raise AipcError(message["error"]["code"], message["error"]["message"])
                return message["result"]
            self._dispatch(message)

    def wait_event(self, timeout: float) -> tuple[str, dict[str, Any]] | None:
        """Wait for the next event; None after `timeout` seconds."""
        self._sock.settimeout(timeout)
        try:
            message = self._read_message()
        except TimeoutError:
            return None
        finally:
            self._sock.settimeout(5.0)
        return message.get("method", ""), message.get("params", {})

    def _dispatch(self, message: dict[str, Any]) -> None:
        if "method" in message and self.on_event:
            self.on_event(message["method"], message.get("params", {}))

    def _read_message(self) -> dict[str, Any]:
        while b"\n" not in self._buffer:
            chunk = self._sock.recv(65536)
            if not chunk:
                raise ConnectionError("the target closed the connection")
            self._buffer += chunk
        line, self._buffer = self._buffer.split(b"\n", 1)
        return json.loads(line)


@dataclass
class Target:
    """A scipd that answered discovery."""

    address: str
    port: int
    name: str
    hostname: str = ""
    board: str = ""
    version: str = ""
    commit: str = ""
    extra: dict[str, Any] = field(default_factory=dict)

    @staticmethod
    def from_announce(address: str, announce: dict[str, Any]) -> Target:
        return Target(
            address=address,
            port=int(announce.get("port", DEFAULT_PORT)),
            name=announce.get("name", address),
            hostname=announce.get("hostname", ""),
            board=announce.get("board", ""),
            version=announce.get("version", ""),
            commit=announce.get("commit", ""),
            extra=announce,
        )

    def label(self) -> str:
        board = f" - {self.board}" if self.board else ""
        return f"{self.name} ({self.address}:{self.port}){board}"


DISCOVER = b'{"aipc":"discover"}'


def parse_announce(data: bytes) -> dict[str, Any] | None:
    try:
        message = json.loads(data)
    except ValueError:
        return None
    if isinstance(message, dict) and message.get("aipc") == "announce":
        return message
    return None


def discover(
    addresses: list[str] | None = None, port: int = DISCOVERY_PORT, timeout: float = 1.0
) -> list[Target]:
    """Ask the LAN (or the given addresses) for targets; each answers once."""
    addresses = addresses or ["255.255.255.255", "127.0.0.1"]
    found: dict[tuple[str, int], Target] = {}
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        for address in addresses:
            with contextlib.suppress(OSError):  # e.g. no route for a broadcast address
                sock.sendto(DISCOVER, (address, port))
        deadline = time.monotonic() + timeout
        while (left := deadline - time.monotonic()) > 0:
            sock.settimeout(left)
            try:
                data, (address, _) = sock.recvfrom(4096)
            except TimeoutError:
                break
            announce = parse_announce(data)
            if announce:
                target = Target.from_announce(address, announce)
                found[(target.address, target.port)] = target
    return list(found.values())


def main(argv: list[str] | None = None) -> int:
    """`aipc`: call one method from the shell, or list targets."""
    parser = argparse.ArgumentParser(
        prog="aipc",
        description="Talk to a scipd target over AIPC.",
        epilog="examples:\n"
        "  aipc --discover\n"
        "  aipc 192.168.50.2 system.info\n"
        '  aipc 192.168.50.2 pipeline.set \'{"text": "gray ! blur ksize=5"}\'\n'
        '  aipc 192.168.50.2 events.subscribe \'{"topics": ["stats"]}\' --follow',
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("host", nargs="?", help="target address")
    parser.add_argument("method", nargs="?", default="system.info")
    parser.add_argument("params", nargs="?", help="JSON object")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--discover", action="store_true", help="list targets on the LAN")
    parser.add_argument("--follow", action="store_true", help="then print events until Ctrl+C")
    args = parser.parse_intermixed_args(argv)

    if args.discover or not args.host:
        for target in discover():
            print(target.label())
        return 0
    params = json.loads(args.params) if args.params else None
    try:
        with AipcClient(args.host, args.port) as client:
            print(json.dumps(client.call(args.method, params), indent=2))
            while args.follow:
                event = client.wait_event(timeout=3600)
                if event:
                    print(json.dumps({"event": event[0], "params": event[1]}))
    except AipcError as e:
        print(f"aipc: {e}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
