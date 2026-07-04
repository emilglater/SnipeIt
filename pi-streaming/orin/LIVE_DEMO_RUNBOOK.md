# Live demo runbook — Pi ↔ Orin end-to-end (W5)

Goal of the demo: the app shows the live camera with **stable-id detection
boxes**, and on **lock** the mount **slews to follow** a person as they move.

Roles: **Pi operator** (starts the Pi, watches logs), **Orin operator** (starts
the Orin pipeline), **subject** (person in frame). Daylight only.

---

## 0. Pre-flight (2 min)

**Pi:**
```bash
cd ~/SnipeIt/pi-streaming
# a) GigE link to the Orin is up
ip -brief addr show eth0            # expect: UP  10.42.0.1/24
ping -c2 10.42.0.2                  # expect: 0% loss   (Orin reachable)
# b) camera present
rpicam-hello --list-cameras | head  # expect: imx477 listed
# c) build is current
make streaming_server              # expect: Build complete (no errors)
# d) nothing stale holding ports/camera
pkill -f streaming_server; pkill -f mediamtx; pkill -f 'ffmpeg.*camera_stream'
```
If `eth0` has no IP: `sudo nmcli connection up orin-link` then re-ping.

**Viewer device (app phone/tablet):** ONE app instance at a time — the WS server
(port 8555) evicts the previous client on a new connect, so two apps fight; a
second viewer must use VLC (RTSP only, `rtsp://10.42.1.1:8554/stream`). On the
viewer: mobile data OFF (or "switch to mobile data" disabled) and Samsung
"Intelligent Wi-Fi → Switch to better Wi-Fi networks" OFF — otherwise any
*uplink* hiccup (teammate hotspot / college Wi-Fi drop) makes Android abandon
the no-internet hotspot and the app shows "RTSP offline" even though the Pi
link is perfect. Do NOT use the Galaxy Tab S8: its radio measured 6-8% frame
loss to every AP config we tried (device fault) — use a phone.

**Wi-Fi architecture (two radios):** internal Broadcom = dedicated AP
`SnipeItNet` (uap0, 10.42.1.1, ch36/5GHz); TP-Link Archer T2U Plus dongle
(wlan1, 8821au DKMS driver) = uplink STA for SSH (GILAD at home / phone
hotspot in the field — all client profiles are pinned to wlan1; `ssh
snipeit@rpi5.local` finds it on any network). Dongle is capped to 20 MHz
(`rtw_bw_mode=0x00`) so its ch48 uplink can't overlap the AP's ch36. If Wi-Fi
vanishes after a kernel upgrade, rebuild the driver:
`cd ~/8821au-20210708 && git pull && sudo ./install-driver.sh`.

**Environment:** good daylight on the scene. In low light the IMX477 auto-exposure
drops the frame rate hard (~0.5 fps) and FFmpeg's probe can fail — this is the #1
cause of "black screen / no video". The system is daylight-only by design.

**Orin:** container up with `--network host`; receiver ready to bind
`udpsrc port=5600` and PUSH to `tcp://10.42.0.1:5556`. (Optional transport smoke
test below.)

---

## 1. Optional transport smoke test (no camera, no person) — 1 min

Confirms Pi→Orin video + Orin→Pi detections round-trip *before* trusting the
camera/lighting. Uses the videotest source (color bars → the Orin finds no
people → empty detections, but the loop proves out).
```bash
# Pi:
mkfifo /tmp/camera_stream.h264 2>/dev/null
./streaming_server streaming_config_test.json      # camera_source=test
```
Bring up the Orin receiver. On the Pi, in another shell:
```bash
watch -n1 'ss -tnp | grep 5556'    # expect an ESTAB line = Orin PUSH connected
cat /sys/class/net/eth0/statistics/tx_bytes   # should climb (RTP going out)
```
Server log should show `Frame sender up ...`, `publishing to mediaMTX`,
`System ready`. Stop with Ctrl+C. If this works, transport is good.

---

## 2. Start the real demo

**Pi (foreground; Ctrl+C stops everything):**
```bash
cd ~/SnipeIt/pi-streaming
./start.sh
```
Wait for these lines:
- `Frame sender up: camera -> H.264 FIFO + H.265+SEI RTP 10.42.0.2:5600`
- `Live camera pipeline is publishing to mediaMTX`
- `System ready!`

**Orin:** start the receiver + inference + PUSH now.

**App:** connect to `ws://<pi-ip>:8555`; video plays from
`rtsp://<pi-ip>:8554/stream`. (Sensor telemetry should also start updating.)

