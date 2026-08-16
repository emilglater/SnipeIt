#!/bin/bash
###############################################
# Stream Video to mediaMTX
# 
# Usage: ./stream_video.sh /path/to/video.mp4
#
# This script streams an MP4 file to mediaMTX
# at real-time speed with PTS preservation.
###############################################

set -e

# Check arguments
if [ $# -lt 1 ]; then
    echo "Usage: $0 <video_file> [stream_name]"
    echo ""
    echo "Arguments:"
    echo "  video_file   - Path to the MP4 file to stream"
    echo "  stream_name  - Optional: RTSP stream name (default: 'stream')"
    echo ""
    echo "Example:"
    echo "  $0 /home/pi/videos/test.mp4"
    echo "  $0 /home/pi/videos/test.mp4 mystream"
    exit 1
fi

VIDEO_FILE="$1"
STREAM_NAME="${2:-stream}"
RTSP_URL="rtsp://localhost:8554/${STREAM_NAME}"

# Verify video file exists
if [ ! -f "$VIDEO_FILE" ]; then
    echo "Error: Video file not found: $VIDEO_FILE"
    exit 1
fi

# Verify FFmpeg is installed
if ! command -v ffmpeg &> /dev/null; then
    echo "Error: FFmpeg is not installed"
    echo "Install with: sudo apt update && sudo apt install ffmpeg"
    exit 1
fi

# Get video information
echo "=========================================="
echo "  Video Streaming"
echo "=========================================="
echo ""
echo "Video file: $VIDEO_FILE"
echo "Stream URL: $RTSP_URL"
echo ""

# Show video info
echo "Video information:"
ffprobe -v quiet -show_entries format=duration,bit_rate -show_entries stream=codec_name,width,height,r_frame_rate -of default=noprint_wrappers=1 "$VIDEO_FILE" 2>/dev/null | head -10
echo ""

echo "Starting stream... (Press Ctrl+C to stop)"
echo ""

# Stream the video
# -re          : Read at native framerate (real-time playback speed)
# -i           : Input file
# -c copy      : Copy codecs without re-encoding (preserves PTS)
# -f rtsp      : Output format
# -rtsp_transport tcp : Use TCP for RTSP (more reliable)
ffmpeg -re \                                                                                                                      
      -i "$VIDEO_FILE" \                                                                                                            
      -c:v libx264 -preset ultrafast -tune zerolatency \                                                                            
      -g 30 -keyint_min 30 \                                                                                                        
      -c:a aac -b:a 128k \                                                                                                          
      -f rtsp \                                                                                                                     
      -rtsp_transport tcp \                                                                                                         
      "$RTSP_URL" 

echo ""
echo "Stream ended."