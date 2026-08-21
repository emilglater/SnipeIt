# Makefile for Pi Streaming System
#
# Usage:
#   make              - Build the streaming server into bin/
#   make all          - Build everything
#   make test_orin    - Build and run the Orin-module unit tests
#   make clean        - Remove build/ and bin/
#
# Build output is kept out of the source tree: object files (plus the .d
# dependency files gcc generates) go to build/, executables to bin/. Both are
# gitignored.

CC      = gcc
CFLAGS  = -Wall -Wextra -g -O2 -D_GNU_SOURCE -Isrc -Iorin -Iacoustic -I../src
LDFLAGS = -L../bin/debug -lsnipeit -lwebsockets -lm -pthread -luring -lgpiod -lfftw3f -lasound -lzmq

# -MMD -MP make gcc emit a .d file per object listing the headers it included,
# so touching a header rebuilds exactly the objects that use it. This replaces
# the hand-maintained header list the old Makefile had to keep in sync.
DEPFLAGS = -MMD -MP

ZMQ_LIBS   = -lzmq
GST_CFLAGS = $(shell pkg-config --cflags gstreamer-1.0 2>/dev/null)
GST_LIBS   = $(shell pkg-config --libs gstreamer-1.0 2>/dev/null)

BUILD_DIR = build
BIN_DIR   = bin

# Source files linked into the streaming server.
SRCS = src/main.c src/websocket_server.c src/process_manager.c src/config.c \
       src/ddl_bridge.c \
       acoustic/acoustic_bridge.c acoustic/acoustic_config.c acoustic/ring_buffer.c \
       acoustic/onset_detector.c acoustic/gcc_phat.c acoustic/srp_phat.c \
       acoustic/alsa_capture.c \
       orin/pose_ring.c orin/detection_msg.c orin/aiming.c orin/orin_receiver.c \
       orin/sei_frame_id.c orin/frame_sender.c orin/tracker.c

# Objects mirror the source directory structure inside build/.
OBJS = $(SRCS:%.c=$(BUILD_DIR)/%.o)
DEPS = $(OBJS:.o=.d)

SERVER      = $(BIN_DIR)/streaming_server
SNIPEIT_LIB = ../bin/debug/libsnipeit.a

# Convenience names so `make streaming_server` / `make test_tracker` still work
# without typing the bin/ prefix.
.PHONY: all clean help streaming_server test_orin \
        test_pose_ring test_detection_msg test_aiming test_sei_frame_id \
        test_tracker test_orin_receiver \
        orin_sender_demo orin_sei_capture orin_rtp_stream orin_live_loop

all: streaming_server

streaming_server:   $(SERVER)
test_pose_ring:     $(BIN_DIR)/test_pose_ring
test_detection_msg: $(BIN_DIR)/test_detection_msg
test_aiming:        $(BIN_DIR)/test_aiming
test_sei_frame_id:  $(BIN_DIR)/test_sei_frame_id
test_tracker:       $(BIN_DIR)/test_tracker
test_orin_receiver: $(BIN_DIR)/test_orin_receiver
orin_sender_demo:   $(BIN_DIR)/orin_sender_demo
orin_sei_capture:   $(BIN_DIR)/orin_sei_capture
orin_rtp_stream:    $(BIN_DIR)/orin_rtp_stream
orin_live_loop:     $(BIN_DIR)/orin_live_loop

$(BIN_DIR):
	mkdir -p $@

# --- Streaming server -------------------------------------------------------

# Build the static library via the root Makefile.
$(SNIPEIT_LIB):
	$(MAKE) -C .. lib

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(GST_CFLAGS) $(DEPFLAGS) -c $< -o $@

$(SERVER): $(OBJS) $(SNIPEIT_LIB) | $(BIN_DIR)
	$(CC) $(CFLAGS) $(GST_CFLAGS) -o $@ $(OBJS) $(LDFLAGS) $(GST_LIBS)
	@echo ""
	@echo "========================================"
	@echo "  Build complete: $(SERVER)"
	@echo "========================================"
	@echo ""
	@echo "Before running, make sure to:"
	@echo "  1. Check orin_host / orin_rtp_port in config/streaming_config.json"
	@echo "  2. Start the detector on the Orin (it replies over ZeroMQ)"
	@echo ""
	@echo "To run:"
	@echo "  ./script/start.sh [config_file]   (creates the camera FIFO, then runs the server)"
	@echo ""

# --- Unit tests -------------------------------------------------------------
# Each test compiles its own sources directly rather than reusing build/*.o:
# they are deliberately self-contained (no libsnipeit, no GStreamer), which is
# what lets them run on a host without the full dependency set.

