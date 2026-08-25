#!/usr/bin/env python3
"""
probe_camera_fov.py

Measure the field of view of the capture path in orin/frame_sender.c. Run ON
THE PI WITH THE CAMERA CONNECTED and the streaming server stopped -- the camera
can only be opened once.

aiming.c turns a bounding box into a servo angle using a fixed HFOV/VFOV, so a
wrong FOV mis-aims every detection by a fixed proportion. The pinhole formula is
exact; the unknown is the sensor rectangle libcamera maps onto the output, which
it calls ScalerCrop and reports in per-frame metadata.

Two stages, because libcamerasrc exposes neither the sensor mode nor ScalerCrop:

  1. Run the real pipeline head under gst-launch and read the caps libcamerasrc
     negotiates on its src pad -- the true camera output size.
  2. Replay that stream configuration through the libcamera Python API (the
     layer libcamerasrc sits on) and read the sensor mode and ScalerCrop off a
     live frame.

Stage 2 only means anything because stage 1 confirms the size it replays; on a
mismatch, rerun at the size stage 1 reports.

Copy the printed HFOV/VFOV into AIM_DEFAULT_*_DEG in orin/aiming.c.
"""

import argparse
import contextlib
import math
import os
import re
import selectors
import subprocess
import tempfile

# Installed C-mount telephoto lens: the one number libcamera cannot report.
DEFAULT_FOCAL_MM = 16.0

# Must match what frame_sender.c requests. The rate does not affect FOV, but it
# is part of the caps stage 1 reproduces.
DEFAULT_WIDTH  = 1920
DEFAULT_HEIGHT = 1080
DEFAULT_FPS    = 30

# The source head of frame_sender.c's pipeline, verbatim apart from the sink.
GST_PIPELINE = (
    "libcamerasrc ! videoconvert ! videoscale"
    " ! video/x-raw,format=I420,width={w},height={h},framerate={fps}/1"
    " ! fakesink"
)

# gst-launch -v prints one of these per pad once caps are negotiated.
CAPS_RE   = re.compile(r"GstLibcameraPad:src: caps = (video/x-raw.*)")
SENSOR_RE = re.compile(r"Selected sensor format: (\S+)")


def fov_deg(extent_px: float, pitch_mm: float, focal_mm: float) -> float:
    """FOV = 2 * arctan( (extent_px * pitch) / (2 * focal) )."""
    half = (extent_px * pitch_mm) / (2.0 * focal_mm)
    return math.degrees(2.0 * math.atan(half))


@contextlib.contextmanager
def captured_fd_stderr():
    """Capture writes to fd 2, including those from C++.

    libcamera logs the sensor mode from its pipeline handler, which never passes
    through sys.stderr. Only an fd-level redirect sees it.
    """
    saved = os.dup(2)
    tmp = tempfile.TemporaryFile(mode="w+b")
    try:
        os.dup2(tmp.fileno(), 2)
        yield tmp
    finally:
        os.dup2(saved, 2)
        os.close(saved)
        tmp.seek(0)


def probe_gst_caps(width: int, height: int, fps: int, timeout_s: float) -> str:
    """Stage 1: the caps libcamerasrc negotiates in the real pipeline.

    Returns the caps string, or None if negotiation never happened.
    """
    pipeline = GST_PIPELINE.format(w=width, h=height, fps=fps)
    print("=== Stage 1: caps libcamerasrc negotiates ===")
    print(f"  gst-launch-1.0 -v {pipeline}")

    proc = subprocess.Popen(
        ["gst-launch-1.0", "-v"] + pipeline.split(),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    caps = None
    tail = []
    try:
        # Negotiation completes well before the first buffer, so stop at that
        # line rather than running the pipeline for a fixed time.
        for line in proc.stdout:
            tail.append(line.rstrip())
            m = CAPS_RE.search(line)
            if m:
                caps = m.group(1).strip()
                break
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=timeout_s)
        except subprocess.TimeoutExpired:
            proc.kill()

    if caps is None:
        print("  FAILED: libcamerasrc never negotiated caps. Last output:")
        for line in tail[-15:]:
            print(f"    {line}")
        return None

    print(f"  negotiated: {caps}")
    return caps


