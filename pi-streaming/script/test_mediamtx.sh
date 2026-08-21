#!/bin/bash
###############################################
# mediaMTX Installation Test Script
###############################################

set -e  # Exit on error

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR/.."   # run from pi-streaming/, where the mediamtx binary lives

echo "=========================================="
echo "  mediaMTX Installation Test"
echo "=========================================="

# Check if mediamtx binary exists
echo ""
echo "[1/5] Checking mediaMTX binary..."
if [ -f "./mediamtx" ]; then
    echo "  ✓ mediamtx binary found"
else
    echo "  ✗ mediamtx binary not found!"
    echo "  Run these commands to download:"
    echo "    wget https://github.com/bluenviron/mediamtx/releases/download/v1.9.3/mediamtx_v1.9.3_linux_arm64v8.tar.gz"
    echo "    tar -xzf mediamtx_v1.9.3_linux_arm64v8.tar.gz"
    exit 1
fi

# Check if config file exists
echo ""
echo "[2/5] Checking configuration file..."
if [ -f "./config/mediamtx.yml" ]; then
    echo "  ✓ mediamtx.yml found"
else
    echo "  ✗ mediamtx.yml not found!"
    exit 1
fi

# Check if FFmpeg is installed
echo ""
echo "[3/5] Checking FFmpeg installation..."
if command -v ffmpeg &> /dev/null; then
    FFMPEG_VERSION=$(ffmpeg -version | head -n1)
    echo "  ✓ FFmpeg found: $FFMPEG_VERSION"
else
    echo "  ✗ FFmpeg not found!"
    echo "  Install with: sudo apt update && sudo apt install ffmpeg"
    exit 1
fi

# Check if port 8554 is available
echo ""
echo "[4/5] Checking port 8554 availability..."
if netstat -tuln 2>/dev/null | grep -q ":8554 " || ss -tuln 2>/dev/null | grep -q ":8554 "; then
    echo "  ✗ Port 8554 is already in use!"
    echo "  Check what's using it: sudo lsof -i :8554"
    exit 1
else
    echo "  ✓ Port 8554 is available"
fi

# Test starting mediaMTX
echo ""
echo "[5/5] Testing mediaMTX startup..."
./mediamtx ./config/mediamtx.yml &
MEDIAMTX_PID=$!
sleep 3

# Check if it's still running
if kill -0 $MEDIAMTX_PID 2>/dev/null; then
    echo "  ✓ mediaMTX started successfully (PID: $MEDIAMTX_PID)"
    
    # Check if port is now listening
    if ss -tuln 2>/dev/null | grep -q ":8554 "; then
        echo "  ✓ RTSP server listening on port 8554"
    else
        echo "  ⚠ Port 8554 not detected (may need more time)"
    fi
    
    # Cleanup
    echo ""
    echo "Stopping test instance..."
    kill $MEDIAMTX_PID 2>/dev/null
    wait $MEDIAMTX_PID 2>/dev/null || true
    echo "  ✓ mediaMTX stopped"
else
    echo "  ✗ mediaMTX failed to start!"
    echo "  Check the output above for errors"
    exit 1
fi

echo ""
echo "=========================================="
echo "  All tests passed!"
echo "=========================================="
echo ""
echo "To test video streaming manually:"
echo ""
echo "  Terminal 1 - Start mediaMTX:"
echo "    ./mediamtx ./config/mediamtx.yml"
echo ""
echo "  Terminal 2 - Stream a test video:"
echo "    ffmpeg -re -f lavfi -i testsrc=duration=60:size=640x480:rate=30 \\"
echo "           -c:v libx264 -preset ultrafast -tune zerolatency \\"
echo "           -f rtsp rtsp://localhost:8554/stream"
echo ""
echo "  Terminal 3 (or another device) - Play the stream:"
echo "    ffplay rtsp://<PI_IP_ADDRESS>:8554/stream"
echo ""
echo "  Or connect your Android app to: rtsp://<PI_IP_ADDRESS>:8554/stream"
echo ""