# Pi<->Orin frame_id -> pose ring buffer. Needs pose_ring.c + pthread only.
$(BIN_DIR)/test_pose_ring: test/test_pose_ring.c orin/pose_ring.c orin/pose_ring.h | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ test/test_pose_ring.c orin/pose_ring.c -pthread
	@echo "Build complete: $@"

# Orin detection-message JSON parser. Needs detection_msg.c + libm (lround).
$(BIN_DIR)/test_detection_msg: test/test_detection_msg.c orin/detection_msg.c orin/detection_msg.h | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ test/test_detection_msg.c orin/detection_msg.c -lm
	@echo "Build complete: $@"

# bbox+pose -> servo command geometry. servo_config.h macros come via -I../src.
$(BIN_DIR)/test_aiming: test/test_aiming.c orin/aiming.c orin/aiming.h | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ test/test_aiming.c orin/aiming.c -lm
	@echo "Build complete: $@"

# H.265 SEI frame_id carrier. Pure bytes, no external deps.
$(BIN_DIR)/test_sei_frame_id: test/test_sei_frame_id.c orin/sei_frame_id.c orin/sei_frame_id.h | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ test/test_sei_frame_id.c orin/sei_frame_id.c
	@echo "Build complete: $@"

# Angular-space tracker (needs aiming + libm).
$(BIN_DIR)/test_tracker: test/test_tracker.c orin/tracker.c orin/aiming.c \
                         orin/tracker.h orin/aiming.h orin/detection_msg.h orin/pose_ring.h | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ test/test_tracker.c orin/tracker.c orin/aiming.c -lm
	@echo "Build complete: $@"

# Integration test: real ZeroMQ PUSH->PULL round-trip through the receiver.
# Needs libzmq3-dev (sudo apt install libzmq3-dev).
$(BIN_DIR)/test_orin_receiver: test/test_orin_receiver.c orin/orin_receiver.c \
                               orin/detection_msg.c orin/pose_ring.c \
                               orin/orin_receiver.h orin/detection_msg.h orin/pose_ring.h | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ test/test_orin_receiver.c orin/orin_receiver.c \
	      orin/detection_msg.c orin/pose_ring.c $(ZMQ_LIBS) -lm -pthread
	@echo "Build complete: $@"

# All Orin-module tests that DON'T need zmq.
test_orin: test_pose_ring test_detection_msg test_aiming test_sei_frame_id test_tracker
	@echo "Running pose_ring + detection_msg + aiming + sei + tracker tests..."
	$(BIN_DIR)/test_pose_ring && $(BIN_DIR)/test_detection_msg && \
	$(BIN_DIR)/test_aiming && $(BIN_DIR)/test_sei_frame_id && $(BIN_DIR)/test_tracker

# --- Demos ------------------------------------------------------------------
# Need the GStreamer dev packages. The frame sender itself is part of the
# streaming server; these exercise it standalone, without the full service.

# Camera-less smoke test of the H.265 frame sender + per-frame SEI tagging.
$(BIN_DIR)/orin_sender_demo: demo/sender_demo.c orin/frame_sender.c orin/sei_frame_id.c \
                             orin/frame_sender.h orin/sei_frame_id.h | $(BIN_DIR)
	$(CC) $(CFLAGS) $(GST_CFLAGS) -o $@ \
	      demo/sender_demo.c orin/frame_sender.c orin/sei_frame_id.c $(GST_LIBS)
	@echo "Build complete: $@  (run: $@ [seconds])"

# Capture a short SEI-tagged H.265 elementary stream to a file, for the Orin
# side to validate NVDEC-decode + SEI extraction offline.
$(BIN_DIR)/orin_sei_capture: demo/sei_capture_demo.c orin/frame_sender.c orin/sei_frame_id.c \
                             orin/frame_sender.h orin/sei_frame_id.h | $(BIN_DIR)
	$(CC) $(CFLAGS) $(GST_CFLAGS) -o $@ \
	      demo/sei_capture_demo.c orin/frame_sender.c orin/sei_frame_id.c $(GST_LIBS)
	@echo "Build complete: $@  (run: $@ <out.hevc> [seconds])"

# Live RTP/H.265+SEI streamer to the Orin (real camera by default).
$(BIN_DIR)/orin_rtp_stream: demo/rtp_stream_demo.c orin/frame_sender.c orin/sei_frame_id.c \
                            orin/frame_sender.h orin/sei_frame_id.h | $(BIN_DIR)
	$(CC) $(CFLAGS) $(GST_CFLAGS) -o $@ \
	      demo/rtp_stream_demo.c orin/frame_sender.c orin/sei_frame_id.c $(GST_LIBS)
	@echo "Build complete: $@  (run: $@ [host] [port] [seconds] [camera|test])"

