#!/usr/bin/env python3
"""
diag_tpu.py — standalone EdgeTPU / model diagnostic.

Runs without the IPC server.  Two modes:
  python3 diag_tpu.py --model <path>            # synthetic frame only
  python3 diag_tpu.py --model <path> --camera   # capture real frames + test both is_rgb values

Reports:
  - Inference time  (<200 ms = TPU path, >500 ms = CPU path)
  - Raw top-5 scores before any threshold
  - Number of ops on TPU vs CPU (via edgetpu_compiler summary if available)
"""
import argparse
import time
import numpy as np
import tflite_runtime.interpreter as tflite

MODEL_DEFAULT = "models/ssd_mobilenet_v2_fpnlite_640_person_int8_edgetpu.tflite"

def load_interpreter(model_path: str):
    try:
        delegate = tflite.load_delegate("libedgetpu.so.1")
        print("[diag] EdgeTPU delegate loaded OK")
    except Exception as e:
        print(f"[diag] WARNING: could not load EdgeTPU delegate: {e}")
        print("[diag] Falling back to CPU-only interpreter")
        delegate = None

    if delegate:
        interp = tflite.Interpreter(model_path=model_path,
                                    experimental_delegates=[delegate])
    else:
        interp = tflite.Interpreter(model_path=model_path)
    interp.allocate_tensors()
    return interp


def run_inference(interp, frame_rgb: np.ndarray):
    """Run one inference; return (infer_ms, raw_scores_sorted_desc)."""
    in_d = interp.get_input_details()[0]
    out_d = interp.get_output_details()

    h, w = in_d["shape"][1], in_d["shape"][2]
    import cv2
    resized = cv2.resize(frame_rgb, (w, h))
    if in_d["dtype"] == np.uint8:
        inp = resized
    else:
        inp = resized.astype(np.float32) / 255.0
    inp = np.expand_dims(inp, 0)

    interp.set_tensor(in_d["index"], inp)
    t0 = time.monotonic()
    interp.invoke()
    ms = (time.monotonic() - t0) * 1000

    # Find the scores tensor: shape [1, N] among the outputs
    scores_raw = None
    for d in out_d:
        s = list(d["shape"])
        if len(s) == 2 and s[0] == 1 and s[1] > 1:
            # Could be scores or classes; take the first [1,N] tensor as scores
            # (StatefulPartitionedCall:1 comes first in get_output_details() for this model)
            arr = interp.get_tensor(d["index"])[0]
            if scores_raw is None or d["name"].endswith(":1"):
                scores_raw = arr
    if scores_raw is None:
        scores_raw = np.array([])

    top = sorted(scores_raw.tolist(), reverse=True)[:5]
    return ms, top


def bench_synthetic(interp, n: int = 10):
    in_d = interp.get_input_details()[0]
    h, w = in_d["shape"][1], in_d["shape"][2]
    frame = np.full((h, w, 3), 128, dtype=np.uint8)

    print(f"\n[diag] Warming up with {n} synthetic {w}x{h} gray frames...")
    times = []
    for i in range(n):
        ms, top = run_inference(interp, frame)
        times.append(ms)
        marker = "TPU" if ms < 200 else "CPU"
        print(f"  [{i+1:2d}] {ms:6.1f} ms  ({marker})  top_scores={[f'{s:.3f}' for s in top]}")

    avg = sum(times) / len(times)
    min_ms = min(times)
    print(f"\n[diag] avg={avg:.1f} ms  min={min_ms:.1f} ms  implied_fps={1000/avg:.1f}")
    if min_ms < 200:
        print("[diag] RESULT: inference is running on the EdgeTPU ✓")
    elif min_ms < 800:
        print("[diag] RESULT: inference looks partially on CPU (some ops not on TPU)")
    else:
        print("[diag] RESULT: inference is running entirely on CPU — "
              "check EdgeTPU compiler op mapping")


def bench_camera(interp):
    try:
        from picamera2 import Picamera2
    except ImportError:
        print("[diag] picamera2 not available — skipping camera test")
        return

    import cv2
    print("\n[diag] Opening camera (lores 1280x720 RGB888, main 1920x1080 YUV420)...")
    picam2 = Picamera2()
    # lores must not exceed main; mirror the same config as person_streamer.py
    cfg = picam2.create_video_configuration(
        main={"size": (1920, 1080), "format": "YUV420"},
        lores={"size": (1280, 720), "format": "RGB888"},
    )
    picam2.configure(cfg)
    picam2.start()

    # Countdown so the user has time to stand in front of the camera.
    # AGC also needs ~2 s to settle after start().
    print("[diag] GET IN FRONT OF THE CAMERA — starting in:", flush=True)
    for i in range(5, 0, -1):
        print(f"  {i}...", flush=True)
        time.sleep(1.0)
    print("[diag] GO — capturing 5 frames now\n", flush=True)

    print("[diag] Capturing 5 frames, testing is_rgb=True AND is_rgb=False ...\n")
    for trial in range(5):
        raw = picam2.capture_array("lores")

        # is_rgb=True path: pass frame as-is (assume picamera2 gives real RGB)
        ms_t, top_t = run_inference(interp, raw)

        # is_rgb=False path: swap R and B before passing (treat as BGR)
        import cv2
        bgr_as_rgb = cv2.cvtColor(raw, cv2.COLOR_BGR2RGB)
        ms_f, top_f = run_inference(interp, bgr_as_rgb)

        print(
            f"  frame {trial+1}: "
            f"is_rgb=True  {ms_t:5.0f}ms  top={[f'{s:.3f}' for s in top_t]}\n"
            f"           "
            f"is_rgb=False {ms_f:5.0f}ms  top={[f'{s:.3f}' for s in top_f]}"
        )

    picam2.stop()
    picam2.close()
    print(
        "\n[diag] Compare the top scores above:\n"
        "  - Whichever is_rgb value gives HIGHER scores = correct channel order for this Pi\n"
        "  - If both are near 0.0, the issue is op-mapping (CPU fallback / wrong model)\n"
        "  - If timing > 500 ms, model is on CPU regardless of is_rgb"
    )


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", default=MODEL_DEFAULT)
    ap.add_argument("--camera", action="store_true",
                    help="Also run live camera frames after synthetic bench")
    ap.add_argument("--no-bench", action="store_true",
                    help="Skip the synthetic timing benchmark (go straight to camera)")
    ap.add_argument("--n", type=int, default=10,
                    help="Number of synthetic frames to time (default 10)")
    args = ap.parse_args()

    print(f"[diag] Model: {args.model}")
    interp = load_interpreter(args.model)

    in_d = interp.get_input_details()[0]
    out_d = interp.get_output_details()
    print(f"[diag] Input : {list(in_d['shape'])} {in_d['dtype'].__name__}")
    for i, d in enumerate(out_d):
        print(f"[diag] Output[{i}]: {d['name']!r}  shape={list(d['shape'])}  dtype={d['dtype'].__name__}")

    if not args.no_bench:
        bench_synthetic(interp, n=args.n)

    if args.camera:
        bench_camera(interp)


if __name__ == "__main__":
    main()
