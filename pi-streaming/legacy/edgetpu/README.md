# Retired EdgeTPU streaming + detection module (fallback)

This is the pre-Orin, on-Pi detection pipeline, retired 2026-07-01 when detection
moved to the Orin. Kept as a fallback.

- `person_streamer.py` — picamera2 camera owner + Coral EdgeTPU person detector;
  wrote H.264 to the FIFO for the app video and detections to the C server over
  the Unix socket (`ipc_client.py`).
- `ipc_client.py` — Unix-socket IPC client (detection JSON to streaming_server).
- `unix_socket.c` / `unix_socket.h` — the C server side of that same Unix socket
  (`/tmp/detection.sock`): accepted the Python client, sent START/STOP, read
  detection JSON. Moved here 2026-08-16; it had been left in `pi-streaming/` and
  was still compiled into `streaming_server` even though `main.c` stopped
  calling it when detection moved to the Orin.
- `test_ipc.c` — C harness that drove `unix_socket.c` against `test_ipc.py`.
- `models/` — the EdgeTPU / tflite detection models + labels.
- `check.py`, `diag_tpu.py`, `probe_uint8.py`, `test_on_video.py`, `test_ipc.py`
  — EdgeTPU diagnostics / dev tools.

The full working snapshot is also on the git branch **`edgetpu-fallback`**.

To restore: move these back to `pi-streaming/`, revert `main.c`/`start.sh` to the
`edgetpu-fallback` versions, and run `person_streamer.py` under the edgetpu venv.
The new pipeline instead runs the C GStreamer sender (orin/frame_sender) as the
camera owner and receives detections from the Orin over ZeroMQ (orin/*).