# Full single-process live loop: sender + PULL receiver sharing a pose ring.
# Needs BOTH GStreamer and ZeroMQ. Proves transport + SEI echo + pose-join +
# aiming end to end with the Orin.
$(BIN_DIR)/orin_live_loop: demo/live_loop_demo.c orin/frame_sender.c orin/sei_frame_id.c \
                           orin/orin_receiver.c orin/detection_msg.c orin/pose_ring.c \
                           orin/aiming.c \
                           orin/frame_sender.h orin/sei_frame_id.h orin/orin_receiver.h \
                           orin/detection_msg.h orin/pose_ring.h orin/aiming.h | $(BIN_DIR)
	$(CC) $(CFLAGS) $(GST_CFLAGS) -o $@ \
	      demo/live_loop_demo.c orin/frame_sender.c orin/sei_frame_id.c \
	      orin/orin_receiver.c orin/detection_msg.c orin/pose_ring.c orin/aiming.c \
	      $(GST_LIBS) $(ZMQ_LIBS) -lm -pthread
	@echo "Build complete: $@  (run: $@ [orin_host] [rtp_port] [zmq_bind] [seconds] [camera|test])"

# --- Static analysis --------------------------------------------------------
# Mirrors the root Makefile's cppcheck setup with two deliberate differences:
#
#  1. No MISRA addon. MISRA C:2012 over GStreamer/libwebsockets/pthread code
#     produces hundreds of findings that can only be suppressed, not fixed.
#     The value here is --enable=all catching real defects.
#  2. No --safety / --check-level=exhaustive / --report-type. Those need
#     cppcheck >= 2.11; Debian bookworm ships 2.10. (The root `make lint`
#     currently fails on this machine for exactly that reason.)
#
# The threadsafety addon is kept: this code is multi-threaded, so it earns its
# place. It flags every function-local static, so genuinely single-threaded
# ones carry an inline suppression stating which thread owns them.

CPPCHECK_DIR   = $(BUILD_DIR)/cppcheck
CPPCHECK_FLAGS  = --enable=all
CPPCHECK_FLAGS += --inconclusive
CPPCHECK_FLAGS += --error-exitcode=1
CPPCHECK_FLAGS += --inline-suppr
CPPCHECK_FLAGS += --suppress=missingIncludeSystem
CPPCHECK_FLAGS += --suppress=unmatchedSuppression
# usleep is formally obsolescent in POSIX.2008 but universally available on
# Linux, and the sleeps here are coarse (100 ms) supervision waits.
CPPCHECK_FLAGS += --suppress=usleepCalled
# Module APIs are checked one directory at a time, so an entry point used only
# by the streaming server (or not yet used) looks unused. Not a useful signal.
CPPCHECK_FLAGS += --suppress=unusedFunction
CPPCHECK_FLAGS += --language=c
CPPCHECK_FLAGS += --std=c11
CPPCHECK_FLAGS += --library=posix.cfg
CPPCHECK_FLAGS += --platform=../script/type_sizes.xml
CPPCHECK_FLAGS += --relative-paths=.
CPPCHECK_FLAGS += --addon=../script/threadsafety.json
CPPCHECK_FLAGS += -Isrc -Iorin -Iacoustic -I../src
CPPCHECK_FLAGS += --cppcheck-build-dir=$(CPPCHECK_DIR)
CPPCHECK_FLAGS += -j1

# legacy/ is deliberately excluded: it is retired code kept for reference.
LINT_DIRS = src orin acoustic test demo

.PHONY: lint
lint: | $(CPPCHECK_DIR)
	cppcheck $(CPPCHECK_FLAGS) $(LINT_DIRS)
	@echo "Lint clean"

$(CPPCHECK_DIR):
	mkdir -p $@

# --- Housekeeping -----------------------------------------------------------

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)
	rm -f /tmp/mediamtx.log /tmp/ffmpeg.log
	@echo "Clean complete"

help:
	@echo "Pi Streaming System - Makefile"
	@echo ""
	@echo "Targets:"
	@echo "  all              - Build all targets (default)"
	@echo "  streaming_server - Build the streaming server into bin/"
	@echo "  test_orin        - Build and run the Orin-module unit tests"
	@echo "  clean            - Remove build/ and bin/"
	@echo "  help             - Show this help"
	@echo ""
	@echo "Quick start:"
	@echo "  1. make"
	@echo "  2. Edit config/streaming_config.json (orin_host, orin_rtp_port)"
	@echo "  3. Start the detector on the Orin"
	@echo "  4. ./script/start.sh"
	@echo "  5. Connect Android app"

# Header dependencies discovered by -MMD. Must come last; the leading '-'
# ignores them on a clean tree where no .d files exist yet.
-include $(DEPS)
