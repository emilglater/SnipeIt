#!/usr/bin/env python3
"""
probe_camera_fov.py

Ground-truth the field of view of the 1080p capture path used by
person_streamer.py. Run this ON THE PI WITH THE CAMERA CONNECTED.

Why this exists:
    person_streamer.py configures the camera with
        main  = 1920x1080  (16:9)
        lores = 1280x720
    but does NOT pin a `raw`/sensor mode or a ScalerCrop, so picamera2
    auto-selects the sensor mode. Which mode it picks (and the crop the ISP
    applies to turn a 4:3 sensor mode into a 16:9 output) is what actually
    determines the FOV. The pinhole FOV formula is exact; the only unknown is
    the sensor rectangle (`ScalerCrop`) the pipeline really uses. This script
    reads it straight from the running camera and prints the resulting FOV so
    the geometry module (orin/aiming) can be configured with real numbers.

Output: the selected raw mode, the applied ScalerCrop, and the H/V FOV of the
1080p main stream, computed from ScalerCrop. Copy HFOV/VFOV into the AimConfig.
"""

import math

# IMX477 physical parameters (Sony datasheet / Pi HQ camera).
PIXEL_PITCH_MM   = 1.55e-3       # 1.55 micrometres per pixel
LENS_FOCAL_MM    = 16.0          # installed C-mount telephoto lens
# ScalerCrop is expressed in the sensor's full pixel-array coordinate system.
# For the IMX477 the active array is 4056 x 3040; picamera2 reports the exact
# PixelArraySize, which we read below rather than assume.


def fov_deg(extent_px: float, pitch_mm: float, focal_mm: float) -> float:
    """FOV = 2 * arctan( (extent_px * pitch) / (2 * focal) )."""
    half = (extent_px * pitch_mm) / (2.0 * focal_mm)
    return math.degrees(2.0 * math.atan(half))


def main() -> int:
    try:
        from picamera2 import Picamera2
    except Exception as e:  # noqa: BLE001
        print(f"picamera2 import failed: {e}")
        return 1

    picam2 = Picamera2()

    print("=== Available sensor modes ===")
    for i, m in enumerate(picam2.sensor_modes):
        print(f"  [{i}] size={m.get('size')} fmt={m.get('format')} "
              f"crop_limits={m.get('crop_limits')} fps={m.get('fps')}")

    # Replicate person_streamer.py's configuration EXACTLY.
    video_config = picam2.create_video_configuration(
        main={"size": (1920, 1080), "format": "YUV420"},
        lores={"size": (1280, 720), "format": "RGB888"},
    )
    picam2.configure(video_config)

    cfg = picam2.camera_configuration()
    raw = cfg.get("raw")
    print("\n=== Selected configuration ===")
    print(f"  main: {cfg['main']['size']}  format={cfg['main']['format']}")
    print(f"  raw : {raw['size'] if raw else 'NONE'}  "
          f"format={raw['format'] if raw else '-'}")

    props = picam2.camera_properties
    pixel_array = props.get("PixelArraySize")
    print(f"  PixelArraySize: {pixel_array}")

    # ScalerCrop (the sensor rectangle mapped to the output) is reported in
    # request metadata, so start, grab one frame's metadata, then stop.
    picam2.start()
    md = picam2.capture_metadata()
    picam2.stop()
    picam2.close()

    scaler_crop = md.get("ScalerCrop")
    print(f"  ScalerCrop (x, y, w, h): {scaler_crop}")

    if not scaler_crop:
        print("\nScalerCrop unavailable in metadata; cannot compute exact FOV.")
        return 2

    _, _, crop_w, crop_h = scaler_crop
    hfov = fov_deg(crop_w, PIXEL_PITCH_MM, LENS_FOCAL_MM)
    vfov = fov_deg(crop_h, PIXEL_PITCH_MM, LENS_FOCAL_MM)

    print("\n=== Effective FOV of the 1080p stream (from ScalerCrop) ===")
    print(f"  Cropped sensor extent: {crop_w} x {crop_h} px "
          f"= {crop_w * PIXEL_PITCH_MM:.3f} x {crop_h * PIXEL_PITCH_MM:.3f} mm")
    print(f"  HFOV = {hfov:.2f} deg")
    print(f"  VFOV = {vfov:.2f} deg")
    print("\nPlug these into AimConfig.hfov_deg / vfov_deg (orin/aiming.h).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
