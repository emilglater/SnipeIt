# Capstone "Products" chapter — pi-streaming evidence pack

Scope: `pi-streaming/` and `pi-streaming/orin/` only (camera/streaming/detection
pipeline, Pi↔Orin protocol, tracker). Excludes sensor drivers, acoustic module,
Android app internals.

Compiled 2026-08-03. Sections A/B1/E are **not yet measured** — they require
staged live runs (see the coordination section).

---

# Provenance key

| Mark | Meaning |
|---|---|
| **[DOC]** | Contemporaneous handoff doc in `pi-streaming/orin/`, written at the time |
| **[GIT]** | Commit metadata or diff |
| **[LOG]** | Raw captured terminal output surviving in a session transcript |
| **[ARTIFACT]** | Measured on 2026-08-03 from a surviving binary file |
| **[NOTE]** | The project engineering log (`memory/project_pi_orin_protocol.md`) — written contemporaneously, but it is a summary; the raw tool output behind it is gone |
| **[CODE]** | Current source, read 2026-08-03 |
| **NOT MEASURED** | No number exists |

---

# A. Measurements — NOT RUN, holding for coordination

Current device state (read-only probes, safe to run, no scene dependency):

```bash
pidof streaming_server mediamtx ffmpeg   # → nothing running
ip -brief addr show eth0                 # → UP  10.42.0.1/24
ping -c1 10.42.0.2                       # → 0% loss, 0.807 ms
rpicam-hello --list-cameras              # → imx477 present
# 6 s passive listen on 10.42.0.1:5556   # → 0 inbound connects
```

**The Orin host answers ping but its detector app is not running** — a live
detector retries its ZMQ PUSH ~5/s, and 0 connects were seen in 6 s. So A3, A4,
B1 and E3 are blocked on the Orin operator.

## What each measurement needs before it starts

**A1 — sustained encode rate, 60 s, outdoor.** Needs: camera + **real outdoor
daylight on the scene**, app connected (dual encode is the production load).
Orin not required — RTP is fire-and-forget UDP.

⚠️ **Read this before staging A1.** The number is not a pure encoder number. The
IMX477's auto-exposure lengthens frame duration in low light, so at 1080p the
*delivered* rate is jointly limited by light and by x265. **[NOTE]** records
~0.5 fps on idle CPU indoors purely from AE. So "sustained H.265 encode rate"
splits into two different figures:

- **delivered capture+encode fps in daylight** — the honest system number, and
  the one belonging in the report as a limitation;
- **encoder ceiling**, which needs `camera_source=test` (videotestsrc).

Substituting the test source is a synthetic-input swap and must be flagged as
such. It will not be run unless explicitly requested, and if it is, it goes in
the report as a separate, separately-labelled number.

**A2 — 2 vs 4 cores.** The 2-core number falls out of A1 free. The 4-core arm
requires editing `pools=2`→`pools=4` in `main.c:172`, rebuilding, and running
with **the app connected and someone watching the video**, because the failure
*is* the app video freezing. Recommendation: **don't reproduce it.** It is a
known-bad state that starves `hostapd`, and the demo currently works. Take the
2-core number plus the recorded original observation (below).

**A3 — end-to-end latency.** **Does not exist today. Nothing is timestamped
end-to-end.** See the exact patch below. Once added, needs the full live loop.

**A4 — pose-ring join stats, ≥300 frames.** Needs the full loop with the Orin
detecting. At the observed ~2.4 fps, 300 frames is **≥2 min 5 s**; budget
3–4 min. Miss-*causes* also need instrumenting (below).

**A5 — app-facing RTSP branch.** Two different numbers again: `ffprobe` against
the RTSP branch **on the Pi** measures what mediaMTX serves, with no phone;
**delivered** FPS to the app depends on the phone's Wi-Fi and needs the phone
connected. Both are worth reporting.

**B1 — tracker under pan.** Needs full system + **two people visible
simultaneously, both in frame across a complete pan sweep**, ideally with one
standing still (the world-stationary target whose ID-hold count is the actual
claim). The pan must be a full scan sweep, not a hand-nudge, or the "frames held
across a full pan" number means nothing.

## Proposed order — one session gets A1, A3, A4, A5, B1, E3

1. Confirm: **Orin detector up**, app on a phone (not the Galaxy Tab S8 —
   **[DOC]** records 6–8% frame loss on that device), **outdoor daylight**.
2. Add the A3/A4 instrumentation and rebuild (**code change — needs approval**).
3. Run 1 (~90 s), camera at a static outdoor scene, **no people**: gives A1,
   A2's 2-core arm, A5.
4. Run 2 (~4 min), **two people, one stationary, camera doing a full scan
   pan**: gives A3, A4, B1, E3.

## A3 — exactly what has to be added

The ingredients are already there; nothing computes with them. **[CODE]**

- `PoseEntry.capture_ts_ms` — CLOCK_MONOTONIC ms at capture, `pose_ring.h:44`,
  populated per frame.
- `now_ms_mono()` — already in `ddl_bridge.c:314`.
- `orin_detection_handler()` (`ddl_bridge.c:218`) receives the joined `pose` and
  later calls `ddl_servo_set_target()`.

