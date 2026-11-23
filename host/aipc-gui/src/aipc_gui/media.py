"""Video on the host, with GStreamer (system python3-gi). No Qt in here.

Streamer     a local video file -> H.264 over RTP/UDP to the target's
             input rtp://:5000, with player controls (play, pause, seek,
             speed, loop, A-B loop) and a preview of what is sent.
RtpReceiver  H.264 over RTP/UDP from the target's output udp:HOST:5002 ->
             frames for display.

Frames are handed out as `Frame` (RGBx bytes) from GStreamer's threads; the
GUI turns them into QImages on its own thread. Bus messages are handled in
`poll()`, which the GUI calls from a timer, so no GLib main loop is needed.

Looping uses *segment seeks*: GStreamer reports SEGMENT_DONE at the end of the
range and we seek back without flushing. The running time, and with it the
RTP timestamps the target sees, keeps increasing, so the target's receiver
never notices the jump.
"""

from __future__ import annotations

import threading
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path

try:
    import gi

    gi.require_version("Gst", "1.0")
    from gi.repository import Gst

    Gst.init(None)
    GST_ERROR = ""
except (ImportError, ValueError) as error:  # no python3-gi, or no GStreamer typelib
    Gst = None
    GST_ERROR = str(error)


SECOND = 1_000_000_000  # GStreamer time unit: nanoseconds
RTP_H264_CAPS = "application/x-rtp,media=video,clock-rate=90000,encoding-name=H264,payload=96"


def available() -> bool:
    return Gst is not None


def has_element(name: str) -> bool:
    return available() and Gst.ElementFactory.find(name) is not None


def first_element(*names: str) -> str | None:
    return next((name for name in names if has_element(name)), None)


@dataclass
class Frame:
    """One RGBx picture (4 bytes per pixel, rows `stride` bytes apart)."""

    width: int
    height: int
    stride: int
    data: bytes


FrameCallback = Callable[[Frame], None]


