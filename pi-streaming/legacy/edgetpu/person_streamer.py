#!/usr/bin/env python3
"""
person_streamer.py (IPC-only detector)

Purpose
- Connects as a *client* to the C IPC server (Unix Domain Socket).
- Waits for START/STOP commands (newline-delimited JSON).
- On START: opens the provided video_path (file or RTSP), runs Person-only Object Detection
  on EdgeTPU (Coral USB) using:
    - tflite_runtime + EdgeTPU delegate (libedgetpu.so.1)
    - model: ssd_mobilenet_v2_coco_quant_postprocess_edgetpu.tflite (EdgeTPU compiled)
- Sends detections back to the C server via IPC as newline-delimited JSON in this exact format:
  {
    "type": "target_detection",
    "timestamp_ms": <ms since start of current session/video>,
    "detections": [
      {
        "id": "1",
        "class": "HUMAN",
        "confidence": 0.85,
        "bbox": {"x": 100, "y": 50, "width": 200, "height": 400}
      }
    ]
  }

Notes
- No WebSocket / no app communication here. Only IPC with the Pi C server.
- START command keys used (as in test_ipc.py):
    cmd='start', video_path, duration_sec, fps, loop, frame_interval
- STOP command:
    cmd='stop'
"""

import argparse
import fcntl
import os
import time
from dataclasses import dataclass
from typing import List, Optional, Tuple

import cv2
import numpy as np

from ipc_client import IPCClient, make_detection


# ----------------------------
# Data structures
# ----------------------------
@dataclass
class Detection:
    label: str  # "person"
    score: float
    bbox_xywh: Tuple[int, int, int, int]  # x, y, w, h in pixels