Three additions:

1. At handler entry: `lat_a = now_ms_mono() - pose->capture_ts_ms` → capture →
   detection-returned.
2. Immediately after a successful `ddl_servo_set_target()` (`ddl_bridge.c:307`):
   `lat_b` the same way → capture → servo command issued.
3. Accumulate both into a reservoir or fixed histogram and print mean + p95 on
   the existing 5 s `[BRIDGE]` stats line, or append CSV to a file for post-run
   analysis.

**Caveats that must go in the report:** `capture_ts_ms` is stamped where the
frame enters the encode branch, so it excludes sensor exposure/ISP time — it is
*not* photon-to-servo. And `lat_b` only exists while locked.

**For A4's miss causes**, `pose_ring_lookup()` (`pose_ring.h:107`) returns a bare
`false` and cannot distinguish *aged out of the 16-deep ring* from *never
recorded*. Separating them needs the lookup to also report whether `frame_id` is
older than the oldest retained entry. Without that, A4 gives a miss *count* but
its causes are guesswork. The slew-settling skip is already counted separately
as `slew_skip`.

## The one real surviving encode-rate measurement

**[LOG]** — a captured run on the real camera, 2026-07-06, app connected over
WS, Orin not pushing:

```
[SENDER] streaming 1920x1080@30 H.265 (libcamera) -> sink
[MAIN] Frame sender up: camera -> H.264 FIFO + H.265+SEI RTP 10.42.0.2:5600
configuring streams: (0) 1920x1080-YUV420/Rec709
[BRIDGE] stats: cap=2.8 fps  orin=0.0 msg/s  miss=0  slew_skip=0  aim=0  (last 5s)
[BRIDGE] stats: cap=2.4 ... 2.2 ... 2.4 ... 2.4 ... 2.2
[SENDER] stopped (captured=82 ...)
```

Six consecutive 5 s windows: **mean 2.4 fps, min 2.2, max 2.8**, 82 frames
captured over the run. **Lighting is not documented in this log** and the Orin
leg was idle. Treat as indicative, not as A1.

**Production encoder parameter line** **[CODE]**, `main.c:164–172` +
`frame_sender.c:356`:

```
x265enc tune=zerolatency speed-preset=ultrafast bitrate=20000 key-int-max=12
        option-string="pools=2:keyint=12:min-keyint=12:scenecut=0:open-gop=0"
```

Input 1920×1080 I420, caps framerate 30/1 (caps only — actual delivery is
light-bound). ABR 20 Mb/s. **[DOC]** `PI_REPLY_GOP_FIX.md` carries the same
table.

**App-facing branch, from config** **[CODE]** — `app_preview_width/height/fps`
all default to `0` (`config.c:124–126`) and are absent from
`streaming_config.json`, so they fall back to the capture values. The branch
therefore inserts **no** `videoscale` and **no** `videorate`:

```
x264enc tune=zerolatency speed-preset=ultrafast bitrate=4000 key-int-max=30
  → h264parse config-interval=1 → filesink /tmp/camera_stream.h264
  → FFmpeg (remux, no re-encode) → mediaMTX → rtsp://<pi>:8554/stream
```

So: **1920×1080, H.264, 4000 kb/s**, not rate-limited. **Delivered FPS: NOT
MEASURED.**

**A2 — what was originally observed** **[CODE]**, the comment at
`main.c:161–164`, written at the time of the change:

> *"x265 at superfast on all cores pinned the CPU at 0% idle, starving
> hostapd/mediaMTX/net softirq — the app's video froze even though the local
> FIFO→FFmpeg path kept 30 fps."*

The tell is precise and worth quoting in the report: the **local** encode path
held 30 fps while the **app's** video froze — so it was network-service
starvation, not encoder collapse. **Freeze duration: NOT MEASURED.** No 4-core
FPS figure exists.

---

# B. Tracker

## B2 — the 2026-07-06 ID churn fix

**[GIT]** `5001a34`, 2026-07-06 17:39:19 +0100. **[NOTE]** carries the
diagnosis.

**Symptom (operator-reported):** IDs churned for the same person; markedly worse
in scan mode; on a lock slew the IDs "ruined" and the camera then chased whoever
inherited the old ID.

**Root cause — stale pose.** `ddl_bridge_record_capture_pose()` read servo
angles from `app_get_ddl_snapshot()`. That snapshot is refreshed by the
broadcaster's `copy_frame` only once per scheduler cycle, and
`eSCHEDULER_CYCLE_TIME_MS = 2000`. So for up to 2 s after every 10° scan step,
frames were stamped with **pre-move** angles. The tracker's whole premise is
mapping each bbox to a world bearing using that pose — a 10° pose error throws
the bearing ~10° off, far outside the 2–3° association gate, so **every track
failed to associate and re-spawned on every scan step.** The angular-space
design was working exactly as specified; it was being fed a lie.

**Fix — four parts** (one commit, all four in `5001a34`):

1. New `servo_fsm_get_pose()` / `ddl_servo_get_pose()`: mutex-guarded commanded
   angles updated the instant `servo_set_both_angles`/`servo_init_angles` writes
   the PCA9685. Bridge stamps frames from that.
