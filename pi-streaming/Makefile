# Makefile for Pi Streaming System
#
# Usage:
#   make              - Build main streaming server
#   make all          - Build everything
#   make clean        - Remove built files

CC      = gcc
CFLAGS  = -Wall -Wextra -g -O2 -D_GNU_SOURCE -I. -I../src -Iacoustic
LDFLAGS = -L../bin/debug -lsnipeit -lwebsockets -lm -pthread -luring -lgpiod -lfftw3f -lasound -lzmq
# Source files
SRCS = main.c websocket_server.c process_manager.c config.c ddl_bridge.c \
       acoustic/acoustic_bridge.c acoustic/acoustic_config.c acoustic/ring_buffer.c \
       acoustic/onset_detector.c acoustic/gcc_phat.c acoustic/srp_phat.c \
       acoustic/alsa_capture.c \
       orin/pose_ring.c orin/detection_msg.c orin/aiming.c orin/orin_receiver.c \
       orin/sei_frame_id.c orin/frame_sender.c orin/tracker.c
HDRS = websocket_server.h process_manager.h config.h ddl_bridge.h \
       acoustic/acoustic_bridge.h acoustic/acoustic_config.h acoustic/ring_buffer.h \
       acoustic/onset_detector.h acoustic/gcc_phat.h acoustic/srp_phat.h \
       acoustic/alsa_capture.h \
       orin/pose_ring.h orin/detection_msg.h orin/aiming.h orin/orin_receiver.h \
       orin/sei_frame_id.h orin/frame_sender.h orin/tracker.h

SNIPEIT_LIB = ../bin/debug/libsnipeit.a

.PHONY: all clean help

all: streaming_server

# Standalone unit test for the Pi<->Orin frame_id -> pose ring buffer.
# Self-contained: only needs pose_ring.c + pthread, no libsnipeit.
test_pose_ring: orin/test_pose_ring.c orin/pose_ring.c orin/pose_ring.h
	$(CC) $(CFLAGS) -o $@ orin/test_pose_ring.c orin/pose_ring.c -pthread
	@echo "Build complete: test_pose_ring  (run: ./test_pose_ring)"

# Standalone unit test for the Orin detection-message JSON parser.
# Self-contained: only needs detection_msg.c + libm (lround).
test_detection_msg: orin/test_detection_msg.c orin/detection_msg.c orin/detection_msg.h
	$(CC) $(CFLAGS) -o $@ orin/test_detection_msg.c orin/detection_msg.c -lm
	@echo "Build complete: test_detection_msg  (run: ./test_detection_msg)"

# Standalone unit test for the bbox+pose -> servo command geometry.
# Self-contained: aiming.c + libm (servo_config.h macros come via -I../src).
test_aiming: orin/test_aiming.c orin/aiming.c orin/aiming.h
	$(CC) $(CFLAGS) -o $@ orin/test_aiming.c orin/aiming.c -lm
	@echo "Build complete: test_aiming  (run: ./test_aiming)"

# Standalone unit test for the H.265 SEI frame_id carrier. Pure bytes.
test_sei_frame_id: orin/test_sei_frame_id.c orin/sei_frame_id.c orin/sei_frame_id.h
	$(CC) $(CFLAGS) -o $@ orin/test_sei_frame_id.c orin/sei_frame_id.c
	@echo "Build complete: test_sei_frame_id  (run: ./test_sei_frame_id)"

# Standalone unit test for the angular-space tracker (needs aiming + libm).
test_tracker: orin/test_tracker.c orin/tracker.c orin/aiming.c \
              orin/tracker.h orin/aiming.h orin/detection_msg.h orin/pose_ring.h
	$(CC) $(CFLAGS) -o $@ orin/test_tracker.c orin/tracker.c orin/aiming.c -lm
	@echo "Build complete: test_tracker  (run: ./test_tracker)"

# --- ZeroMQ-dependent targets ----------------------------------------------
# These need libzmq3-dev (sudo apt install libzmq3-dev). Kept OUT of the
# default `all`/streaming_server build so the system still builds on a host
# without the headers. Once the Orin receiver is wired into main.c, move
# orin/orin_receiver.c into SRCS and add -lzmq to LDFLAGS.
ZMQ_LIBS = -lzmq

# Integration test: real PUSH->PULL round-trip through the receiver.
test_orin_receiver: orin/test_orin_receiver.c orin/orin_receiver.c \
                    orin/detection_msg.c orin/pose_ring.c \
                    orin/orin_receiver.h orin/detection_msg.h orin/pose_ring.h
	$(CC) $(CFLAGS) -o $@ orin/test_orin_receiver.c orin/orin_receiver.c \
	      orin/detection_msg.c orin/pose_ring.c $(ZMQ_LIBS) -lm -pthread
	@echo "Build complete: test_orin_receiver  (run: ./test_orin_receiver)"

# --- GStreamer-dependent target (frame sender, protocol item 1) -------------
# Needs the GStreamer dev packages. Kept OUT of the default build until the
# sender is wired into streaming_server.
GST_CFLAGS = $(shell pkg-config --cflags gstreamer-1.0 2>/dev/null)
GST_LIBS   = $(shell pkg-config --libs gstreamer-1.0 2>/dev/null)

