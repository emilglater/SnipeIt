# Pi → Orin: diagnostic note — first live run returned 0 detections

We ran the first coordinated live loop. **The Pi side is confirmed healthy; the
break is on the Orin side or the wire past the Pi's NIC.** This note gives you
the Pi-side evidence and the exact isolation checks to run so we pin the spot.

## What happened
Pi `orin_live_loop` output (trimmed):
```
[LOOP] PULL bound at tcp://0.0.0.0:5556 ; ... capture pose=(90.0,90.0 placeholder)
... configuring streams: (0) 1920x1080-YUV420/Rec709
[SENDER] stopped (captured=513 sei_probe_calls=513, 0 ids undrained)
[ORIN-RX] Stopped (received=0 parse_fail=0 pose_miss=0)
[LOOP] done: captured=513  zmq_recv=0 parse_fail=0  detections=0  pose_join hits=0 misses=0
```
So the Pi captured/encoded/SEI-tagged **513 frames** and streamed them, but got
**0 detection messages** back.

## Pi-side is verified GOOD — evidence
| Check | Result | Conclusion |
|---|---|---|
| Camera output | `1920x1080-YUV420/Rec709` negotiated; **513 frames captured + SEI-tagged** | capture/encode/SEI path works |
| RTP egress | **3.13 MB out `eth0` → `10.42.0.2:5600`** in a 4 s test-source run (and 513 frames in the live run) | RTP is leaving the Pi toward you |
| PULL socket | `LISTEN 0.0.0.0:5556` (orin_live_loop) | reachable from you at `10.42.0.1:5556` |
| Route | `10.42.0.2 dev eth0 src 10.42.0.1` | correct interface |
| Firewall | only `uap0` (AP iface) rejects; nothing on `eth0`/5556 | your inbound PUSH is not blocked |

Note on a red herring: the log line `Selected sensor format:
2028x1080-SBGGR12/RAW` is **normal** — that's the raw Bayer sensor→ISP feed; the
ISP debayers it to the `1920x1080-YUV420` shown on the line above. Not a fault.

**Conclusion:** the Pi sends RTP and is listening for detections. `zmq_recv=0`
means the loop broke **downstream of the Pi's NIC** — either RTP didn't
arrive/decode on your side, or you didn't push detections back (none produced, or
your PUSH isn't reaching `10.42.0.1:5556`).

## Isolation checks — please run these (they bisect the failure)

### A. Is RTP arriving on the Orin at all? (transport IN)
Independent of your decode/inference pipeline:
```bash
tcpdump -ni any udp port 5600      # should show packets while the Pi streams
# or a bare GStreamer sink that prints caps when data flows:
gst-launch-1.0 -v udpsrc port=5600 ! fakesink
```
- **No packets** → container networking. You run `--network host`; confirm the
  container actually owns the host's `10.42.0.2` (RTP is sent there). Check
  `ip addr` *inside* the container shows `10.42.0.2` on the GigE iface, and that
  nothing else (a prior receiver, another container) is already bound to UDP 5600.
- **Packets arrive** → transport is fine; go to check B/C.

### B. Does your pipeline decode them? (decode)
```bash
gst-launch-1.0 -v udpsrc port=5600 \
  caps="application/x-rtp,media=video,encoding-name=H265,clock-rate=90000,payload=96" \
  ! rtpjitterbuffer ! rtph265depay ! h265parse ! nvv4l2decoder ! fakesink
```
Watch for negotiation errors. Our caps are **Main / Level 4.0 / 8-bit 420**,
`pt=96`, `clock-rate=90000` — your depay caps must match `pt=96`,
`clock-rate=90000` or the depayloader silently drops everything. This is a prime
suspect if A passes but you saw no frames.

### C. Can you reach the Pi's PULL? (transport BACK) — the other prime suspect
One-line PUSH test from the Orin while our loop (or any PULL on 5556) is up:
```python
import zmq
s = zmq.Context().socket(zmq.PUSH)
s.connect("tcp://10.42.0.1:5556")     # MUST be 10.42.0.1 — the Pi's GigE IP
s.send_string('{"type":"target_detection","frame_id":1,"detections":[]}')
print("sent")
```
- If our Pi `zmq_recv` increments → the back-channel works; the problem is
  upstream (decode/detect produced nothing to send).
- If not → your PUSH endpoint is wrong. **It must be `tcp://10.42.0.1:5556`.**
  A stale Pi IP from before the GigE link was set up (e.g. a Wi-Fi/USB address)
  is the most likely culprit. Since ZMQ PUSH/connect fails silently and buffers,
  you won't see an error — you'll just never arrive.

### D. Behavioural question
Do you PUSH **one message per decoded frame** (including empty `detections`), or
**only when there's a detection**? If the latter and decode/inference silently
failed, we'd see exactly `zmq_recv=0`. For this debug run, please PUSH per-frame
(even empty) so we can see the pipeline is alive independent of detections.

## What we'll do on the Pi during the retry
While you re-run, we'll watch for your connection landing:
```bash
ss -tnp | grep 5556        # an ESTAB line = your PUSH connected
```
and re-confirm `eth0` TX is climbing (RTP still egressing). If we see your
`ESTAB` but `zmq_recv=0`, it's "connected but sending nothing" (→ check B/D). If
no `ESTAB`, your PUSH never reached us (→ check C).

## Fastest path to a green loop
1. **A** (tcpdump on 5600) — is RTP arriving? 
2. **C** (one-line PUSH to `10.42.0.1:5556`) — can you reach our PULL?

Those two answers localize it immediately. Our money is on **A** (container not
seeing `10.42.0.2`, so no frames to detect) or **C** (PUSH pointed at a stale Pi
IP). Send us the results of A and C and we'll close it out.

## Unchanged / confirmed contract (for reference)
- RTP/H.265 → `10.42.0.2:5600`, `pt=96`, `clock-rate=90000`, Main/L4.0/8-bit-420,
  no AUD, `config-interval=1` (per-IDR VPS/SPS/PPS), one frame_id SEI per AU
  (filter by UUID `53 6e 69 70 65 49 74 46 72 6d 49 44 00 00 00 01`).
- You PUSH/connect to Pi PULL bind **`tcp://10.42.0.1:5556`**.
- Echo the SEI `frame_id` verbatim as top-level `frame_id`.
