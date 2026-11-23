"""The Qt client against a real scipd, driven by Qt's event loop."""

from __future__ import annotations

from aipc_gui.protocol import REJECTED
from aipc_gui.qt_client import QtAipcClient, QtDiscovery


def test_requests_events_and_errors(qtbot, scipd):
    client = QtAipcClient()
    with qtbot.waitSignal(client.connected, timeout=3000):
        client.connect_to(scipd.host, scipd.port)
    assert client.local_address == "127.0.0.1"

    results, errors, events = [], [], []
    client.event.connect(lambda method, params: events.append(method))
    client.call("events.subscribe", {"topics": ["config"]})
    client.call("system.ping", on_result=results.append)
    client.call("pipeline.set", {"text": "blur ksize=4"}, on_error=errors.append)
    client.call("pipeline.set", {"text": "gray"})
    qtbot.waitUntil(lambda: bool(results and errors and "event.config" in events), timeout=3000)
    assert "uptime" in results[0]
    assert errors[0].code == REJECTED

    failures = []
    client.failed.connect(failures.append)
    client.call("no.such.method")
    qtbot.waitUntil(lambda: bool(failures), timeout=3000)
    assert failures[0].startswith("no.such.method:")
    client.close()


def test_reports_a_refused_connection(qtbot, scipd):
    client = QtAipcClient()
    with qtbot.waitSignal(client.disconnected, timeout=4000) as blocker:
        client.connect_to("127.0.0.1", 1)  # nothing listens there
    assert blocker.args[0]


def test_discovery(qtbot, scipd):
    discovery = QtDiscovery(port=scipd.discovery_port)
    found = []
    discovery.found.connect(found.append)
    with qtbot.waitSignal(discovery.finished, timeout=3000):
        discovery.start(timeout_ms=500)
    assert [t.name for t in found] == ["pytest-target"]
