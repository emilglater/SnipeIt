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

#include "config.h"
#include "unix_socket.h"
#include "ddl_bridge.h"
#include "websocket_server.h"
#include "process_manager.h"
#include "acoustic_bridge.h"

#define DDL_BRIDGE_INTERVAL_MS 1000
#define LWS_WAKER_INTERVAL_MS  20   /* kick the lws poll this often -> loop runs at ~50 Hz */

// Global state for signal handler
static volatile bool g_running = true;

// Application state
typedef struct
{
    StreamingConfig config;
    IPCConnection ipc;
    WebSocketServer ws;
    ProcessManager pm;
    DdlBridge* bridge;
    bool python_connected;
    bool android_connected;
    bool streaming_active;
    AcousticBridge* acoustic_bridge;
} AppState;

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
static bool start_camera_stream(AppState *app)
{
    if (app->streaming_active)
        return true;
    if (!app->python_connected)
        return false;

    printf("[MAIN] Starting live camera pipeline...\n");

    if (pm_is_ffmpeg_running(&app->pm))
        pm_stop_ffmpeg(&app->pm);

    // Fresh FIFO inode so FFmpeg reads a clean, SPS-led stream with no leftover
    // data or stale writer from a previous session.
    unlink(app->config.video_path);
    if (mkfifo(app->config.video_path, 0666) != 0)
    {
        perror("[MAIN] mkfifo failed to recreate camera FIFO");
        return false;
    }

    const int MAX_FFMPEG_TRIES = 4;
    for (int attempt = 1; attempt <= MAX_FFMPEG_TRIES; attempt++)
    {
        if (pm_start_ffmpeg(&app->pm, &app->config) != 0)
        {
            fprintf(stderr, "[MAIN] Failed to start FFmpeg\n");
            ipc_send_stop(&app->ipc);
            return false;
        }

        // Give FFmpeg time to exec and reach the FIFO open() call.
        usleep(500000);  // 500ms

        if (attempt == 1)
        {
            // Bring up the camera (once).  On later attempts it is already running
            // and feeding the FIFO, so we only restart FFmpeg against it.
            if (ipc_send_start(&app->ipc,
                               app->config.video_path,         // FIFO path Python writes
                               app->config.video_duration_sec,
                               app->config.video_fps,
                               app->config.loop_video,
                               app->config.detection_frame_interval) != 0)
            {
                fprintf(stderr, "[MAIN] Failed to send START to Python\n");
                pm_stop_ffmpeg(&app->pm);
                return false;
            }
        }

        // Wait for camera init + FFmpeg's probe (analyzeduration is 2 s).
        printf("[MAIN] Waiting for camera and stream to initialise (attempt %d/%d)...\n",
               attempt, MAX_FFMPEG_TRIES);
        usleep(3000000);  // 3 s > FFmpeg's 2 s analyzeduration, so the probe has resolved

        // FFmpeg still alive after the probe window => it found the stream and is
        // publishing.  If it exited, the probe failed -> retry.
        if (pm_is_ffmpeg_running(&app->pm))
        {
            app->streaming_active = true;
            printf("[MAIN] Live camera pipeline is publishing to mediaMTX\n");
            return true;
        }

        printf("[MAIN] FFmpeg exited before publishing (probe failure); "
               "retrying against the live camera...\n");
    }

    fprintf(stderr, "[MAIN] FFmpeg never started publishing after %d attempts\n",
            MAX_FFMPEG_TRIES);
    ipc_send_stop(&app->ipc);
    return false;
}

// On-demand start for a *file* source: Python opens the file, then FFmpeg reads
// it.  (Unlike the camera, a file stream is started when an app connects and
// stopped when it disconnects -- there is no point streaming a file to nobody.)
static bool start_file_stream(AppState *app)
{
    if (app->streaming_active)
        return true;
    if (!app->python_connected)
        return false;

    printf("[MAIN] Starting file stream...\n");

    if (pm_is_ffmpeg_running(&app->pm))
        pm_stop_ffmpeg(&app->pm);

    if (ipc_send_start(&app->ipc,
                       app->config.video_path,
                       app->config.video_duration_sec,
                       app->config.video_fps,
                       app->config.loop_video,
                       app->config.detection_frame_interval) != 0)
    {
        fprintf(stderr, "[MAIN] Failed to send START to Python\n");
        return false;
    }

    usleep(100000);  // 100ms for Python to open the video file

    if (pm_start_ffmpeg(&app->pm, &app->config) != 0)
    {
        fprintf(stderr, "[MAIN] Failed to start FFmpeg\n");
        ipc_send_stop(&app->ipc);
        return false;
    }

    printf("[MAIN] Waiting for stream to initialise...\n");
    usleep(1000000);  // 1 second for FFmpeg to start publishing
    app->streaming_active = true;
    return true;
}

// Callback when Android connects via WebSocket
static void on_android_connect(void *user_data)
{
    AppState *app = (AppState *)user_data;

    printf("[MAIN] Android client connected!\n");
    app->android_connected = true;

    if (!app->python_connected)
    {
        fprintf(stderr, "[MAIN] Python not connected yet; cannot serve stream\n");
        return;
    }

    // For the live camera the pipeline is already running (started at start-up),
    // so this is just "tell the app it's ready".  start_camera_stream() returns
    // immediately when already streaming, and also recovers if the start-up
    // attempt had failed.  File sources start on demand here.
    bool ok = config_is_fifo(app->config.video_path)
                  ? start_camera_stream(app)
                  : start_file_stream(app);

    if (ok)
        send_stream_ready(app);
    else
        fprintf(stderr, "[MAIN] Stream not available; not sending stream_ready\n");
}

