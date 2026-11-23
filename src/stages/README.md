# Processing functions

Every file here is one function. It registers itself, so nothing else (CLI,
`scipd`, the control interface, the GUI) needs to change when you add, remove or
modify one: they all ask the registry what exists and which parameters it takes.

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
