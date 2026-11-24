# sc-image-proc (scip)

An image processing pipeline that is developed and tested on a Linux PC and
deployed to an NXP i.MX 8M Plus board (MYiR MYD-JX8MP) or a Radxa ROCK 5B+
(Rockchip RK3588) for performance work.

The same source builds for both targets; only the CMake preset changes.

Two ways to use it:

- `scip-cli` runs one pipeline from the command line and reports where the
  time goes (benchmarks, quick checks).
- `scipd` runs on a target (the PC or the board) and is controlled over the
  network with **AIPC**, the AMC Image Proc Control protocol
  ([docs/aipc-protocol.md](docs/aipc-protocol.md)). The host GUI
  ([host/aipc-gui](host/aipc-gui)) uses it to build the pipeline from the
  functions the target offers, stream a video file to the target with player
  controls, choose its outputs and watch the result.

## Layout

```
include/scip/        public headers
  frame.hpp            Frame / FrameInfo / PixelFormat: the image buffer type
  stage.hpp            Stage: interface every processing step implements
  pipeline.hpp         Pipeline: runs stages in order, records per-stage latency
  source.hpp           FrameSource: interface for inputs (files, patterns, video, camera)
  sink.hpp             FrameSink: interface for outputs (display, video file, network)
  params.hpp           ParamSpec: self-describing function parameters
  registry.hpp         StageRegistry: all functions, pipeline text ("gray ! blur ksize=5")
  io.hpp               openSource() / openSink() from URIs; cameras and displays
  engine.hpp           Engine: input -> functions -> outputs, reconfigurable while running
  aipc.hpp             the AIPC control interface (Service, Server)
  cv.hpp               zero-copy Frame <-> cv::Mat views
src/
  core/                scip_core: depends only on the C++ standard library
  stages/              scip_stages: processing functions (CPU reference, OpenCV)
  gst/                 scip_gst: GStreamer helpers; scip_gststages: GStreamer filters as functions
  io/                  scip_io: sources and sinks
    gst/               GStreamer: video files, camera, RTP, display, H.264 record / UDP
  engine/              scip_engine: the running pipeline, on its own thread
  aipc/                scip_aipc: JSON-RPC over TCP, UDP discovery, system information
  common/              internal helpers (not installed)
apps/
  scip-cli/            command-line runner / benchmark tool
  scipd/               the target daemon, controlled over AIPC
host/aipc-gui/         host GUI and `aipc` command (Python, PySide6, uv)
tests/                 GoogleTest unit tests + CLI smoke tests
cmake/                 warnings, sanitizers, build info, board toolchain files
scripts/               deploy / run / test on the board
docs/                  board facts (board.md: i.MX, board-rock5bplus.md: RK3588), the AIPC protocol
```

Library dependencies only point downwards:
`core` <- `stages`, `io` <- `engine` <- `aipc` <- `apps`.
Hardware backends (NEON, OpenCL, G2D, NPU) will be added as more libraries
next to `stages`, each behind a CMake option.

## Prerequisites

PC (Ubuntu 24.04): `cmake` >= 3.25, `ninja`, `g++`, `libopencv-dev`, `clang-format`,
`libgstreamer1.0-dev`, `libgstreamer-plugins-base1.0-dev`, `gstreamer1.0-libav`,
`gstreamer1.0-plugins-ugly` (x264 encoder),
`qemu-user-static` (optional; runs cross-built tests on the PC) and
`gdb-multiarch` (optional; remote debugging on the board).

Board: the Yocto SDK that matches the board image, installed at
`/mnt/ambat/work/sc_display/sdk` (override with `SCIP_SDK_DIR`). The SDK
does not need to be sourced; `cmake/toolchains/imx8mp-myir-sdk.cmake` uses it directly.

ROCK 5B+ (RK3588): `g++-12-aarch64-linux-gnu` on the PC and a sysroot copied
from the board with `scripts/rk3588-sysroot.sh` (to `~/sysroots/rock5bplus-debian12`,
override with `SCIP_SYSROOT_DIR`); see [docs/board-rock5bplus.md](docs/board-rock5bplus.md).

## Build and test

| Preset | What |
|---|---|
| `host-debug` | PC, debug, everyday development |
| `host-asan` | PC, debug + AddressSanitizer / UBSan |
| `host-release` | PC, optimised |
| `imx8mp-release` | board, `-O3`, for benchmarks |
| `imx8mp-relwithdebinfo` | board, optimised + symbols, for `perf` |
| `imx8mp-debug` | board, for `gdbserver` |
| `rk3588-release` / `-relwithdebinfo` / `-debug` | the same for the ROCK 5B+ |

```bash
cmake --workflow --preset host-check      # configure + build + test on the PC
cmake --workflow --preset imx8mp-check    # cross-build + test under QEMU
cmake --workflow --preset rk3588-check    # the same for the ROCK 5B+
```

