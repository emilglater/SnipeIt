# Pi↔Orin Protocol — Pi-side implementation brief (for the Orin-side developer)

This describes the **Pi side** of the Pi↔Orin detection-offload system as it is
actually implemented today, the **wire contracts you (Orin) must match**, and a
set of **open questions** we need you to answer. Code lives in
`pi-streaming/orin/` on the Pi repo.

---

## 1. Topology / role split (shared model)

- Direct **Gigabit Ethernet**, Pi↔Orin, no switch, static IPs. Bandwidth is
  effectively free on this link.
- **Pi owns**: the camera (IMX477 + 16 mm lens), servos (pan/tilt), sensors,
  the app link, and all geometry/lock-on. The Pi is the hub.
- **Orin is a stateless detector**: frames in, detections out. It does **not**
  talk to the app, and it does **not** deal with pose, distance, or aiming.

Three data flows:
1. **Frames Pi → Orin**: H.265 over RTP/RTSP. (Pi encodes, Orin decodes.)
2. **Detections Orin → Pi**: ZeroMQ, small JSON messages.
3. App link Pi ↔ app: existing, unchanged, not your concern.

The Pi tags every outgoing frame with a `frame_id` and records the servo pose at
capture time in a ring buffer keyed by `frame_id`. When your detections come
back referencing that `frame_id`, the Pi rejoins them to the capture-time pose
locally and drives the servos. **So all you echo back is the `frame_id` — never
pose, distance, or angles.**

---

## 2. CONTRACT A — Frame stream (Pi → Orin), H.265 + frame_id in SEI

### Codec / encoder
- **H.265 (HEVC)**, one **1080p** stream. The Pi is a **Raspberry Pi 5**, which
  has **no hardware video encoder**, so we encode in **software (x265)** via
  GStreamer (`x265enc`). Expect:
  - `tune=zerolatency`, `bframes=0` (no B-frames), short GOP
    (`key-int-max≈30`, ~1 s at 30 fps).
  - SPS/PPS/VPS repeated before each keyframe (`rtph265pay config-interval=1`)
    so you can start decoding mid-stream.
  - High bitrate (default 20 Mb/s) — we deliberately spend bits on small-target
    detail; bandwidth is free on the GigE link.
