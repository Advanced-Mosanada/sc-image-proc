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
| `gray,blur:5` | 7.6 ms (gray 2.9, blur 4.7) | 131 fps |
| `resize:640x360,gray,blur:5` | 4.7 ms (resize 3.7) | 214 fps |
