#!/bin/bash
# Starter script for the full SnipeIt Pi pipeline.
# Run with: ./start.sh    (Ctrl+C to stop everything cleanly)

set -e
cd "$(dirname "$0")"   # always run from pi-streaming/

# ---- 0. Clean up leftovers from a previous run --------------------------------
echo "[start] Cleaning up previous processes..."
pkill -f streaming_server 2>/dev/null || true
pkill -f mediamtx           2>/dev/null || true
pkill -f person_streamer.py 2>/dev/null || true
# Kill any FFmpeg orphaned by a previous run.  FFmpeg is forked by the C server
# and is reparented to init if the server dies uncleanly; a survivor keeps the
# mediaMTX publisher slot for our stream, so the next run's FFmpeg can't publish
# and Android sees a black screen.  Match on our FIFO input path so we don't
# touch unrelated ffmpeg processes.
pkill -f "ffmpeg.*/tmp/camera_stream.h264" 2>/dev/null || true
sleep 2

# ---- 0b. Create the FIFO that picamera2 writes H.264 into -------------------
# FFmpeg reads this FIFO and remuxes it to mediaMTX (no re-encode).
# The FIFO must exist before streaming_server starts so config_validate() passes.
CAMERA_FIFO="/tmp/camera_stream.h264"
rm -f "$CAMERA_FIFO"
mkfifo "$CAMERA_FIFO"
echo "[start] Created camera FIFO: $CAMERA_FIFO"

# ---- 1. Verify Coral USB before doing anything --------------------------------
if ! lsusb | grep -qiE 'global unichip|google'; then
    echo "[start] ERROR: Coral USB not detected. Plug it into a blue USB-3 port and retry."
    exit 1
fi
echo "[start] Coral USB detected."

# ---- 2. Start the C server in the background, log to file ---------------------
echo "[start] Launching streaming_server (logs: /tmp/streaming_server.log)..."
./streaming_server > /tmp/streaming_server.log 2>&1 &
SERVER_PID=$!

# Wait until it's blocked on the IPC accept (= ready for the detector)
for i in $(seq 1 30); do
    if grep -q "Waiting for Python" /tmp/streaming_server.log 2>/dev/null; then
        echo "[start] streaming_server ready (PID $SERVER_PID)"
        break
    fi
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
        echo "[start] ERROR: streaming_server died early. Last 20 log lines:"
        tail -20 /tmp/streaming_server.log
        exit 1
    fi
    sleep 0.5
done

# ---- 3. Cleanup trap so Ctrl+C kills everything -------------------------------
cleanup() {
    echo
    echo "[start] Stopping..."
    kill "$SERVER_PID" 2>/dev/null || true
    pkill -f mediamtx  2>/dev/null || true
    rm -f "$CAMERA_FIFO"
    exit 0
}
trap cleanup INT TERM

# ---- 4. Activate venv & run the Python detector in foreground -----------------
echo "[start] Activating edgetpu venv & starting person_streamer.py..."
echo "[start] (Press Ctrl+C to stop the whole pipeline.)"
source ~/venvs/edgetpu/bin/activate
python3 person_streamer.py \
    --model  /home/snipeit/SnipeIt/pi-streaming/models/ssd_mobilenet_v2_fpnlite_640_person_int8.tflite \
    --threshold 0.30 \
    --camera \
    --verbose
