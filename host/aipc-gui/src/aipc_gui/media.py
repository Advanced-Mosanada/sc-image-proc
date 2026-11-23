"""Video on the host, with GStreamer (system python3-gi). No Qt in here.

Streamer     a local video file -> the target, with player controls (play,
             pause, seek, speed, loop, A-B loop) and a preview of what is
             sent. Either H.264 over RTP/UDP (input rtp://:5000; the
             compression is set by StreamSettings) or raw frames in GDP
             over TCP (input tcp://:5000; lossless, ~124 Mbit/s for PAL).
RtpReceiver  H.264 over RTP/UDP from the target's output udp:HOST:5002 ->
             frames for display.

Everything happens in `poll()`, which the GUI calls from a timer on its own
thread: bus messages are handled and the newest frame is pulled from the
appsink and handed to `on_frame` as `Frame` (RGBx bytes). No Python runs on
GStreamer's streaming threads (a Python callback there halved the preview
rate: every frame had to wait for the GIL), and no GLib main loop is needed.

Looping uses *segment seeks*: GStreamer reports SEGMENT_DONE at the end of the
range and we seek back without flushing. The running time, and with it the
RTP timestamps the target sees, keeps increasing, so the target's receiver
never notices the jump.
"""

from __future__ import annotations

import time
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


FrameCallback = Callable[[Frame], None]  # called from poll(), on the caller's thread


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


def _pull_frame(pipeline, name: str) -> Frame | None:
    """The newest frame of an appsink (max-buffers=1 drop=true), without waiting."""
    sample = pipeline.get_by_name(name).emit("try-pull-sample", 0)
    return _frame_from_sample(sample) if sample is not None else None


def _error_text(message) -> str:
    error, debug = message.parse_error()
    source = message.src.get_name() if message.src else "?"
    return f"{source}: {error.message}"


# ---------------------------------------------------------------------------
# Streamer
# ---------------------------------------------------------------------------


X264_PRESETS = (
    "ultrafast",
    "superfast",
    "veryfast",
    "faster",
    "fast",
    "medium",
)
RAW_FORMATS = {"I420": 1.5, "BGR": 3.0}  # bytes per pixel
CONNECT_TIMEOUT = 3.0  # seconds: the target may still be opening its tcp:// input