2. New `eSERVO_EVENT_TARGET_UPDATE`, published after each successful
   `ddl_servo_set_target`, handled in `TARGET_LOCK` — lock-follow now corrects at
   detection rate (2–8 Hz) in small steps instead of one accumulated jump per
   2 s.
3. Slew-settling gate: a >`POSE_SETTLE_JUMP_DEG`(3°) commanded-pose jump opens a
   `POSE_SETTLE_MS`(300 ms) window; detections captured inside it are treated as
   a pose miss (raw forward, no tracker/aim), so tracks coast through the
   physical slew rather than re-spawning. 1.5 s coast vs 2 s scan dwell means IDs
   survive.
4. `tilt_sign` flipped to `+1` in `aiming.c` — separate bug, mechanical: the
   vertical axis had been remounted (range 70..150, level ~110), inverting the
   2026-07-01 calibration.

Plus the `[BRIDGE]` stats line, added in the same commit to close the
observability gap.

**What confirmed it worked — do NOT write "confirmed" in the report.**

The only confirmation on record is **[LOG]**, the operator's last message of that
session:

> *"I ran but forgot to tell you, never mind everything was run succesfuly. I
> want to commit the changes we made…"*

That is a verbal all-clear with **no captured output** — no stats line, no
counters, no ID trace. The session's own summary asserts "the outdoor validation
run succeeded," but that assertion derives solely from the sentence above.
Separately, **[NOTE]** written that same afternoon lists as **STILL PENDING**:
*"full validation run with app connected (lock centering = tilt_sign, smooth
follow = TARGET_UPDATE, id survival across scan steps = fresh pose + settle
gate, slew_skip sanity in stats line)."*

And every `[BRIDGE]` stats line that survives anywhere shows
`orin=0.0 msg/s, miss=0, slew_skip=0, aim=0` — i.e. **no captured stats line has
ever shown the tracker actually running.**

Honest wording for the report: *the fix was deployed and the operator reported a
successful run; no instrumented post-fix measurement was captured.* The A4/B1 run
would be the first.

## B3 — were image-space trackers ever run? **No. Design argument only.**

ByteTrack/SORT/Norfair were **never installed, never run, and never observed to
break under pan.** There is no code, no dependency, no log, and no benchmark
anywhere in the repo or the surviving transcripts.

The actual decision path **[NOTE]**, **[DOC]**: the Orin team answered Q3
declaring themselves stateless with per-frame 1-based indices, pushing tracking
onto the Pi. The Pi side named "ByteTrack/Norfair-style" as the intended
approach, then on reaching `TRACKER_DESIGN.md` reasoned that (a) the camera pans
constantly so image-space association would break, and (b) with ≤ a handful of
targets, "full ByteTrack/Kalman/Hungarian is overkill" — and wrote a greedy
nearest-neighbour associator in angular space instead. Both the pan argument and
the complexity argument are *a priori*.

**Write it as a design argument, not a tested alternative.** The supporting
evidence for the design is `test_tracker.c` (4/4 pass), which asserts a
world-stationary target keeps one ID as the camera pans — but that is a
**synthetic test with injected poses**, not a comparison against an image-space
tracker, and not field data.

## B4 — final tuned values: **none were ever changed.**

**[CODE]** `tracker.c:15–19` vs **[DOC]** `TRACKER_DESIGN.md` "proposed
defaults":

| Param | Proposed | Current | Changed? |
|---|---|---|---|
| `GATE_MIN_DEG` | 2.0° | **2.0f** | No |
| `GATE_K` | 1.5 | **1.5f** | No |
| `MAX_COAST_MS` | 1500 | **1500u** | No |
| `N_INIT` | 2 | **2** | No |
| EMA `alpha` | 0.5 | **0.5f** (`BEARING_ALPHA`) | No |
| `MAX_TRACKS` | 32 | **32** | No |

**Not one tracker tunable was ever modified after field observation.** The gates
were listed as tuning candidates if churn persisted, but the churn turned out to
be a stale-pose bug upstream, so the tracker was never retuned. The documented
constant-velocity predictor in `tracker.c:31–36` also remains
**unimplemented** — it is an extension point, not a feature.

---

# C. The retired EdgeTPU path

**Scope caveat up front:** the EdgeTPU work predates every surviving session
transcript (oldest is 2026-06-28; the EdgeTPU session is 2026-05-28 and its
transcript is gone). The Coral is not physically connected, so **none of C1–C3
can be re-measured.** What follows is git, code, the engineering note, and — for
C2/C3 — static analysis of the surviving model files.

## C1 — corrected figures

The commonly-quoted ~670 ms / ~970 ms are **close but not exact.** **[NOTE]**,
2026-05-28, records:

- **~665 ms/inference** (~1.5 fps) — EdgeTPU delegate loaded, partial offload
- **~962 ms** — plain int8, no delegate, pure CPU
- Stated conclusion: *"the EdgeTPU IS helping (~30% faster) but not enough"*

Use **665 ms** and **962 ms**, or write "~0.67 s / ~0.96 s".

