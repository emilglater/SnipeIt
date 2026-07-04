"""
probe_uint8.py -- find the uint8 input encoding that reproduces variant C's
strong score, since the Pi runtime rejects float32 input.

check2.py ran float [0,1] in a permissive env. We need the uint8 array that
makes THIS runtime produce the same strong, scene-reactive output. We test
several uint8 candidates directly.

Run on the Pi (edgetpu venv) from ~/pi-streaming/:
    python3 probe_uint8.py
"""
import cv2, numpy as np
import tflite_runtime.interpreter as tflite

MODEL = "models/ssd_mobilenet_v2_fpnlite_640_person_int8.tflite"
VIDEO = "/home/snipeit/SnipeIt/videos/recording_20260513_155602.mp4"

interp = tflite.Interpreter(model_path=MODEL)
interp.allocate_tensors()
in_det = interp.get_input_details()[0]
out_det = interp.get_output_details()
H, W = int(in_det["shape"][1]), int(in_det["shape"][2])

scores_idx = None
for d in out_det:
    if "StatefulPartitionedCall" in d.get("name", "") and d["name"].endswith(":1"):
        scores_idx = d["index"]
if scores_idx is None:
    for d in out_det:
        if len(list(d["shape"])) == 2:
            scores_idx = d["index"]; break

cap = cv2.VideoCapture(VIDEO)
total = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
cap.set(cv2.CAP_PROP_POS_FRAMES, total // 3)
ret, frame = cap.read(); cap.release()
assert ret

def letterbox(bgr):
    fh, fw = bgr.shape[:2]
    s = min(W/fw, H/fh)
    nw, nh = int(round(fw*s)), int(round(fh*s))
    r = cv2.resize(bgr, (nw, nh))
    c = np.full((H, W, 3), 128, dtype=np.uint8)
    py, px = (H-nh)//2, (W-nw)//2
    c[py:py+nh, px:px+nw] = r
    return np.ascontiguousarray(c[:, :, ::-1])  # BGR->RGB

rgb = letterbox(frame)

def run(u8):
    interp.set_tensor(in_det["index"], u8[np.newaxis].astype(np.uint8))
    interp.invoke()
    s = interp.get_tensor(scores_idx)[0]
    return [round(float(v),4) for v in sorted(s, reverse=True)[:5]]

# What float [0,1] BECAME when the permissive runtime cast it to uint8:
c_float = rgb.astype(np.float32) / 255.0
c_as_u8 = c_float.astype(np.uint8)          # truncates -> mostly 0, some 1

print("=== uint8 candidates, top-5 scores (person frame) ===")
print(f"  raw_uint8_[0,255]        -> {run(rgb)}")
print(f"  variantC_float_cast_u8   -> {run(c_as_u8)}   (what permissive rt fed)")
print(f"  rounded_float_cast_u8    -> {run(np.round(c_float).astype(np.uint8))}")

# Gray control for the strongest candidate(s)
gray = np.full((H, W, 3), 128, dtype=np.uint8)
print("\n=== gray control ===")
print(f"  raw_uint8_[0,255]        -> {run(gray)}")
print(f"  variantC_float_cast_u8   -> {run((gray.astype(np.float32)/255.0).astype(np.uint8))}")