Or step by step:

```bash
cmake --preset host-debug
cmake --build --preset host-debug
ctest --preset host-debug
```

All presets build with warnings as errors. Build `imx8mp-release` regularly
even while working on the PC: the board has OpenCV 4.4 / GCC 10, the PC newer
versions, and the cross-build is what catches API differences.

## Run on the board

```bash
scripts/deploy.sh                       # build imx8mp-release, copy to /opt/scip on the board
scripts/board-test.sh                   # unit tests on the board
scripts/board-run.sh scip-cli -i pattern:1920x1080 -p 'gray ! blur ksize=5' -n 200
```

Board address and folder: `SCIP_BOARD` (default `root@192.168.50.2`) and
`SCIP_BOARD_DIR` (default `/opt/scip`).

The ROCK 5B+ is selected with `SCIP_TARGET=rk3588` (for `deploy.sh`, an
`rk3588-*` preset is enough). Defaults: `radxa@192.168.2.2` (direct cable), `/home/radxa/scip`:

```bash
scripts/rk3588-sysroot.sh               # once, and after upgrading packages on the board
scripts/deploy.sh rk3588-release
SCIP_TARGET=rk3588 scripts/board-test.sh
SCIP_TARGET=rk3588 scripts/board-run.sh scip-cli -i pattern:1920x1080 -p 'gray ! blur ksize=5' -n 200
```

### Benchmarking notes

- The CPU governor is `ondemand` by default. For stable numbers set
  `performance` on the board first:
  `echo performance | tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor`
- Watch the temperature (`/sys/class/thermal/thermal_zone0/temp`); throttling
  changes results.
- The board's root filesystem is NFS: sources load images into memory once so
  file I/O stays out of the timings.

### Debugging on the board

```bash
scripts/deploy.sh imx8mp-debug
scripts/board-run.sh gdbserver :2345 /opt/scip/bin/scip-cli -p gray
# on the PC:
gdb-multiarch build/imx8mp-debug/apps/scip-cli \
    -ex 'set sysroot /mnt/ambat/work/sc_display/sdk/sysroots/cortexa53-crypto-poky-linux' \
    -ex 'target remote 192.168.50.2:2345'
```

## Using scip-cli

```
scip-cli -i <input> [-p <stages>] [-o <output>]... [-n frames] [--realtime] [--gray-input]
scip-cli --help | --list-stages | --version
```

Pipelines are written like GStreamer launch lines: `gray ! blur ksize=5 ! resize width=640 height=480`;
`none` = pass-through. `scip-cli --list-stages` lists every function with its parameters.

| Input `-i` | |
|---|---|
| `pattern:WxH[:gray]` | generated test image, repeated (`-n`, default 100) |
| `photo.png` / `.jpg` ... | still image, repeated |
| `video.mp4` / `.mkv` / `.mov` ... | video file, until it ends (or `-n`) |
| `camera:/dev/video0[:WxH]` | V4L2 camera, until Ctrl+C; `--list-devices` shows its sizes |
| `rtp://:5000` | H.264 over RTP/UDP sent to this machine (live) |
| `tcp://:5000` | raw frames in GDP over TCP, lossless (live), e.g. from the GUI |
| `rtsp://...`, `http(s)://...` | network streams GStreamer can play |
| `gst:'... ! appsink name=scip_sink'` | any GStreamer pipeline |

| Output `-o` (repeatable) | |
|---|---|
| `display` | a window on the PC; the screen via KMS on the board |
| `display:40` | board: pick the connector (39 = LVDS panel, 40 = HDMI) |
| `udp:HOST:PORT` | H.264/RTP stream; the CLI prints the command to watch it |
| `out.mp4` / `out.mkv` | H.264 recording (VPU on the board, x264 on the PC) |
| `out.png` | the last frame |
| `gst:'appsrc name=scip_src ! ...'` | any GStreamer pipeline |

`--realtime` plays a file at its own frame rate (for watching); without it frames
are processed as fast as possible (for measuring). `--gray-input` decodes straight
to gray, which is the cheapest path when the first stage would be `gray` anyway.

### Your own video, step by step

On the PC:

```bash
build/host-release/apps/scip-cli -i ~/videos/clip.mp4 -p 'gray ! blur ksize=5' -o display --realtime
```

On the board, shown on its screen, recorded, or streamed to the PC:

```bash
scripts/board-copy.sh ~/videos/clip.mp4          # -> /home/root/videos/ on the board
scripts/board-run.sh scip-cli -i /home/root/videos/clip.mp4 -p 'gray ! blur ksize=5' -o display:40 --realtime
scripts/board-run.sh scip-cli -i /home/root/videos/clip.mp4 -p 'gray ! blur ksize=5' -o /tmp/out.mp4
scripts/board-run.sh scip-cli -i /home/root/videos/clip.mp4 -p 'gray ! blur ksize=5' -o udp:192.168.50.1:5000 --realtime
```