| Item | Value |
|---|---|
| Model file (TPU path) | `ssd_mobilenet_v2_fpnlite_640_person_int8_edgetpu.tflite` **[ARTIFACT]** 10,686,048 bytes |
| Model file (CPU path) | `ssd_mobilenet_v2_fpnlite_640_person_int8.tflite` **[ARTIFACT]** 5,663,416 bytes |
| Detector input resolution | **640×640×3, uint8** **[ARTIFACT]**, verified: `shape [1,640,640,3]` |
| Camera feed into detector | picamera2 **lores 1280×720 RGB888**, letterboxed to 640×640 with gray-128 pad |
| Date | **2026-05-28** |
| Both models retained at | `pi-streaming/legacy/edgetpu/models/` |

**How it was measured — this matters for the report.** `diag_tpu.py`
(**[CODE]**, survives) times `interpreter.invoke()` with `time.monotonic()`
around it, over a default of 10 **synthetic** frames, reporting avg/min. Its own
header says *"Inference time (<200 ms = TPU path, >500 ms = CPU path)"*. So these
are **inference-call timings on synthetic input, n≈10** — not end-to-end pipeline
latency and not a camera-fed sustained rate. Say so, or the number over-claims.

The exact command **[NOTE]**:

```bash
source ~/venvs/edgetpu/bin/activate
python3 diag_tpu.py --model models/ssd_mobilenet_v2_fpnlite_640_person_int8_edgetpu.tflite --no-bench --camera
```

## C2 — recovered from the artifact; corrects the "multi-subgraph" premise

**No compiler log survives.** No `edgetpu_compiler` output exists anywhere on the
device — the repo, the legacy dir and every transcript were searched. So the
literal question ("what did the compiler log report") is **NOT RECORDED**.

**But the compiled artifact survives**, and the partition is recoverable from it.
**[ARTIFACT]** — both `.tflite` flatbuffers parsed 2026-08-03 (no Coral needed,
no inference):

```
compiled  ssd_mobilenet_v2_fpnlite_640_person_int8_edgetpu.tflite : 45 nodes
    node 0: edgetpu-custom-op        ← the ONLY TPU segment
    nodes 1–44: on CPU —
        22  CONV_2D          8  RESHAPE        4  PACK
         2  ADD              2  DEPTHWISE_CONV_2D
         2  CONCATENATION    2  DEQUANTIZE
         1  LOGISTIC         1  TFLite_Detection_PostProcess

uncompiled ssd_mobilenet_v2_fpnlite_640_person_int8.tflite : 150 nodes
    92 CONV_2D, 21 DEPTHWISE_CONV_2D, 14 RESHAPE, 12 ADD, 4 PACK,
     2 CONCATENATION, 2 DEQUANTIZE, 1 QUANTIZE, 1 LOGISTIC,
     1 TFLite_Detection_PostProcess
```

**Correction to the framing.** "Multi-subgraph compile penalty" is not what the
artifact shows. There is exactly **one** TPU segment, not many. The compiler
folded ~106 of the 150 ops into that single `edgetpu-custom-op` (the MobileNetV2
backbone) and then **stopped**, leaving **44 ops on the ARM cores — including 22
convolutions**, which is the FPN detection head plus the whole post-process tail.

Accurate statement: *the compiler placed the backbone on the TPU as one segment
but could not map the FPN head, leaving 44 ops — 22 of them convolutions —
executing on CPU.* That is why the delegate bought only ~30% (962→665 ms)
instead of the ~30 ms a fully-mapped model gives. Unlike the rest of C, this is
measurable today.

Reproduce with:

```bash
~/venvs/edgetpu/bin/python3 -c "
import tflite_runtime.interpreter as tflite; from collections import Counter
it=tflite.Interpreter(model_path='models/ssd_mobilenet_v2_fpnlite_640_person_int8_edgetpu.tflite')
print(Counter(o['op_name'] for o in it._get_ops_details()))"
```

(Construct the interpreter but **don't** call `allocate_tensors()` — that needs
the delegate.)

The op-level attribution in **[NOTE]** — *"FPN reshape/concat/transpose ops the
EdgeTPU compiler left on CPU"* — is directionally confirmed by the artifact
(8 RESHAPE, 2 CONCATENATION, 4 PACK all on CPU), though the dominant cost is the
22 CPU convolutions, and there are no TRANSPOSE ops at all.

## C3 — the input-quantization contract: diagnosed **and fixed**

Fully documented by **[GIT]** `815c68e` (2026-05-30 14:58:03 +0100), which is
primary evidence — the diff and its code comment.

**Observable symptom:** detection scores flat at **~0.05–0.13** on every frame,
**scene-invariant** — the same values on a person as on background. No usable
detections at any threshold.

**How it was diagnosed** — a two-stage bisection:

1. `check.py` dumps the model's true input contract, then runs one frame through
   five candidate encodings (`A` raw uint8, `B` quantized-from-[-1,1], `C`
   quantized-from-[0,1], `D` quantized-from-[0,255], `E` uint8−128), **plus a
   solid gray-128 control frame**. Its stated decision rule: if one encoding
   scores high on the person frame *and* differs on the gray control, it's
   preprocessing; if all stay flat, the model is bad.
