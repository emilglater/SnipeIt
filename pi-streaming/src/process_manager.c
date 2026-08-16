#include "process_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

void pm_init(ProcessManager *pm)
{
    *pm = (ProcessManager){
        .mediamtx_pid = 0,
        .ffmpeg_pid = 0,
        .mediamtx_running = false,
        .ffmpeg_running = false
    };
}

int pm_start_mediamtx(ProcessManager *pm, const StreamingConfig *config)
{
    if (pm->mediamtx_running)
    {
        printf("[PM] mediaMTX is already running (PID: %d)\n", pm->mediamtx_pid);
        return 0;
    }
    
    printf("[PM] Starting mediaMTX...\n");
    
    pid_t pid = fork();
    
    if (pid < 0)
    {
        perror("[PM] fork failed for mediaMTX");
        return -1;
    }
    
    if (pid == 0)
    {
        // Child process - exec mediaMTX
        
        // Redirect stdout/stderr to /dev/null or log file
        // (mediaMTX is quite verbose)
        freopen("/tmp/mediamtx.log", "w", stdout);
        freopen("/tmp/mediamtx.log", "a", stderr);
        
        // Execute mediaMTX
        execl(config->mediamtx_path, 
              config->mediamtx_path,
              config->mediamtx_config,
              (char *)NULL);
        
        // If exec fails
        perror("[PM] execl mediaMTX failed");
        _exit(127);
    }
    
    // Parent process
    pm->mediamtx_pid = pid;
    pm->mediamtx_running = true;
    
    printf("[PM] mediaMTX started (PID: %d)\n", pm->mediamtx_pid);
    
    return 0;
}

void pm_stop_mediamtx(ProcessManager *pm)
{
    if (!pm->mediamtx_running || pm->mediamtx_pid <= 0)
    {
        return;
    }
    
    printf("[PM] Stopping mediaMTX (PID: %d)...\n", pm->mediamtx_pid);
    
    // Send SIGTERM first (graceful shutdown).  SIGCHLD is SIG_IGN (see main.c),
    // so children are auto-reaped and waitpid() would return ECHILD here — poll
    // liveness with kill(pid, 0) instead (0 = alive, ESRCH = gone).
    if (kill(pm->mediamtx_pid, SIGTERM) == 0)
    {
        // Wait up to 3 seconds for graceful exit
        int wait_count = 0;
        while (wait_count < 30)
        {
            if (kill(pm->mediamtx_pid, 0) != 0 && errno == ESRCH)
            {
                // Process exited
                printf("[PM] mediaMTX stopped gracefully\n");
                pm->mediamtx_pid = 0;
                pm->mediamtx_running = false;
                return;
            }

            usleep(100000); // 100ms
            wait_count++;
        }

        // Still running, force kill
        printf("[PM] mediaMTX not responding, sending SIGKILL\n");
        kill(pm->mediamtx_pid, SIGKILL);
    }

    pm->mediamtx_pid = 0;
    pm->mediamtx_running = false;
    printf("[PM] mediaMTX stopped\n");
}

bool pm_is_mediamtx_running(ProcessManager *pm)
{
    pm_check_processes(pm);
    return pm->mediamtx_running;
}