For the UDP stream, start the viewer on the PC first (scip-cli prints this command):

```bash
gst-launch-1.0 udpsrc port=5000 caps="application/x-rtp,media=video,clock-rate=90000,encoding-name=H264,payload=96" ! rtpjitterbuffer latency=100 ! rtph264depay ! avdec_h264 ! videoconvert ! autovideosink sync=false
```

The report separates where the time goes: `source (read)` (decode / waiting),
each stage, and `outputs (write)`. On the board the VPU decoder library prints
`ERROR: NO START CODE PREFIX` / `BYTE_STREAM` lines; they are harmless noise.

## Controlling a target: scipd, AIPC and the GUI

`scipd` runs the engine and serves AIPC on TCP port 5600 (discovery on UDP
5601). Everything can be changed while video runs: input, functions and their
parameters, outputs.

```bash
build/host-debug/apps/scipd                     # on the PC
scripts/deploy.sh && scripts/board-run.sh scipd --name board-1   # on the board, until Ctrl+C
```

To leave it running on the board in the background (after the terminal closes,
not across a reboot):

```bash
scripts/deploy.sh rk3588-release                      # 1. build and copy to the board
SCIP_TARGET=rk3588 scripts/board-scipd.sh start       # 2. start scipd there
cd host/aipc-gui && uv run aipc-gui                   # 3. the GUI lists it; or connect to 192.168.2.2:5600
```

`board-scipd.sh` also takes `stop`, `restart`, `status` and `log`; extra
arguments after `start` go to scipd (e.g. `--name lab-board`). Without
`SCIP_TARGET` it uses the i.MX board. `deploy.sh` stops a running scipd, so
start it again after each deploy.

The GUI finds targets on the LAN (see [host/aipc-gui](host/aipc-gui) for setup):

```bash
cd host/aipc-gui && uv run aipc-gui
```

A typical session: choose *Video from this PC* as input, open a file in the
player and press play. Add functions from the list, tune them with the
generated controls, and watch the result, which the target sends back to the
PC on UDP port 5002 (*Outputs* tab), side by side with what was sent.
Statistics show the time per function.

How the video gets to the target, all in the *Input* tab:

| | sent as | the target's input |
|---|---|---|
| *Video from this PC*, H.264 | compressed by the PC: bitrate or constant quality, x264 preset, key frames | `rtp://:5000` |
| *Video from this PC*, raw | every pixel as decoded (I420: ~124 Mbit/s for PAL, ~330 for 720p30) | `tcp://:5000` |
| *File or URL on the target* | nothing: the target plays its own file, the player controls it | the file |

In the last case the target sends its input back too (UDP 5004, compressed:
it is only for watching), so the left view shows what the functions get.

From the shell or scripts:

```bash
cd host/aipc-gui
uv run aipc --discover
uv run aipc 192.168.50.2 pipeline.set '{"text": "gray ! blur ksize=5"}'
uv run aipc 192.168.50.2 stats.get
```

or by hand: `nc 192.168.50.2 5600` and type `{"jsonrpc":"2.0","id":1,"method":"system.info"}`.

## Adding a function

Each function registers itself with its parameters, and everything else
follows: pipeline text, `--list-stages`, AIPC `functions.list` and the GUI's
controls. See [src/stages/README.md](src/stages/README.md):

1. Implement `scip::Stage` in `src/stages/<name>.cpp`:
   `configure()` validates the input and returns the output format;
   `process()` fills the pre-allocated output (no allocations per frame).
2. Describe it with a `StageInfo` (name, title, parameters) and
   `SCIP_REGISTER_STAGE(...)` in the same file; add the file to
   `src/stages/CMakeLists.txt`.
3. Add tests in `tests/test_stages.cpp` that compare against a reference
   (OpenCV or the plain C++ version) with an explicit tolerance.
4. Benchmark on the board with `scip-cli` or watch it live with the GUI.

The built-in functions (contrast, spatial and temporal noise reduction,
sharpening, ...) and their cost on the board are listed in
[src/stages/README.md](src/stages/README.md). GStreamer filter elements
installed on a machine (`videoflip`, `gamma`, ...)
are offered as functions too, their parameters read from the element's
properties; `gst pipeline="..."` runs any chain of elements.

## Conventions

- C++17, formatted with `.clang-format` (`clang-format -i <files>`).
- The plain C++ / OpenCV version of a stage is the reference; accelerated
  versions must match it within a stated tolerance in the tests.
- Errors in configuration throw `std::invalid_argument` with a message saying
  what was wrong; the CLI prints it.