2. Variant **C** won, but only in a permissive runtime that accepted float32. The
   Pi's `tflite_runtime` rejects float32 into a uint8 tensor — so
   `probe_uint8.py` was written to find the uint8 array reproducing C's response.

**Root cause, verified 2026-08-03 [ARTIFACT]:** the input tensor is declared
`uint8` with **quantization scale = 1.0, zero_point = 0** — a no-op
quantization — while the network was not trained on raw uint8 [0,255]. Confirmed
against the model file:

```
shape: [1, 640, 640, 3]   dtype: uint8
quantization (scale, zero_point): (1.0, 0)
```

**It was fixed**, in that commit **[GIT]**:

```python
input_tensor = np.round(canvas.astype(np.float32) / 255.0).astype(np.uint8)
```

i.e. the encoding the trained weights respond to is float [0,1] **rounded to a
0/1 mask** (pixel ≥128 → 1, else 0), handed to the graph as uint8.

**Surviving numbers, from the commit's own comment:**

- fixed encoding: **0.50** on a person frame vs **0.18** on the gray control
- raw uint8: **0.094**, flat on both

So: **diagnosed and worked around, not left broken.** Note the honest framing —
this is a workaround for a bad model export, not a repair of the export. A model
whose real input is a 1-bit mask is a broken artifact.

## C4 — the premise is wrong, in both directions

The question was whether CPU-only was a workaround for the compile penalty
*rather than* a bring-up step. **[GIT]** `815c68e` supports neither reading.

The commit does three things: switches `start.sh` from the `_edgetpu.tflite`
model to the plain `int8.tflite`; makes delegate loading **conditional** on
`"_edgetpu"` being in the filename (rather than removing it); and applies the C3
input-encoding fix.

- **Not a workaround for the compile penalty.** Going CPU-only was *slower* — it
  gave up the delegate's ~30% and moved 665 ms → 962 ms per frame. You cannot
  work around a performance penalty by taking a bigger one.
- **Not a bring-up step before wiring the TPU.** The TPU path was already wired
  and benchmarked **two days earlier** (2026-05-28). This commit moved *away*
  from it.

**What it actually was:** the commit title is *"Establish a working detection
model"*, and its substantive change is the input-encoding fix. The evidence
indicates CPU-only was about getting **correct detections** — the encoding fix
was found and validated against the plain int8 model (`probe_uint8.py` hardcodes
`MODEL = ".../person_int8.tflite"`), and they shipped the configuration that
demonstrably detected, accepting the ~300 ms/frame cost. Keeping the delegate
path filename-conditional rather than deleting it is consistent with that:
neither path was being abandoned, one was being made to work.

That last paragraph is **inference from the diff**, not a recorded statement. No
note or log says *why* CPU-only was chosen. For an airtight report, write only
what the diff shows and leave the motive out.

## C5 — YOLO: never attempted, and not investigated on paper either

**Zero references to YOLO anywhere** — no code, no docs, no notes, no commits, no
transcripts. The whole repo and all surviving sessions were grepped.

Don't write that YOLO was "investigated on paper" — there is no evidence it was
considered at all. The recorded alternative was different: **[NOTE]** recommends,
if the model proved broken, retraining **SSD MobileNet V2 at 320×320** (not
FPNLite), on the reasoning that it fully maps to the EdgeTPU at ~5 ms/frame and
that *"FPNLite 640 was designed for GPU and is fundamentally a bad fit for
Coral."* That retrain was **never carried out** — the platform moved to the Orin
instead. If a "considered alternative" is wanted for the report, that's the real
one.

---

# D. Pipeline facts

## D1 — original split-feed design (picamera2 era)

**[CODE]** `legacy/edgetpu/person_streamer.py:405–430`:

| Branch | Config |
|---|---|
| **main** | `1920×1080`, format `YUV420` → picamera2 `H264Encoder(bitrate=8_000_000)` → `FileOutput` → FIFO `/tmp/camera_stream.h264` → FFmpeg (remux, no re-encode) → mediaMTX → `rtsp://<pi>:8554/stream` |
| **lores** | `1280×720`, format `RGB888` (uint8) → letterbox to 640×640 → EdgeTPU detector |

**Box rescaling:** detection ran on the 720p lores stream, so boxes were scaled
by 1920/1280 = **1.5×** back into 1080p coordinates before going to the app. The
source comment notes that without this every box lands at 0.667× its correct
position — shifted toward the top-left.

**Why picamera2 had to be sole camera owner:** the IMX477 can only be held once.
**[DOC]** `PI_TO_ORIN_LIVERUN.md` states it plainly for the successor design —
*"the full service would grab the camera that the sender also needs (the IMX477
can only be held once)"* — and it is the same constraint that forced the
single-process live-loop harness later. So the process owning the camera had to
produce **both** the app video and the detector frames; picamera2's
`main`+`lores` dual-stream config is precisely what makes one owner serve two
consumers.

**IPC contract to the C server** **[CODE]** — Unix domain socket at
**`/tmp/detection.sock`** (`unix_socket.h:22`, mirrored in `ipc_client.py:35`).
Python connects as client, C server binds. Payload, compact JSON, bbox in
1920×1080 pixels:

```json
{"type":"target_detection","timestamp_ms":123,
 "detections":[{"id":"1","class":"HUMAN","confidence":0.85,
                "bbox":{"x":100,"y":50,"width":200,"height":400}}]}
```

`forward_detection()` in `main.c` relayed this **verbatim** to the app over the
WebSocket — no reformatting.

## D2 — the C/GStreamer rewrite

**[DOC]** `INTEGRATION_PLAN.md` lists the three contracts that had to stay
byte-identical, and what it took:

1. **Video.** Preserved by keeping the *entire downstream half* untouched — same
   FIFO path, same FFmpeg remux, same mediaMTX, same RTSP URL. The change was
   only who writes the FIFO: the new GStreamer `tee` grew a second branch
   `queue ! x264enc ! h264parse ! filesink location=<fifo>` replacing picamera2's
   `H264Encoder`+`FileOutput`. Bitrate did move, 8000 → **4000 kb/s**
   (`streaming_config.json`), but that was a later, deliberate fix for an RTSP
   write-queue overflow, not the port.
2. **Detections.** The Orin already emitted the same schema, so this was
   *re-sourcing, not reformatting* — the plan's own phrase. `build_detection_json`
   in `ddl_bridge` serializes to the identical shape. The one new field,
   `confirmed`, is **additive and optional**.
3. **`sensor_data` + commands.** Untouched — already lived in `ddl_bridge`.

**The one genuinely hard problem was threading:** the Orin handler runs on the
ZMQ receiver thread, while every `ws_send_json` had always been on the main
thread. Rather than make libwebsockets thread-safe, the handler writes into a
mutex-guarded 16-slot ring (`DET_FWD_QUEUE`) that `ddl_bridge_pump_detections()`
drains from the main loop — keeping **all** WS sends single-threaded.

A rendezvous detail also had to be preserved: FFmpeg opens the FIFO for read
*first*, then the sender's `filesink` opens for write, with full-restart retry on
FFmpeg probe failure. And the FIFO must pre-exist at config-probe time, which is
why `start.sh` still does `mkfifo`.

**Did anything break on the app side during the switch?** **No — not from the
port itself.** **[NOTE]** records W1/W2 validated with the app receiving
`stream_ready` + `sensor_data` + `target_detection` byte-compatible with the old
Python output.

What *did* break came later and was **semantic, not structural**: once the Pi
gained a tracker, `id` changed meaning from per-frame index to stable track ID,
and `timestamp_ms` from epoch to Orin-monotonic. The app still ran its TPU-era
tracker/re-ID layer on top, and **[NOTE]** records the first indoor full-system
run showing IDs churning on screen from two trackers disagreeing. That drove
`APP_POST_TPU_BRIEF.md` and the app-side removal of its tracker (2026-07-06,
`DETECTION_CONTRACT_HANDOFF.md`). Also recorded there: the old app had been
sending its **own internal `T1` id** in a `targetId` field on lock — both the
value and the field name wrong — so every indoor lock had been silently falling
through to the Pi's highest-confidence fallback. Honest answer to "did anything
break": **the wire format survived the rewrite intact; the field semantics did
not, and the app kept assuming the old ones.**

## D3 — the GOP/keyframe deadlock

**Dates — and there is a discrepancy that should not be papered over.**

- `HANDOFF_PI_GOP_FIX.md` — dated **2026-07-06**, Orin→Pi. Cites four failed runs
  at **16:59–17:28 Orin local**, and a validation run at **~18:49 Orin clock**.
- `PI_REPLY_GOP_FIX.md` — dated **2026-07-06**, Pi→Orin. States the fix was
  *"deployed in streaming_server, built 2026-07-06 ~17:08."*
- **[GIT]** `5001a34` committed **2026-07-06 17:39:19 +0100**.

The reply claims a **17:08** build responding to a handoff that itself cites an
**18:49** validation. Those cannot both be right on one clock. The two documents
label their times differently ("Orin local" vs unqualified), so the likeliest
explanation is a Pi/Orin clock offset — but that is **unverified**. Write
"2026-07-06" without intra-day times rather than assert a sequence.

**Operational consequence, plain terms.** **Four capture runs were lost**
(Orin-side count, from the handoff). Duration of the outage: an afternoon.

**How it presented before the root cause was known.** It looked like *nothing was
wrong*. From the Pi: the camera ran, the encoder ran, RTP flowed out `eth0` at
normal rates, every frame was SEI-tagged, no errors anywhere. From the Orin:
**zero frames decoded**, and the capture files came out **0 bytes**. The failure
was completely silent on the sending side and total on the receiving side — the
Pi had no local symptom to chase.

The mechanism: the x265 stream carried almost no keyframes (**2 in 565 frames**,
~4 min apart, in the morning capture). Any Orin restart joining mid-stream
received only reference-less P-frames; NVDEC held them waiting for a
random-access point, its input pool exhausted, and backpressure deadlocked the
whole receive pipeline — which also froze the capture tee, hence the 0-byte
files. The runs that *did* work worked only because the Orin happened to be up
before the stream started. So the true variable was **start order**, which nobody
was tracking.