---

## 3. Run the demo

1. **Subject steps into frame.** Within ~1–2 frames the app draws a box on them
   with a **stable id** that persists as they move (not a flickering per-frame
   number). Class shows `HUMAN`, with a confidence.
2. **Lock:** operator selects that target in the app → app sends `select_target`
   `action:"lock"` (optionally the target's stable `id`). The mount **slews to
   point at the subject** and keeps following as they walk left/right/toward/away.
3. **Unlock:** operator sends `select_target action:"unlock"` → the mount stops
   following (returns to scan).
4. **Manual aim** (any time): app `set_servo_angles {horizontal_deg, vertical_deg}`
   moves the mount directly and cancels any lock.

### Success criteria
- ✅ App shows live video + a `HUMAN` box tracking the person with a **stable id**.
- ✅ On lock, the mount turns **toward** the subject (correct direction — signs
  were rig-verified) and follows their motion.
- ✅ Distance estimate is sane (roughly right order of magnitude).
- ✅ Unlock returns to scan; sensor telemetry keeps flowing throughout.

---

## 4. Pi-side observation (while it runs)

The app is the main view. From the Pi you can cross-check headlessly:
```bash
# RTP is leaving toward the Orin (counter climbs):
watch -n1 'cat /sys/class/net/eth0/statistics/tx_bytes'
# Orin's detection PUSH is connected:
ss -tnp | grep 5556                 # an ESTAB line
# Server + ffmpeg logs (start.sh is foreground; or):
tail -f /tmp/ffmpeg.log             # "frame= ... fps=" climbing = video flowing
```
(The headless WS monitors used during bring-up — servo angle + detection ids
without the app — live in the session scratchpad; ask and I'll regenerate
`ws_track_check.py` / `ws_lock_watch.py` if you want them at the rig.)

---

## 5. Troubleshooting (mapped to known failure points)

| Symptom | Likely cause → fix |
|---|---|
| **No video / black screen** in app | Check `/tmp/ffmpeg.log` shows `Stream mapping:` and `frame= fps=` climbing. The server now verifies real publishing (log says "publishing to mediaMTX" only when true) and retries 4×; if it reports "camera starved", check the **lens cap** and light. Low light no longer stalls the stream (framerate is pinned at the source) but the image will be dark/noisy — detection still needs daylight. |
| Video plays then **freezes after ~20 s** (app) | The phone's RTSP session can't drain the stream — `/tmp/mediamtx.log` shows `write queue is full`. Fixed levers: `app_preview_bitrate_kbps` in streaming_config.json (now 4000) and `writeQueueSize` in mediamtx.yml (now 2048). If it recurs: check phone Wi-Fi signal, then lower bitrate further or drop `app_preview_fps`. |
| Video OK, **no detection boxes** | FIRST: `ping 10.42.0.2` — if it fails the Orin is down/unreachable (Pi eth0 link + ARP can look fine while the Orin is off). Else RTP not decoding on the Orin, OR Orin not PUSHing. Check `ss -tnp \| grep 5556` for the Orin's ESTAB; on the Orin, `tcpdump -ni any udp port 5600` for arriving RTP and confirm depay caps `pt=96, clock-rate=90000`. (This was run #1's packetization bug — now fixed; if it recurs, capture the RTP.) |
| Boxes, but **id flickers** each frame | Pose joins missing (per-frame ids, no `confirmed`) → tracker isn't running. Check the Orin echoes the **SEI frame_id verbatim**; check fps isn't so low that frames age out of the 16-deep pose ring. |
| Lock does nothing | A brand-new target takes **2 frames to confirm** before it's lockable. Also lock needs pose hits (skip-on-miss) — see id-flicker row. Confirm the app's lock carries the **stable id** it's displaying. |
| Mount turns the **wrong way** | Signs are rig-verified (`pan_sign=-1, tilt_sign=-1` in `aiming.c`); if the rig changed, flip the offending sign and rebuild. |
| Tracking **lags on fast motion** | Expected edge case — enable the documented constant-velocity predictor in `tracker.c` (extension point), or widen `GATE_*`. Measure first. |
| Distance way off | Re-run `orin/probe_camera_fov.py` and update `AIM_DEFAULT_*` (FOV depends on lens/sensor mode). |

---

## 6. Shutdown
Ctrl+C in the `start.sh` terminal — `streaming_server` tears down the sender,
FFmpeg, and mediaMTX; the trap removes the FIFO. Stop the Orin container.