# ----------------------------
# EdgeTPU SSD Person detector (tflite_runtime only; no pycoral)
# ----------------------------
class EdgeTPUPersonDetector:
    def __init__(self, model_path: str, labels_path: Optional[str], threshold: float,
                 verbose: bool = False):
        self.model_path = model_path
        self.labels_path = labels_path
        self.threshold = float(threshold)

        if not self.model_path:
            raise ValueError("Missing --model (EdgeTPU SSD .tflite)")

        try:
            import tflite_runtime.interpreter as tflite
        except ImportError as e:
            raise RuntimeError("tflite_runtime not installed. Install with: pip install tflite-runtime") from e

        self._tflite = tflite

        # Load EdgeTPU delegate only when the model filename signals it was
        # compiled for the TPU.  Plain INT8 models run on CPU via XNNPACK.
        if "_edgetpu" in os.path.basename(self.model_path):
            try:
                delegate = self._tflite.load_delegate("libedgetpu.so.1")
            except Exception as e:
                raise RuntimeError(
                    "Failed to load EdgeTPU delegate 'libedgetpu.so.1'. "
                    "Verify EdgeTPU runtime is installed and the Coral USB is connected."
                ) from e
            self.interpreter = self._tflite.Interpreter(
                model_path=self.model_path,
                experimental_delegates=[delegate],
            )
        else:
            self.interpreter = self._tflite.Interpreter(model_path=self.model_path)
        self.interpreter.allocate_tensors()

        in_details = self.interpreter.get_input_details()[0]
        self._in_index = in_details["index"]
        self._in_dtype = in_details["dtype"]
        in_shape = in_details["shape"]  # [1, H, W, 3]
        self._in_h = int(in_shape[1])
        self._in_w = int(in_shape[2])

        self._out_details = self.interpreter.get_output_details()
        self._boxes_i, self._classes_i, self._scores_i, self._count_i = self._map_outputs()
        self._verbose = verbose

        self.labels = self._read_labels(self.labels_path) if self.labels_path else {}

        # Resolve person class IDs
        self._person_ids = set()
        for k, v in self.labels.items():
            if str(v).strip().lower() == "person":
                self._person_ids.add(int(k))
        # fallback for COCO conventions (0-based / 1-based)
        if not self._person_ids:
            self._person_ids.update([0, 1])

        # Startup summary — always visible so ops-on-TPU vs CPU is detectable from logs.
        def _shape(i):
            return list(self._out_details[i]["shape"]) if i is not None else None
        print(
            f"[detector] model      : {os.path.basename(self.model_path)}\n"
            f"[detector] input      : {list(in_shape)} {self._in_dtype.__name__}\n"
            f"[detector] outputs    : boxes={self._boxes_i}{_shape(self._boxes_i)} "
            f"scores={self._scores_i}{_shape(self._scores_i)} "
            f"count={self._count_i}{_shape(self._count_i)}\n"
            f"[detector] threshold  : {self.threshold}\n"
            f"[detector] verbose    : {self._verbose}",
            flush=True,
        )

    def _read_labels(self, path: str) -> dict:
        labels = {}
        with open(path, "r", encoding="utf-8") as f:
            lines = [ln.strip() for ln in f.readlines() if ln.strip() and not ln.strip().startswith("#")]
        for idx, ln in enumerate(lines):
            parts = ln.replace(":", " ").split()
            if len(parts) >= 2 and parts[0].isdigit():
                labels[int(parts[0])] = " ".join(parts[1:])
            else:
                labels[idx] = ln
        return labels

    def _map_outputs(self):
        """
        Typical SSD postprocess outputs:
          0: boxes [1,N,4] (ymin,xmin,ymax,xmax) normalized
          1: classes [1,N]
          2: scores [1,N]
          3: num_detections [1]
        Names often contain 'TFLite_Detection_PostProcess' and suffixes ':1/:2/:3'.
        Newer TF exports use 'StatefulPartitionedCall:N' with shape-based ordering.
        """
        name_to_i = {d.get("name", ""): i for i, d in enumerate(self._out_details)}

        boxes_i = classes_i = scores_i = count_i = None

        # Pass 1: TFLite_Detection_PostProcess naming (older EdgeTPU-compiled models)
        for name, i in name_to_i.items():
            if "TFLite_Detection_PostProcess" in name:
                if name.endswith("TFLite_Detection_PostProcess") or name.endswith(":0"):
                    boxes_i = i
                elif name.endswith(":1"):
                    classes_i = i
                elif name.endswith(":2"):
                    scores_i = i
                elif name.endswith(":3"):
                    count_i = i

        # Pass 2: StatefulPartitionedCall naming (TF SavedModel / FPNLite exports).
        # Convention: :0=num_detections [1], :1=scores [1,N], :2=classes [1,N],
        # :3=boxes [1,N,4].  Use shape to disambiguate robustly.
        if boxes_i is None:
            for name, i in name_to_i.items():
                if "StatefulPartitionedCall" in name:
                    suffix = name.rsplit(":", 1)[-1] if ":" in name else ""
                    shape = list(self._out_details[i].get("shape", []))
                    if len(shape) == 1:
                        count_i = i
                    elif len(shape) == 3 and int(shape[-1]) == 4:
                        boxes_i = i
                    elif len(shape) == 2 and suffix == "1":
                        scores_i = i
                    elif len(shape) == 2 and suffix == "2":
                        classes_i = i

        # Common fallback (assumes TFLite_Detection_PostProcess list ordering)
        if boxes_i is None and len(self._out_details) == 4:
            boxes_i, classes_i, scores_i, count_i = 0, 1, 2, 3

        # Shape heuristic: boxes tensor always has last dim 4
        if boxes_i is None:
            for i, d in enumerate(self._out_details):
                shape = d.get("shape", [])
                if len(shape) >= 2 and int(shape[-1]) == 4:
                    boxes_i = i
                    break

        return boxes_i, classes_i, scores_i, count_i

    def detect(self, frame: np.ndarray, is_rgb: bool = False) -> List[Detection]:
        """
        Run inference on a single frame.

        frame   -- HxWx3 uint8 array.
        is_rgb  -- True if channels are already in RGB order (e.g. from picamera2
                   RGB888 lores stream).  False (default) assumes BGR (OpenCV),
                   e.g. cv2.VideoCapture frames from a video file.
        """
        if frame is None or frame.size == 0:
            return []

        orig_h, orig_w = frame.shape[:2]

        # Ensure RGB for the model input.
        if is_rgb:
            rgb = frame
        else:
            rgb = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)

        # --- Letterbox: resize preserving aspect ratio, pad to the square model input ---
        # The EdgeTPU model input is square (640x640); camera/file frames are 16:9.
        # Squashing 16:9 into a square horizontally compresses people (~44% thinner),
        # distorting them away from training data and costing detections, worst at the
        # small/blurry 5-20m targets. Instead: scale to fit, pad the remainder with
        # neutral gray (128), and reverse the transform when mapping boxes back.
        # Computed per-frame, so it handles 720p (camera) and 1080p (file) alike.
        scale = min(self._in_w / orig_w, self._in_h / orig_h)
        new_w = int(round(orig_w * scale))
        new_h = int(round(orig_h * scale))
        resized = cv2.resize(rgb, (new_w, new_h), interpolation=cv2.INTER_LINEAR)

        pad_x = (self._in_w - new_w) // 2
        pad_y = (self._in_h - new_h) // 2

        canvas = np.full((self._in_h, self._in_w, 3), 128, dtype=np.uint8)
        canvas[pad_y:pad_y + new_h, pad_x:pad_x + new_w] = resized

        # Input contract for this FPNLite export is broken: the input tensor is
        # declared uint8 with quant scale=1.0/zero_point=0 (a no-op), but the
        # network was NOT trained on raw uint8 [0,255]. Feeding raw bytes gives
        # dead, scene-invariant scores (~0.05-0.13). The encoding the trained
        # weights actually respond to is float [0,1] ROUNDED to a 0/1 mask
        # (pixel >= 128 -> 1, else 0), then handed to the graph as uint8.
        # Verified with probe_uint8.py: this encoding scores 0.50 on a person
        # frame vs 0.18 on a gray control; raw uint8 scores 0.094 flat on both.
        # The Pi tflite_runtime rejects float32 into a uint8 tensor, so we do
        # the round in float then cast back to uint8 (values are only 0 or 1).
        input_tensor = np.round(canvas.astype(np.float32) / 255.0).astype(np.uint8)

        input_tensor = np.expand_dims(input_tensor, axis=0)
        self.interpreter.set_tensor(self._in_index, input_tensor)
        _t0 = time.monotonic()
        self.interpreter.invoke()
        _infer_ms = (time.monotonic() - _t0) * 1000

        if self._boxes_i is None or self._scores_i is None:
            raise RuntimeError("Could not map SSD outputs for this model.")

        boxes = self.interpreter.get_tensor(self._out_details[self._boxes_i]["index"])
        scores = self.interpreter.get_tensor(self._out_details[self._scores_i]["index"])

        if self._count_i is not None:
            count = int(self.interpreter.get_tensor(self._out_details[self._count_i]["index"])[0])
        else:
            count = int(scores.shape[1]) if scores.ndim == 2 else int(scores.shape[0])

        # Unbatch
        if boxes.ndim == 3:
            boxes = boxes[0]
        if scores.ndim == 2:
            scores = scores[0]

        n = min(count, len(scores), len(boxes))

        if self._verbose:
            top = sorted([float(scores[i]) for i in range(n)], reverse=True)[:5]
            top_str = ", ".join(f"{s:.3f}" for s in top)
            path = "TPU" if _infer_ms < 200 else "CPU"
            print(
                f"[detect] {_infer_ms:.0f}ms ({path})  "
                f"count={count}  top_scores=[{top_str}]",
                flush=True,
            )

        dets: List[Detection] = []

        for i in range(n):
            score = float(scores[i])
            if score < self.threshold:
                continue

            # Single-class person model: every detection above threshold IS a person.
            # No class-index filtering on purpose -- it avoids the COCO label-map
            # id-mismatch trap (a wrong/old labels file silently drops all boxes).

            # SSD box order: [ymin, xmin, ymax, xmax], normalized to the 640x640
            # LETTERBOXED canvas -- NOT the original frame. Reverse the letterbox:
            ymin, xmin, ymax, xmax = [float(v) for v in boxes[i]]

            x1 = (xmin * self._in_w - pad_x) / scale
            y1 = (ymin * self._in_h - pad_y) / scale
            x2 = (xmax * self._in_w - pad_x) / scale
            y2 = (ymax * self._in_h - pad_y) / scale

            # Clamp to the original frame
            x1 = max(0, min(int(round(x1)), orig_w - 1))
            y1 = max(0, min(int(round(y1)), orig_h - 1))
            x2 = max(0, min(int(round(x2)), orig_w - 1))
            y2 = max(0, min(int(round(y2)), orig_h - 1))

            w = max(1, x2 - x1)
            h = max(1, y2 - y1)

            dets.append(Detection(label="person", score=score, bbox_xywh=(x1, y1, w, h)))

        dets.sort(key=lambda d: d.score, reverse=True)
        return dets