@dataclass(frozen=True)
class StreamSettings:
    """How the video travels to the target.

    raw=False: H.264 over RTP/UDP, compressed by x264 (openh264 as a fallback):
      quality 0 = constant bitrate `bitrate_kbps`; quality 1..51 = constant
      quality (x264's CRF, lower is better, ~18 looks lossless); `preset`
      trades encoding speed for quality per bit; a key frame every
      `keyframe_interval` frames lets a late receiver start.
    raw=True: every pixel as decoded, in GDP over TCP. I420 is what most videos
      decode to (4:2:0), so it is lossless for them at half the size of BGR.
    """

    raw: bool = False
    raw_format: str = "I420"
    bitrate_kbps: int = 6000
    quality: int = 0
    preset: str = "ultrafast"
    keyframe_interval: int = 30

    def uri(self, port: int) -> str:
        """The target's input for this stream."""
        return f"{'tcp' if self.raw else 'rtp'}://:{port}"

    def megabits(self, width: int, height: int, fps: float) -> float | None:
        """Network load in Mbit/s; None when it depends on the content (quality mode)."""
        if self.raw:
            return width * height * RAW_FORMATS[self.raw_format] * (fps or 30.0) * 8 / 1e6
        return None if self.quality else self.bitrate_kbps / 1000

    def describe(self) -> str:
        if self.raw:
            return f"raw {self.raw_format}"
        rate = f"quality {self.quality}" if self.quality else f"{self.bitrate_kbps} kbit/s"
        return f"H.264 {rate}, {self.preset}"


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
        settings: StreamSettings | None = None,
    ):
        if not available():
            raise RuntimeError(f"GStreamer for Python is not available: {GST_ERROR}")
        self.on_frame = on_frame
        self.on_error = on_error
        self.preview_width = preview_width
        self.settings = settings or StreamSettings()
        self.host = "127.0.0.1"
        self.port = 5000
        self.pipeline = None
        self.info: MediaInfo | None = None
        self.state = "closed"
        self.rate = 1.0
        self.loop = False
        self.a: float | None = None  # A-B loop, seconds
        self.b: float | None = None
        self.broken = False  # the connection to the target failed: reconnect on play

    # --- file ---------------------------------------------------------------

    def open(self, path: str | Path) -> MediaInfo:
        """Load a file and show its first frame (paused)."""
        self.close()
        path = Path(path).resolve()
        if not path.exists():
            raise FileNotFoundError(path)
        description = (
            f'uridecodebin uri="{path.as_uri()}" caps="video/x-raw(ANY)" expose-all-streams=false '
            "! videoconvert ! tee name=t "
            # To the target: paced by the clock (sync=true) like a real camera.
            f"t. ! queue max-size-buffers=4 ! {self._sender()} "
            # Preview: small, never holds the stream back. Decoded frames arrive
            # up to the encoder queue's 4 frames early (they wait for their time
            # in the sink), so this queue must hold more than that or it drops.
            "t. ! queue max-size-buffers=8 leaky=downstream ! videoscale ! videoconvert "
            f"! video/x-raw,format=RGBx,width={self.preview_width},pixel-aspect-ratio=1/1 "
            "! appsink name=preview max-buffers=1 drop=true sync=true"
        )
        # Raw: the TCP connection is made here; the target may still be (re)opening
        # its input, so try again for a moment.
        deadline = time.monotonic() + (CONNECT_TIMEOUT if self.settings.raw else 0)
        while True:
            self.pipeline = Gst.parse_launch(description)
            self.pipeline.set_state(Gst.State.PAUSED)
            change, _, _ = self.pipeline.get_state(10 * SECOND)
            if change != Gst.StateChangeReturn.FAILURE:
                break
            error = self._pop_error() or "cannot play this file"
            self.close()
            if time.monotonic() > deadline or "connect" not in error.lower():
                raise RuntimeError(error)
            time.sleep(0.2)
        self.info = self._media_info(str(path))
        self.state = "paused"
        self.a = self.b = None
        self.broken = False
        return self.info

    def close(self) -> None:
        if self.pipeline is not None:
            self.pipeline.set_state(Gst.State.NULL)
            self.pipeline = None
        self.state = "closed"
        self.info = None

    def set_destination(self, host: str, port: int) -> None:
        """Where the video goes; works while playing."""
        if (host, port) == (self.host, self.port):
            return
        self.host, self.port = host, port
        if self.pipeline is None:
            return
        if self.settings.raw:
            self._rebuild()  # a TCP connection cannot be moved
        else:
            udp = self.pipeline.get_by_name("udp")
            udp.set_property("host", host)
            udp.set_property("port", port)

    def configure(self, settings: StreamSettings) -> None:
        """Change how the video is sent; an open file continues where it was."""
        if settings == self.settings:
            return
        self.settings = settings
        if self.pipeline is not None:
            self._rebuild()

    # --- transport ----------------------------------------------------------

    def play(self) -> None:
        if self.pipeline is None:
            return
        if self.broken:
            self._rebuild()  # e.g. the target restarted its input
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
        """Deliver the newest preview frame and handle bus messages. Call it
        more often than the frame rate (the GUI uses a 15 ms timer)."""
        if self.pipeline is None:
            return
        frame = _pull_frame(self.pipeline, "preview")
        if frame is not None and self.on_frame is not None:
            self.on_frame(frame)
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
                self.broken = True
                if self.on_error:
                    self.on_error(text)

    # --- internals ------------------------------------------------------------

    def _sender(self) -> str:
        """Encoder (or not) and network sink, for the current settings."""
        st = self.settings
        if st.raw:
            if st.raw_format not in RAW_FORMATS:
                raise ValueError(f"raw format must be one of {', '.join(RAW_FORMATS)}")
            return (
                f"videoconvert ! video/x-raw,format={st.raw_format} ! gdppay "
                f"! tcpclientsink name=tcp host={self.host} port={self.port} sync=true"
            )
        return (
            f"{self._encoder()} ! rtph264pay config-interval=1 pt=96 "
            f"! udpsink name=udp host={self.host} port={self.port} sync=true"
        )

    def _encoder(self) -> str:
        # Software H.264 tuned for latency: no B-frames, and a key frame every
        # `keyframe_interval` frames so a receiver that joins late starts quickly.
        st = self.settings
        if has_element("x264enc"):
            if st.preset not in X264_PRESETS:
                raise ValueError(f"preset must be one of {', '.join(X264_PRESETS)}")
            rate = (
                f"pass=qual quantizer={st.quality}" if st.quality else f"bitrate={st.bitrate_kbps}"
            )
            return (
                "videoconvert ! video/x-raw,format=I420 ! x264enc tune=zerolatency "
                f"speed-preset={st.preset} key-int-max={st.keyframe_interval} {rate}"
            )
        if has_element("openh264enc"):  # no presets or quality mode
            return (
                "videoconvert ! video/x-raw,format=I420 ! openh264enc "
                f"bitrate={st.bitrate_kbps * 1000} gop-size={st.keyframe_interval}"
            )
        raise RuntimeError("no H.264 encoder (install gstreamer1.0-plugins-ugly for x264enc)")

    def _rebuild(self) -> None:
        """Open the same file again (new settings or destination), where it was."""
        if self.info is None:
            return
        path, position, state = self.info.path, self.position(), self.state
        a, b = self.a, self.b
        self.open(path)
        self.a, self.b = a, b
        self.seek(position)
        if state == "playing":
            self.play()

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
            "! appsink name=sink max-buffers=1 drop=true sync=false"
        )
        self.pipeline = Gst.parse_launch(description)
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
        """Deliver the newest frame and handle errors; call it from a timer."""
        if self.pipeline is None:
            return
        frame = _pull_frame(self.pipeline, "sink")
        if frame is not None:
            self.frames += 1
            if self.on_frame is not None:
                self.on_frame(frame)
        bus = self.pipeline.get_bus()
        while (message := bus.pop()) is not None:
            if message.type == Gst.MessageType.ERROR:
                self.error = _error_text(message)
                self.stop()
                return
