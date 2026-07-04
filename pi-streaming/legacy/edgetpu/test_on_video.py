"""
test_on_video.py -- run the PATCHED EdgeTPUPersonDetector on a recorded mp4,
bypassing the IPC/C-server stack entirely.

This isolates model+preprocessing quality from the live-camera path. It imports
the real detector class from person_streamer.py, so it exercises the exact
detect() code you just patched (float [0,1] input).

Run on the Pi (edgetpu venv) from ~/pi-streaming/:
    python3 test_on_video.py
"""

import sys
import cv2
import numpy as np

# Import the real detector class from the module you patched.
from person_streamer import EdgeTPUPersonDetector

MODEL = "models/ssd_mobilenet_v2_fpnlite_640_person_int8.tflite"
VIDEO = "/home/snipeit/SnipeIt/videos/recording_20260513_155602.mp4"
THRESHOLD = 0.20          # low for the demo; raise if too many false positives
SAMPLE_EVERY = 30         # run detection every Nth frame (mp4 path, no realtime)
MAX_FRAMES = 40           # how many sampled frames to report

detector = EdgeTPUPersonDetector(MODEL, None, THRESHOLD, verbose=True)

cap = cv2.VideoCapture(VIDEO)
total = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
print(f"\nVideo: {VIDEO}  ({total} frames)\n")

frame_idx = 0
reported = 0
hits = 0
peak = 0.0

while reported < MAX_FRAMES:
    ret, frame = cap.read()
    if not ret:
        break
    if frame_idx % SAMPLE_EVERY != 0:
        frame_idx += 1
        continue

    # cv2 frames are BGR -> is_rgb=False (matches the file/video path in detect()).
    dets = detector.detect(frame, is_rgb=False)
    n = len(dets)
    top = max((d.score for d in dets), default=0.0)
    peak = max(peak, top)
    if n > 0:
        hits += 1
    print(f"  frame {frame_idx:5d}  detections>={THRESHOLD}: {n:2d}  top_conf={top:.3f}")
    frame_idx += 1
    reported += 1

cap.release()

print("\n=== SUMMARY ===")
print(f"  sampled frames        : {reported}")
print(f"  frames with detections: {hits}")
print(f"  peak confidence seen  : {peak:.3f}")
print()
if peak >= 0.40:
    print("  STRONG: model + preprocessing are working on recorded video.")
    print("  If the LIVE camera is still weak after this, the problem is the")
    print("  camera path (color order: try --camera-bgr, or lighting/focus).")
elif peak >= 0.25:
    print("  MARGINAL: model reacts but weakly. Usable for the demo at a low")
    print("  threshold; expect misses. Mention this honestly in the slides.")
else:
    print("  WEAK on video too: the model itself is the limit. Take Option C")
    print("  (COCO test model) for Sunday and present FPNLite metrics in slides.")