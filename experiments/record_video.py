#!/usr/bin/env python3
"""
Record H.264 video from the Arducam IMX477 to an MP4 file, and stream the
same H.264 feed live over TCP so a viewer (e.g. ffplay) can preview what
the camera sees while the recording runs.

Stop with Ctrl+C. The file is finalized cleanly on exit. The live stream
is independent of the recording — viewers may connect and disconnect at
any time without affecting the file on disk.

Viewer command, from a PC on the same network:
    ffplay tcp://rpi5.local:8888 -fflags nobuffer -flags low_delay -framedrop

Requires picamera2 (pre-installed on Raspberry Pi OS Bookworm/Trixie):
    sudo apt install -y python3-picamera2

Prerequisite: the camera must be enabled for the CAM0 port in
/boot/firmware/config.txt:
    camera_auto_detect=0
    dtoverlay=imx477,cam0
...followed by a reboot. Verify with:
    rpicam-hello --list-cameras
"""

import queue
import signal
import socket
import sys
import threading
import time
from datetime import datetime
from pathlib import Path

from picamera2 import Picamera2
from picamera2.encoders import H264Encoder
from picamera2.outputs import FfmpegOutput, Output

# ---------------- Configuration ----------------
WIDTH = 1920                                                        # sensor native max: 4056
HEIGHT = 1080                                                       # sensor native max: 3040
FPS = 30                                                            # bounded by sensor mode
BITRATE = 10_000_000                                                # 10 Mbps — good quality for training data
OUTPUT_DIR = Path(__file__).resolve().parent.parent / "videos"      # where MP4s are saved
CAMERA_INDEX = 0                                                    # 0 = first detected camera
STREAM_HOST = "0.0.0.0"                                             # listen on all interfaces
STREAM_PORT = 8888                                                  # matches the focus-calibration ffplay command
# -----------------------------------------------


class TcpStreamOutput(Output):
    """
    Picamera2 Output that streams encoded H.264 to a single TCP viewer.

    - Recording proceeds regardless of viewer presence.
    - At most one viewer at a time; a new connection replaces the previous.
    - The encoder thread never blocks on the network: frames go into a
      bounded queue drained by a writer thread; on backpressure, the oldest
      frame is dropped so the stream stays live instead of falling behind.
    - A dead viewer is detected on the next send; the listener accepts the
      next connection without affecting the recording.
    """

    def __init__(self, host="0.0.0.0", port=8888, max_queue=120):
        super().__init__()
        self._host = host
        self._port = port
        self._queue = queue.Queue(maxsize=max_queue)
        self._stop_event = threading.Event()
        self._conn_lock = threading.Lock()
        self._conn = None
        self._listen_sock = None
        self._accept_thread = None
        self._writer_thread = None

    def start(self):
        super().start()
        self._listen_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._listen_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._listen_sock.bind((self._host, self._port))
        self._listen_sock.listen(1)
        print(f"[stream] Listening on tcp://{self._host}:{self._port}")
        self._accept_thread = threading.Thread(target=self._accept_loop, daemon=True)
        self._writer_thread = threading.Thread(target=self._writer_loop, daemon=True)
        self._accept_thread.start()
        self._writer_thread.start()

    def stop(self):
        self._stop_event.set()
        try:
            if self._listen_sock is not None:
                self._listen_sock.close()
        except Exception:
            pass
        with self._conn_lock:
            if self._conn is not None:
                try:
                    self._conn.shutdown(socket.SHUT_RDWR)
                except Exception:
                    pass
                try:
                    self._conn.close()
                except Exception:
                    pass
                self._conn = None
        super().stop()

    def outputframe(self, frame, keyframe=True, timestamp=None, packet=None, audio=False):
        if audio or not self.recording:
            return
        try:
            self._queue.put_nowait(frame)
        except queue.Full:
            # Drop the oldest queued frame to make room for the newest.
            try:
                self._queue.get_nowait()
                self._queue.put_nowait(frame)
            except (queue.Empty, queue.Full):
                pass

    def _accept_loop(self):
        while not self._stop_event.is_set():
            try:
                conn, addr = self._listen_sock.accept()
            except OSError:
                break  # listen socket closed during shutdown
            with self._conn_lock:
                if self._conn is not None:
                    try:
                        self._conn.close()
                    except Exception:
                        pass
                self._conn = conn
            print(f"[stream] Viewer connected: {addr[0]}:{addr[1]}")

    def _writer_loop(self):
        while not self._stop_event.is_set():
            try:
                data = self._queue.get(timeout=0.5)
            except queue.Empty:
                continue
            with self._conn_lock:
                conn = self._conn
            if conn is None:
                continue
            try:
                conn.sendall(data)
            except (BrokenPipeError, ConnectionResetError, OSError):
                with self._conn_lock:
                    if self._conn is conn:
                        try:
                            self._conn.close()
                        except Exception:
                            pass
                        self._conn = None
                print("[stream] Viewer disconnected")


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
    file_output = FfmpegOutput(str(output_path))
    # TcpStreamOutput sends the same H.264 stream to a live viewer (if any)
    # on STREAM_PORT. Independent of the file: viewers can come and go.
    net_output = TcpStreamOutput(host=STREAM_HOST, port=STREAM_PORT)

    # Install a SIGINT handler that flips a flag; the actual stop happens
    # in the main thread so picamera2's teardown runs cleanly.
    stop_requested = {"flag": False}

    def handle_sigint(signum, frame):
        if not stop_requested["flag"]:
            stop_requested["flag"] = True
            print("\nStop requested, finalizing video...", flush=True)
        else:
            # Second Ctrl+C: give up on clean teardown and exit immediately.
            print("\nForced exit.", flush=True)
            signal.signal(signal.SIGINT, signal.SIG_DFL)
            raise KeyboardInterrupt

    signal.signal(signal.SIGINT, handle_sigint)

    print(f"Recording to: {output_path}")
    print(f"Resolution:   {WIDTH}x{HEIGHT} @ {FPS} fps  (bitrate {BITRATE / 1e6:.1f} Mbps)")
    print(f"Live stream:  tcp://<pi-host>:{STREAM_PORT}  (open with ffplay)")
    print("Press Ctrl+C to stop.\n")

    picam2.start_recording(encoder, [file_output, net_output])
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