Fixes, both sides: Orin drops AUs until the first keyframe and re-inserts cached
VPS/SPS/PPS; Pi pins `keyint=min-keyint=12, scenecut=0, open-gop=0`. **[DOC]**
records the Pi verified this by NAL-scanning a 60-frame encode: `IDR_N_LP` at
frames 0, 12, 24, 36, 48 — exactly every 12, zero CRA. At ~2.4 fps that's ~5 s
worst-case blind time, and start order stopped mattering.

One thread stayed **unresolved**: the Orin asked why the morning stream had ~4
min between keyframes while the evening had ~12 s. The Pi could not answer — the
binary had been rebuilt at 14:57 and overwritten. All inspectable builds pin
`key-int-max=30`, matching the evening cadence; the morning gaps are consistent
with x265's default `keyint=250` plus scene-cut, i.e. a stale dev build.
**Recorded as unreconstructable.** Don't state a cause.

A second constraint surfaced here worth a line: each AU leaves the payloader as a
single line-rate burst, and the Orin's stock 208 KB UDP receive buffer meant any
AU larger than that lost its tail silently — the ~126 KB IDRs were just under the
limit. Fixed Orin-side (`rmem_max=8388608`), with an explicit ask for **no**
sender-side pacing.

## D4 — the first live run that returned 0 detections

**The fault was on the Pi.** Specifically Pi-side RTP **packetization** — not the
wire, not the Orin.

The bisection is a good narrative because the first hypothesis was wrong.
`PI_TO_ORIN_DIAG.md` asserts *"the Pi side is confirmed healthy; the break is on
the Orin side or the wire past the Pi's NIC"*, backed by a five-row evidence
table (513 frames captured and SEI-tagged, 3.13 MB RTP egressing `eth0`, PULL
socket listening, correct route, no firewall). Every row was individually true,
and the conclusion drawn from them was wrong.

What actually settled it **[DOC]** `PI_TO_ORIN_FIX.md`: the Orin suggested
looping the Pi's own sender into a **local** `rtph265depay` on the Pi. **That
local loopback also produced 0 frames** — proving the bug was Pi-side without
involving the Orin at all. Hex-dumping the Pi's own RTP then showed it exactly:

- **Packet 1, 13 bytes:** 12-byte RTP header + a **1-byte payload `0x4e`** — the
  first byte of the SEI NAL header, stranded alone.
- **Packet 2:** a fragmentation unit (type 49) with **`FuType=55`** — and
  55 = `0x6e >> 1`, where `0x6e` is the `'n'` **inside the UUID string
  "S*n*ipeItFrmID"**. The payloader had lost NAL alignment and was reading UUID
  bytes as NAL headers.

**Root cause:** the frame_id SEI was spliced at bitstream level on `h265parse`'s
**src** pad — *after* parsing — so `h265parse` never re-validated the
hand-inserted NAL and `rtph265pay` mis-parsed the AU's NAL boundaries. The
elementary-stream-to-file path (`pi_sei_sample.hevc`) worked precisely because it
never passes through `rtph265pay`, which is why offline validation had passed
cleanly.

**Fix:** move the splice to `h265parse`'s **sink** pad. One line. **Validated**
by Pi-local loopback: 37 sent → **37 decoded frames** (ffprobe
`nb_read_frames=37`), **37 frame_id SEIs recovered from the received RTP**,
contiguous 1..37; SEI round-trip self-test 59/59.

**Keep this distinct from a second, later Pi-side fault.** On 2026-07-04 an "Orin
unreachable" episode was root-caused **[NOTE]** to a NetworkManager profile
collision: `ethernet-usb0` (USB gadget, `ipv4.method=shared`) defaults to
**10.42.0.1/24**, colliding with the Orin link subnet; its route metric 100 beat
`eth0`'s 101, so all 10.42.0.2 traffic blackholed into a dead `usb0`. Different
run, different fault, also on the Pi.

Transferable lesson stated at the time: **never hand-splice NALs on a parser's
src pad when a payloader is downstream** — splice upstream so the parser
normalizes for it.

## D5 — SEI over RTP header extension: **rejected on reasoning, never prototyped**

There is **no RTP-header-extension implementation** anywhere — grepping
`header.extension|rtp.ext|hdrext|extmap` across `pi-streaming/` returns only two
hits, both prose in `PI_SIDE_BRIEF.md:48` and `sei_frame_id.h:6` describing the
decision. No code, no test, no branch.

The reasoning **[DOC]** `PI_SIDE_BRIEF.md`: SEI *"lives in the elementary stream
and survives RTP (de)packetisation / any RTSP relay, and any HEVC parser can read
it."* Since the transport choice (raw RTP/UDP vs an RTSP server) was still open
at decision time, a tag that survives either was worth more than one bound to RTP
framing.

There was also a **hard toolchain constraint** **[NOTE]**: at the time the
intended encoder was FFmpeg CLI, which can do **neither** method — no per-frame
SEI inject *and* no custom RTP extension. So the choice was never "SEI vs
RTP-ext" as drop-in options; it was "which tagging scheme justifies moving off
the FFmpeg CLI." SEI was made concrete first (`sei_frame_id.{h,c}` + passing unit
tests) explicitly to make the decision tangible, and the RTP-ext alternative was
never built.

