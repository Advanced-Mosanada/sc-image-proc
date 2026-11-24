# Thermal video enhancement: the chosen pipeline and how it was chosen

Goal, for the thermal (IR) video `combined_focused_25fps.mp4`: lower the noise,
show details and edges more, raise the contrast and the detail of homogeneous
areas, in a time an embedded board can afford.

## The pipeline

```
tnr window=3 noise=4 align=true skip_repeats=true ! guided radius=4 eps=0.00025 ! dde radius=4 eps=0.01 contrast=1 clip=3 tiles=8 gain=3.5 fine_gain=1.6 noise=3
```

| step | what it does here |
|---|---|
| `tnr window=3 noise=4 align=true skip_repeats=true` | temporal noise reduction: follows the panning camera (global shift), averages where the aligned frames agree; a repeated frame gives the previous output |
| `guided radius=4 eps=0.00025` | light edge-preserving smoothing: removes the pixel noise that is left, keeps every edge above ~4 levels |
| `dde ...` | detail enhancement: local contrast (CLAHE) on a smooth base layer, texture and fine detail boosted above the noise level, strong edges left as they are (no halos), base kept in float (no contour bands) |

Run it:

```bash
# PC: watch it, or write the result
build/host-release/apps/scip-cli -i ~/Videos/ir-proc-video/combined_focused_25fps.mp4 --gray-input \
  -p 'tnr window=3 noise=4 align=true skip_repeats=true ! guided radius=4 eps=0.00025 ! dde radius=4 eps=0.01 contrast=1 clip=3 tiles=8 gain=3.5 fine_gain=1.6 noise=3' \
  -o display --realtime
# board (RK3588 shown; for the i.MX 8M Plus leave SCIP_TARGET out and deploy imx8mp-release)
scripts/deploy.sh rk3588-release
SCIP_TARGET=rk3588 scripts/board-copy.sh ~/Videos/ir-proc-video/combined_focused_25fps.mp4
SCIP_TARGET=rk3588 scripts/board-run.sh scip-cli -i /home/radxa/videos/combined_focused_25fps.mp4 --gray-input \
  -p '<the pipeline>' -n 600
```

In the GUI, add the three functions with these values. `skip_repeats` is for
video with repeated frames (as this one, see below); with a camera it does no harm.

Variants, all passing the same checks (table below):

- **without `align`** (`tnr window=3 noise=4 skip_repeats=true ! ...`):
  ~10 % cheaper, the same picture on average, a little more noise when the
  camera pans fast or the scene is noisier (sigma 5: 3.48 instead of 3.14).
- **less noise, less texture**: `guided ... eps=0.0005` and `dde ... gain=3`
  (noise 1.38 instead of 1.63, homogeneous-area texture 0.97 instead of 1.13).
- **more texture**: `fine_gain=2.0`, at the clipping / banding limits.

Turning one knob: `dde noise` (higher = smoother flat areas), `dde gain` /
`fine_gain` (texture / finest detail), `dde clip` (local contrast; above 3 the
picture pumps), `guided eps` (higher = less noise and less low-contrast
texture, e.g. foliage).

## What the video is

Measured on the file itself (`tools/enhance-eval/make_dataset.py`):

- 640x512, 8-bit gray, H.264 at 8 Mbit/s, 10599 frames at 25 fps.
- **40 % of the frames are exact repeats**: the camera delivered ~15 fps
  and the file was converted to 25 fps (pattern 10110 10110 ...).
- Noise: temporal sigma 1.1-3.2 levels in flat areas (median 1.9), plus a
  **fixed pattern** (sigma 1.1: a fine grid and crack-like structures, found by
  averaging the high-pass of 1230 frames while the camera moves).
- The camera pans almost all the time (median 9.6 levels mean change between
  frames); a global shift explains most of it (residual 3.3).
- The camera already sharpens (thin bright rims along edges): more sharpening
  quickly gives halos.

## How it was evaluated

Details and every number: [tools/enhance-eval](../tools/enhance-eval/README.md).
In short:

1. **Reference and known noise.** 12 clips of 48 frames from all over the
   video. A pseudo-clean reference x (multi-frame non-local means, h = 3), and
   x + the measured noise (white sigma 2.5 per frame + the measured fixed
   pattern); a second set with sigma 5.
2. **Signal and noise kept apart.** For a pipeline P: P(x) is what P does to
   the scene, P(x + n) - P(x) the noise that reaches the output. A contrast
   gain multiplies both, so "more contrast" cannot hide "more noise". The
   real frames are measured the same way (P(real) - P(x)).
3. **Metrics** on the scene (edge gradient, texture of homogeneous areas,
   local contrast, detail-to-noise ratio) and on artefacts (halos next to
   strong edges, false contour bands, clipping, SSIM structure, flicker).
4. **A rule fixed before searching**: the most enhancement
   E = (edge x homD x lc)^(1/3) among pipelines with noise at least 20 % below
   the input, real-video noise not above the input, halo <= 0.25 levels,
   structure >= 0.95, clipping <= 1 %, flicker <= 0.6 levels.
