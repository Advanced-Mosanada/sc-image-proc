# Processing functions

Every file here is one function. It registers itself, so nothing else (CLI,
`scipd`, the control interface, the GUI) needs to change when you add, remove or
modify one: they all ask the registry what exists and which parameters it takes.

## The functions

Time per frame on the board (i.MX 8M Plus, one Cortex-A53 core, `performance`
governor, 1280x720, imx8mp-release), colour / gray. Colour frames go through
their brightness (Y of YCrCb) for the contrast functions, so hues stay.

| Category | Function | What it is for | bgr | gray |
|---|---|---|---|---|
| contrast | `levels` | black / white point and gamma, by hand | 3.8 | 1.3 |
| | `equalize` | global histogram equalisation, strong | 11 | 1.9 |
| | `autolevels` | stretch the used range; limits smoothed over time | 9.3 | 5.8 |
| | `clahe` | local (tiled) equalisation, detail in dark/flat areas | 18 | 9.2 |
| | `retinex` | remove uneven lighting, lift shadows | 36 | 28 |
| | `dde` | thermal detail enhancement: local contrast on a base layer, texture above the noise boosted, no halos or bands (*) | | |
| denoise | `blur` | Gaussian, ksize 5 | 5.6 | 1.9 |
| | `median` | impulse noise (hot/dead pixels); ksize 3 / 5 | 8 / 67 | 3.1 / 28 |
| | `bilateral` | edge-preserving smoothing | 40 | 20 |
| | `guided` | edge-preserving, cost independent of the radius | 52 | 17 |
| | `nlmeans` | best quality, a reference: seconds per HD frame | 1850 | 840 |
| temporal | `temporal_mean` | running average; trails on motion | 17 | 5.5 |
| | `temporal_median` | flicker without trails; 3 / 5 frames | 17 / 35 | 5.8 / 11 |
| | `tnr` | motion-adaptive: averages only where nothing moves; `window=3` and `align=true` (follows a panning camera) (*) | 33 | 14 |
| sharpen | `sharpen` | 3x3 Laplacian, finest detail | 21 | 7.2 |
| | `unsharp` | unsharp mask; threshold spares noise | 20 | 6.9 |
| | `detail` | detail boost without halos at strong edges | 53 | 18 |

(*) not yet measured on the board; on the PC (one core, 640x512 gray) `dde`
radius 4 takes 3.6 ms and `tnr window=3 align=true` 1.5 ms, which the A53/PC
ratios of the other functions put at roughly 25-30 and 10 ms on the A53.

Typical chains: `tnr ! autolevels ! unsharp threshold=4` (noisy camera),
`retinex ! detail` (backlit scenes). For thermal (IR) video use the pipeline
chosen in [docs/thermal-enhancement.md](../../docs/thermal-enhancement.md):
`tnr window=3 noise=4 align=true skip_repeats=true ! guided radius=4 eps=0.00025 ! dde radius=4 eps=0.01 contrast=1 clip=3 tiles=8 gain=3.5 fine_gain=1.6 noise=3`.
`clahe` alone nearly triples the noise of thermal video, and `temporal_median`
smears a moving camera's picture.

The temporal functions take `skip_repeats=true` for video converted to a higher
frame rate (repeated frames): a repeat gives the previous output instead of
being counted as a new sample.
Functions with history (`temporal_*`, `tnr`, `autolevels`) start over when
the stream's format or the pipeline changes.

The slow ones (`guided`, `detail`, `bilateral`, `nlmeans`, `retinex`) spend
their time in full-image float passes, which on the A53 are limited by memory
bandwidth: candidates for fused NEON loops or the GPU (OpenCL).

## Adding a function

1. Copy the closest existing file (e.g. `invert.cpp` without parameters,
   `threshold.cpp` with some) to `src/stages/<name>.cpp`.
2. Implement the `Stage`:
   - `configure(input)` checks the input format and returns the output format.
     Throw `std::invalid_argument` with a helpful message for inputs you do not
     support (e.g. "needs gray8 input (put 'gray' before it)").
   - `process(in, out)` writes into `out`, which is already allocated. Do not
     allocate per frame; keep scratch buffers as members.
3. Describe it in `makeInfo()`: `name` (used in pipelines), `title`,
   `description`, `category` and `params`, built with the `param::` helpers:

   ```cpp
   param::integer("ksize", "Kernel size (odd)", 5, 3, 31, 2, "px")  // 3, 5, 7 ... 31
   param::number("sigma", "Standard deviation", 0.0, 0.0, 20.0, 0.1)
   param::boolean("invert", "Swap foreground and background", false)
   param::choice("mode", "Algorithm", "fixed", {"fixed", "otsu"})
   ```

   The factory receives the parameters already validated and with defaults filled
   in; read them with `getInt`, `getFloat`, `getBool`, `getString`.
4. `SCIP_REGISTER_STAGE(makeInfo());`
5. Add the file to `CMakeLists.txt` and a test to `tests/test_stages.cpp`
   (compare against OpenCV or a hand-computed result).

Try it: `scip-cli --list-stages`, then `scip-cli -i clip.mp4 -p 'gray ! <name> key=value'`.

## Removing or replacing a function

Delete the file and its CMake line. A faster version of an existing function
(NEON, OpenCL, G2D) can register under a new name (`blur_neon`) so both can be
compared side by side in the same pipeline, then replace the original.

## GStreamer filters

Standard GStreamer video filters (`videoflip`, `videobalance`, `gamma`, ...) are
exposed as functions too, with their parameters read from the element itself; see
`src/gst/gst_stages.cpp`.