- The Pi does **not** resize for you. You receive 1080p and do your own
  NVDEC decode + GPU resize to your detector input (960×960 now, maybe
  1280×1280 later — that's entirely your side).

### frame_id tagging — DECIDED: **H.265 SEI** (not RTP header extension)
We chose SEI because it lives in the elementary stream and survives RTP
(de)packetisation / any RTSP relay, and any HEVC parser can read it. We have
this **built and tested** on the Pi. You must parse it exactly:

- It is an **Annex-B prefix-SEI NAL**, `nal_unit_type = 39`.
- `payloadType = 5` (`user_data_unregistered`).
- Payload = **16-byte UUID** followed by the **frame_id as 4 bytes big-endian**.
- The UUID (both sides must use this exact value):
  ```
  53 6e 69 70 65 49 74 46 72 6d 49 44 00 00 00 01
  ```
- Standard **emulation-prevention** bytes are applied; de-EPB before reading.
- The SEI NAL is spliced **before the first VCL NAL** of each access unit.

Reference implementation (encode + parse, with unit tests) is in
`orin/sei_frame_id.{h,c}` / `orin/test_sei_frame_id.c`. Your decoder side needs
the matching parse: scan NALs, find type 39, de-EPB, match the UUID, read the
4-byte BE id. Every access unit carries exactly one such SEI.

### Transport — OPEN (see questions)
We can send **RTP/H.265 over UDP** straight to your IP:port (recommended for a
direct link — lowest latency), or publish via **RTSP** that you pull. Tell us
which and the address/port.

---

## 3. CONTRACT B — Detection stream (Orin → Pi), ZeroMQ + JSON

### Transport / sockets — DECIDED
- **ZeroMQ PUSH/PULL.** The **Pi is PULL and binds**; the **Orin is PUSH and
  connects**. (So you can restart freely without the Pi reconfiguring.)
- Pi bind endpoint default: **`tcp://0.0.0.0:5556`** (you connect to
  `tcp://<pi-ip>:5556`). Tell us if you want a different port.
- One ZeroMQ message = one detection message (the JSON below). We can handle a
  trailing newline but it's not required.

### Message schema — DECIDED: **JSON** (matches the app's existing format + a `frame_id`)
```json
{
  "type": "target_detection",
  "frame_id": 12345,
  "timestamp_ms": 678,
  "detections": [
    {
      "id": "1",
      "class": "HUMAN",
      "confidence": 0.85,
      "bbox": { "x": 100, "y": 50, "width": 200, "height": 400 }
    }
  ]
}
```
- **`frame_id`** (uint32): echo back the exact id you read from the SEI of the
  frame these detections came from. This is the join key — without it we can't
  rejoin to capture-time pose (we then fall back to current pose, less
  accurate).
- `detections[]`: may be empty. The Pi caps at 32 per message.
- **`bbox`**: pixel `x, y, width, height`. **Coordinate space is an open
  question — see Q2.** Our geometry currently assumes the **full 1920×1080
  source frame** (top-left origin, +x right, +y down).
- `id` (string): target id. **Semantics are an open question — see Q3.**
- `class`: string label, e.g. `"HUMAN"`, `"DRONE"`.

Reference parser (with tests) is in `orin/detection_msg.{h,c}`. Anything beyond
these fields is ignored, so you can add fields without breaking us.

### What the Pi does with it
The Pi joins `frame_id → capture pose`, then computes the angular offset from
`bbox + FOV + pose` and slews the servos. **Distance and angle are computed on
the Pi** from bbox + FOV + known target height — you don't send them.

---

## 4. What is built & tested on the Pi side (status)

All in `pi-streaming/orin/`, all with passing unit/integration tests:
- `pose_ring` — frame_id→pose ring buffer (thread-safe).
- `detection_msg` — JSON detection parser (the schema above).
- `orin_receiver` — ZeroMQ PULL receiver; integration test does a real
  PUSH→PULL round-trip and joins to pose.
- `aiming` — bbox + pose + FOV → servo pan/tilt command + range estimate.
- `sei_frame_id` — the H.265 SEI carrier above.
- `frame_sender` (GStreamer) — the H.265 sender; **validated live on the Pi**
  (videotestsrc → x265enc → SEI splice → sink): 68/68 frame_ids round-tripped
  through the encoded stream in order, zero mismatches.
- Lock-on is wired into the live app: app sends "lock" (optionally with a
  target id) → each matching detection drives the servos to follow.

Still to do on the Pi (not blocking you): wire the sender into the main service,
use the real camera (was offline during dev), and plug in the transport
endpoint once you answer Q1. Also: Pi-5 **software** 1080p30 HEVC is demanding
(see Q5).

---

## 5. OPEN QUESTIONS for the Orin side (please answer)

**Q1 — Frame transport.** Do you want **RTP/H.265 over UDP** (give us the
**Orin IP + UDP port** you'll listen on — our recommendation for the direct
link), or should the Pi run an **RTSP** server you pull from (give the URL form
you prefer)? Either way the codec/SEI above is unchanged.

**Q2 — bbox coordinate space.** Please return bbox in **full 1920×1080
source-frame pixels** (i.e. map your 960×960 detector boxes back to the original
1080p frame, undoing any letterbox/resize) so our FOV→angle math is correct. Can
you confirm you'll do that? If you'd rather send normalised `[0,1]` coords or
detector-input (960×960) coords, tell us and we'll adapt the geometry — but we
need to **agree on one** explicitly.

**Q3 — target id semantics.** Is `detections[].id` a **stable track id across
frames** (i.e. you run tracking, and id "3" is the same physical target over
time), or just a per-frame index? Our operator "lock onto target X" follows a
specific id across frames, so stable track ids would make lock-by-id work; if
ids aren't stable we fall back to "follow highest-confidence detection."

**Q4 — class labels.** What's the exact set of `class` strings you'll emit
(e.g. `"HUMAN"`, `"DRONE"`, …)? We mostly pass them through, but want to match
them in the app/UI.

**Q5 — framerate expectations.** Pi-5 software HEVC at 1080p30 may not hold a
full 30 fps. Are you fine with a variable/possibly-lower framerate (each frame
still SEI-tagged), or do you need a guaranteed rate? If the latter, we may drop
to a lower capture resolution or a faster x265 preset — your detection-quality
needs should drive that tradeoff.

**Q6 — SEI parsing confirmation.** Can you confirm your decode path can extract
the prefix-SEI (type 39, user_data_unregistered, the UUID above, 4-byte BE id)
per access unit and associate it with the decoded frame? If your decoder strips
SEI before you can read it, we need to know — that would change the tagging
approach.

**Q7 — anything you need from the frame stream** that we're not providing
(e.g. a specific GOP, AUD NALs, a particular pixel format/profile/level for
NVDEC, IDR-on-request)? Now's the time to ask.

---

### TL;DR of decisions already locked
- frame_id carrier = **H.265 SEI** (UUID + 4-byte BE id, before first VCL NAL).
- Detections = **ZeroMQ**, **Pi PULL/binds `:5556`**, **Orin PUSH/connects**,
  **JSON** with top-level `frame_id` echoed from the SEI.
- Orin returns only **bbox + class + confidence + id + frame_id**. No pose, no
  distance, no angles — those are Pi-side.
