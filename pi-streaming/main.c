/*
    Pi Streaming Server - Main Orchestrator

    This program coordinates all components:
    - mediaMTX (RTSP server)
    - FFmpeg (video streaming)
    - WebSocket server (Android communication)
    - Unix socket IPC (Python detection communication)

    Flow:
    1. Load configuration
    2. Start mediaMTX
    3. Initialize WebSocket server
    4. Initialize Unix socket IPC
    5. Wait for Python detection script to connect
    6. Wait for Android app to connect (WebSocket)
    7. On Android connect: Start FFmpeg, send START to Python
    8. Forward detection data from Python to Android
    9. On Android disconnect: Stop FFmpeg, send STOP to Python
    10. Loop back to step 6

    --------------------------------------------------------------------------
    TIMING FIX (the reason this file changed):

    libwebsockets >= 3.2 IGNORES the timeout argument to lws_service().  The call
    blocks inside poll() until a socket event happens, using lws's own internal
    timeout (which can be ~30 s on an idle link).  Because the main loop is the
    only thing servicing lws, that made the ENTIRE loop -- IPC reads, the sensor
    bridge, everything -- stall until the Android app happened to send a packet.
    That is why sensor data arrived in ragged 10-20 s bursts instead of every 1 s.

    The fix: a dedicated "waker" thread calls lws_cancel_service() every 20 ms.
    That is the ONLY lws function documented as safe to call from another thread;
    it writes a byte into lws's internal event pipe, forcing the poll() inside
    lws_service() to return.  The main loop therefore wakes and runs a full
    iteration at ~50 Hz, completely independent of WebSocket traffic.
    --------------------------------------------------------------------------
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>
#include <stdbool.h>
#include <sys/stat.h>   // mkfifo
#include <sys/types.h>
#include <time.h>
#include <pthread.h>
#include <libwebsockets.h>
#include <gst/gst.h>

#include "config.h"
#include "ddl_bridge.h"
#include "websocket_server.h"
#include "process_manager.h"
#include "acoustic_bridge.h"
#include "orin/frame_sender.h"

#define DDL_BRIDGE_INTERVAL_MS 1000
#define LWS_WAKER_INTERVAL_MS  20   /* kick the lws poll this often -> loop runs at ~50 Hz */

// Global state for signal handler
static volatile bool g_running = true;

// Application state
typedef struct
{
    StreamingConfig config;
    WebSocketServer ws;
    ProcessManager pm;
    DdlBridge* bridge;
    bool android_connected;
    bool streaming_active;
    AcousticBridge* acoustic_bridge;
    FrameSender* sender;   // Orin H.265+SEI sender + H.264 app-preview (camera owner)
} AppState;

// Fired by the frame sender on the capture thread when a frame is assigned a
// frame_id (before encode). Record the capture-time servo pose so a returning
// Orin detection can be joined back to where the camera was pointing.
static void on_sender_frame_captured(uint32_t frame_id, void *user)
{
    ddl_bridge_record_capture_pose((DdlBridge *)user, frame_id);
}

// ---------------------------------------------------------------------------
// Waker thread: forces lws_service()'s poll() to return every LWS_WAKER_INTERVAL_MS
// so the main loop never stalls waiting for WebSocket traffic.  lws_cancel_service()
// is the only lws call that is safe from a thread other than the service thread.
// ---------------------------------------------------------------------------
static void *lws_waker(void *arg)
{
    struct lws_context *ctx = (struct lws_context *)arg;
    while (g_running)
    {
        usleep(LWS_WAKER_INTERVAL_MS * 1000);
        lws_cancel_service(ctx);   // wakes the poll inside ws_service() on the main thread
    }
    return NULL;
}

// Signal handler for clean shutdown
static void signal_handler(int sig)
{
    (void)sig;
    printf("\n[MAIN] Received shutdown signal\n");
    g_running = false;
}

