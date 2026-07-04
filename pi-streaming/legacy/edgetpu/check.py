"""
check2.py -- quant-aware input diagnostic for the FPNLite person model.

Goal: settle whether the flat ~0.05-0.13 scores are a PREPROCESSING bug
(input dtype / quantization not applied) or a genuinely weak MODEL.

It prints the model's true input contract, then runs the SAME frame through
several input-encoding variants and prints the top scores for each. If one
variant produces clearly higher / scene-reactive scores, the model is fine and
detect() just needs the matching preprocessing. If ALL variants stay flat,
it's the model -- fall back to Option C for Sunday.

Run on the Pi (edgetpu venv) from ~/pi-streaming/:
    python3 check2.py
"""

import cv2
import numpy as np
import tflite_runtime.interpreter as tflite

MODEL = "models/ssd_mobilenet_v2_fpnlite_640_person_int8.tflite"
VIDEO = "/home/snipeit/SnipeIt/videos/recording_20260513_155602.mp4"

# ---------------------------------------------------------------------------
# Load model and report the FULL input contract -- this is the crux.
# ---------------------------------------------------------------------------
interp = tflite.Interpreter(model_path=MODEL)
interp.allocate_tensors()
in_det = interp.get_input_details()[0]
out_det = interp.get_output_details()

H, W = int(in_det["shape"][1]), int(in_det["shape"][2])
in_dtype = in_det["dtype"]
in_scale, in_zp = in_det["quantization"]  # (scale, zero_point); (0.0, 0) if float

print("=== INPUT CONTRACT ===")
print(f"  shape         : {list(in_det['shape'])}")
print(f"  dtype         : {in_dtype.__name__}")
print(f"  quant scale   : {in_scale}")
print(f"  quant zero_pt : {in_zp}")
print(f"  quant details : {in_det.get('quantization_parameters')}")

# Map outputs by shape/suffix (same convention as person_streamer._map_outputs)
def find_scores_index():
    for i, d in enumerate(out_det):
        name = d.get("name", "")
        shape = list(d["shape"])
        if "StatefulPartitionedCall" in name and len(shape) == 2 and name.endswith(":1"):
            return d["index"]
    # fallback: first [1,N] tensor
    for d in out_det:
        if len(list(d["shape"])) == 2:
            return d["index"]
    raise RuntimeError("no scores tensor found")

scores_idx = find_scores_index()

