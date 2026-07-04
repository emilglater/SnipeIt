# Integration plan — retire EdgeTPU, wire the Orin pipeline into streaming_server

Goal: replace the Python EdgeTPU streaming+detection module with the C/GStreamer
Orin-based module, **preserving every app-facing contract** (RTSP video,
`target_detection` JSON, `sensor_data`, command handling) and enabling **live
servo lock-on**. Archive the EdgeTPU code as a fallback.

## Contracts that must stay byte-identical (verified in code)
1. **Video:** `rtsp://<pi>:8554/stream`, from raw H.264 in the FIFO
   `/tmp/camera_stream.h264` → FFmpeg (remux, no re-encode) → mediaMTX. Currently
   1080p, 8 Mbps.
2. **Detections → app:** forwarded verbatim over the WebSocket as
   `{"type":"target_detection","timestamp_ms":<int>,"detections":[{"id","class",
   "confidence","bbox":{x,y,width,height}}]}` (compact, bbox in 1920×1080). The
   Orin already emits this schema.
3. **`sensor_data` + commands** (`set_servo_angles`, `select_target` lock/unlock):
   unchanged — handled entirely in `ddl_bridge`, untouched by this work.

## Current vs target

**Current**
```
person_streamer.py (owns camera)
   main 1080p YUV420 -> H264(8Mbps) -> FIFO -> FFmpeg -> mediaMTX -> app
   lores 720p RGB    -> EdgeTPU     -> unix socket -> main.c forward_detection -> ws -> app
main.c: mediaMTX + FFmpeg + waits for Python IPC + forwards Python detections
ddl_bridge: sensors + servo + Orin receiver (drives servos on lock);
            record_capture_pose real but never called; NO detection->app forward
```

**Target**
```
C GStreamer sender (owns camera)   [extends orin/frame_sender.c]
   libcamerasrc 1080p ! tee
       |- 1080p ! x265 + SEI ! RTP  -> Orin
       '- 1080p ! x264       ! FIFO -> FFmpeg -> mediaMTX -> app
   on_frame_captured -> ddl_bridge_record_capture_pose(frame_id)   [REAL pose]
Orin receiver (ddl_bridge): parse ->
   (a) drive servos on lock  [exists]
   (b) forward target_detection JSON -> app   [NEW]
main.c: mediaMTX + FFmpeg + ws + ddl_bridge + acoustic + start C sender
        (no Python IPC, no Coral)
EdgeTPU code archived -> fallback branch + legacy/ dir
```

## Work items (with files)

### W0 — FOV calibration (do first; accuracy for servos)
Run `orin/probe_camera_fov.py` on the live camera, replace the provisional
`AIM_DEFAULT_HFOV/VFOV` (22.2/12.6) in `orin/aiming.c` with measured values;
verify `pan_sign/tilt_sign` against the rig. *Files:* `aiming.c`. Quick, no
hardware moves except a sign check.

### W1 — Sender becomes the camera owner + H.264 app branch
- Extend `frame_sender.{c,h}`: add a `tee` after the source; second branch
  `queue ! x264enc ! h264parse ! filesink location=<fifo>` (config: fifo path,
  H.264 bitrate, enable-app-branch flag). Keep the existing source-aware
  libcamera head and the H.265+SEI→RTP branch unchanged.
- `main.c`: call `gst_init()`; after mediaMTX+FFmpeg are up, start the sender
  (RTP → Orin, H.264 → FIFO); reuse the FFmpeg-publishing verify/retry logic.
- Wire `on_frame_captured -> ddl_bridge_record_capture_pose`.
- *Files:* `frame_sender.{c,h}`, `main.c`, `config.{c,h}` + `streaming_config.json`
  (add `orin_host`, `orin_rtp_port`; `video_path` already the FIFO),
  `Makefile` (add `orin/frame_sender.c` to `SRCS`, GST cflags/libs to the
  `streaming_server` build).

### W2 — Forward Orin detections to the app (NEW)
- In `ddl_bridge`, serialize `OrinDetectionMsg` → the exact `target_detection`
  schema and send to the app. **Threading:** the Orin handler runs on the
  receiver thread; today all `ws_send_json` calls are on the main thread.
  Plan: the handler writes the latest detection JSON into a small mutex-guarded
  slot/queue in the bridge; a new `ddl_bridge_pump_detections()` called from the
  main loop drains it and `ws_send_json`s — keeps **all** WS sends on the main
  thread (no need to make libwebsockets thread-safe).
