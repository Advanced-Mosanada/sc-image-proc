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
  source.hpp           FrameSource: interface for inputs (files, patterns, camera)
  stages.hpp           built-in stages + text factory ("gray,blur:5")
  io.hpp               image files, test patterns, source URIs
  cv.hpp               zero-copy Frame <-> cv::Mat views
src/
  core/                scip_core: depends only on the C++ standard library
  stages/              scip_stages: processing stages (CPU reference, OpenCV)
  io/                  scip_io: sources and image writing
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
scip-cli -i <image file | pattern:WxH[:gray]> -p <stages> [-o out.png] [-n frames] [-w warmup]
scip-cli --list-stages
```

Stages: `gray`, `blur:K[:SIGMA]`, `resize:WxH`, chained with commas.

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