# ----------------------------
# FIFO helper
# ----------------------------
def _open_fifo_for_writing(path: str, timeout: float = 10.0):
    """
    Open a FIFO for writing without hanging the main thread.

    Polls with O_WRONLY|O_NONBLOCK (fails instantly when no reader exists)
    until FFmpeg has opened the other end.  Once a reader is confirmed, clears
    O_NONBLOCK so subsequent writes are blocking — exactly what picamera2's
    FileOutput expects.

    Also bumps the pipe buffer to 1 MB, giving FFmpeg ~1 s of headroom at
    8 Mbps before the first write would block.

    Returns an unbuffered binary file object.  The caller is responsible for
    closing it when the recording stops.

    Raises TimeoutError if no reader appears within *timeout* seconds.
    """
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            fd = os.open(path, os.O_WRONLY | os.O_NONBLOCK)
        except OSError:
            time.sleep(0.05)
            continue

        # Switch back to blocking mode — writes should block when pipe is full,
        # not silently drop data.
        flags = fcntl.fcntl(fd, fcntl.F_GETFL)
        fcntl.fcntl(fd, fcntl.F_SETFL, flags & ~os.O_NONBLOCK)

        # 1 MB pipe buffer: at 8 Mbps this is ~1 s of data, covering FFmpeg's
        # RTSP connection handshake without stalling the camera pipeline.
        try:
            fcntl.fcntl(fd, fcntl.F_SETPIPE_SZ, 1024 * 1024)
        except OSError:
            pass

        return os.fdopen(fd, "wb")

    raise TimeoutError(f"[person_streamer] Timed out waiting for FIFO reader on {path}")