// Tell the app the (already-live) stream is ready to pull.
static void send_stream_ready(AppState *app)
{
    char ready_msg[256];
    snprintf(ready_msg, sizeof(ready_msg),
             "{\"event\":\"stream_ready\",\"rtsp_port\":%d,\"stream_name\":\"%s\"}",
             app->config.rtsp_port,
             app->config.rtsp_stream_name);
    ws_send(&app->ws, ready_msg);
    ws_service(&app->ws, 0);  // Flush immediately
    printf("[MAIN] Sent stream_ready to Android (rtsp://<PI_IP>:%d/%s)\n",
           app->config.rtsp_port, app->config.rtsp_stream_name);
}

// Bring up the live camera pipeline (FFmpeg + picamera2) and VERIFY FFmpeg is
// actually publishing to mediaMTX before returning success.
//
// This is started ONCE at program start-up and then kept running for the whole
// session -- it is deliberately NOT tied to Android connect/disconnect.  Doing
// the start/stop per connection was the root of the intermittent black screens:
// FFmpeg probing a freshly-(re)started raw-H.264 FIFO is not reliable, and a
// blind "sleep then assume success" sent stream_ready even when FFmpeg had died
// on the probe ("Output file #0 does not contain any stream").
//
// Reliability comes from (a) a fresh FIFO inode (clean SPS-led stream, no stale
// writer/data), and (b) a verify-and-retry loop: start FFmpeg, let the camera
// come up, then confirm FFmpeg is still alive (= it caught an SPS and is
// publishing).  If it died, restart it against the now-warm camera (which emits
// an SPS every ~1 s) and try again.  Returns true only once FFmpeg is confirmed
// publishing.
// Build and start the Orin frame sender: it OWNS the camera and fans the 1080p
// capture out two ways — H.265+SEI over RTP to the Orin, and a software H.264
// "app preview" written into the FIFO the app's FFmpeg reads. It also records
// the capture-time servo pose per frame (on_sender_frame_captured) so returning
// Orin detections join back to the right pose. Replaces person_streamer.py.
static bool start_frame_sender(AppState *app)
{
    if (app->sender != NULL)
        return true;

    FrameSenderConfig cfg;
    frame_sender_config_default(&cfg);
    cfg.source            = (strcmp(app->config.camera_source, "test") == 0)
                                ? FRAME_SENDER_SOURCE_VIDEOTEST
                                : FRAME_SENDER_SOURCE_LIBCAMERA;
    cfg.width             = 1920;
    cfg.height            = 1080;
    cfg.fps               = 30;
    /* Keep 2 of the 4 cores free: x265 at superfast on all cores pinned the
     * CPU at 0% idle, starving hostapd/mediaMTX/net softirq — the app's video
     * froze even though the local FIFO->FFmpeg path kept 30 fps. ultrafast on
     * a 2-core pool costs little Orin fps and leaves the system responsive. */
    cfg.speed_preset      = "ultrafast";
    /* Deterministic 12-frame closed GOP: the Orin can only start (or recover)
     * decoding at a keyframe, so the cadence must be pinned — scenecut off,
     * closed GOP — not left to x265's scene-adaptive insertion. 12 frames is
     * ~5 s worst-case blind time at the measured ~2.4 fps encode rate (keyint
     * counts FRAMES, so wall-clock shrinks when light/fps improves). Keyframe
     * bitrate cost is ~+4%. See orin/HANDOFF_PI_GOP_FIX.md. */
    cfg.key_int_max       = 12;
    cfg.x265_extra        = "pools=2:keyint=12:min-keyint=12:scenecut=0:open-gop=0";
    cfg.orin_branch       = app->config.orin_enabled;
    cfg.sink              = FRAME_SENDER_SINK_RTP_UDP;
    cfg.host              = app->config.orin_host;
    cfg.port              = app->config.orin_rtp_port;
    cfg.app_preview       = true;
    cfg.app_fifo_path     = app->config.video_path;   // the FIFO FFmpeg reads
    cfg.app_width         = app->config.app_preview_width;
    cfg.app_height        = app->config.app_preview_height;
    cfg.app_fps           = app->config.app_preview_fps;
    cfg.app_bitrate_kbps  = app->config.app_preview_bitrate_kbps;
    cfg.app_speed_preset  = app->config.app_preview_preset;
    cfg.on_frame_captured = on_sender_frame_captured;
    cfg.user              = app->bridge;

    app->sender = frame_sender_start(&cfg);
    if (app->sender == NULL)
    {
        fprintf(stderr, "[MAIN] frame sender failed to start\n");
        return false;
    }
    if (cfg.orin_branch)
        printf("[MAIN] Frame sender up: %s -> H.264 FIFO + H.265+SEI RTP %s:%d\n",
               cfg.source == FRAME_SENDER_SOURCE_VIDEOTEST ? "videotest" : "camera",
               cfg.host, cfg.port);
    else
        printf("[MAIN] Frame sender up: %s -> H.264 FIFO only (Orin branch DISABLED"
               " via orin_enabled=false)\n",
               cfg.source == FRAME_SENDER_SOURCE_VIDEOTEST ? "videotest" : "camera");
    return true;
}

