# AIPC: AMC Image Proc Control, protocol version 1.1

AIPC is how a host (the GUI, a script, `nc`) controls a running `scipd` on the
PC or on a board: what video comes in, which functions process it, where the
result goes, and how fast it all runs.

It is [JSON-RPC 2.0](https://www.jsonrpc.org/specification) over TCP, one
message per line. That choice keeps it standard (libraries exist for every
language), readable (you can type commands by hand) and easy to log.

```
host (GUI)                               target (scipd)
   |  UDP 5601  {"aipc":"discover"}  ---->  |    find targets on the LAN
   |  <----  {"aipc":"announce", ...}       |
   |                                        |
   |  TCP 5600  JSON-RPC requests  ------>  |    control
   |  <------  responses + event.* notifications
   |                                        |
   |  UDP 5000  H.264/RTP video  -------->  |    input  rtp://:5000
   |  or TCP 5000  raw video (GDP)  ----->  |    input  tcp://:5000   (lossless)
   |  or UDP 5000  raw video (RTP)  ----->  |    input  rtpraw://:5000 (never late)
   |  <--------  H.264/RTP video  UDP 5002  |    output udp:HOST:5002
   |  <--------  H.264/RTP video  UDP 5004  |    input output udp:HOST:5004 (the input)
```

Video never travels over the control connection; it uses standard RTP
streams that any GStreamer, VLC or ffmpeg can produce or show, or raw
frames: in GStreamer's GDP framing over TCP when nothing may be lost, or
over RTP/UDP (`rtpgstpay`) when a frame may be lost but never late.

## 1. Transport

| | |
|---|---|
| Control | TCP, default port **5600** (`scipd --port`) |
| Discovery | UDP, default port **5601** (`scipd --discovery-port`, 0 = off) |
| Framing | one JSON object per line, UTF-8, terminated by `\n` (JSON Lines). No raw newline inside a message; JSON escapes them in strings. |
| Size | at most 1 MiB per message; longer lines close the connection |
| Clients | several at once (default 8); all see the same target |
| Security | none: meant for a lab network. Bind to one interface with `scipd --bind ADDRESS`. |

Try it by hand:

```bash
nc 192.168.50.2 5600
{"jsonrpc":"2.0","id":1,"method":"system.ping"}
```

## 2. Messages

Standard JSON-RPC 2.0.

**Request** (client to server):

```json
{"jsonrpc": "2.0", "id": 7, "method": "pipeline.add", "params": {"function": "blur", "params": {"ksize": 5}}}
```

- `id` is a number or a string chosen by the client, echoed in the response.
- `params` is always an object (named parameters) or left out.
- A request without `id` is a *notification*: it is executed, nothing is
  answered. Avoid it for commands that can fail.
- Batches (arrays) are not supported and answered with error -32600.

**Success response**:

```json
{"jsonrpc": "2.0", "id": 7, "result": {"stage": {"id": 3, "...": "..."}}}
```

**Error response**:

```json
{"jsonrpc": "2.0", "id": 7, "error": {"code": -32000, "message": "blur: parameter 'ksize': 4 is not 3 + n * 2"}}
```

`message` is meant for humans: show it to the user as it is.

**Event notification** (server to client, only after `events.subscribe`):

```json
{"jsonrpc": "2.0", "method": "event.state", "params": {"state": "running", "message": ""}}
```

Responses may arrive interleaved with events; match responses by `id`.
Requests from one client are executed in order.

### Compatibility rules

- Clients must ignore fields they do not know; servers add fields in minor
  versions (1.1, 1.2...).
- A new major version (2.0) may change or remove things. Check
  `system.info` → `protocol.version` after connecting.
- Field names are `snake_case`.

## 3. Errors

| code | name | when |
|---|---|---|
| -32700 | Parse error | the line is not valid JSON |
| -32600 | Invalid request | not a JSON-RPC 2.0 request object |
| -32601 | Method not found | unknown `method` |
| -32602 | Invalid params | a field is missing or has the wrong JSON type |
| -32603 | Internal error | a bug on the server; please report |
| -32000 | Rejected | the request is well-formed but not acceptable: unknown function, value out of range, bad input/output URI |
| -32001 | Not found | no stage with this `id`, no such output |
| -32002 | Unavailable | not possible in this build or on this machine (e.g. no GStreamer) |

A rejected command changes nothing.

## 4. Data types

### Value

Function parameters have one of five types. On the wire:

| type | JSON | example |
|---|---|---|
| `bool` | `true` / `false` | `"invert": true` |
| `int` | integer | `"ksize": 5` |
| `float` | number (integers accepted) | `"sigma": 1.5` |
| `string` | string | `"pipeline": "videoflip method=clockwise"` |
| `enum` | string, one of `choices` | `"mode": "otsu"` |

### ParamSpec: describes one parameter

```json
{
  "name": "ksize",
  "type": "int",
  "description": "kernel size (odd)",
  "default": 3,
  "min": 3, "max": 31, "step": 2,
  "unit": "px"
}
```

| field | |
|---|---|
| `name`, `type`, `description`, `default` | always present |
| `min`, `max` | numbers; present when the value is limited |
| `step` | present for `int` when only `min + n*step` is valid |
| `choices` | array of strings, for `enum` |
| `unit` | present when meaningful (`px`, `deg`...) |

Everything a GUI needs to draw a widget: a check box (`bool`), a spin box
(`int`, with step), a slider (`float` with range), a combo box (`enum`), a
text field (`string`).

### Function: something that can be put in the pipeline

```json
{
  "name": "blur",
  "title": "Gaussian blur",
  "description": "Smooth the image with a Gaussian kernel.",
  "category": "filter",
  "backend": "native",
  "params": [ParamSpec, ...]
}
```

`backend` is `native` (C++/OpenCV in scip) or `gstreamer` (a GStreamer
element, its parameters read from the element's properties). The list differs
between machines and builds: always ask `functions.list`, never hard-code it.

### Stage: one function in the pipeline

```json
{"id": 3, "function": "blur", "params": {"ksize": 5}, "enabled": true, "text": "blur ksize=5"}
```

- `id` is assigned by the server, stable while the stage exists, never reused
  in the server's lifetime. Use it to edit, move or remove the stage.
- `params` holds only the values that were set; the others are at their
  `default`.
- `text` is the same in the command-line syntax of `scip-cli -p`.
- A disabled stage stays in place but frames pass it untouched.

### Pipeline

```json
{"stages": [Stage, ...], "text": "gray ! blur ksize=5"}
```

`text` covers the enabled stages only, so it is what actually runs.

### FrameInfo

```json
{"width": 1920, "height": 1080, "format": "bgr8"}
```

`format` is `gray8` or `bgr8`. `width` and `height` are 0 before the first frame.

### Input and output URIs

The same strings as `scip-cli -i` / `-o`:

| input | |
|---|---|
| `pattern:640x480[:gray]` | generated test image |
| `/path/file.png` | still image, repeated |
| `/path/clip.mp4` | video file on the target (mp4, mkv, mov, avi, webm, h264) |
| `camera:/dev/video0[:1280x720]` | V4L2 camera (live) |
| `rtp://:5000` | H.264 over RTP/UDP sent to the target, e.g. by the GUI (live) |
| `tcp://:5000` | Raw frames in GDP over TCP, the GUI's lossless mode (live; the size travels with the frames; waits for the next sender when one leaves) |
| `rtpraw://:5000` | Raw frames over RTP/UDP (`rtpgstpay config-interval=1`), the GUI's raw UDP mode (live; the size travels in-band, resent every second; a frame that lost a packet is dropped whole) |
| `rtsp://...`, `http(s)://...`, `file://...` | anything GStreamer's `uridecodebin` plays |
| `gst:... ! appsink name=scip_sink` | any GStreamer pipeline |

| output | |
|---|---|
| `display` | a window on a desktop, else the default screen (KMS) |
| `display:40` | a given DRM connector, see `output.displays` |
| `udp:192.168.50.1:5002` | H.264 over RTP/UDP to a host |
| `/path/out.mp4`, `.mkv` | recording |
| `/path/last.png` | the last frame, written when the output is removed |
| `gst:appsrc name=scip_src ! ...` | any GStreamer pipeline |

## 5. Methods

`→` shows the `result`. Optional parameters are marked `?` with their default.

### 5.1 system

#### `system.ping`

→ `{"time": 1763900000.123, "uptime": 42.5}`: server clock (Unix seconds)
and seconds since `scipd` started. Use it to measure round-trip time.

#### `system.info`

Everything about the target, for the GUI's "about" page and for logs.

```json
{
  "protocol": {"name": "AIPC", "version": "1.0"},
  "server": {
    "name": "scipd", "version": "0.1.0",
    "commit": "bfd67592a8", "describe": "bfd67592a8-dirty", "branch": "feature/aipc",
    "commit_date": "2026-11-23T14:02:11+03:00", "dirty": true,
    "build_type": "Release", "compiler": "GNU 10.2.0", "target_cpu": "aarch64",
    "pid": 812, "started": 1763899950.0
  },
  "device": {
    "name": "board-1",
    "hostname": "myd-jx8mp",
    "board": "MYD-JX8MP board",
    "machine": "aarch64",
    "cpu": {"model": "Cortex-A53", "cores": 4},
    "memory_mb": 5826
  },
  "os": {
    "name": "NXP i.MX Release Distro 5.10-gatesgarth",
    "kernel": "5.10.9-myir+g...",
    "kernel_build": "#1 SMP PREEMPT ..."
  },
  "libraries": {"gstreamer": "1.18.0", "opencv": "4.4.0"},
  "capabilities": {
    "gstreamer": true,
    "window_system": false,
    "hardware": {"vpu_decoder": true, "vpu_encoder": true, "g2d": true},
    "inputs": ["pattern", "image", "video", "camera", "rtp", "tcp", "rtpraw", "uri", "gst"],
    "outputs": ["display", "udp", "video", "image", "gst"]
  },
  "control": {"port": 5600, "discovery_port": 5601, "clients": 1}
}
```

- `device.name` is `scipd --name`, else the host name. It is what the GUI
  lists.
- `device.board` comes from the device tree (`/proc/device-tree/model`) on
  boards and from DMI on PCs; empty if unknown.
- Fields may be empty strings when unknown; they are never left out.

#### `system.methods`

→ `{"methods": ["functions.describe", "functions.list", ...]}`, sorted.

### 5.2 functions

#### `functions.list`

→ `{"functions": [Function, ...]}`, sorted by name.

#### `functions.describe`

`{"name": "blur"}` → `Function`. Error -32000 for an unknown name.

### 5.3 pipeline

Every method returns the whole `Pipeline` afterwards, so a GUI can redraw from
the answer; `add` and `update` also return the stage they touched.

| method | params | result |
|---|---|---|
| `pipeline.get` | | `Pipeline` |
| `pipeline.set` | `text` (`"gray ! blur ksize=5"`) **or** `stages`: `[{"function", "params"?, "enabled"?}]` | `Pipeline` |
| `pipeline.add` | `function`, `params`? `{}`, `index`? end, `enabled`? `true` | `Pipeline` + `stage` |
| `pipeline.update` | `id`, `params`? (merged into the current values), `enabled`? | `Pipeline` + `stage` |
| `pipeline.remove` | `id` | `Pipeline` |
| `pipeline.move` | `id`, `index` (0 = first; clamped) | `Pipeline` |
| `pipeline.clear` | | `Pipeline` |

Changes apply between two frames. When the input pauses (the GUI's player is
paused, a live sender stopped), a change is applied to the last frame and the
result sent to the outputs at once, so a paused video can be tuned; these
repeats do not count in `frames` or `fps`. If a stage cannot process the current
frames (e.g. `clahe` needs `gray8` but gets `bgr8`), the command still
succeeds: the stage is *bypassed* and `stats.get` reports its `error`. That
keeps the video running while you experiment.

To reset a parameter to its default, `pipeline.update` it with the
`default` value from `functions.describe`.

### 5.4 input

#### `input.get`

→ `Input`:

```json
{"uri": "rtp://:5000", "loop": false, "realtime": true, "gray": false, "max_fps": 0, "max_frames": 0,
 "player": false}
```

| field | |
|---|---|
| `uri` | see section 4; `""` = no input |
| `loop` | video files: start again at the end |
| `realtime` | video files: play at their frame rate instead of as fast as possible |
| `gray` | decode straight to gray8 (cheaper when the pipeline starts with `gray`) |
| `max_fps` | limit non-live inputs; 0 = no limit (patterns and images default to 30) |
| `max_frames` | stop after this many frames; 0 = never |
| `player` | video files: control it like a player (section 5.5): it opens paused on its first frame and stays open at its end |

#### `input.set`

Fields of `Input`; `uri` is required, the others default as shown above.
→ `Input`. The URI is checked at once (-32000 for a typo, a missing file or
camera); opening happens in the background and is reported by `stats.get` /
`event.state` (`waiting`, then `running`, or `error`). Setting the same input
again restarts it.

#### `input.devices`

```json
{"cameras": [{"path": "/dev/video0", "name": "HD Pro Webcam C920", "driver": "uvcvideo",
              "bus": "usb-xhci-hcd.1.auto-1", "uri": "camera:/dev/video0",
              "modes": [{"width": 2304, "height": 1536, "fps": 2.0, "format": "YUYV",
                         "uri": "camera:/dev/video0:2304x1536"},
                        {"width": 640, "height": 480, "fps": 30.0, "format": "YUYV",
                         "uri": "camera:/dev/video0:640x480"}]}]}
```

`modes` lists the uncompressed sizes, largest first, each with its best
frame rate. Without a size the camera's largest mode is used, which can be
slow: offer the user the modes.

### 5.5 player

A video file on the target, opened with `input.set` and `"player": true`, is
controlled like a media player: the host plays the file without sending it,
and can watch it through an input output (5.6). Without a player input these
methods fail with -32000, except `playback.get` (`"available": false`).

| method | params | result |
|---|---|---|
| `playback.get` | | `Playback` |
| `playback.play` | | `Playback`; at the end, plays again from the start (of the A-B range) |
| `playback.pause` | | `Playback` |
| `playback.seek` | `position`: seconds | `Playback`; while paused, the frame it lands on is processed and sent |
| `playback.set` | `rate`?, `loop`?, `a`?, `b`? | `Playback`; `a`/`b` in seconds, both or neither, `null` clears them |

`Playback`:

```json
{"available": true, "state": "playing", "position": 12.48, "duration": 60.0,
 "rate": 1.0, "loop": false, "a": null, "b": null}
```

| field | |
|---|---|
| `state` | `playing`, `paused`, or `ended` (paused at the end) |
| `position` | seconds, of the last frame processed |
| `loop` | the whole video; with `a` and `b` it loops between them (and plays only that) |

The same object, without `available`, is `playback` in `Status` (5.7): a host
moves its position slider from there and between two updates by itself
(`position + elapsed × rate` while playing).

#### `files.list`

`{"path"?: folder}` → the folders, videos and images in a folder of the
target, to choose a file to play; without `path`, the folder of the current
input file, else the home folder. Hidden entries are left out.

```json
{"path": "/home/root/videos", "parent": "/home/root",
 "entries": [{"name": "day", "path": "/home/root/videos/day", "kind": "folder", "size": 0},
             {"name": "clip.mp4", "path": "/home/root/videos/clip.mp4", "kind": "video", "size": 4823112}]}
```

### 5.6 output

The target sends the processed video to zero or more outputs at once, and
its input, as it comes in, to zero or more *input outputs* (to watch a
camera or a file next to the result; they cost an encoder on the target).

| method | params | result |
|---|---|---|
| `output.get` | | `{"outputs": ["display:40", "udp:192.168.50.1:5002"], "input_outputs": ["udp:192.168.50.1:5004"]}` |
| `output.set` | `outputs`?: `[uri, ...]`, `input_outputs`?: `[uri, ...]` (at least one; a list left out stays) | same |
| `output.add` | `uri` | same; -32000 if already present |
| `output.remove` | `uri` | same; -32001 if not present |
| `output.displays` | | see below |

Outputs open with the first processed frame and reopen when the frame format
changes (e.g. after adding `resize`). One that fails is dropped and its
`error` is shown in `stats.get`; the others keep going.

`output.displays`:

```json
{
  "window_system": false,
  "displays": [
    {"card": "/dev/dri/card1", "connector": 40, "name": "HDMI-A-1", "connected": true,
     "width": 1920, "height": 1080, "refresh_hz": 60, "uri": "display:40"}
  ]
}
```

With `window_system: true` (a PC desktop), `display` opens a window and the
connector list is informative only.

### 5.7 stats

#### `stats.get`

→ `Status`:

```json
{
  "state": "running",
  "message": "",
  "input": {"uri": "rtp://:5000", "live": true, "fps": 30.0,
            "info": {"width": 1920, "height": 1080, "format": "bgr8"}},
  "output_info": {"width": 1920, "height": 1080, "format": "gray8"},
  "fps": 29.8,
  "frames": 1234,
  "timing": {"read_ms": 30.1, "process_ms": 6.2, "process_p95_ms": 7.0, "write_ms": 2.4},
  "stages": [
    {"id": 1, "text": "gray", "enabled": true, "active": true, "error": "", "mean_ms": 1.1, "p95_ms": 1.3},
    {"id": 2, "text": "clahe", "enabled": true, "active": false, "error": "clahe: needs gray8", "mean_ms": 0, "p95_ms": 0}
  ],
  "outputs": [
    {"uri": "udp:192.168.50.1:5002", "description": "udp 192.168.50.1:5002 (vpuenc_h264)", "active": true, "error": ""}
  ],
  "input_outputs": [],
  "playback": null
}
```

`playback` is a `Playback` (5.5) when the input is a player, else `null`.

| `state` | |
|---|---|
| `idle` | no input, or it ended (`message`: `"input ended"`) |
| `waiting` | input open, no frames (yet): a network sender has not started or paused |
| `running` | frames are flowing |
| `paused` | a player input is paused (`message`: `"end of the video"` at its end) |
| `error` | the input failed; `message` says why |

Timings are means over the last ~120 frames; `fps` counts the last second.
`read_ms` includes waiting for a live source, so on a 30 fps stream it is
about 33 ms even when decoding is fast.

### 5.8 events

#### `events.subscribe`

`{"topics": ["stats", "state", "config"], "interval_ms"?: 1000}` →
`{"topics": [...], "interval_ms": 1000}` (all topics of this connection).

#### `events.unsubscribe`

`{"topics"?: all}` → `{"topics": [...remaining]}`

| topic | notification | params | when |
|---|---|---|---|
| `stats` | `event.stats` | `Status` | every `interval_ms` (100 or more) |
| `state` | `event.state` | `{"state", "message"}` | when the state or message changes |
| `config` | `event.config` | `{"what": "pipeline" \| "input" \| "outputs" \| "playback"}` | after any client changed it: fetch it again with `*.get` |

Subscriptions belong to the connection and end with it.

## 6. Discovery

A client finds targets by sending one UDP datagram to port 5601, as broadcast
(`255.255.255.255` or the subnet's broadcast address) or directly to a host:

```json
{"aipc": "discover"}
```

Every `scipd` answers to the sender's address and port:

```json
{"aipc": "announce", "protocol": "1.0", "name": "board-1", "hostname": "myd-jx8mp",
 "board": "MYD-JX8MP board", "version": "0.1.0", "commit": "bfd67592a8", "port": 5600}
```

The client then connects to the datagram's source address on `port`. Other
datagrams are ignored. Wait about a second for answers; retry once, UDP may
drop.

## 7. A session

```text
--> {"jsonrpc":"2.0","id":1,"method":"system.info"}
<-- {"jsonrpc":"2.0","id":1,"result":{"protocol":{"name":"AIPC","version":"1.0"}, ...}}
--> {"jsonrpc":"2.0","id":2,"method":"functions.list"}
<-- {"jsonrpc":"2.0","id":2,"result":{"functions":[...]}}
--> {"jsonrpc":"2.0","id":3,"method":"input.set","params":{"uri":"rtp://:5000"}}
<-- {"jsonrpc":"2.0","id":3,"result":{"uri":"rtp://:5000","loop":false, ...}}
--> {"jsonrpc":"2.0","id":4,"method":"output.set","params":{"outputs":["udp:192.168.50.1:5002"]}}
<-- {"jsonrpc":"2.0","id":4,"result":{"outputs":["udp:192.168.50.1:5002"]}}
--> {"jsonrpc":"2.0","id":5,"method":"events.subscribe","params":{"topics":["state","stats"]}}
<-- {"jsonrpc":"2.0","id":5,"result":{"topics":["state","stats"],"interval_ms":1000}}
                         ... the GUI starts sending video to port 5000 ...
<-- {"jsonrpc":"2.0","method":"event.state","params":{"state":"running","message":""}}
--> {"jsonrpc":"2.0","id":6,"method":"pipeline.set","params":{"text":"gray ! blur ksize=5"}}
<-- {"jsonrpc":"2.0","id":6,"result":{"stages":[{"id":1,...},{"id":2,...}],"text":"gray ! blur ksize=5"}}
--> {"jsonrpc":"2.0","id":7,"method":"pipeline.update","params":{"id":2,"params":{"ksize":4}}}
<-- {"jsonrpc":"2.0","id":7,"error":{"code":-32000,"message":"blur: parameter 'ksize': 4 is not 3 + n * 2"}}
<-- {"jsonrpc":"2.0","method":"event.stats","params":{"state":"running","fps":29.9, ...}}
```

## 8. Changes

| version | |
|---|---|
| 1.0 | first version |
| 1.1 | `tcp://` input (raw frames); player inputs: `input.set` `player`, `playback.*`, `files.list`, state `paused`; `input_outputs` in `output.*` and `Status` |
| 1.2 | `rtpraw://` input (raw frames over RTP/UDP); `tcp` and `rtpraw` listed in `capabilities.inputs` |