# ----------------------------
# Live camera session using picamera2 (Option 1 — lowest latency)
# ----------------------------
def process_session_camera(
    client: IPCClient,
    detector: EdgeTPUPersonDetector,
    fifo_path: str,
    fps: float,
    frame_interval: int,
    camera_is_rgb: bool = True,
) -> int:
    """
    Captures directly from the Pi camera via picamera2.

    Two simultaneous streams:
      - main  1920x1080 YUV420 → hardware H.264 encoder → FileOutput(fifo_path)
              FFmpeg reads the FIFO and remuxes to mediaMTX → Android RTSP
      - lores 1280x720  RGB888 uint8 → EdgeTPU detection → IPC → Android WebSocket

    The FIFO must already exist (created by start.sh with mkfifo).
    FFmpeg must be started by the C server BEFORE this function is called.
    _open_fifo_for_writing() polls until FFmpeg has its O_RDONLY open, then
    returns a pre-opened, 1 MB-buffered file object passed to FileOutput so the
    camera pipeline never blocks on a full pipe during FFmpeg's RTSP handshake.
    """
    try:
        from picamera2 import Picamera2
        from picamera2.encoders import H264Encoder
        from picamera2.outputs import FileOutput
    except ImportError as exc:
        print(f"[person_streamer] ERROR: picamera2 not available — {exc}")
        client.send_detection(0, [])
        return 0

    target_fps = fps if fps and fps > 1.0 else 30.0

    picam2 = Picamera2()

    # main: 1080p YUV420 fed into the hardware H.264 encoder
    # lores: 720p RGB888 uint8 captured in-process for EdgeTPU detection
    video_config = picam2.create_video_configuration(
        main={"size": (1920, 1080), "format": "YUV420"},
        lores={"size": (1280, 720), "format": "RGB888"},
        controls={"FrameRate": target_fps},
    )
    picam2.configure(video_config)

    # The detector runs on the lores 1280x720 stream, so detect() returns
    # boxes in 720p pixel space. The Android app overlays boxes on the MAIN
    # 1920x1080 RTSP stream. Without rescaling, every box is drawn at
    # 1280/1920 = 0.667x its correct position/size -> shifted to the top-left
    # and shrunk (captures head+hand instead of the full body). Scale lores
    # pixel coords up to main-stream space before sending over IPC.
    # Derived from the configured sizes (not hardcoded) so it stays correct
    # if either resolution changes. Here both axes are 1.5 (16:9 -> 16:9).
    main_w, main_h = 1920, 1080
    lores_w, lores_h = 1280, 720
    sx = main_w / lores_w   # 1.5
    sy = main_h / lores_h   # 1.5

    # Hardware H.264 encoder:
    #   8 Mbps for 1080p, SPS/PPS repeated before every IDR frame so that
    #   Android clients connecting mid-stream never see a green screen,
    #   keyframe every ~1 second.
    encoder = H264Encoder(
        bitrate=8_000_000,
        repeat=True,
        iperiod=int(target_fps),
    )

    print(f"[person_streamer] Waiting for FFmpeg to open FIFO {fifo_path}...")
    try:
        fifo_out = _open_fifo_for_writing(fifo_path)
    except TimeoutError as exc:
        # FFmpeg never opened the read end (e.g. it died on a probe error).  Abort
        # this session cleanly instead of crashing the whole detector — the camera
        # is released and we return to the main loop to await the next START.
        print(f"[person_streamer] {exc} — aborting session (FFmpeg never opened the FIFO)")
        picam2.close()
        return 0
    print(
        f"[person_streamer] Starting camera: "
        f"1080p H.264 → {fifo_path} | 720p RGB for detection"
    )
    picam2.start_recording(encoder, FileOutput(fifo_out))

    frame_idx = 0
    sent_msgs = 0
    infer_calls = 0
    session_start = time.time()

    print(f"[camera] is_rgb={camera_is_rgb}  frame_interval={frame_interval}", flush=True)

    try:
        while client.is_connected():
            cmd = client.check_for_command()
            if cmd and cmd.get("cmd") == "stop":
                print("[person_streamer] Received STOP during camera session")
                break

            # Capture latest lores frame: (720, 1280, 3) uint8 RGB888
            frame = picam2.capture_array("lores")

            if frame_interval <= 0:
                frame_interval = 1

            if (frame_idx % frame_interval) == 0:
                dets = detector.detect(frame, is_rgb=camera_is_rgb)
                infer_calls += 1

                # Print fps + detection stats every 30 inference calls (~5 s at 6 fps)
                if infer_calls % 30 == 0:
                    elapsed = time.time() - session_start
                    ifps = infer_calls / elapsed if elapsed > 0 else 0
                    print(
                        f"[camera] infer_calls={infer_calls}  "
                        f"infer_fps={ifps:.1f}  "
                        f"sent={sent_msgs}  "
                        f"last_dets={len(dets)}",
                        flush=True,
                    )

                ts_ms = int((time.time() - session_start) * 1000)

                detections_payload = [
                    make_detection(
                        detection_id=str(i + 1),
                        obj_class="HUMAN",
                        confidence=float(d.score),
                        # Rescale 720p detector coords -> 1080p main-stream coords
                        # so the app's overlay aligns with the displayed video.
                        x=int(round(d.bbox_xywh[0] * sx)),
                        y=int(round(d.bbox_xywh[1] * sy)),
                        width=int(round(d.bbox_xywh[2] * sx)),
                        height=int(round(d.bbox_xywh[3] * sy)),
                    )
                    for i, d in enumerate(dets)
                ]

                client.send_detection(ts_ms, detections_payload)
                sent_msgs += 1

            frame_idx += 1

    finally:
        # Tear down defensively: on shutdown FFmpeg (the FIFO reader) may already
        # be gone, so flushing/closing fifo_out can raise BrokenPipeError.  Don't
        # let one failing step skip the others or dump a scary traceback — the
        # outcome is the same (everything released) regardless of shutdown order.
        print("[person_streamer] Stopping camera recording...")
        for step, fn in (
            ("stop_recording", picam2.stop_recording),
            ("close camera", picam2.close),
            ("close fifo", fifo_out.close),
        ):
            try:
                fn()
            except Exception as exc:
                print(f"[person_streamer] (cleanup) {step} failed: {exc!r}")

    return sent_msgs


