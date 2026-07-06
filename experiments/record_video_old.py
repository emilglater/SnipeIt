#!/usr/bin/env python3
"""
Record H.264 video from the Arducam IMX477 to an MP4 file.

Stop with Ctrl+C. The file is finalized cleanly on exit.

Requires picamera2 (pre-installed on Raspberry Pi OS Bookworm/Trixie):
    sudo apt install -y python3-picamera2

Prerequisite: the camera must be enabled for the CAM0 port in
/boot/firmware/config.txt:
    camera_auto_detect=0
    dtoverlay=imx477,cam0
...followed by a reboot. Verify with:
    rpicam-hello --list-cameras
"""

import signal
import sys
import time
from datetime import datetime
from pathlib import Path

from picamera2 import Picamera2
from picamera2.encoders import H264Encoder
from picamera2.outputs import FfmpegOutput

# ---------------- Configuration ----------------
WIDTH = 1920                                                        # sensor native max: 4056
HEIGHT = 1080                                                       # sensor native max: 3040
FPS = 30                                                            # bounded by sensor mode
BITRATE = 10_000_000                                                # 10 Mbps — good quality for training data
OUTPUT_DIR = Path(__file__).resolve().parent.parent / "videos"      # where MP4s are saved
CAMERA_INDEX = 0                                                    # 0 = first detected camera
# -----------------------------------------------


def build_output_path() -> Path:
    """Timestamped filename so successive runs don't overwrite each other."""
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    return OUTPUT_DIR / f"recording_{stamp}.mp4"


def main() -> int:
    output_path = build_output_path()

    picam2 = Picamera2(camera_num=CAMERA_INDEX)

    # Use a video configuration: the "main" stream is what gets encoded.
    # FrameDurationLimits is in microseconds; we set both bounds to 1/FPS
    # so the sensor runs at exactly the requested rate.
    frame_duration_us = int(1_000_000 / FPS)
    video_config = picam2.create_video_configuration(
        main={"size": (WIDTH, HEIGHT), "format": "RGB888"},
        controls={"FrameDurationLimits": (frame_duration_us, frame_duration_us)},
    )
    picam2.configure(video_config)

    encoder = H264Encoder(bitrate=BITRATE)
    # FfmpegOutput wraps the H.264 stream into an MP4 container on the fly,
    # and — crucially — writes a proper moov atom on clean shutdown so the
    # file is playable without post-processing.
    output = FfmpegOutput(str(output_path))

    # Install a SIGINT handler that flips a flag; the actual stop happens
    # in the main thread so picamera2's teardown runs cleanly.
    stop_requested = {"flag": False}

    def handle_sigint(signum, frame):
        if not stop_requested["flag"]:
            stop_requested["flag"] = True
            print("\nStop requested, finalizing video...", flush=True)

    signal.signal(signal.SIGINT, handle_sigint)

    print(f"Recording to: {output_path}")
    print(f"Resolution:   {WIDTH}x{HEIGHT} @ {FPS} fps  (bitrate {BITRATE / 1e6:.1f} Mbps)")
    print("Press Ctrl+C to stop.\n")

    picam2.start_recording(encoder, output)
    start_time = time.monotonic()

    try:
        while not stop_requested["flag"]:
            time.sleep(0.1)
    finally:
        picam2.stop_recording()
        picam2.close()

    elapsed = time.monotonic() - start_time
    size_mb = output_path.stat().st_size / (1024 * 1024) if output_path.exists() else 0.0
    print(f"Done. Duration: {elapsed:.1f}s, Size: {size_mb:.1f} MB")
    print(f"Saved: {output_path.resolve()}")
    return 0


if __name__ == "__main__":
    sys.exit(main())