int pm_start_ffmpeg(ProcessManager *pm, const StreamingConfig *config)
{
    if (pm->ffmpeg_running)
    {
        printf("[PM] FFmpeg is already running (PID: %d)\n", pm->ffmpeg_pid);
        return 0;
    }
    
    printf("[PM] Starting FFmpeg stream...\n");
    
    pid_t pid = fork();
    
    if (pid < 0)
    {
        perror("[PM] fork failed for FFmpeg");
        return -1;
    }
    
    if (pid == 0)
    {
        // Child process - exec FFmpeg
        
        // Redirect stdout/stderr to log file
        freopen("/tmp/ffmpeg.log", "w", stdout);
        freopen("/tmp/ffmpeg.log", "a", stderr);
        
        // Build RTSP URL
        char rtsp_url[256];
        snprintf(rtsp_url, sizeof(rtsp_url), "rtsp://localhost:%d/%s",
                 config->rtsp_port, config->rtsp_stream_name);
        
        // Choose FFmpeg command based on input source type.
        if (config_is_fifo(config->video_path))
        {
            // Live camera: picamera2 hardware encoder writes raw H.264 Annex-B to the
            // FIFO at real-time rate.  FFmpeg just remuxes into RTSP — no re-encode,
            // so CPU usage is minimal and latency is as low as possible.
            // repeat=true in picamera2 ensures SPS/PPS precedes every IDR frame,
            // so late-joining Android clients never see a green screen.
            //
            // -fflags +nobuffer keeps output latency low (no read-ahead before
            // forwarding frames).  probesize/analyzeduration are NOT set tiny:
            // raw H.264 carries no container header, so FFmpeg must read until it
            // sees an SPS to learn the frame size.  With -probesize 32 /
            // -analyzeduration 0 it frequently gave up first ("Could not find
            // codec parameters ... unspecified size" → "Output file #0 does not
            // contain any stream") and exited, which on reconnect cascaded into a
            // dead stream.  These are upper bounds (FFmpeg stops as soon as it has
            // the info), so they add no steady-state latency; 1 MB / 1 s is ample
            // to capture the first SPS+IDR (SPS/PPS repeat before every IDR).
            // Build the video framerate string from config
            char fps_str[16];
            snprintf(fps_str, sizeof(fps_str), "%.0f", config->video_fps > 0 ? config->video_fps : 30.0);

            execl("/usr/bin/ffmpeg", "ffmpeg",
                  "-probesize", "10000000",              // Up to 10 MB so byte budget is never the limit
                  "-analyzeduration", "2000000",         // Up to 2 s to find stream params (≥2 SPS intervals)
                  "-fflags", "+nobuffer",                // No input read-ahead buffering
                  "-use_wallclock_as_timestamps", "1",   // Stamp each packet with real wall-clock
                  "-r", fps_str,                         // Input frame rate (helps demuxer timing)
                  "-f", "h264",                          // Raw H.264 elementary stream
                  "-i", config->video_path,              // Named pipe written by picamera2
                  "-c:v", "copy",                        // Remux only — no re-encode
                  "-an",
                  "-f", "rtsp",
                  "-rtsp_transport", "tcp",
                  rtsp_url,
                  (char *)NULL);
        }
        else if (config->loop_video)
        {
            execl("/usr/bin/ffmpeg", "ffmpeg",
                  "-re",                        // Read at native framerate
                  "-stream_loop", "-1",         // Loop infinitely
                  "-i", config->video_path,     // Input file
                  "-c:v", "libx264",            // Re-encode video with H.264
                  "-profile:v", "baseline",     // Baseline profile for Android
                  "-level", "4.0",              // Level 4.0 for 1080p compatibility
                  "-preset", "ultrafast",       // Fastest encoding (low CPU)
                  "-tune", "zerolatency",       // Minimize latency
                  "-pix_fmt", "yuv420p",        // Explicit pixel format for compatibility
                  "-g", "30",                   // Keyframe every 30 frames (~1 sec)
                  "-keyint_min", "30",          // Minimum keyframe interval
                  "-b:v", "5M",                 // Video bitrate (some decoders need this)
                  "-maxrate", "6M",             // Max bitrate
                  "-bufsize", "10M",            // Buffer size
                  "-an",                        // No audio (source has none)
                  "-f", "rtsp",                 // Output format
                  "-rtsp_transport", "tcp",     // Use TCP for RTSP
                  rtsp_url,                     // Output URL
                  (char *)NULL);
        }
        else
        {
            execl("/usr/bin/ffmpeg", "ffmpeg",
                  "-re",                        // Read at native framerate
                  "-i", config->video_path,     // Input file
                  "-c:v", "libx264",            // Re-encode video with H.264
                  "-profile:v", "baseline",     // Baseline profile for Android
                  "-level", "4.0",              // Level 4.0 for 1080p compatibility
                  "-preset", "ultrafast",       // Fastest encoding (low CPU)
                  "-tune", "zerolatency",       // Minimize latency
                  "-pix_fmt", "yuv420p",        // Explicit pixel format for compatibility
                  "-g", "30",                   // Keyframe every 30 frames (~1 sec)
                  "-keyint_min", "30",          // Minimum keyframe interval
                  "-b:v", "5M",                 // Video bitrate (some decoders need this)
                  "-maxrate", "6M",             // Max bitrate
                  "-bufsize", "10M",            // Buffer size
                  "-an",                        // No audio (source has none)
                  "-f", "rtsp",                 // Output format
                  "-rtsp_transport", "tcp",     // Use TCP for RTSP
                  rtsp_url,                     // Output URL
                  (char *)NULL);
        }
        
        // If exec fails
        perror("[PM] execl ffmpeg failed");
        _exit(127);
    }
    
    // Parent process
    pm->ffmpeg_pid = pid;
    pm->ffmpeg_running = true;
    
    printf("[PM] FFmpeg started (PID: %d), streaming to RTSP port %d\n", 
           pm->ffmpeg_pid, config->rtsp_port);
    
    return 0;
}