def probe_libcamera(width: int, height: int, focal_mm: float, show_log: bool) -> int:
    """Stage 2: read the sensor mode and ScalerCrop for that configuration."""
    print("\n=== Stage 2: sensor mode and ScalerCrop (libcamera) ===")
    try:
        import libcamera as lc
    except ImportError as e:
        print(f"  libcamera Python bindings unavailable: {e}")
        return 1

    log = ""
    # libcamera chatters on fd 2 throughout; capture it and surface only the
    # sensor-mode line, unless the caller asked for the rest.
    with captured_fd_stderr() as errfile:
        try:
            cm = lc.CameraManager.singleton()
            if not cm.cameras:
                raise RuntimeError("no cameras found")
            cam = cm.cameras[0]
            cam.acquire()

            # libcamerasrc requests VideoRecording, so this selects the same
            # sensor mode the real pipeline gets.
            cfg = cam.generate_configuration([lc.StreamRole.VideoRecording])
            cfg.at(0).size = lc.Size(width, height)
            status = cfg.validate()
            cam.configure(cfg)

            stream = cfg.at(0).stream
            allocator = lc.FrameBufferAllocator(cam)
            if allocator.allocate(stream) <= 0:
                raise RuntimeError("buffer allocation failed")

            requests = []
            for buf in allocator.buffers(stream):
                req = cam.create_request()
                req.add_buffer(stream, buf)
                requests.append(req)

            cam.start()
            for req in requests:
                cam.queue_request(req)

            # ScalerCrop settles with AE/AGC, so read the 4th frame, not the 1st.
            sel = selectors.DefaultSelector()
            sel.register(cm.event_fd, selectors.EVENT_READ)
            completed = []
            while len(completed) < 4:
                if not sel.select(5):
                    raise RuntimeError("timed out waiting for frames")
                for req in cm.get_ready_requests():
                    completed.append(req)
                    if len(completed) < 4:
                        req.reuse()
                        cam.queue_request(req)

            # Both maps are keyed by ControlId objects; re-key by name so a
            # lookup miss is a clean None, not a miss on object identity.
            metadata = {str(k): v for k, v in completed[-1].metadata.items()}
            props = {str(k): v for k, v in cam.properties.items()}
            cam.stop()
            cam.release()
        except Exception as e:  # noqa: BLE001
            errfile.seek(0)
            log = errfile.read().decode(errors="replace")
            print(f"  FAILED: {e}")
            print(log)
            return 1
        errfile.seek(0)
        log = errfile.read().decode(errors="replace")

    if show_log:
        print(log)

    sensor = SENSOR_RE.search(log)
    print(f"  validated stream: {cfg.at(0)}  (validate -> {status})")
    print(f"  sensor mode     : {sensor.group(1) if sensor else 'not reported'}")

    unit_cell = props.get("UnitCellSize")
    array = props.get("PixelArraySize")
    crop = metadata.get("ScalerCrop")
    print(f"  PixelArraySize  : {array}")
    print(f"  UnitCellSize    : {unit_cell} nm")
    print(f"  ScalerCrop      : {crop}")

    if crop is None:
        print("\nScalerCrop absent from metadata; cannot compute exact FOV.")
        return 2
    if unit_cell is None:
        print("\nUnitCellSize absent from properties; cannot compute exact FOV.")
        return 2

    # UnitCellSize is the pixel pitch in nanometres, one value per axis.
    pitch_h_mm = unit_cell.width / 1e6
    pitch_v_mm = unit_cell.height / 1e6
    crop_w, crop_h = crop.size.width, crop.size.height

    hfov = fov_deg(crop_w, pitch_h_mm, focal_mm)
    vfov = fov_deg(crop_h, pitch_v_mm, focal_mm)

    print(f"\n=== Effective FOV of the {width}x{height} stream ===")
    print(f"  lens focal length     : {focal_mm} mm")
    print(f"  cropped sensor extent : {crop_w} x {crop_h} px"
          f" = {crop_w * pitch_h_mm:.3f} x {crop_h * pitch_v_mm:.3f} mm")
    print(f"  HFOV = {hfov:.2f} deg")
    print(f"  VFOV = {vfov:.2f} deg")
    print("\nPlug these into AIM_DEFAULT_HFOV_DEG / AIM_DEFAULT_VFOV_DEG"
          " (orin/aiming.c).")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[1].strip())
    ap.add_argument("--width", type=int, default=DEFAULT_WIDTH)
    ap.add_argument("--height", type=int, default=DEFAULT_HEIGHT)
    ap.add_argument("--fps", type=int, default=DEFAULT_FPS,
                    help="capture rate for stage 1; does not affect FOV")
    ap.add_argument("--focal-mm", type=float, default=DEFAULT_FOCAL_MM,
                    help="installed lens focal length in mm")
    ap.add_argument("--skip-gst", action="store_true",
                    help="skip stage 1 (use when gst-launch is unavailable)")
    ap.add_argument("--gst-timeout", type=float, default=15.0,
                    help="seconds to wait for the gst pipeline to shut down")
    ap.add_argument("--show-libcamera-log", action="store_true",
                    help="print libcamera's own log output")
    args = ap.parse_args()

    if not args.skip_gst:
        caps = probe_gst_caps(args.width, args.height, args.fps, args.gst_timeout)
        if caps is None:
            print("\nStage 1 failed, so stage 2 would be measuring an\n"
                  "unverified configuration. Fix the pipeline first, or pass\n"
                  "--skip-gst if you accept that.")
            return 1
        got = re.search(r"width=\(int\)(\d+).*height=\(int\)(\d+)", caps)
        if got and (int(got.group(1)), int(got.group(2))) != (args.width, args.height):
            print(f"  WARNING: libcamerasrc negotiated {got.group(1)}x{got.group(2)},"
                  f" not the requested {args.width}x{args.height}.")
            print("  Rerun with those dimensions -- the FOV below is for a"
                  " configuration the pipeline does not use.")

    return probe_libcamera(args.width, args.height, args.focal_mm,
                           args.show_libcamera_log)


if __name__ == "__main__":
    raise SystemExit(main())