// Callback when Android disconnects
static void on_android_disconnect(void *user_data)
{
    AppState *app = (AppState *)user_data;

    printf("[MAIN] Android client disconnected\n");
    app->android_connected = false;

    // Live camera: keep the pipeline running so the stream stays up and the next
    // connection is instant (and we avoid the FFmpeg/camera restart races that
    // caused black screens).  File source: stop -- no point streaming to nobody.
    if (!config_is_fifo(app->config.video_path) && app->streaming_active)
    {
        printf("[MAIN] Stopping file stream...\n");
        if (app->python_connected)
            ipc_send_stop(&app->ipc);
        pm_stop_ffmpeg(&app->pm);
        app->streaming_active = false;
        printf("[MAIN] File stream stopped, waiting for new connection\n");
    }
}

// Forward detection data from Python to Android
static void forward_detection(AppState *app, const char *json, size_t len)
{
    if (app->android_connected)
    {
        if (ws_send_json(&app->ws, json, len) != 0)
        {
            fprintf(stderr, "[MAIN] Failed to forward detection to Android\n");
        }
        // Note: ws_send_json queues messages - they are flushed by the next
        // ws_service() call at the bottom of the main loop.
    }
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
        .python_connected = false,
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

    // Initialize Unix socket IPC
    printf("[MAIN] Initializing IPC server...\n");
    if (ipc_server_init(&app.ipc) != 0)
    {
        fprintf(stderr, "[MAIN] Failed to initialize IPC server\n");
        g_running = false;
        pthread_join(waker_tid, NULL);
        ws_cleanup(&app.ws);
        pm_cleanup(&app.pm);
        return 1;
    }

    // Wait for Python detection script to connect
    printf("[MAIN] Waiting for Python detection script to connect...\n");
    printf("[MAIN] (Run 'python3 detection.py' in another terminal)\n\n");

    if (ipc_accept_client(&app.ipc) != 0)
    {
        fprintf(stderr, "[MAIN] Failed to accept Python connection\n");
        g_running = false;
        pthread_join(waker_tid, NULL);
        ipc_cleanup(&app.ipc);
        ws_cleanup(&app.ws);
        pm_cleanup(&app.pm);
        return 1;
    }
    app.python_connected = true;

    // Bring the live camera pipeline up NOW and keep it running for the whole
    // session, independent of Android connect/disconnect.  Starting it once (and
    // verifying FFmpeg is actually publishing) is what makes a fresh run + first
    // connect reliable: by the time an app connects, the stream is already live,
    // so connecting just sends stream_ready -- no per-connect FFmpeg probe race.
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
    char msg_buffer[MAX_MSG_SIZE];
    int total_processed = 0;  // Total detection messages processed

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

        /* 1. Drain detection messages from Python (non-blocking) and queue them
         *    for the Android app. */
        if (app.python_connected && ipc_check_client_connected(&app.ipc))
        {
            int len;
            int msg_count = 0;
            int batch_limit = 10;  // Read up to 10 messages per batch

            while (batch_limit-- > 0 &&
                   (len = ipc_recv_message(&app.ipc, msg_buffer, sizeof(msg_buffer))) > 0)
            {
                msg_count++;
                total_processed++;
                forward_detection(&app, msg_buffer, len);   // queues for Android
            }

            if (msg_count > 0)
            {
                printf("[MAIN] Processed %d detection messages (total: %d)\n",
                       msg_count, total_processed);
            }

            if (len < 0)
            {
                // Python disconnected
                printf("[MAIN] Python detection script disconnected\n");
                printf("[MAIN] Total detection messages processed: %d\n", total_processed);
                app.python_connected = false;

                if (app.streaming_active)
                {
                    pm_stop_ffmpeg(&app.pm);
                    app.streaming_active = false;
                }
            }
        }

        /* 2. Emit a sensor_data frame (gated to once per period_ms inside). */
        ddl_bridge_tick(app.bridge);

        /* 3. Emit an acoustic event if one is pending (early-returns otherwise). */
        acoustic_bridge_tick(app.acoustic_bridge);

        /* 4. File-source "playback ended" detection.  Must NOT apply to the live
         *    camera/FIFO source -- there FFmpeg exiting is an error, not normal
         *    end-of-stream, and sending STOP here would tear down the live camera
         *    session (and could hit Python while it is blocked opening the FIFO). */
        if (app.streaming_active && !app.config.loop_video &&
            !config_is_fifo(app.config.video_path))
        {
            pm_check_processes(&app.pm);

            if (!pm_is_ffmpeg_running(&app.pm))
            {
                printf("[MAIN] Video playback ended\n");
                if (app.python_connected)
                {
                    ipc_send_stop(&app.ipc);
                }
                app.streaming_active = false;
            }
        }

        /* 5. Service libwebsockets: flushes any messages queued in steps 1-3 and
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

    // Stop streaming if active
    if (app.streaming_active)
    {
        if (app.python_connected)
        {
            ipc_send_stop(&app.ipc);
        }
        pm_stop_ffmpeg(&app.pm);
    }

    acoustic_bridge_stop(app.acoustic_bridge);
    ddl_bridge_stop(app.bridge);

    ipc_cleanup(&app.ipc);
    ws_cleanup(&app.ws);
    pm_cleanup(&app.pm);

    printf("[MAIN] Shutdown complete\n");
    return 0;
}