def _frame_from_sample(sample) -> Frame | None:
    caps = sample.get_caps()
    buffer = sample.get_buffer()
    if caps is None or buffer is None:
        return None
    s = caps.get_structure(0)
    width, height = s.get_value("width"), s.get_value("height")
    ok, info = buffer.map(Gst.MapFlags.READ)
    if not ok:
        return None
    try:
        data = bytes(info.data)
    finally:
        buffer.unmap(info)
    return Frame(width, height, len(data) // height if height else 0, data)


def _attach_appsink(pipeline, name: str, on_frame: FrameCallback | None) -> None:
    sink = pipeline.get_by_name(name)

    def new_sample(appsink):
        sample = appsink.emit("pull-sample")
        if sample is not None and on_frame is not None:
            frame = _frame_from_sample(sample)
            if frame is not None:
                on_frame(frame)
        return Gst.FlowReturn.OK

    sink.connect("new-sample", new_sample)


def _error_text(message) -> str:
    error, debug = message.parse_error()
    source = message.src.get_name() if message.src else "?"
    return f"{source}: {error.message}"


# ---------------------------------------------------------------------------
# Streamer
# ---------------------------------------------------------------------------


@dataclass
class MediaInfo:
    path: str
    duration: float  # seconds, 0 if unknown
    width: int = 0
    height: int = 0
    fps: float = 0.0


class Streamer:
    """Plays a video file to a target as H.264/RTP.

    States: "closed" (no file), "paused", "playing", "ended".
    """

    def __init__(
        self,
        on_frame: FrameCallback | None = None,
        on_error: Callable[[str], None] | None = None,
        preview_width: int = 640,
        bitrate_kbps: int = 6000,
    ):
        if not available():
            raise RuntimeError(f"GStreamer for Python is not available: {GST_ERROR}")
        self.on_frame = on_frame
        self.on_error = on_error
        self.preview_width = preview_width
        self.bitrate_kbps = bitrate_kbps
        self.host = "127.0.0.1"
        self.port = 5000
        self.pipeline = None
        self.info: MediaInfo | None = None
        self.state = "closed"
        self.rate = 1.0
        self.loop = False
        self.a: float | None = None  # A-B loop, seconds
        self.b: float | None = None

    # --- file ---------------------------------------------------------------

    def open(self, path: str | Path) -> MediaInfo:
        """Load a file and show its first frame (paused)."""
        self.close()
        path = Path(path).resolve()
        if not path.exists():
            raise FileNotFoundError(path)
        encoder = self._encoder()
        description = (
            f'uridecodebin uri="{path.as_uri()}" caps="video/x-raw(ANY)" expose-all-streams=false '
            "! videoconvert ! tee name=t "
            # To the target: paced by the clock (sync=true) like a real camera.
            f"t. ! queue max-size-buffers=4 ! {encoder} "
            "! rtph264pay config-interval=1 pt=96 "
            f"! udpsink name=udp host={self.host} port={self.port} sync=true "
            # Preview: small, never holds the stream back. Decoded frames arrive
            # up to the encoder queue's 4 frames early (they wait for their time
            # in the sink), so this queue must hold more than that or it drops.
            "t. ! queue max-size-buffers=8 leaky=downstream ! videoscale ! videoconvert "
            f"! video/x-raw,format=RGBx,width={self.preview_width},pixel-aspect-ratio=1/1 "
            "! appsink name=preview emit-signals=true max-buffers=1 drop=true sync=true"
        )
        self.pipeline = Gst.parse_launch(description)
        _attach_appsink(self.pipeline, "preview", self.on_frame)
        self.pipeline.set_state(Gst.State.PAUSED)
        change, _, _ = self.pipeline.get_state(10 * SECOND)
        if change == Gst.StateChangeReturn.FAILURE:
            error = self._pop_error() or "cannot play this file"
            self.close()
            raise RuntimeError(error)
        self.info = self._media_info(str(path))
        self.state = "paused"
        self.a = self.b = None
        return self.info

    def close(self) -> None:
        if self.pipeline is not None:
            self.pipeline.set_state(Gst.State.NULL)
            self.pipeline = None
        self.state = "closed"
        self.info = None

    def set_destination(self, host: str, port: int) -> None:
        """Where the video goes; works while playing."""
        self.host, self.port = host, port
        if self.pipeline is not None:
            udp = self.pipeline.get_by_name("udp")
            udp.set_property("host", host)
            udp.set_property("port", port)

    # --- transport ----------------------------------------------------------

    def play(self) -> None:
        if self.pipeline is None:
            return
        if self.state == "ended":
            self.seek(self._range()[0])
        self.pipeline.set_state(Gst.State.PLAYING)
        self.state = "playing"

    def pause(self) -> None:
        if self.pipeline is None:
            return
        self.pipeline.set_state(Gst.State.PAUSED)
        if self.state == "playing":
            self.state = "paused"

    def toggle(self) -> None:
        self.pause() if self.state == "playing" else self.play()

    def stop(self) -> None:
        """Pause at the start."""
        self.pause()
        self.seek(self._range()[0])
        if self.state != "closed":
            self.state = "paused"

    def seek(self, seconds: float) -> None:
        """Jump to a position (accurate: the exact frame, not the previous key frame)."""
        if self.pipeline is None:
            return
        start, end = self._range()
        if self.looping():
            seconds = min(max(seconds, start), end) if end else max(seconds, start)
        self._seek(max(0.0, seconds), flush=True)
        if self.state == "ended":
            self.state = "paused"

    def skip(self, seconds: float) -> None:
        """Relative seek: negative rewinds."""
        self.seek(self.position() + seconds)

    def set_rate(self, rate: float) -> None:
        """Playback speed, e.g. 0.5 or 2.0 (forward only)."""
        if rate <= 0:
            raise ValueError("rate must be > 0")
        self.rate = rate
        if self.pipeline is not None:
            self._seek(self.position(), flush=True)

    def set_loop(self, loop: bool) -> None:
        self.loop = loop
        self._reseek()

    def set_ab(self, a: float | None, b: float | None) -> None:
        """Loop between a and b seconds; None, None clears it."""
        if a is not None and b is not None and b <= a:
            raise ValueError("B must be after A")
        self.a, self.b = a, b
        self._reseek()

    def looping(self) -> bool:
        return self.loop or (self.a is not None and self.b is not None)

    def position(self) -> float:
        if self.pipeline is None:
            return 0.0
        ok, position = self.pipeline.query_position(Gst.Format.TIME)
        return position / SECOND if ok and position >= 0 else 0.0

    def duration(self) -> float:
        return self.info.duration if self.info else 0.0

    # --- events ---------------------------------------------------------------

    def poll(self) -> None:
        """Handle bus messages; call regularly (the GUI uses a 50 ms timer)."""
        if self.pipeline is None:
            return
        bus = self.pipeline.get_bus()
        while (message := bus.pop()) is not None:
            if message.type == Gst.MessageType.SEGMENT_DONE:
                start, end = self._range()
                self._seek(start, flush=False)  # seamless: running time continues
            elif message.type == Gst.MessageType.EOS:
                self.pipeline.set_state(Gst.State.PAUSED)
                self.state = "ended"
            elif message.type == Gst.MessageType.ERROR:
                text = _error_text(message)
                self.pipeline.set_state(Gst.State.PAUSED)
                self.state = "ended"
                if self.on_error:
                    self.on_error(text)

    # --- internals ------------------------------------------------------------

    def _encoder(self) -> str:
        # Software H.264, tuned for latency: no B-frames, a key frame every
        # second so a receiver that joins late starts quickly.
        if has_element("x264enc"):
            return (
                "videoconvert ! video/x-raw,format=I420 ! x264enc tune=zerolatency "
                f"speed-preset=ultrafast key-int-max=30 bitrate={self.bitrate_kbps}"
            )
        if has_element("openh264enc"):
            return (
                "videoconvert ! video/x-raw,format=I420 ! openh264enc "
                f"bitrate={self.bitrate_kbps * 1000} gop-size=30"
            )
        raise RuntimeError("no H.264 encoder (install gstreamer1.0-plugins-ugly for x264enc)")

    def _media_info(self, path: str) -> MediaInfo:
        ok, duration = self.pipeline.query_duration(Gst.Format.TIME)
        info = MediaInfo(path, duration / SECOND if ok and duration > 0 else 0.0)
        pad = self.pipeline.get_by_name("t").get_static_pad("sink")
        caps = pad.get_current_caps() if pad else None
        if caps is not None:
            s = caps.get_structure(0)
            info.width, info.height = s.get_value("width"), s.get_value("height")
            ok, num, den = s.get_fraction("framerate")
            if ok and den:
                info.fps = num / den
        return info

    def _range(self) -> tuple[float, float | None]:
        """What plays: the A-B range when set, else the whole file."""
        if self.a is not None and self.b is not None:
            return self.a, self.b
        return 0.0, None

    def _reseek(self) -> None:
        if self.pipeline is not None and self.state != "closed":
            self.seek(self.position())

    def _seek(self, seconds: float, flush: bool) -> None:
        flags = Gst.SeekFlags.ACCURATE
        if flush:
            flags |= Gst.SeekFlags.FLUSH
        if self.looping():
            flags |= Gst.SeekFlags.SEGMENT  # SEGMENT_DONE instead of EOS
        _, end = self._range()
        stop_type = Gst.SeekType.SET if end is not None else Gst.SeekType.NONE
        stop = int(end * SECOND) if end is not None else -1
        self.pipeline.seek(
            self.rate,
            Gst.Format.TIME,
            flags,
            Gst.SeekType.SET,
            int(seconds * SECOND),
            stop_type,
            stop,
        )
        if flush:
            # Flushing seeks complete asynchronously; wait so position() is right.
            self.pipeline.get_state(2 * SECOND)

    def _pop_error(self) -> str | None:
        message = self.pipeline.get_bus().pop_filtered(Gst.MessageType.ERROR)
        return _error_text(message) if message else None


# ---------------------------------------------------------------------------
# RtpReceiver
# ---------------------------------------------------------------------------


class RtpReceiver:
    """Shows what a target sends with output udp:THIS_HOST:PORT."""

    def __init__(self, port: int, on_frame: FrameCallback | None = None, max_width: int = 1280):
        if not available():
            raise RuntimeError(f"GStreamer for Python is not available: {GST_ERROR}")
        self.port = port
        self.on_frame = on_frame
        self.max_width = max_width
        self.pipeline = None
        self.frames = 0
        self.error = ""
        self._lock = threading.Lock()

    def start(self) -> None:
        self.stop()
        # avdec_h264 by name: on PCs decodebin may pick a VA-API decoder that
        # drops frames of a live RTP stream.
        decoder = first_element("avdec_h264", "openh264dec") or "decodebin"
        description = (
            f'udpsrc port={self.port} caps="{RTP_H264_CAPS}" '
            "! rtpjitterbuffer latency=50 drop-on-latency=true ! rtph264depay ! h264parse "
            f"! {decoder} ! videoconvert ! videoscale "
            f"! video/x-raw,format=RGBx,width=[1,{self.max_width}],pixel-aspect-ratio=1/1 "
            "! appsink name=sink emit-signals=true max-buffers=1 drop=true sync=false"
        )
        self.pipeline = Gst.parse_launch(description)

        def counted(frame: Frame) -> None:
            with self._lock:
                self.frames += 1
            if self.on_frame:
                self.on_frame(frame)

        _attach_appsink(self.pipeline, "sink", counted)
        self.error = ""
        self.pipeline.set_state(Gst.State.PLAYING)

    def stop(self) -> None:
        if self.pipeline is not None:
            self.pipeline.set_state(Gst.State.NULL)
            self.pipeline = None

    @property
    def running(self) -> bool:
        return self.pipeline is not None

    def poll(self) -> None:
        if self.pipeline is None:
            return
        bus = self.pipeline.get_bus()
        while (message := bus.pop()) is not None:
            if message.type == Gst.MessageType.ERROR:
                self.error = _error_text(message)
                self.stop()
                return
