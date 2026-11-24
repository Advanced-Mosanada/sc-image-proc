# Target board facts: Radxa ROCK 5B+ (`rk3588-*` presets)

Collected 2026-11-23 from the running board.
Re-check (and re-create the sysroot) after re-flashing or upgrading packages.
The i.MX 8M Plus board is described in [board.md](board.md).

## Hardware

| | |
|---|---|
| Board | Radxa ROCK 5B+, Rockchip RK3588 |
| CPU | 4x Cortex-A76 (2 at 2.35 GHz, 2 at 2.26 GHz) + 4x Cortex-A55 (1.8 GHz), governor `ondemand` |
| RAM | 8 GB (+4 GB zram swap) |
| GPU / NPU | Mali-G610 (`/dev/dri/card2`, `renderD130`), RKNPU (`/dev/dri/card1`, `renderD129`, `librknnrt`) |
| Video | `/dev/video-dec0`, `/dev/video-enc0` (MPP), RGA 2D (`librga`) |
| Camera | none connected; `/dev/video0` is the HDMI input (`rk_hdmirx`) |
| Display | HDMI-A-1 connected 1920x1080@60, connector **208** (`scip-cli --list-devices`) |
| Storage | eMMC/SD `mmcblk1`, 59 GB root (not NFS: files on the board are local) |

## Software on the board

| | |
|---|---|
| Image | Radxa Debian 12 (bookworm), KDE on X11 (gdm3 + kwin) |
| Kernel | 6.1.43-15-rk2312 |
| Network | direct cable to the PC, 1 Gbit/s: board `192.168.2.2` (enP4p65s0), PC `192.168.2.1` (enp5s0); SSH as `radxa` with key login; `sudo` asks for a password. (Earlier on the LAN as `192.168.100.21`: `SCIP_BOARD=radxa@192.168.100.21`.) |
| Libraries | OpenCV 4.6.0, GStreamer 1.22.0, GCC 12.2 / glibc 2.36 |
| GStreamer (Rockchip) | `mppvideodec`, `mpph264enc`, `mpph265enc`, `mppjpegenc/dec` (no RGA element) |
| Other GStreamer | `x264enc`, `openh264enc/dec`, `avdec_*`, `kmssink`, `waylandsink`, `v4l2src` |
| Tools | `v4l2-ctl`, `gst-launch-1.0`, `gdb-minimal`; no `cmake`, `gdbserver`, `perf`, `rsync`, `ldconfig` |
| Clock | synchronised (systemd-timesyncd) |

## Cross toolchain

| | |
|---|---|
| Compiler | Ubuntu `g++-12-aarch64-linux-gnu` (GCC 12.4), `-mcpu=cortex-a76.cortex-a55+crypto` |
| Sysroot | `~/sysroots/rock5bplus-debian12` (override `SCIP_SYSROOT_DIR`), ~2.1 GB, made by `scripts/rk3588-sysroot.sh` |
| Toolchain file | `cmake/toolchains/rk3588-debian12.cmake` |

The sysroot is copied from the board; the `-dev` packages it lacks (GStreamer, glib,
libdrm, ...) are fetched on the board with `apt-get download` in the versions
matching the installed libraries and unpacked on the PC only. Nothing is
installed on the board.

Ubuntu's cross GCC would otherwise use its own glibc 2.39 / libstdc++ 12.4 from
`/usr/aarch64-linux-gnu` before the sysroot (binaries then need `GLIBC_2.38`
and do not start on the board). The toolchain file puts the sysroot's headers,
start files and libraries first; a release build needs at most `GLIBC_2.34`
and `GLIBCXX_3.4.29`. Check after toolchain changes:

```bash
aarch64-linux-gnu-objdump -T build/rk3588-release/apps/scipd | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1
```

## Deployment

`SCIP_TARGET=rk3588` (or an `rk3588-*` preset for `deploy.sh`) selects the board:
`radxa@192.168.2.2`, installed to `/home/radxa/scip` (no root needed), videos in
`/home/radxa/videos`. 111/111 tests pass under QEMU, 107/107 unit tests on the board.

To control it from the PC over the network:

```bash
scripts/deploy.sh rk3588-release
SCIP_TARGET=rk3588 scripts/board-scipd.sh start --name rock5bplus
cd host/aipc-gui && uv run aipc --discover     # rock5bplus (192.168.2.2:5600) - Radxa ROCK 5B+
```

Ports: TCP 5600 (AIPC), UDP 5601 (discovery); video to the board on 5000,
results back to the PC on UDP 5002 / 5004. 5600 and 5601 are confirmed reachable
from the PC; the image has no `ufw`.

## First baseline (scip 0.1.0, rk3588-release, ondemand governor, one thread)

| Pipeline, 1920x1080 BGR input | mean | throughput |
|---|---|---|
| `gray ! blur ksize=5` | 2.0 ms (gray 0.65, blur 1.3) | 505 fps |
| `resize width=640 height=360 ! gray ! blur ksize=5` | 5.2 ms (resize 4.9) | 191 fps |

(i.MX 8M Plus: 7.6 ms / 131 fps and 4.7 ms / 214 fps.) The threads run on
whichever core the scheduler picks; pin with `taskset -c 4-7` to measure the
A76 cores only.

## Board-specific notes

- **Decoding** already uses the VPU: `decodebin` picks `mppvideodec`.
- **Encoding** (`.mp4`, `udp:` outputs) uses `x264enc` on the CPU: the H.264
  sink only knows the i.MX `vpuenc_h264`. 1280x720 to `.mp4`: 13 ms per frame.
  Using `mpph264enc` is the obvious next step.
- **RTP input** uses `avdec_h264` (CPU): the explicit decoder list knows `vpudec`
  only.
- **AIPC `system.info`** reports `vpu_decoder`/`vpu_encoder`/`g2d` false (it
  looks for the i.MX elements) and the CPU as "Cortex-A55" (the first core).
- **Display** `display` / `display:208` runs at 60 fps (vsync) next to the X11
  desktop; not yet checked by eye on the screen.
