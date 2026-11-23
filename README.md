# sc-image-proc (scip)

An image processing pipeline that is developed and tested on a Linux PC and
deployed to an NXP i.MX 8M Plus board (MYiR MYD-JX8MP) for performance work.

The same source builds for both targets; only the CMake preset changes.

## Layout

```
include/scip/        public headers
  frame.hpp            Frame / FrameInfo / PixelFormat: the image buffer type
  stage.hpp            Stage: interface every processing step implements
  pipeline.hpp         Pipeline: runs stages in order, records per-stage latency
  source.hpp           FrameSource: interface for inputs (files, patterns, video, camera)
  sink.hpp             FrameSink: interface for outputs (display, video file, network)
  stages.hpp           built-in stages + text factory ("gray,blur:5")
  io.hpp               openSource() / openSink() from URIs, image files, test patterns
  cv.hpp               zero-copy Frame <-> cv::Mat views
src/
  core/                scip_core: depends only on the C++ standard library
  stages/              scip_stages: processing stages (CPU reference, OpenCV)
  io/                  scip_io: sources and sinks
    gst/               GStreamer: video files, camera, display, H.264 record / UDP
  common/              internal helpers (not installed)
apps/scip-cli/         command-line runner / benchmark tool
tests/                 GoogleTest unit tests + CLI smoke tests
cmake/                 warnings, sanitizers, build info, board toolchain file
scripts/               deploy / run / test on the board
docs/                  notes (board facts, design decisions)
```

Library dependencies only point downwards: `core` <- `stages`, `io` <- `apps`.
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

## Build and test

| Preset | What |
|---|---|
| `host-debug` | PC, debug, everyday development |
| `host-asan` | PC, debug + AddressSanitizer / UBSan |
| `host-release` | PC, optimised |
| `imx8mp-release` | board, `-O3`, for benchmarks |
| `imx8mp-relwithdebinfo` | board, optimised + symbols, for `perf` |
| `imx8mp-debug` | board, for `gdbserver` |

```bash
cmake --workflow --preset host-check      # configure + build + test on the PC
cmake --workflow --preset imx8mp-check    # cross-build + test under QEMU
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
scripts/board-run.sh scip-cli -i pattern:1920x1080 -p gray,blur:5 -n 200
```

Board address and folder: `SCIP_BOARD` (default `root@192.168.50.2`) and
`SCIP_BOARD_DIR` (default `/opt/scip`).

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

Stages: `gray`, `blur:K[:SIGMA]`, `resize:WxH`, chained with commas; `none` = pass-through.

| Input `-i` | |
|---|---|
| `pattern:WxH[:gray]` | generated test image, repeated (`-n`, default 100) |
| `photo.png` / `.jpg` ... | still image, repeated |
| `video.mp4` / `.mkv` / `.mov` ... | video file, until it ends (or `-n`) |
| `camera:/dev/video0[:WxH]` | V4L2 camera, until Ctrl+C |
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
build/host-release/apps/scip-cli -i ~/videos/clip.mp4 -p gray,blur:5 -o display --realtime
```

On the board, shown on its screen, recorded, or streamed to the PC:

```bash
scripts/board-copy.sh ~/videos/clip.mp4          # -> /home/root/videos/ on the board
scripts/board-run.sh scip-cli -i /home/root/videos/clip.mp4 -p gray,blur:5 -o display:40 --realtime
scripts/board-run.sh scip-cli -i /home/root/videos/clip.mp4 -p gray,blur:5 -o /tmp/out.mp4
scripts/board-run.sh scip-cli -i /home/root/videos/clip.mp4 -p gray,blur:5 -o udp:192.168.50.1:5000 --realtime
```

For the UDP stream, start the viewer on the PC first (scip-cli prints this command):

```bash
gst-launch-1.0 udpsrc port=5000 caps="application/x-rtp,media=video,clock-rate=90000,encoding-name=H264,payload=96" ! rtpjitterbuffer latency=100 ! rtph264depay ! avdec_h264 ! videoconvert ! autovideosink sync=false
```

The report separates where the time goes: `source (read)` (decode / waiting),
each stage, and `outputs (write)`. On the board the VPU decoder library prints
`ERROR: NO START CODE PREFIX` / `BYTE_STREAM` lines; they are harmless noise.

## Adding a stage

1. Implement `scip::Stage` in `src/stages/<name>.cpp`:
   `configure()` validates the input and returns the output format;
   `process()` fills the pre-allocated output (no allocations per frame).
2. Add a `make<Name>()` declaration to `include/scip/stages.hpp` and a spec case
   to `src/stages/factory.cpp`.
3. Add tests in `tests/test_stages.cpp` that compare against a reference
   (OpenCV or the plain C++ version) with an explicit tolerance.
4. Benchmark on the board with `scip-cli`.

## Conventions

- C++17, formatted with `.clang-format` (`clang-format -i <files>`).
- The plain C++ / OpenCV version of a stage is the reference; accelerated
  versions must match it within a stated tolerance in the tests.
- Errors in configuration throw `std::invalid_argument` with a message saying
  what was wrong; the CLI prints it.