static void stop_frame_sender(AppState *app)
{
    if (app->sender != NULL)
    {
        frame_sender_stop(app->sender);
        app->sender = NULL;
    }
}

// FFmpeg prints "Stream mapping:" (default loglevel) only after the input
// probe succeeded AND the RTSP output was opened — i.e. it is really
// publishing. "Still alive" is NOT enough: on a starved FIFO FFmpeg blocks
// inside the probe read() indefinitely — running, publishing nothing.
static bool ffmpeg_reports_publishing(void)
{
    FILE *f = fopen("/tmp/ffmpeg.log", "r");
    if (f == NULL)
        return false;
    char line[512];
    bool found = false;
    while (fgets(line, sizeof(line), f) != NULL)
    {
        if (strstr(line, "Stream mapping:") != NULL)
        {
            found = true;
            break;
        }
    }
    fclose(f);
    return found;
}

// Bring up the live camera pipeline (FFmpeg reading the FIFO + the frame sender
// writing it) and VERIFY FFmpeg is actually publishing to mediaMTX before
// returning. Started ONCE at start-up and kept running for the whole session
// (not tied to Android connect/disconnect) — same reliability rationale as the
// old picamera2 path: a fresh FIFO inode + a verify-and-retry loop. On a probe
// failure we fully restart BOTH ends (FFmpeg + sender) against a fresh FIFO so
// there is never a half-open FIFO (a dead reader would SIGPIPE the sender).
static bool start_camera_stream(AppState *app)
{
    if (app->streaming_active)
        return true;

    printf("[MAIN] Starting live camera pipeline (frame sender -> FIFO + Orin)...\n");

    const int MAX_TRIES = 4;
    for (int attempt = 1; attempt <= MAX_TRIES; attempt++)
    {
        // Clean slate: tear down both ends and give the FIFO a fresh inode.
        stop_frame_sender(app);
        if (pm_is_ffmpeg_running(&app->pm))
            pm_stop_ffmpeg(&app->pm);
        unlink(app->config.video_path);
        if (mkfifo(app->config.video_path, 0666) != 0)
        {
            perror("[MAIN] mkfifo failed to recreate camera FIFO");
            return false;
        }

        // FFmpeg first: it opens the FIFO O_RDONLY and blocks until a writer
        // (the sender's filesink) opens the write end — the rendezvous.
        if (pm_start_ffmpeg(&app->pm, &app->config) != 0)
        {
            fprintf(stderr, "[MAIN] Failed to start FFmpeg\n");
            return false;
        }
        usleep(500000);  // let FFmpeg exec and reach the FIFO open()

        // Sender opens the write end -> both unblock; H.264 flows into the FIFO
        // and H.265+SEI flows to the Orin.
        if (!start_frame_sender(app))
        {
            pm_stop_ffmpeg(&app->pm);
            return false;
        }

        printf("[MAIN] Waiting for the stream to initialise (attempt %d/%d)...\n",
               attempt, MAX_TRIES);

        // Poll up to 8 s (probe needs ~2 s of stream data) for POSITIVE proof
        // of publishing — FFmpeg's "Stream mapping:" in its log. FFmpeg merely
        // being alive proves nothing: on a starved FIFO it blocks in the probe
        // forever. If it exits, the probe failed -> full restart.
        for (int waited_ms = 0; waited_ms < 8000; waited_ms += 250)
        {
            if (!pm_is_ffmpeg_running(&app->pm))
                break;
            if (ffmpeg_reports_publishing())
            {
                app->streaming_active = true;
                printf("[MAIN] Live camera pipeline is publishing to mediaMTX\n");
                return true;
            }
            usleep(250000);
        }

        if (pm_is_ffmpeg_running(&app->pm))
            printf("[MAIN] FFmpeg alive but not publishing after 8 s — camera "
                   "starved? Restarting both ends...\n");
        else
            printf("[MAIN] FFmpeg exited before publishing (probe failure); "
                   "restarting both ends...\n");
    }

    fprintf(stderr, "[MAIN] Camera pipeline never published after %d attempts\n",
            MAX_TRIES);
    stop_frame_sender(app);
    return false;
}

