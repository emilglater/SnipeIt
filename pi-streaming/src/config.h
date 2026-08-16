/*
    Configuration File Handler

    This module handles reading configuration from a JSON file.
    Configuration includes video path, loop settings, ports, etc.
 */

#ifndef CONFIG_H
#define CONFIG_H

/* Standard Libraries */
#include <stdbool.h>

// Default configuration file path
#define DEFAULT_CONFIG_PATH "./config/streaming_config.json"

// Maximum path length
#define MAX_PATH_LENGTH 512

typedef struct
{
    char video_path[MAX_PATH_LENGTH];       // Path to the video file
    char mediamtx_path[MAX_PATH_LENGTH];    // Path to mediaMTX binary
    char mediamtx_config[MAX_PATH_LENGTH];  // Path to mediaMTX config file
    int websocket_port;                     // WebSocket server port (default: 8555)
    int rtsp_port;                          // RTSP server port (default: 8554)
    char rtsp_stream_name[64];              // RTSP stream name (default: "stream")
    bool loop_video;                        // Whether to loop the video
    int detection_frame_interval;           // Process every Nth frame (default: 5)
    double video_duration_sec;              // Video duration in seconds (auto-detected)
    double video_fps;                       // Video FPS (auto-detected)
    int video_width;                        // Video width (auto-detected)
    int video_height;                       // Video height (auto-detected)

    // --- Orin frame sender (replaces the Python camera+detector) ---
    bool orin_enabled;        // false: skip the H.265/Orin branch entirely —
                              // single x264 encoder, app preview only (default true)
    char orin_host[64];       // Orin IP for RTP/H.265+SEI (default "10.42.0.2")
    int  orin_rtp_port;       // Orin UDP port (default 5600)
    char camera_source[16];   // "camera" (libcamera) or "test" (videotestsrc) — validation
    int  app_preview_width;   // app H.264 preview width  (0 = same as capture, 1920)
    int  app_preview_height;  // app H.264 preview height (0 = same as capture, 1080)
    int  app_preview_fps;     // app H.264 preview fps    (0 = same as capture, 30)
    int  app_preview_bitrate_kbps;  // app H.264 bitrate (default 8000)
    char app_preview_preset[16];    // x264 speed-preset (default "ultrafast")
} StreamingConfig;

/**
 * @brief   Returns true if path is a named pipe (FIFO) rather than a regular file.
 * @details Used to skip ffprobe and choose the correct FFmpeg input flags for
 *          live camera streams piped from picamera2's hardware H.264 encoder.
 */
bool config_is_fifo(const char *path);

/**
 * @brief   Initialize config with default values.
 * @param   config A pointer to StreamingConfig structure.
 */
void config_init_defaults(StreamingConfig *config);

/**
 * @brief   Load configuration from JSON file.
 * @param   config A pointer to StreamingConfig structure.
 * @param   config_path Path to the JSON configuration file.
 * @returns 0 on success, -1 on error.
 */
int config_load(StreamingConfig *config, const char *config_path);

/**
 * @brief   Probe video file to get duration, FPS, and dimensions.
 * @details Uses ffprobe to extract video metadata.
 * @param   config A pointer to StreamingConfig structure (video_path must be set).
 * @returns 0 on success, -1 on error.
 */
int config_probe_video(StreamingConfig *config);

/**
 * @brief   Print current configuration to stdout.
 * @param   config A pointer to StreamingConfig structure.
 */
void config_print(const StreamingConfig *config);

/**
 * @brief   Validate configuration.
 * @details Checks that required files exist and values are sensible.
 * @param   config A pointer to StreamingConfig structure.
 * @returns 0 if valid, -1 if invalid.
 */
int config_validate(const StreamingConfig *config);

#endif