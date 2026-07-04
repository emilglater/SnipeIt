#!/bin/bash
# Starter for the SnipeIt Pi pipeline (Orin-based detection).
#
# The C streaming_server now OWNS the camera (via orin/frame_sender): it fans the
# 1080p capture out as H.265+SEI over RTP to the Orin (which runs detection) and
# as an H.264 preview into the FIFO that FFmpeg remuxes to mediaMTX for the app.
# Detections come back from the Orin over ZeroMQ. There is no local Python /
# EdgeTPU detector any more (retired to legacy/edgetpu + branch edgetpu-fallback).
#
# Run with: ./start.sh [config_file]     (Ctrl+C to stop everything cleanly)

set -e
cd "$(dirname "$0")"   # always run from pi-streaming/

# ---- 0. Clean up leftovers from a previous run --------------------------------
echo "[start] Cleaning up previous processes..."
pkill -f streaming_server 2>/dev/null || true
pkill -f mediamtx          2>/dev/null || true
# Kill any FFmpeg orphaned by a previous run (matched on our FIFO input path so we
# don't touch unrelated ffmpeg processes).
pkill -f "ffmpeg.*/tmp/camera_stream.h264" 2>/dev/null || true
sleep 2

# ---- 1. Create the camera FIFO ------------------------------------------------
# The frame sender writes raw H.264 here; FFmpeg reads it and remuxes to mediaMTX.
# It must exist before streaming_server's config probe runs.
CAMERA_FIFO="/tmp/camera_stream.h264"
rm -f "$CAMERA_FIFO"
mkfifo "$CAMERA_FIFO"
echo "[start] Created camera FIFO: $CAMERA_FIFO"

# ---- 2. Cleanup trap ----------------------------------------------------------
cleanup() {
    echo
    echo "[start] Stopping..."
    rm -f "$CAMERA_FIFO"
}
trap cleanup EXIT

# ---- 3. Run the C server in the foreground ------------------------------------
# It brings up mediaMTX + FFmpeg + the WebSocket server + the DDL bridge (sensors,
# servos, Orin ZeroMQ receiver) + the frame sender, then serves the app. Ctrl+C
# sends SIGINT, which streaming_server handles by tearing down all its children.
echo "[start] Launching streaming_server (Ctrl+C to stop the whole pipeline)..."
./streaming_server "$@"