// Callback when Android connects via WebSocket
static void on_android_connect(void *user_data)
{
    AppState *app = (AppState *)user_data;

    printf("[MAIN] Android client connected!\n");
    app->android_connected = true;

    // The live camera pipeline is already running (started at start-up), so this
    // just tells the app it's ready. start_camera_stream() returns immediately
    // when already streaming, and recovers if the start-up attempt had failed.
    if (start_camera_stream(app))
        send_stream_ready(app);
    else
        fprintf(stderr, "[MAIN] Stream not available; not sending stream_ready\n");
}
// Callback when the Android app sends a command via WebSocket.
// Runs on the main thread inside ws_service(), so calling into the bridge /
// event bus / servo set_target here is safe — the bridge synchronises its
// own state and ddl_servo_set_target takes the servo target mutex.
static void on_android_command(const char *payload, size_t len, void *user_data)
{
    AppState *app = (AppState *)user_data;
    ddl_bridge_handle_command(app->bridge, payload, len);
}

// Callback when Android disconnects
static void on_android_disconnect(void *user_data)
{
    AppState *app = (AppState *)user_data;

    printf("[MAIN] Android client disconnected\n");
    app->android_connected = false;

    // The live camera pipeline keeps running so the stream stays up and the next
    // connection is instant (avoiding the FFmpeg/camera restart races that caused
    // black screens). Detections come from the Orin regardless of app presence.
}

static void print_usage(const char *program)
{
    printf("Usage: %s [config_file]\n", program);
    printf("\n");
    printf("Arguments:\n");
    printf("  config_file   Path to JSON configuration file\n");
    printf("                (default: ./streaming_config.json)\n");
    printf("\n");
    printf("Example:\n");
    printf("  %s\n", program);
    printf("  %s /path/to/config.json\n", program);
}