# ---------------------------------------------------------------------------
# Grab a frame ~1/3 into the recording (likely to contain a person).
# ---------------------------------------------------------------------------
cap = cv2.VideoCapture(VIDEO)
total = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
cap.set(cv2.CAP_PROP_POS_FRAMES, total // 3)
ret, frame = cap.read()
cap.release()
assert ret, "could not read frame"
print(f"\nFrame shape: {frame.shape}  (frame {total // 3}/{total})")

# ---------------------------------------------------------------------------
# Letterbox to WxH (identical to detect()): BGR->RGB, gray-128 pad.
# ---------------------------------------------------------------------------
def letterbox(bgr):
    fh, fw = bgr.shape[:2]
    s = min(W / fw, H / fh)
    nw, nh = int(round(fw * s)), int(round(fh * s))
    resized = cv2.resize(bgr, (nw, nh), interpolation=cv2.INTER_LINEAR)
    canvas = np.full((H, W, 3), 128, dtype=np.uint8)
    py, px = (H - nh) // 2, (W - nw) // 2
    canvas[py:py + nh, px:px + nw] = resized
    rgb = canvas[:, :, ::-1]          # BGR -> RGB
    return np.ascontiguousarray(rgb)

rgb_u8 = letterbox(frame)             # HxWx3 uint8, RGB

# ---------------------------------------------------------------------------
# Build candidate input encodings. We feed each, invoke, read top-5 scores.
# ---------------------------------------------------------------------------
def run(tensor):
    interp.set_tensor(in_det["index"], tensor[np.newaxis])
    interp.invoke()
    s = interp.get_tensor(scores_idx)[0]
    top = sorted([float(v) for v in s], reverse=True)[:5]
    return [round(v, 4) for v in top]

variants = {}

# A) raw uint8 cast to the model's declared dtype (what detect() does now,
#    but forced into the real dtype -- exposes the int8 wraparound if any)
variants["A_raw_cast_to_dtype"] = rgb_u8.astype(in_dtype)

# B) PROPER quantization: real_value -> quantized = real/scale + zero_point.
#    For an int8 MobileNet the "real" input is usually float [-1,1] (i.e.
#    (uint8/127.5 - 1)). We apply the model's own scale/zp on top of that.
if in_scale and in_scale > 0:
    real_pm1 = rgb_u8.astype(np.float32) / 127.5 - 1.0      # [-1, 1]
    q = np.round(real_pm1 / in_scale + in_zp)
    info = np.iinfo(in_dtype) if np.issubdtype(in_dtype, np.integer) else None
    if info is not None:
        q = np.clip(q, info.min, info.max)
    variants["B_quant_from_[-1,1]"] = q.astype(in_dtype)

    # C) quantize assuming the "real" input is [0,1] (uint8/255), the other
    #    common training normalization.
    real_01 = rgb_u8.astype(np.float32) / 255.0
    q2 = np.round(real_01 / in_scale + in_zp)
    if info is not None:
        q2 = np.clip(q2, info.min, info.max)
    variants["C_quant_from_[0,1]"] = q2.astype(in_dtype)

    # D) quantize assuming "real" input is raw [0,255] (no normalization)
    q3 = np.round(rgb_u8.astype(np.float32) / in_scale + in_zp)
    if info is not None:
        q3 = np.clip(q3, info.min, info.max)
    variants["D_quant_from_[0,255]"] = q3.astype(in_dtype)
else:
    # Float model: try both normalizations.
    variants["B_float_[-1,1]"] = (rgb_u8.astype(np.float32) / 127.5 - 1.0)
    variants["C_float_[0,1]"] = rgb_u8.astype(np.float32) / 255.0

# E) int8 reinterpret: if model is int8 and we naively pushed uint8 [0,255],
#    values 128-255 wrap to negative. Show what a centered int8 looks like
#    (uint8 - 128) which is the cheap "uint8->int8 shift" some pipelines use.
if np.issubdtype(in_dtype, np.signedinteger):
    variants["E_uint8_minus_128"] = (rgb_u8.astype(np.int16) - 128).astype(in_dtype)

# ---------------------------------------------------------------------------
# Run them all on the same frame.
# ---------------------------------------------------------------------------
print("\n=== TOP-5 SCORES PER INPUT ENCODING (same frame) ===")
for name, t in variants.items():
    print(f"  {name:24s} -> {run(t)}")

# ---------------------------------------------------------------------------
# Control: pure gray frame. A working model should score DIFFERENTLY here
# than on the person frame for the winning encoding.
# ---------------------------------------------------------------------------
gray = np.full((H, W, 3), 128, dtype=np.uint8)
print("\n=== CONTROL: solid gray-128 frame (winning encodings only) ===")
for name, t in variants.items():
    if np.issubdtype(in_dtype, np.integer) and in_scale and in_scale > 0:
        if name == "B_quant_from_[-1,1]":
            real = gray.astype(np.float32) / 127.5 - 1.0
            info = np.iinfo(in_dtype)
            g = np.clip(np.round(real / in_scale + in_zp), info.min, info.max).astype(in_dtype)
            print(f"  {name:24s} -> {run(g)}")
        elif name == "D_quant_from_[0,255]":
            info = np.iinfo(in_dtype)
            g = np.clip(np.round(gray.astype(np.float32) / in_scale + in_zp), info.min, info.max).astype(in_dtype)
            print(f"  {name:24s} -> {run(g)}")

print("\n=== READING THE RESULT ===")
print("If one encoding gives clearly higher top scores on the PERSON frame AND")
print("lower/different scores on the GRAY control -> that's the correct")
print("preprocessing. Patch detect() to match it. Model is FINE.")
print("If ALL encodings stay in the 0.05-0.15 range and barely move between")
print("person and gray -> the model is genuinely weak. Take Option C for Sunday.")