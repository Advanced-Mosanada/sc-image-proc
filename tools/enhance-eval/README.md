# enhance-eval: measuring image-enhancement pipelines

Measures what a scip pipeline does to thermal video (noise, edges, detail,
contrast, artefacts) by running it through `scip-cli` itself, so the numbers are
those of the C++ functions that run on the board. Used to choose the thermal
pipeline in [docs/thermal-enhancement.md](../../docs/thermal-enhancement.md).

Needs Python 3 with `numpy` and `opencv-python` (or the system's `python3-opencv`),
and a `host-release` build:

```bash
cmake --workflow --preset host-check          # or: cmake --preset host-release && cmake --build --preset host-release
cd tools/enhance-eval
python3 make_dataset.py ~/Videos/ir-proc-video/combined_focused_25fps.mp4   # ~15 min, ~1 GB in build/enhance-eval
python3 search.py candidates.txt              # evaluate, then rank (~30 s per pipeline and core)
python3 search.py --rank dde                  # rank what was evaluated (those containing "dde")
EVAL_INPUTS=y,y5,real python3 compare.py final.txt   # per-scene comparison with confidence intervals
SCIP_EVAL_VIDEO=~/Videos/ir-proc-video/combined_focused_25fps.mp4 python3 timing.py '<pipeline>'
python3 timing.py calibrate                   # PC vs. i.MX 8M Plus (A53) ratio
```

`SCIP_EVAL_DIR` moves the data (default `build/enhance-eval`), `SCIP_CLI` picks
another `scip-cli`, `PROCS` the number of pipelines evaluated in parallel (8).
Outputs are deleted as soon as they are measured (each is ~190 MB); the run
refuses to start one with less than 10 GB free.

## Why it is built this way

There is no clean version of a real thermal video, so quality cannot be measured
against the truth directly. The evaluation combines three things:

1. **A reference and a known noise.** `make_dataset.py` takes 12 clips of 48
   frames spread over the video (exact repeats from the 15 -> 25 fps conversion
   removed). The *pseudo-clean reference* `x` is multi-frame non-local means
   (5 frames, patches matched across time, so motion does not blur it). Noise
   measured on the video itself is added to it: white Gaussian noise
   sigma 2.5 (new every frame; the video's flat areas measure 1.1-3.2) plus the
   video's own fixed-pattern noise (sigma 1.1, averaged out of 1230 frames while
   the camera moves). That gives `y = x + n` with `n` known; `y5` uses sigma 5.
2. **Signal and noise separated.** For a pipeline P, `s = P(x)` is what P does
   to the scene and `P(y) - P(x)` is the noise that reaches the output. A pure
   contrast gain k multiplies both by k, so neither "more contrast" nor "less
   noise" can be faked by the other.
3. **The real video too.** `P(real) - P(x)` is the real noise (sensor, fixed
   pattern, H.264) at the output. The real minus the reference also contains
   some fine texture the reference lost, so these values are a little high
   for every pipeline alike.

Metrics, on frames 12..47 of every clip (every 3rd: 144 frames, temporal
functions have warmed up). Masks come from the reference: *edges* = the 10 %
strongest gradients, *homogeneous* = the 50 % weakest, *strong edges* = top 3 %.

| metric | what | good |
|---|---|---|
| `noise` | std of P(y) - P(x) in homogeneous areas (levels); input: 2.75 | low |
| `nReal` | the same for the real video; input: 2.84 | low |
| `edge` | mean gradient of P(y) on edges / that of x | > 1 |
| `homD` | texture amplitude (band 2 px, 7x7 RMS) of P(x) in homogeneous areas / that of x | > 1 |
| `lc` | local contrast (31x31 std of the 2 px-smoothed image) of P(x) / that of x | > 1 |
| `dnr` | homogeneous-area texture of P(x) / the noise of the same band: visibility of real detail over noise; input 0.61 | high |
| `halo` | how far P(x) goes beyond its own local range next to strong edges, after the best local linear map from x (levels). Global and smooth local tone changes (CLAHE) are free, rings are not | < 0.25 |
| `struc` | SSIM structure term of P(y) against x: blind to brightness and contrast changes, sensitive to noise and false structure | high |
| `band%` | false contours: in smooth areas of x (< 0.4 levels/px) the share of pixels where P(x) steps by >= 3 levels to a neighbour; input 7.3 (x's own fine structure) | <= input |
| `clip%` | extra pixels at 0 or 255 | small |
| `flick` | brightness change between frames that the scene did not make (real video, levels) | small |
| `psnr`, `ssim` | against x: only meaningful for denoisers, since contrast functions change levels on purpose | |

PSNR / SSIM against `x` favour non-local means (the reference was made with it),
so they are only used to compare denoisers among themselves. No-reference
"naturalness" scores (BRISQUE, NIQE) are trained on visible-light photos and are
not used for thermal images.

**Selection rule** (fixed before the search): maximise
`E = (edge * homD * lc)^(1/3)` among pipelines with `noise <= 2.20` (at least
20 % below the input), `nReal <= 2.84`, `halo <= 0.25`, `struc >= 0.95`,
`clip <= 1 %`, `flick <= 0.6`. Then look at the frames: numbers do not see
everything. They did not: the frames showed contour bands in a dark sky, and
the band-pass `homD` had counted those steps as "detail". `band% <= 7.3` (no
more than the input) was added to the rule after that, and the finalists and
all `dde` variants were evaluated again. `compare.py` then checks that a difference holds scene by
scene (bootstrap over the 12 clips, paired), not only on average.

## Files

- `make_dataset.py`: measures the video's noise, writes the clips (deterministic:
  same video, same bytes).
- `evalpipe.py`: runs a pipeline on the clips and computes the metrics.
- `search.py`: evaluates lists of pipelines in parallel, ranks them.
- `compare.py`: per-clip metrics, bootstrap intervals, paired differences.
- `timing.py`: time per function, one core, one OpenCV thread; `calibrate`
  compares this PC with the A53 numbers in `src/stages/README.md`.
- `candidates.txt`: the pipelines of the final ranking; `final.txt`: of the
  per-scene comparison.
