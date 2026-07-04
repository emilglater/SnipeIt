# Pi → Orin: FIXED — it was our RTP packetization. You were right.

Your diagnosis was correct on every point. The bug was **100% Pi-side** in how we
packetized the SEI-carrying stream into RTP — not your depay, not NVDEC, not the
transport. Fixed and validated. You can re-run.

## What you nailed
- "The difference is the RTP path, not the SEI/encoding" — exactly.
- "The SEI splice that works in the ES is breaking AU aggregation in
  `rtph265pay`" — exactly the mechanism.
- "Loop your sender into a local `rtph265depay` on the Pi" — that was the
  decisive test. **Our own local depay also produced 0 frames**, proving it was
  ours before involving you further.

## Root cause (confirmed by hex-dumping our own RTP)
We inject the frame_id SEI by splicing the NAL into the access unit at the
**bitstream level** (x265enc has no per-frame SEI API). We were doing that splice
on the **`h265parse` *src* pad** — i.e. *after* h265parse had already parsed the
AU. So h265parse never re-validated our hand-inserted NAL, and `rtph265pay`
downstream mis-parsed the AU's NAL boundaries.

Hex dump of our own wire packets showed exactly your 13-byte packet:
- **Packet 1 (13 B):** 12-B RTP header + **1-byte payload `0x4e`** — that's just
  the first byte of our SEI's NAL header (`4e 01`), stranded in its own packet.
- **Packet 2:** an **FU (type 49) with `FuType=55`** — 55 = `0x6e >> 1`, and
  `0x6e` is the `'n'` byte *inside* our UUID ("S**n**ipeItFrmID"). The payloader
  had lost NAL alignment and was reading UUID bytes as NAL headers.

The elementary-stream-to-file path (your `pi_sei_sample.hevc`) worked precisely
because it never passes through `rtph265pay`.

## The fix (one line, Pi-side)
Moved the SEI splice from the `h265parse` **src** pad to its **sink** pad — i.e.
we now splice into the x265enc byte-stream **before** h265parse, so h265parse
re-parses the SEI-carrying AU and hands correctly-framed NALs to `rtph265pay`.
No change to the SEI format, the UUID, the transport, or anything on your side.

## Validated on the Pi (local loopback, no Orin)
`orin_rtp_stream → udpsrc ! rtph265depay ! h265parse ! filesink`:
- **37 frames sent → 37 decoded frames** (ffprobe `nb_read_frames=37`, HEVC Main
  1920×1080).
- **37 frame_id SEIs recovered from the RECEIVED RTP stream**, contiguous 1..37 —
  so the frame_ids survive h265parse's re-parse + RTP + depay intact.
- Round-trip SEI self-test still passes 59/59.

## Nothing changes on your side
- RTP/H.265 → `10.42.0.2:5600`, `pt=96`, `clock-rate=90000`, Main/L4.0/8-bit-420.
- One frame_id SEI per AU, filter by UUID
  `53 6e 69 70 65 49 74 46 72 6d 49 44 00 00 00 01`, 4-byte BE id.
- You PUSH detections to Pi PULL `tcp://10.42.0.1:5556`, echoing that frame_id.

Your depay/decode/inference/PUSH were all fine — this stream just wasn't a valid
RTP H.265 stream before. It is now.

## Ready to re-run
Same coordinated sequence as before: I start `orin_live_loop` (camera → RTP +
PULL bound), you bring up your receiver + inference + PUSH, we put a person in
frame and watch the detections come back and join to a pose. Say go.

One thing to double-check on your side while we're here (from the earlier debug
note, still unverified): during the failed run your PUSH never reached us
(`ss` showed no ESTAB on 5556). That was almost certainly just "no detections to
send because nothing decoded." But please confirm your PUSH target is
`tcp://10.42.0.1:5556` so that once frames decode, the detections actually land.
The one-line PUSH test from the earlier note is the fastest way to confirm the
back-channel independently.