5. **Search**: every existing function alone, then about 550 combinations
   (random search and one-at-a-time sweeps), all run through `scip-cli` (the
   C++ code that runs on the board).
6. **Frames looked at.** They showed contour bands in a dark sky that no
   metric had caught, and the texture metric had even counted the band steps
   as detail. A banding metric was added (no more than the input's 7.3 %),
   `dde` was fixed, and the finalists were evaluated again.
7. **Scene by scene.** The finalists per clip, with bootstrap 95 % intervals
   and paired differences, so a result is not an average pulled by one scene.
8. Everything is deterministic: the data set is reproduced byte for byte by
   the script, and re-running all 384 pipelines gave identical numbers.

PSNR/SSIM against x are used only among denoisers (contrast functions change
levels on purpose); no-reference scores like BRISQUE/NIQE are trained on
visible-light photos and are not meaningful for thermal images.

## Results

Synthetic noise sigma 2.5 unless noted; input noise 2.75 (sigma 2.5 + fixed
pattern), real video 2.84. Gains are relative to the reference (1 = unchanged).

| Pipeline | noise σ2.5 | noise σ5 | noise real | edge | homD | lc | DNR | halo | band% | struc | clip% | flicker | E | passes |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Input (no processing) | 2.75 | 5.12 | 2.84 | 1.00 | 1.00 | 1.00 | 0.61 | 0.02 | 7.3 | 0.960 | 0.00 | 0.11 | 1.00 | no |
| `clahe` (default) | 7.61 | 13.81 | 7.18 | 1.45 | 2.34 | 1.53 | 0.54 | 0.26 | 34.0 | 0.919 | -0.25 | 0.46 | 1.73 | no |
| `temporal_median ! clahe` (old README suggestion) | 6.16 | 10.40 | 6.48 | 1.11 | 2.87 | 1.48 | 0.75 | 4.03 | 39.7 | 0.808 | -0.31 | 2.14 | 1.68 | no |
| `guided ! clahe` (CLAHE on a denoised image) | 2.60 | 5.43 | 3.06 | 1.45 | 1.66 | 1.66 | 1.15 | 0.31 | 20.4 | 0.962 | -0.32 | 0.51 | 1.59 | no |
| Best chain of the existing functions (1) | 2.08 | 3.74 | 2.56 | 1.44 | 1.67 | 1.57 | 1.34 | 0.19 | 20.5 | 0.963 | 0.86 | 0.45 | 1.56 | no |
| **Recommended** | 1.63 | 3.14 | 2.11 | 1.61 | 1.13 | 1.82 | 1.72 | 0.14 | 7.0 | 0.957 | 0.87 | 0.57 | 1.49 | yes |
| Recommended without `align` | 1.61 | 3.48 | 2.24 | 1.62 | 1.14 | 1.81 | 1.70 | 0.14 | 7.0 | 0.958 | 0.87 | 0.56 | 1.49 | yes |
| Low-noise variant | 1.38 | 2.53 | 1.78 | 1.50 | 0.97 | 1.77 | 1.75 | 0.12 | 5.8 | 0.958 | 0.65 | 0.57 | 1.37 | yes |

(1) `tnr window=3 noise=4 align=true skip_repeats=true ! guided radius=3 eps=0.002 ! clahe clip=1.5 tiles=4 ! detail gain=1.8 radius=8 eps=0.01`
(with the fixed `tnr`). Its `homD` and `E` are inflated by its contour bands.

The recommended pipeline against the input: noise -41 % (synthetic), -26 %
(real video), -39 % at sigma 5; edges x1.61; local contrast x1.82;
homogeneous-area texture x1.13 while the noise there is roughly halved, so its
visibility over the noise (DNR) goes from 0.61 to 1.72 (x2.8); no new bands,
halos 0.14 levels.

Scene by scene (12 clips, bootstrap 95 % intervals), recommended against the
best chain of the existing functions: noise -0.46 [-0.61, -0.29] (better in
11/12 scenes), real-video noise -0.45 [-0.62, -0.29] (11/12), sigma 5 noise
-0.60 [-0.86, -0.35] (11/12), halo -0.05 (9/12), banding -13.6 points
[-15.6, -11.9] (12/12); enhancement E -0.05 [-0.15, +0.04], i.e. not
significantly different.

Looking at the frames: flat areas (walls, sky, car body) are clean, edges and
structures (grille, rails, poles, clouds) clearly stronger, no halos or bands.
The price of the noise reduction: low-contrast texture close to the noise
level, like the inside of a tree crown, comes out smoother (lower `guided eps`
and `dde noise` keep more of it, with more noise).

## What was wrong in the existing functions, and what changed

| function | problem found | change |
|---|---|---|
| `tnr` | **Bug**: the recursive history was 8-bit, so every blend rounded small changes away; with strength 0.8 the output stayed up to 2 levels off for ever (blotches, smeared low-contrast detail; measured: "detail" x1.33 on a clean input that should stay x1.0). | History in 8.8 fixed point. |
| `tnr` | Motion was decided per pixel, where noise and motion overlap: weak noise reduction or trails. | `window`: the change is averaged over 3x3 first (noise averages out, motion does not). |
| `tnr` | A panning camera is "motion" everywhere: nothing was averaged. | `align`: the global shift is measured (phase correlation at 1/4 size, refined by a +-2 px search at full size) and the history moved with it. Noise 2.03 -> 1.83 at the same settings. |
| `tnr`, `temporal_median`, `temporal_mean` | A repeated frame counted as a new sample (a 3-frame median of [A, A, B] is A: no denoising). | `skip_repeats`: a repeat gives the previous output. |
| `temporal_median`, `temporal_mean` | No motion compensation: on this footage they smear (halo 0.8-3.1 levels, PSNR 24-29 dB against 39 for the input). | Not changed; not for a moving camera (README says so now). |
| `clahe` | On a raw thermal frame it nearly triples the noise; on a denoised one it cuts slow gradients into bands. | Not changed; `dde` does local contrast without either. |
| `retinex` | Lowers global contrast here (RMS 55 -> 37). Its per-frame normalisation was suspected of flicker; a temporally smoothed version measured no difference. | Not changed (reverted). |
| `unsharp` | Its threshold is a hard switch (a jump in the sharpening at the threshold). | Not changed (documented behaviour); `dde` uses smooth coring. |

New: `dde` (`src/stages/dde.cpp`), thermal "digital detail enhancement"
(FLIR DDE; Branchitta et al. 2008; Liu & Zhao 2017 "GF&DDE"). Base layer from
the guided filter; local contrast by CLAHE on the base only, in float and
interpolated between levels (no bands); mid (texture) and fine bands with
smooth coring (d^3 / (d^2 + t^2)) and gains that fade out at strong edges
(using the guided filter's coefficient), which keeps halos away. Its output
loop is vectorised (NEON / SSE).

Tests: `tests/test_enhance.cpp` checks each of these properties (history
reaches small changes, alignment follows a pan, repeats, dde identity, noise
smoothing, no ring at a strong edge, no bands where cv::CLAHE makes them).

## Time

This PC (Ryzen 9 3900X), one core, one OpenCV thread, 640x512 gray, release:

| pipeline | tnr | guided | dde | total (p95) |
|---|---|---|---|---|
| recommended | 1.3 (1.55 on new frames) | 1.45 | 3.7 | 6.4 ms (7.3) |
| without `align` | 0.7 | 1.45 | 3.7 | 5.8 ms (6.6) |
| low-noise variant | 1.3 | 1.45 | 3.7 | 6.4 ms (7.3) |

The boards were not reachable, so there is **no board measurement yet**.
Estimate for one Cortex-A53 core of the i.MX 8M Plus: the A53/PC ratio of the
functions measured on both (`src/stages/README.md`) is 3-4 for OpenCV's
vectorised ones (clahe, blur) and ~9 for the float-pass ones (guided, detail),
~7 for `tnr`. That puts the recommended pipeline at roughly **45-55 ms per
frame on one A53 core** (about 20 fps): enough for the ~15 new frames per
second of this video, not for 25 different frames per second on one core.
The RK3588's Cortex-A76 cores are several times faster per core than the A53
and should be well inside 40 ms. Measure with:

```bash
SCIP_TARGET=rk3588 scripts/board-run.sh env OPENCV_FOR_THREADS_NUM=1 scip-cli -i /home/radxa/videos/combined_focused_25fps.mp4 --gray-input -n 600 -p '<the pipeline>'
scripts/board-run.sh env OPENCV_FOR_THREADS_NUM=1 scip-cli -i /home/root/videos/combined_focused_25fps.mp4 --gray-input -n 600 -p '<the pipeline>'
```

(`OPENCV_FOR_THREADS_NUM=1`: one thread, as the estimates; leave it out to let
OpenCV use all cores.)

If the i.MX must do 25 new frames per second: use the variant without `align`,
let OpenCV use the other cores, or (next steps) merge the two
guided filters (`guided` and the one inside `dde` work on nearly the same
image, ~12 ms on the A53), or move the float passes to the GPU.

## Next steps

- Measure on both boards (commands above) and add the numbers to
  `src/stages/README.md`.
- Fixed-pattern noise (sigma 1.1) is now one of the larger noise sources left;
  a scene-based non-uniformity correction would remove it.
- Skip repeated frames for the whole pipeline (not only the temporal
  functions): on this video 40 % less work.

## Reproduce

```bash
cmake --workflow --preset host-check
cd tools/enhance-eval
python3 make_dataset.py ~/Videos/ir-proc-video/combined_focused_25fps.mp4      # ~15 min
EVAL_INPUTS=y,y5,real python3 search.py candidates.txt                         # the table above
EVAL_INPUTS=y,y5,real python3 compare.py final.txt                             # scene by scene
SCIP_EVAL_VIDEO=~/Videos/ir-proc-video/combined_focused_25fps.mp4 python3 timing.py '<pipeline>'
```
