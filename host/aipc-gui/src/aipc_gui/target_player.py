"""The player API of media.Streamer, for a video file the target plays itself.

The target opens the file with input.set {"player": true}; this class turns
the player panel's commands into playback.* calls. The position comes back
with every event.stats (twice a second) and moves on by itself in between,
so the slider runs smoothly. No Qt in here: the client only needs `call()`.
"""

from __future__ import annotations

import time
from collections.abc import Callable
from typing import Any

from .media import MediaInfo


class TargetPlayer:
    """States like Streamer: "closed", "paused", "playing", "ended"."""

    def __init__(self, client, open_file: Callable[[str], None]):
        self.client = client
        self.open_file = open_file  # asks the target to open a file as a player
        self.info: MediaInfo | None = None
        self.state = "closed"
        self.rate = 1.0
        self.loop = False
        self.a: float | None = None
        self.b: float | None = None
        self._position = 0.0
        self._at = time.monotonic()
        self._duration = 0.0

    # --- from the target ------------------------------------------------------

    def update(self, playback: dict[str, Any] | None, source: dict[str, Any] | None = None) -> None:
        """A `Playback` (playback.* results, Status.playback), and the Status
        `input` when there is one (path, size, frame rate)."""
        if not playback or playback.get("available") is False:
            self.state, self.info = "closed", None
            return
        predicted = self.position()
        was_playing = self.state == "playing"
        self.state = playback["state"]
        position = float(playback["position"])
        # The target reports a position up to ~0.25 s old: while it plays on,
        # keep the smooth local estimate unless it is clearly off.
        if not (was_playing and self.state == "playing" and abs(position - predicted) < 0.5):
            self._position, self._at = position, time.monotonic()
        self._duration = float(playback.get("duration") or 0.0)
        self.rate = float(playback["rate"])
        self.loop = bool(playback["loop"])
        self.a, self.b = playback.get("a"), playback.get("b")
        if source is not None:
            info = source.get("info", {})
            self.info = MediaInfo(
                source.get("uri", ""),
                self._duration,
                info.get("width", 0),
                info.get("height", 0),
                float(source.get("fps", 0.0)),
            )
        elif self.info is not None and self.info.duration != self._duration:
            self.info = MediaInfo(
                self.info.path, self._duration, self.info.width, self.info.height, self.info.fps
            )

    # --- the Streamer API -------------------------------------------------------

    def open(self, path: str) -> MediaInfo:
        """Ask the target to play `path` (a file on the target). The details
        arrive with the next statistics."""
        self.open_file(str(path))
        self.info = MediaInfo(str(path), 0.0)
        self.state, self._position, self._at = "paused", 0.0, time.monotonic()
        self.a = self.b = None
        return self.info

    def close(self) -> None:
        """The target keeps its input; nothing to release here."""

    def play(self) -> None:
        if self.state == "ended":
            self._position = self._range()[0]
        self._set_state("playing")
        self._call("playback.play")

    def pause(self) -> None:
        self._position = self.position()
        self._set_state("paused")
        self._call("playback.pause")

    def toggle(self) -> None:
        self.pause() if self.state == "playing" else self.play()

    def stop(self) -> None:
        """Pause at the start."""
        self.pause()
        self.seek(self._range()[0])

    def seek(self, seconds: float) -> None:
        start, end = self._range()
        seconds = max(start, seconds if end is None else min(seconds, end))
        if self._duration:
            seconds = min(seconds, self._duration)
        self._position, self._at = seconds, time.monotonic()
        if self.state == "ended":
            self.state = "paused"
        self._call("playback.seek", {"position": seconds})

    def skip(self, seconds: float) -> None:
        self.seek(self.position() + seconds)

    def set_rate(self, rate: float) -> None:
        if rate <= 0:
            raise ValueError("rate must be > 0")
        self._position, self.rate = self.position(), rate
        self._at = time.monotonic()
        self._call("playback.set", {"rate": rate})

    def set_loop(self, loop: bool) -> None:
        self.loop = loop
        self._call("playback.set", {"loop": loop})

    def set_ab(self, a: float | None, b: float | None) -> None:
        if a is not None and b is not None and b <= a:
            raise ValueError("B must be after A")
        self.a, self.b = a, b
        self._call("playback.set", {"a": a, "b": b})

    def looping(self) -> bool:
        return self.loop or (self.a is not None and self.b is not None)

    def position(self) -> float:
        if self.state != "playing":
            return self._position
        moved = self._position + (time.monotonic() - self._at) * self.rate
        end = self._range()[1] or self._duration
        return min(moved, end) if end else moved

    def duration(self) -> float:
        return self._duration

    def poll(self) -> None:
        """Updates come with the target's events; nothing to poll."""

    # --- internals ----------------------------------------------------------------

    def _range(self) -> tuple[float, float | None]:
        if self.a is not None and self.b is not None:
            return self.a, self.b
        return 0.0, None

    def _set_state(self, state: str) -> None:
        if self.state != "closed":
            self.state = state
            self._at = time.monotonic()

    def _call(self, method: str, params: dict[str, Any] | None = None) -> None:
        self.client.call(method, params or {}, on_result=self._accepted)

    def _accepted(self, playback: dict[str, Any]) -> None:
        """A command's result: the settings as the target took them. Its position
        predates the command, so the local one is kept."""
        position, at = self._position, self._at
        self.update(playback)
        if self.state != "ended":
            self._position, self._at = position, at