# ----------------------------
# Video file processing loop controlled by IPC commands
# ----------------------------
def process_session(
    client: IPCClient,
    detector: EdgeTPUPersonDetector,
    video_path: str,
    duration_sec: float,
    fps: float,
    loop: bool,
    frame_interval: int,
    realtime: bool,
) -> int:
    """
    Opens the video and sends detections every Nth frame.
    Returns number of detection-messages sent in this session.
    """
    cap = cv2.VideoCapture(video_path)
    if not cap.isOpened():
        print(f"[person_streamer] ERROR: cannot open video source: {video_path}")
        # notify server with empty detections (optional)
        client.send_detection(0, [])
        return 0

    # Prefer capture-reported FPS if valid
    cap_fps = cap.get(cv2.CAP_PROP_FPS)
    if cap_fps and cap_fps > 1e-3:
        fps_used = cap_fps
    else:
        fps_used = fps if fps and fps > 1e-3 else 30.0

    frame_time_sec = 1.0 / fps_used
    session_start = time.time()

    sent_msgs = 0
    frame_idx = 0

    # For file sources, we can use CAP_PROP_POS_MSEC for timestamp.
    # If it returns 0 always, we fall back to frame_idx / fps.
    while client.is_connected():
        # Check STOP while running
        cmd = client.check_for_command()
        if cmd and cmd.get("cmd") == "stop":
            print("[person_streamer] Received STOP during processing")
            break

        ret, frame = cap.read()
        if not ret:
            if loop:
                cap.set(cv2.CAP_PROP_POS_FRAMES, 0)
                frame_idx = 0
                session_start = time.time()
                continue
            else:
                break

        if frame_interval <= 0:
            frame_interval = 1

        if (frame_idx % frame_interval) == 0:
            dets = detector.detect(frame)

            # timestamp_ms: time since start of session/video (not epoch)
            ts_ms = int(round(cap.get(cv2.CAP_PROP_POS_MSEC)))
            if ts_ms <= 0:
                ts_ms = int(round((frame_idx / fps_used) * 1000.0))

            detections_payload = []
            for i, d in enumerate(dets):
                x, y, w, h = d.bbox_xywh
                detections_payload.append(
                    make_detection(
                        detection_id=str(i + 1),
                        obj_class="HUMAN",
                        confidence=float(d.score),
                        x=int(x),
                        y=int(y),
                        width=int(w),
                        height=int(h),
                    )
                )

            # IMPORTANT: ipc_client.send_detection wraps into:
            # {type:'target_detection', timestamp_ms:..., detections:[...]}
            client.send_detection(ts_ms, detections_payload)
            sent_msgs += 1

        frame_idx += 1

        if realtime:
            # Sleep to match video FPS - enforce strict timing
            elapsed = time.time() - session_start
            target = frame_idx * frame_time_sec
            sleep_s = target - elapsed
            if frame_idx % 30 == 0:
                drift_ms = (elapsed - target) * 1000
                print(f"[pace] frame={frame_idx} target={target*1000:.0f}ms "
                      f"elapsed={elapsed*1000:.0f}ms drift={drift_ms:+.0f}ms "
                      f"sleep={sleep_s*1000:+.0f}ms", flush=True)
            if sleep_s > 0:
                time.sleep(sleep_s)  # Sleep full amount, don't cap

    cap.release()
    return sent_msgs