Irony worth noting given D4: the argument for SEI was that it survives RTP
packetization — and the one bug that cost a live run was SEI *breaking* RTP
packetization. The reasoning was sound about the format; the failure was in the
splice site.

## D6 — `bframes=0`: **preemptive/structural, not an observed mis-join**

Precisely: no run ever executed with B-frames enabled and produced an observed
mis-join. `bframes=0` is present in the earliest sender config and never changed.

But it isn't *arbitrary* preemption either. There **was** an observed, related
failure **[NOTE]**: the first frame_id↔AU matching scheme keyed on **PTS**, and
`x265enc` does not preserve PTS identically — **0 frames were tagged.** The fix
was to abandon PTS matching for a **FIFO** (the capture probe pushes each
frame_id onto a queue; the SEI probe pops it), which is only correct if encoder
output order is strictly 1:1 with input order — which is exactly what
`bframes=0` guarantees.

Accurate sentence for the report: *`bframes=0` was set from the outset and is a
**precondition** of the FIFO-based frame_id↔AU association that replaced a failed
PTS-keyed scheme; no B-frame-induced mis-join was ever observed, because the
pipeline was never run with B-frames enabled.*

The Pi communicated it to the Orin as **[DOC]** *"B-frames: 0 (strict frame order
for SEI frame_id ↔ pose matching)"* — consistent with a stated design constraint,
not an incident report.

---

# E. Visual evidence

**E1 — photograph of the rig: does not exist on the device.** All of
`/home/snipeit` was searched for images >20 KB; the only hits are VS Code
extension icons. **This must be taken manually.** Suggested single frame: Pi 5 +
Orin, the GigE cable between them, the IMX477 + 16 mm lens on the pan/tilt mount,
and the USB Wi-Fi dongle (it's load-bearing — `wlan1`=STA uplink, internal
Broadcom=AP `SnipeItNet`, and that split is what fixed the app freeze).

**E2 — app screenshot with stable IDs across a pan: does not exist.** Requires
the app running during the staged B1 run, with someone capturing two frames a few
seconds apart showing the same ID on the same person. This has to be captured on
the phone by whoever operates the app — it is not reachable from the Pi. This
screenshot **is** the visual form of the B1 claim, so it should come from the
same run.

**E3 — terminal capture of a full live loop with counters: available from the
staged run,** not before. The counters print in two places:

- The `[BRIDGE]` stats line every 5 s:
  `cap=<fps> orin=<msg/s> miss=<n> slew_skip=<n> aim=<n>`
- The shutdown summary from the sender/receiver:
  `[SENDER] stopped (captured=… )`,
  `[ORIN-RX] Stopped (received=… parse_fail=… pose_miss=…)`

The best surviving example is the **failed** first live run **[DOC]**, useful as
a "diagnostic evidence" figure even though it shows a break:

```
[SENDER] stopped (captured=513 sei_probe_calls=513, 0 ids undrained)
[ORIN-RX] Stopped (received=0 parse_fail=0 pose_miss=0)
[LOOP] done: captured=513  zmq_recv=0 parse_fail=0  detections=0  pose_join hits=0 misses=0
```

And the best surviving **green** loop **[NOTE]**, 2026-07-04, `orin_live_loop`,
60 s: `captured=129, zmq_recv=95, parse_fail=0, detections=101` (HUMAN, conf
0.5–0.9), `pose_join hits=94 miss=1`, ~2.2 fps. That is a real end-to-end
proof — but it is the **standalone harness with placeholder poses (90°,90°)**,
not the full service, so it cannot support any claim about servo poses or
tracking. The raw terminal output for this one is gone; only the summary
survives.

For E3 proper, capture with
`./start.sh 2>&1 | tee ~/run_$(date +%F_%H%M).log`.

---

# Two framing corrections before writing

1. **"Multi-subgraph compile penalty" (C2)** — the artifact shows **one** TPU
   segment plus a 44-op CPU tail, not many subgraphs. The corrected claim is more
   specific and is verifiable today.
2. **"What confirmed it worked" (B2)** — nothing instrumented did. The fix rests
   on a verbal "everything was run successfully" with no captured output, and the
   contemporaneous note lists that validation as still pending. The A4/B1 run
   would be the first actual confirmation, which is a good reason to prioritize
   it.

Also unresolved: the Orin flagged that ~2.4 fps ≈ 420 ms/frame bounds reaction
latency against a **≤150 ms average latency requirement**. **[DOC]** records this
as escalated and **deliberately not changed** — a system-level trade
(resolution / preset / core allocation). If that requirement is in the spec, this
is the sharpest limitation in the chapter, and A1+A3 are exactly the numbers that
quantify it.

---

# Open questions blocking the Section A / B1 / E runs

1. Orin detector up and app connected on a phone?
2. Outdoor daylight confirmed?
3. Include the 4-core reproduction in A2, or take the recorded observation only?
4. Add the A3/A4 instrumentation (code change)?
5. For A1/A5 — delivered-rate number only, or delivered + encoder-ceiling as two
   separately labelled figures?
