"""Shared fixtures: a real scipd from the C++ build, Qt without a screen."""

from __future__ import annotations

import os
import re
import socket
import subprocess
from collections.abc import Iterator
from dataclasses import dataclass
from pathlib import Path

import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")  # tests never open windows

REPO = Path(__file__).resolve().parents[3]


def find_scipd() -> Path | None:
    """$SCIPD, else the host-debug or host-release build."""
    if os.environ.get("SCIPD"):
        return Path(os.environ["SCIPD"])
    for preset in ("host-debug", "host-release"):
        candidate = REPO / "build" / preset / "apps" / "scipd"
        if candidate.exists():
            return candidate
    return None


def free_udp_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


@dataclass
class Scipd:
    host: str
    port: int
    discovery_port: int
    process: subprocess.Popen


@pytest.fixture
def scipd() -> Iterator[Scipd]:
    """A scipd on free ports, stopped after the test."""
    binary = find_scipd()
    if binary is None:
        pytest.skip("scipd not built (cmake --workflow --preset host-check), or set $SCIPD")
    discovery = free_udp_port()
    process = subprocess.Popen(
        [
            str(binary),
            "--bind",
            "127.0.0.1",
            "--port",
            "0",
            "--discovery-port",
            str(discovery),
            "--name",
            "pytest-target",
        ],
        stderr=subprocess.PIPE,
        text=True,
    )
    # First line: "scipd: AIPC 1.0 on 127.0.0.1:PORT, ..."
    line = process.stderr.readline()
    match = re.search(r"on [\d.]+:(\d+)", line)
    if not match:
        process.kill()
        pytest.fail(f"scipd did not start: {line!r}")
    yield Scipd("127.0.0.1", int(match.group(1)), discovery, process)
    process.terminate()
    process.wait(timeout=10)