- *Files:* `ddl_bridge.{c,h}` (+ a `build_detection_json` helper), `main.c`
  (call the pump in the loop).

### W3 — Enable + verify live servo lock-on
Path already exists (`aim_compute -> ddl_servo_set_target` on lock). With real
poses (W1) and measured FOV (W0), verify on the rig: lock/unlock from the app,
correct slew direction (signs), limits. *Files:* `aiming.c` (signs if needed).
Hardware-in-the-loop.

### W4 — Retire + archive EdgeTPU
- Archive `person_streamer.py`, `ipc_client.py`, `models/`, the EdgeTPU detector
  + helper scripts to **(a)** a new git branch `edgetpu-fallback` and **(b)** a
  `pi-streaming/legacy/edgetpu/` dir on the working branch.
- `main.c`: remove the Python-IPC ingestion (the wait-for-Python `ipc_accept`,
  the detection drain loop + `forward_detection`-from-Python). **Keep** mediaMTX,
  FFmpeg, the FIFO, ws, ddl_bridge, acoustic.
- `start.sh`: drop the Coral check, venv, and `person_streamer.py` launch; the C
  server now owns the camera. Keep FIFO creation + cleanup.
- *Files:* `main.c`, `start.sh`, `unix_socket.*` (usage), file moves.

### W5 — End-to-end demo validation
Camera → app shows live 1080p + detection boxes; person walks, mount tracks via
live servos; sensors/commands unaffected.

## STATUS (2026-07-01)
- **W0 FOV** — DONE (21.07/11.95 in aiming.c).
- **W1 camera owner + app video** — DONE & validated (ffmpeg publishing 1080p to
  mediaMTX + RTP to Orin).
- **W2 Orin→app detections** — DONE & validated (exact schema over WS).
- **W3 servo signs** — DONE on rig (pan_sign flipped to -1, tilt -1). + skip-on-miss
  added (no servo update on a pose-ring miss).
- **W4 retire/archive EdgeTPU** — DONE & validated. Files moved to
  legacy/edgetpu/ + branch edgetpu-fallback; main.c Python-IPC path stripped;
  start.sh rewritten (no Coral/venv/person_streamer). All unstaged, no commit.
- **W5 live demo** — pending (camera+Orin+person, daylight; needs the rig).
- **TRACKER** — DONE & validated. orin/tracker.{h,c} (greedy NN in
  motion-compensated angular space), test_tracker.c 4/4 pass, wired into
  ddl_bridge (stable ids + optional "confirmed" field in the app JSON;
  lock-by-stable-id, lock-only-confirmed). Design: orin/TRACKER_DESIGN.md.

## Implementation order
W0 (FOV) → W1 (camera owner + app video parity, EdgeTPU still present) →
W2 (app sees boxes from Orin) → W3 (servo lock-on on rig) →
W4 (retire/archive + startup cleanup) → W5 (demo).
Rationale: get app **video** working from the C sender before removing Python, so
each step is independently verifiable and reversible.

## Risks / decisions (RESOLVED)
- **R1 — dual software encode.** Measure 1080p+1080p FIRST; do **not**
  pre-emptively degrade. Priority under CPU contention:
  1. **Orin stream stays 1080p** (resolution is the binding constraint for
     detection range — never drop it).
  2. Orin stream keeps the best stable fps it can (no fps floor — each frame is
     independent + SEI-tagged).
  3. App preview degrades only if needed, cheapest lever first:
     **(a) faster x264 preset on the app branch** (ultrafast/superfast — the
     preview doesn't need compression efficiency) → **(b) lower app fps**
     (less operator-visible than a res drop for a moving person) →
     **(c) 720p app preview** as last resort. bbox coords stay 1920×1080 so the
     app overlay is unaffected regardless.
- **R2 — detection-forward threading:** drain on the main loop (handler stores
  latest JSON under a mutex; main loop `ws_send_json`s). No app/contract impact.
- **R3 — archive location:** BOTH — git branch `edgetpu-fallback` (full
  snapshot) + `pi-streaming/legacy/edgetpu/` in-tree copy.
- **R4 — `timestamp_ms` in forwarded detections:** pass the Orin's value
  through (app treats it as informational).