int main(int argc, char *argv[])
{
    setvbuf(stdout, NULL, _IONBF, 0);  // Unbuffered stdout for real-time logs
    setvbuf(stderr, NULL, _IONBF, 0);  // Unbuffered stderr for real-time error logs
    fprintf(stderr, "[BUILD] compiled %s %s\n", __DATE__, __TIME__);

    // Initialize GStreamer for the Orin frame sender (camera -> H.265+SEI -> Orin,
    // and the H.264 app-preview branch -> FIFO).
    gst_init(&argc, &argv);

    const char *config_path = DEFAULT_CONFIG_PATH;

    // Parse command line arguments
    if (argc > 2)
    {
        print_usage(argv[0]);
        return 1;
    }

    if (argc == 2)
    {
        if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)
        {
            print_usage(argv[0]);
            return 0;
        }
        config_path = argv[1];
    }

    printf("==========================================\n");
    printf("  Pi Streaming Server\n");
    printf("==========================================\n\n");

    // Initialize application state
    AppState app = {
        .android_connected = false,
        .streaming_active = false
    };

    // Set up signal handlers for clean shutdown
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    // Note: SIGCHLD handler set after config_probe_video() to allow pclose() to work

    // Load configuration
    printf("[MAIN] Loading configuration from %s\n", config_path);
    if (config_load(&app.config, config_path) != 0)
    {
        fprintf(stderr, "[MAIN] Failed to load configuration\n");
        return 1;
    }

    // Probe video file
    if (config_probe_video(&app.config) != 0)
    {
        fprintf(stderr, "[MAIN] Failed to probe video file\n");
        return 1;
    }

    // Validate configuration
    if (config_validate(&app.config) != 0)
    {
        fprintf(stderr, "[MAIN] Configuration validation failed\n");
        return 1;
    }

    config_print(&app.config);

    // Now set SIGCHLD to ignore to prevent zombie processes from child processes
    // (mediaMTX, FFmpeg). This must be after config_probe_video() which uses pclose().
    signal(SIGCHLD, SIG_IGN);

    // Initialize process manager
    pm_init(&app.pm);

    // Start mediaMTX
    printf("[MAIN] Starting mediaMTX...\n");
    if (pm_start_mediamtx(&app.pm, &app.config) != 0)
    {
        fprintf(stderr, "[MAIN] Failed to start mediaMTX\n");
        return 1;
    }

    // Wait for mediaMTX to be ready
    if (pm_wait_for_mediamtx_ready(&app.config, 10) != 0)
    {
        fprintf(stderr, "[MAIN] mediaMTX not ready\n");
        pm_cleanup(&app.pm);
        return 1;
    }

    // Initialize WebSocket server
    printf("[MAIN] Initializing WebSocket server on port %d...\n", app.config.websocket_port);
    if (ws_init(&app.ws, app.config.websocket_port) != 0)
    {
        fprintf(stderr, "[MAIN] Failed to initialize WebSocket server\n");
        pm_cleanup(&app.pm);
        return 1;
    }

    ws_set_callbacks(&app.ws, on_android_connect, on_android_disconnect, &app);
    ws_set_command_callback(&app.ws, on_android_command);

    // Initialize DDL bridge (starts the DDL snapshot refresh loop / AO threads)
    app.bridge = ddl_bridge_start(&app.ws, DDL_BRIDGE_INTERVAL_MS);
    if (app.bridge == NULL)
    {
        fprintf(stderr, "[MAIN] Failed to start DDL bridge\n");
        ws_cleanup(&app.ws);
        pm_cleanup(&app.pm);
        return 1;
    }

    // Start the waker thread now that the lws context exists.  THIS is what keeps
    // the main loop running at ~50 Hz regardless of WebSocket traffic (see the
    // big comment at the top of the file for the full explanation).
    pthread_t waker_tid;
    if (pthread_create(&waker_tid, NULL, lws_waker, app.ws.context) != 0)
    {
        fprintf(stderr, "[MAIN] Failed to start lws waker thread\n");
        ddl_bridge_stop(app.bridge);
        ws_cleanup(&app.ws);
        pm_cleanup(&app.pm);
        return 1;
    }

    // Initialize Acoustic bridge -- NON-FATAL: if I2S isn't available, the
    // bridge returns NULL and we continue without acoustic localization.
    app.acoustic_bridge = acoustic_bridge_start(&app.ws);
    if (app.acoustic_bridge == NULL)
    {
        fprintf(stderr, "[MAIN] WARNING: acoustic bridge failed to start; "
                        "continuing without acoustic localization\n");
        // intentionally NOT returning 1 -- rest of system must run
    }

    // Detection comes from the Orin over ZeroMQ (handled in the DDL bridge) — the
    // local Python detector + its Unix-socket IPC are retired (see legacy/edgetpu).

    // Bring the live camera pipeline up NOW and keep it running for the whole
    // session, independent of Android connect/disconnect. The frame sender owns
    // the camera: H.264 preview -> FIFO -> FFmpeg -> mediaMTX -> app, and
    // H.265+SEI -> RTP -> Orin. Starting it once (and verifying FFmpeg is
    // publishing) is what makes a fresh run + first connect reliable.
    if (config_is_fifo(app.config.video_path))
    {
        if (!start_camera_stream(&app))
        {
            fprintf(stderr, "[MAIN] WARNING: camera pipeline failed to start at "
                            "boot; will retry when an app connects\n");
        }
    }

    printf("\n[MAIN] System ready!\n");
    printf("[MAIN] Waiting for Android app to connect via WebSocket...\n");
    printf("[MAIN] Android should connect to: ws://<PI_IP>:%d\n", app.config.websocket_port);
    printf("[MAIN] Video stream will be at: rtsp://<PI_IP>:%d/%s\n\n",
           app.config.rtsp_port, app.config.rtsp_stream_name);

    // ---- Main event loop ------------------------------------------------------
    // Pacing: ws_service() at the bottom blocks inside lws's poll() until either a
    // socket event arrives OR the waker thread cancels it (~20 ms).  So one loop
    // iteration is ~20 ms.  No usleep() is needed -- ws_service IS the pacer -- and
    // the loop is no longer gated on the Android app sending packets.
    while (g_running)
    {
        /* ITER-DIAG: stays SILENT on a healthy run.  With the waker, every
         * iteration is ~20 ms, so we only log if one takes longer than 200 ms,
         * i.e. a real stall.  No [ITER-DIAG] output == the fix is working. */
        static struct timespec prev_iter = {0, 0};
        struct timespec now_iter;
        clock_gettime(CLOCK_MONOTONIC, &now_iter);
        if (prev_iter.tv_sec != 0)
        {
            long since_us = (long)(now_iter.tv_sec  - prev_iter.tv_sec ) * 1000000L
                          + (long)(now_iter.tv_nsec - prev_iter.tv_nsec) / 1000L;
            if (since_us > 200000L)   /* only log iterations slower than 200 ms */
            {
                fprintf(stderr, "[ITER-DIAG] iteration gap was %ld us\n", since_us);
            }
        }
        prev_iter = now_iter;

        /* 1. Emit a sensor_data frame (gated to once per period_ms inside). */
        ddl_bridge_tick(app.bridge);

        /* 2. Forward any Orin detections queued by the receiver thread to the
         *    app (target_detection JSON). Cheap when the queue is empty. */
        ddl_bridge_pump_detections(app.bridge);

        /* 3. Emit an acoustic event if one is pending (early-returns otherwise). */
        acoustic_bridge_tick(app.acoustic_bridge);

        /* 4. Service libwebsockets: flushes any messages queued above and
         *    processes incoming WS data.  Blocks until the waker cancels it
         *    (~20 ms) or a socket event arrives.  lws ignores the timeout value;
         *    the waker thread provides the real pacing. */
        ws_service(&app.ws, LWS_WAKER_INTERVAL_MS);
    }

    // ---- Cleanup --------------------------------------------------------------
    printf("\n[MAIN] Shutting down...\n");

    /* Stop the waker BEFORE destroying the lws context, otherwise it would call
     * lws_cancel_service() on freed memory and crash on shutdown.  g_running is
     * already false here (set by the signal handler), so the waker exits within
     * one LWS_WAKER_INTERVAL_MS. */
    pthread_join(waker_tid, NULL);

    // Stop the sender FIRST (it drains EOS and closes the FIFO write end, and
    // its on_frame_captured callback calls into the DDL bridge — so it must stop
    // before ddl_bridge_stop below), then FFmpeg.
    stop_frame_sender(&app);
    if (app.streaming_active)
    {
        pm_stop_ffmpeg(&app.pm);
    }

    acoustic_bridge_stop(app.acoustic_bridge);
    ddl_bridge_stop(app.bridge);

    ws_cleanup(&app.ws);
    pm_cleanup(&app.pm);

    printf("[MAIN] Shutdown complete\n");
    return 0;
}