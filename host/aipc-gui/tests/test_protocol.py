"""The plain-Python client against a real scipd."""

from __future__ import annotations

import pytest

from aipc_gui import protocol
from aipc_gui.protocol import AipcClient, AipcError


def test_calls_and_errors(scipd):
    with AipcClient(scipd.host, scipd.port) as target:
        info = target.call("system.info")
        assert info["protocol"]["version"] == protocol.PROTOCOL_VERSION
        assert info["device"]["name"] == "pytest-target"

        names = [f["name"] for f in target.call("functions.list")["functions"]]
        assert "blur" in names

        with pytest.raises(AipcError) as error:
            target.call("pipeline.set", {"text": "blur ksize=4"})
        assert error.value.code == protocol.REJECTED
        assert "ksize" in error.value.message

        with pytest.raises(AipcError) as error:
            target.call("no.such.method")
        assert error.value.code == protocol.METHOD_NOT_FOUND


def test_events_arrive_between_responses(scipd):
    events = []
    with AipcClient(scipd.host, scipd.port, on_event=lambda m, p: events.append(m)) as target:
        target.call("events.subscribe", {"topics": ["config", "state"]})
        target.call("pipeline.set", {"text": "gray"})
        target.call("system.ping")  # events queued before this reply are dispatched
        assert "event.config" in events


def test_discovery(scipd):
    targets = protocol.discover(["127.0.0.1"], port=scipd.discovery_port, timeout=1.0)
    assert [t.name for t in targets] == ["pytest-target"]
    assert targets[0].port == scipd.port


def test_command_line(scipd, capsys):
    assert protocol.main([scipd.host, "--port", str(scipd.port), "system.ping"]) == 0
    assert '"uptime"' in capsys.readouterr().out
    assert (
        protocol.main([scipd.host, "--port", str(scipd.port), "pipeline.set", '{"text": "nope"}'])
        == 1
    )