# Camera-less smoke test of the H.265 frame sender + per-frame SEI tagging.
orin_sender_demo: orin/sender_demo.c orin/frame_sender.c orin/sei_frame_id.c \
                  orin/frame_sender.h orin/sei_frame_id.h
	$(CC) $(CFLAGS) $(GST_CFLAGS) -o $@ \
	      orin/sender_demo.c orin/frame_sender.c orin/sei_frame_id.c $(GST_LIBS)
	@echo "Build complete: orin_sender_demo  (run: ./orin_sender_demo [seconds])"

# Capture a short SEI-tagged H.265 elementary stream to a file, for the Orin
# side to validate NVDEC-decode + SEI extraction offline.
orin_sei_capture: orin/sei_capture_demo.c orin/frame_sender.c orin/sei_frame_id.c \
                  orin/frame_sender.h orin/sei_frame_id.h
	$(CC) $(CFLAGS) $(GST_CFLAGS) -o $@ \
	      orin/sei_capture_demo.c orin/frame_sender.c orin/sei_frame_id.c $(GST_LIBS)
	@echo "Build complete: orin_sei_capture  (run: ./orin_sei_capture <out.hevc> [seconds])"

# Live RTP/H.265+SEI streamer to the Orin (real camera by default).
orin_rtp_stream: orin/rtp_stream_demo.c orin/frame_sender.c orin/sei_frame_id.c \
                 orin/frame_sender.h orin/sei_frame_id.h
	$(CC) $(CFLAGS) $(GST_CFLAGS) -o $@ \
	      orin/rtp_stream_demo.c orin/frame_sender.c orin/sei_frame_id.c $(GST_LIBS)
	@echo "Build complete: orin_rtp_stream  (run: ./orin_rtp_stream [host] [port] [seconds] [camera|test])"

# Full single-process live loop: sender + PULL receiver sharing a pose ring.
# Needs BOTH GStreamer and ZeroMQ. Proves transport + SEI echo + pose-join +
# aiming end to end with the Orin.
orin_live_loop: orin/live_loop_demo.c orin/frame_sender.c orin/sei_frame_id.c \
                orin/orin_receiver.c orin/detection_msg.c orin/pose_ring.c \
                orin/aiming.c \
                orin/frame_sender.h orin/sei_frame_id.h orin/orin_receiver.h \
                orin/detection_msg.h orin/pose_ring.h orin/aiming.h
	$(CC) $(CFLAGS) $(GST_CFLAGS) -o $@ \
	      orin/live_loop_demo.c orin/frame_sender.c orin/sei_frame_id.c \
	      orin/orin_receiver.c orin/detection_msg.c orin/pose_ring.c orin/aiming.c \
	      $(GST_LIBS) $(ZMQ_LIBS) -lm -pthread
	@echo "Build complete: orin_live_loop  (run: ./orin_live_loop [orin_host] [rtp_port] [zmq_bind] [seconds] [camera|test])"

# All Orin-module tests that DON'T need zmq.
test_orin: test_pose_ring test_detection_msg test_aiming test_sei_frame_id test_tracker
	@echo "Running pose_ring + detection_msg + aiming + sei + tracker tests..."
	./test_pose_ring && ./test_detection_msg && ./test_aiming && ./test_sei_frame_id && ./test_tracker

# Build the static library via the root Makefile.
$(SNIPEIT_LIB):
	$(MAKE) -C .. lib

streaming_server: $(SRCS) $(HDRS) $(SNIPEIT_LIB)
	$(CC) $(CFLAGS) $(GST_CFLAGS) -o $@ $(SRCS) $(LDFLAGS) $(GST_LIBS)
	@echo ""
	@echo "========================================"
	@echo "  Build complete: streaming_server"
	@echo "========================================"
	@echo ""
	@echo "Before running, make sure to:"
	@echo "  1. Check orin_host / orin_rtp_port in streaming_config.json"
	@echo "  2. Start the detector on the Orin (it replies over ZeroMQ)"
	@echo ""
	@echo "To run:"
	@echo "  ./start.sh [config_file]     (creates the camera FIFO, then runs the server)"
	@echo ""

# Clean build artifacts
clean:
	rm -f streaming_server test_pose_ring test_detection_msg test_aiming test_sei_frame_id test_tracker test_orin_receiver orin_sender_demo orin_sei_capture orin_rtp_stream orin_live_loop
	rm -f *.o orin/*.o
	rm -f /tmp/mediamtx.log /tmp/ffmpeg.log
	@echo "Clean complete"

# Help
help:
	@echo "Pi Streaming System - Makefile"
	@echo ""
	@echo "Targets:"
	@echo "  all              - Build all targets (default)"
	@echo "  streaming_server - Build main streaming server"
	@echo "  test_orin        - Build and run the Orin-module unit tests"
	@echo "  clean            - Remove built files"
	@echo "  help             - Show this help"
	@echo ""
	@echo "Quick start:"
	@echo "  1. make"
	@echo "  2. Edit streaming_config.json (orin_host, orin_rtp_port)"
	@echo "  3. Start the detector on the Orin"
	@echo "  4. ./start.sh"
	@echo "  5. Connect Android app"