void pm_stop_ffmpeg(ProcessManager *pm)
{
    if (!pm->ffmpeg_running || pm->ffmpeg_pid <= 0)
    {
        return;
    }
    
    printf("[PM] Stopping FFmpeg (PID: %d)...\n", pm->ffmpeg_pid);
    
    // Send SIGTERM first.  SIGCHLD is SIG_IGN (see main.c), so children are
    // auto-reaped and waitpid() would return ECHILD here — poll liveness with
    // kill(pid, 0) instead (0 = alive, ESRCH = gone).
    if (kill(pm->ffmpeg_pid, SIGTERM) == 0)
    {
        // Wait up to 2 seconds
        int wait_count = 0;
        while (wait_count < 20)
        {
            if (kill(pm->ffmpeg_pid, 0) != 0 && errno == ESRCH)
            {
                printf("[PM] FFmpeg stopped gracefully\n");
                pm->ffmpeg_pid = 0;
                pm->ffmpeg_running = false;
                return;
            }

            usleep(100000); // 100ms
            wait_count++;
        }

        // Force kill
        printf("[PM] FFmpeg not responding, sending SIGKILL\n");
        kill(pm->ffmpeg_pid, SIGKILL);
    }

    pm->ffmpeg_pid = 0;
    pm->ffmpeg_running = false;
    printf("[PM] FFmpeg stopped\n");
}

bool pm_is_ffmpeg_running(ProcessManager *pm)
{
    pm_check_processes(pm);
    return pm->ffmpeg_running;
}

void pm_check_processes(ProcessManager *pm)
{
    // SIGCHLD is SIG_IGN (see main.c), so children are auto-reaped and waitpid()
    // can't be used to detect a self-exit (it returns ECHILD).  Probe liveness
    // with kill(pid, 0): 0 means alive, ESRCH means the process is gone.  This
    // means we can't recover the exit code/signal, only the fact of exit.

    // Check mediaMTX
    if (pm->mediamtx_pid > 0 && kill(pm->mediamtx_pid, 0) != 0 && errno == ESRCH)
    {
        printf("[PM] mediaMTX exited\n");
        pm->mediamtx_pid = 0;
        pm->mediamtx_running = false;
    }

    // Check FFmpeg
    if (pm->ffmpeg_pid > 0 && kill(pm->ffmpeg_pid, 0) != 0 && errno == ESRCH)
    {
        printf("[PM] FFmpeg exited\n");
        pm->ffmpeg_pid = 0;
        pm->ffmpeg_running = false;
    }
}

void pm_cleanup(ProcessManager *pm)
{
    pm_stop_ffmpeg(pm);
    pm_stop_mediamtx(pm);
    printf("[PM] Cleanup complete\n");
}

int pm_wait_for_mediamtx_ready(const StreamingConfig *config, int timeout_sec)
{
    printf("[PM] Waiting for mediaMTX to be ready on port %d...\n", config->rtsp_port);
    
    int sock;
    struct sockaddr_in addr;
    
    addr.sin_family = AF_INET;
    addr.sin_port = htons(config->rtsp_port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    
    for (int i = 0; i < timeout_sec * 10; i++)
    {
        sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0)
        {
            usleep(100000); // 100ms
            continue;
        }
        
        int result = connect(sock, (struct sockaddr*)&addr, sizeof(addr));
        close(sock);
        
        if (result == 0)
        {
            printf("[PM] mediaMTX is ready\n");
            return 0;
        }
        
        usleep(100000); // 100ms
    }
    
    fprintf(stderr, "[PM] Timeout waiting for mediaMTX\n");
    return -1;
}