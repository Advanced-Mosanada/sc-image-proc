# Target board facts

Collected 2026-11-23 from the running board and the installed SDK.
Re-check after re-flashing or rebuilding the image.

## Hardware

| | |
|---|---|
| Board | MYiR MYD-JX8MP (MYC-JX8MPQ module), NXP i.MX 8M Plus |
| CPU | 4x Cortex-A53, max 1.6 GHz, governor `ondemand` by default |
| RAM | 3 GB LPDDR4 |
| GPU / NPU | `/dev/galcore` (Vivante GC7000UL 3D + NPU), `/dev/dri/renderD128` |
| Camera | Logitech HD Pro Webcam C920 on `/dev/video0` (USB UVC) |

## Software on the board

| | |
|---|---|
| Image | NXP i.MX Release Distro `fsl-imx-xwayland` 5.10-gatesgarth (MYiR BSP, NXP L5.10.9_1.0.0) |
| Kernel | 5.10.9-1.0.0 (custom build from `sc_display/board_manip`) |
| Root filesystem | NFS from the PC: `192.168.50.1:/srv/nfs/myd-jx8mp` |
| Network | board `192.168.50.2`, PC `192.168.50.1` (enp4s0), SSH as root |
| Libraries | OpenCV 4.4.0, GStreamer 1.18, OpenCL, G2D, OpenVX, NNAPI (`libneuralnetworks`) |
| GStreamer (NXP) | `vpuenc_h264`, `vpuenc_hevc`, `vpudec`, `imxvideoconvert_g2d`, `imxcompositor_g2d`, `waylandsink` |
| Tools | `gdbserver`, `perf`, `v4l2-ctl`, `gst-launch-1.0`; no `rsync`, no `ldd` |
| Clock | not synchronised (reads 2020); timestamps on copied files look "in the future" |

## SDK (cross toolchain)

| | |
|---|---|
| Location | `/mnt/ambat/work/sc_display/sdk` |
| Installer | `sc_display/Data/Tools/Toolchains/fsl-imx-xwayland-glibc-x86_64-myir-image-full-cortexa53-crypto-myd-jx8mp-toolchain-5.10-gatesgarth.sh` |
| Compiler | `aarch64-poky-linux-g++` GCC 10.2.0, flags `-mcpu=cortex-a53 -march=armv8-a+crc+crypto` |
| Sysroot | `sysroots/cortexa53-crypto-poky-linux` (16 GB, matches `myir-image-full`) |
| SDK's own CMake | 3.18 (too old for presets; we use the host CMake with our toolchain file) |

## First baseline (scip 0.1.0, imx8mp-release, ondemand governor)

| Pipeline, 1920x1080 BGR input | mean | throughput |
|---|---|---|
| `gray ! blur ksize=5` | 7.6 ms (gray 2.9, blur 4.7) | 131 fps |
| `resize width=640 height=360 ! gray ! blur ksize=5` | 4.7 ms (resize 3.7) | 214 fps |

## Video path on the board (measured, 640x512 H.264, 2183 frames)

| Step | per frame |
|---|---|
| VPU decode (`vpudec` via `decodebin`) | 2.1 ms |
| + GStreamer `videoconvert` -> BGR (CPU) | +20 ms  <- why we avoid it |
| + GStreamer `videoconvert` -> GRAY8 (CPU) | +13 ms |
| + `imxvideoconvert_g2d` -> BGRx (2D GPU) | +2 ms, then BGRx->BGR with OpenCV/NEON |
| gray from the NV12 Y plane (`--gray-input`) | 0 ms (zero-copy) |

Result for `gray ! blur ksize=5` on that video: 45 fps -> 274 fps (BGR input),
65 fps -> 441 fps (`--gray-input`).

## Board quirks found (BSP L5.10.9)

- **kmssink** only works with `force-modesetting=true` and frames of exactly the
  screen mode (planes cannot scale: "Checking scale ratio fail"). The display sink
  asks kmssink for the mode and letterboxes on the CPU (13-17 ms per frame for
  1024x768 / 1920x1080). Connectors: 39 = LVDS 1024x768, 40 = HDMI 1920x1080
  (`modetest -M imx-drm -c`). `imxcompositor_g2d` does not negotiate with kmssink.
- **imxvideoconvert_g2d** can hang inside the GPU driver (`gckOS_WaitSignal`) when
  its buffer pool is exhausted and the pipeline is stopped. The source drains its
  appsink before stopping, and reports a stall after 10 s without frames.
- **vpuenc_h264** accepts 64x64 .. 1920x1088, takes BGRx directly, and rejects
  fractional frame rates (60001/1000) - outputs round to whole fps. Larger frames
  (the C920 at 2304x1536) are scaled down to fit on their way into `udp:` and
  `.mp4` outputs.
- **C920 without a size** opens at 2304x1536, which delivers 2 fps. Its fastest
  large uncompressed mode is 800x448 at 30 fps; `scip-cli --list-devices` and
  AIPC `input.devices` list all modes.
- A running **scipd** keeps its binary busy on the NFS root; `deploy.sh` stops it
  (and waits for it to exit) before replacing `/opt/scip`.

## scipd on the board (measured 2026-11-23, imx8mp-release)

H.264 1280x720 at 30 fps streamed from the PC GUI to `rtp://:5000`, pipeline
`gray ! edges`:

| Outputs | fps | outputs time per frame |
|---|---|---|
| none | 31 | 0.2 ms |
| `udp:192.168.50.1:5002` (VPU encoder) | 30 | 1.7 ms |
| `display:40` (HDMI 1920x1080) | 25 | 21 ms |
| both | 24 | 22 ms |

The display output is the bottleneck: the frame is converted and letterboxed
to the screen size on the CPU (see the kmssink quirk above). Moving that to
G2D, or writing outputs on their own thread, is the next performance step.

Camera C920 at 800x448, `gray ! blur ksize=5 ! edges`, to HDMI and UDP: 30 fps.