def main():
    ap = argparse.ArgumentParser(description="IPC-only EdgeTPU person detection module (no websocket).")
    ap.add_argument("--model", required=True, help="Path to EdgeTPU SSD model (.tflite)")
    ap.add_argument("--labels", default=None, help="Path to labels file (recommended)")
    ap.add_argument("--threshold", type=float, default=0.5, help="Score threshold (default: 0.5)")
    ap.add_argument("--socket-path", default=None, help="Unix socket path override (default: /tmp/detection.sock)")
    ap.add_argument("--realtime", action="store_true", help="Try to pace processing to FPS (recommended)")
    ap.add_argument("--no-realtime", action="store_true", help="Process as fast as possible")
    ap.add_argument(
        "--camera", action="store_true",
        help=(
            "Use picamera2 live camera instead of a video file. "
            "The video_path in the START command is used as the FIFO output path "
            "for the hardware-encoded 1080p H.264 stream (lores 720p RGB goes to the TPU)."
        ),
    )
    ap.add_argument(
        "--camera-bgr", action="store_true",
        help=(
            "Treat picamera2 lores frames as BGR instead of RGB. "
            "Use this if detections are poor on camera but fine on video files "
            "(picamera2 RGB888 sometimes delivers BGR-order bytes on some Pi builds)."
        ),
    )
    ap.add_argument(
        "--verbose", action="store_true",
        help="Print per-inference timing and raw scores (useful for CPU vs TPU diagnosis).",
    )
    args = ap.parse_args()

    realtime = True
    if args.no_realtime:
        realtime = False
    if args.realtime:
        realtime = True

    detector = EdgeTPUPersonDetector(args.model, args.labels, args.threshold,
                                     verbose=args.verbose)

    client = IPCClient(socket_path=args.socket_path) if args.socket_path else IPCClient()
    if not client.connect(timeout=30.0):
        raise SystemExit("[person_streamer] Could not connect to IPC server (C). Is streaming_server running?")

    print("[person_streamer] Connected. Waiting for START/STOP commands...")

    total_msgs = 0
    while client.is_connected():
        cmd = client.wait_for_command(timeout=None)
        if not cmd:
            print("[person_streamer] Connection lost while waiting for command.")
            break

        if cmd.get("cmd") == "start":
            video_path = cmd.get("video_path", "")
            duration_sec = float(cmd.get("duration_sec", 0.0) or 0.0)
            fps = float(cmd.get("fps", 0.0) or 0.0)
            loop = bool(cmd.get("loop", False))
            frame_interval = int(cmd.get("frame_interval", 5))

            if args.camera:
                print(
                    f"[person_streamer] START (camera): fifo={video_path}, "
                    f"fps={fps}, frame_interval={frame_interval}"
                )
                msgs = process_session_camera(
                    client=client,
                    detector=detector,
                    fifo_path=video_path,
                    fps=fps,
                    frame_interval=frame_interval,
                    camera_is_rgb=not args.camera_bgr,
                )
            else:
                print(
                    f"[person_streamer] START: video_path={video_path}, "
                    f"duration_sec={duration_sec}, fps={fps}, loop={loop}, "
                    f"frame_interval={frame_interval}"
                )
                msgs = process_session(
                    client=client,
                    detector=detector,
                    video_path=video_path,
                    duration_sec=duration_sec,
                    fps=fps,
                    loop=loop,
                    frame_interval=frame_interval,
                    realtime=realtime,
                )
            total_msgs += msgs
            print(f"[person_streamer] Session finished. Sent {msgs} detection messages. Total={total_msgs}")

        elif cmd.get("cmd") == "stop":
            print("[person_streamer] STOP received (idle).")
            # just continue waiting; server might send a new START
            continue
        else:
            print(f"[person_streamer] Unknown command: {cmd}")

    client.close()
    print("[person_streamer] Exiting.")


if __name__ == "__main__":
    main()
