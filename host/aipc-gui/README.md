# aipc-gui

Host tools for `scipd` targets (the PC itself or a board), over AIPC, the
AMC Image Proc Control protocol ([docs/aipc-protocol.md](../../docs/aipc-protocol.md)):

- `aipc-gui`: the GUI. It finds targets and shows what they are. You can build
  the pipeline from the functions the target offers, choose its input and
  outputs, stream a video file to it (play, pause, seek, speed, loop, A-B loop),
  and watch the result.
- `aipc`: one AIPC call from the shell, for scripts and quick checks.

## Setup

Python packages come from PyPI through [uv](https://docs.astral.sh/uv/).
GStreamer comes from the system's Python bindings (`python3-gi`), so the
virtual environment must see the system packages:

```bash
sudo apt install python3-gi gir1.2-gstreamer-1.0 gir1.2-gst-plugins-base-1.0 \
    gstreamer1.0-plugins-good gstreamer1.0-plugins-ugly gstreamer1.0-libav
cd host/aipc-gui
uv venv --system-site-packages --python /usr/bin/python3
uv sync
```

`uv sync` keeps the existing `.venv`, including its access to the system
packages.

Why not PyGObject from PyPI? It compiles against the system's GLib and
GStreamer headers (`-dev` packages, a C toolchain). The system copy already
matches the installed GStreamer.

## Use

```bash
uv run aipc-gui                       # finds targets on the LAN
uv run aipc-gui --target 192.168.50.2
uv run aipc --discover
uv run aipc 192.168.50.2 system.info
uv run aipc 192.168.50.2 pipeline.set '{"text": "gray ! blur ksize=5"}'
```

On the target, run `scipd` (the build's `apps/scipd` on the PC, or
`scripts/board-run.sh scipd` for the board).

## Development

```bash
uv run pytest          # starts the scipd of build/host-debug (or $SCIPD)
uv run ruff check . && uv run ruff format --check .
```
