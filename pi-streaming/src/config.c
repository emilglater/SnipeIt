/* Standard Libraries */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/stat.h>

/* User Libraries */
#include "config.h"
// Simple JSON parsing helpers (no external library needed for our simple config)

// Skip whitespace in JSON string
static const char* skip_whitespace(const char *str)
{
    while (*str && isspace((unsigned char)*str))
    {
        str++;
    }
    return str;
}

// Extract string value from JSON (finds "key": "value" and returns value)
static int json_get_string(const char *json, const char *key, char *value, size_t value_size)
{
    char search_key[128];
    snprintf(search_key, sizeof(search_key), "\"%s\"", key);

    const char *key_pos = strstr(json, search_key);
    if (!key_pos) return -1;

    // Find the colon
    const char *colon = strchr(key_pos + strlen(search_key), ':');
    if (!colon) return -1;

    // Find opening quote of value
    const char *quote_start = strchr(colon, '"');
    if (!quote_start) return -1;
    quote_start++; // Move past the quote

    // Find closing quote
    const char *quote_end = strchr(quote_start, '"');
    if (!quote_end) return -1;

    // Copy value
    size_t len = quote_end - quote_start;
    if (len >= value_size) len = value_size - 1;

    strncpy(value, quote_start, len);
    value[len] = '\0';

    return 0;
}

// Extract integer value from JSON
static int json_get_int(const char *json, const char *key, int *value)
{
    char search_key[128];
    snprintf(search_key, sizeof(search_key), "\"%s\"", key);

    const char *key_pos = strstr(json, search_key);
    if (!key_pos) return -1;

    const char *colon = strchr(key_pos + strlen(search_key), ':');
    if (!colon) return -1;

    colon = skip_whitespace(colon + 1);

    *value = atoi(colon);
    return 0;
}

// Extract boolean value from JSON
static int json_get_bool(const char *json, const char *key, bool *value)
{
    char search_key[128];
    snprintf(search_key, sizeof(search_key), "\"%s\"", key);

    const char *key_pos = strstr(json, search_key);
    if (!key_pos) return -1;

    const char *colon = strchr(key_pos + strlen(search_key), ':');
    if (!colon) return -1;

    colon = skip_whitespace(colon + 1);

    if (strncmp(colon, "true", 4) == 0)
    {
        *value = true;
    }
    else
    {
        *value = false;
    }

    return 0;
}

bool config_is_fifo(const char *path)
{
    struct stat st;
    return (stat(path, &st) == 0 && S_ISFIFO(st.st_mode));
}

void config_init_defaults(StreamingConfig *config)
{
    *config = (StreamingConfig){
        .video_path = "",
        .mediamtx_path = "./mediamtx",
        .mediamtx_config = "./config/mediamtx.yml",
        .websocket_port = 8555,
        .rtsp_port = 8554,
        .rtsp_stream_name = "stream",
        .loop_video = false,
        .detection_frame_interval = 5,
        .video_duration_sec = 0.0,
        .video_fps = 30.0,
        .video_width = 0,
        .video_height = 0,
        .orin_enabled = true,
        .orin_host = "10.42.0.2",
        .orin_rtp_port = 5600,
        .camera_source = "camera",
        .app_preview_width = 0,
        .app_preview_height = 0,
        .app_preview_fps = 0,
        .app_preview_bitrate_kbps = 8000,
        .app_preview_preset = "ultrafast"
    };
}

int config_load(StreamingConfig *config, const char *config_path)
{
    FILE *file = fopen(config_path, "r");
    if (!file)
    {
        perror("config_load: fopen");
        return -1;
    }

    // Get file size
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);

    if (file_size <= 0 || file_size > 65536)
    {
        fprintf(stderr, "[CONFIG] Invalid config file size\n");
        fclose(file);
        return -1;
    }

    // Read file contents
    char *json = malloc(file_size + 1);
    if (!json)
    {
        fprintf(stderr, "[CONFIG] Memory allocation failed\n");
        fclose(file);
        return -1;
    }

    size_t bytes_read = fread(json, 1, file_size, file);
    fclose(file);

    if (bytes_read != (size_t)file_size)
    {
        fprintf(stderr, "[CONFIG] Failed to read config file\n");
        free(json);
        return -1;
    }
    json[file_size] = '\0';

    // Initialize with defaults first
    config_init_defaults(config);

    // Parse JSON fields (ignore errors for optional fields)
    json_get_string(json, "video_path", config->video_path, sizeof(config->video_path));
    json_get_string(json, "mediamtx_path", config->mediamtx_path, sizeof(config->mediamtx_path));
    json_get_string(json, "mediamtx_config", config->mediamtx_config, sizeof(config->mediamtx_config));
    json_get_string(json, "rtsp_stream_name", config->rtsp_stream_name, sizeof(config->rtsp_stream_name));
    json_get_int(json, "websocket_port", &config->websocket_port);
    json_get_int(json, "rtsp_port", &config->rtsp_port);
    json_get_int(json, "detection_frame_interval", &config->detection_frame_interval);
    json_get_bool(json, "loop_video", &config->loop_video);

    json_get_bool(json, "orin_enabled", &config->orin_enabled);
    json_get_string(json, "orin_host", config->orin_host, sizeof(config->orin_host));
    json_get_int(json, "orin_rtp_port", &config->orin_rtp_port);
    json_get_string(json, "camera_source", config->camera_source, sizeof(config->camera_source));
    json_get_int(json, "app_preview_width",  &config->app_preview_width);
    json_get_int(json, "app_preview_height", &config->app_preview_height);
    json_get_int(json, "app_preview_fps",    &config->app_preview_fps);
    json_get_int(json, "app_preview_bitrate_kbps", &config->app_preview_bitrate_kbps);
    json_get_string(json, "app_preview_preset", config->app_preview_preset, sizeof(config->app_preview_preset));

    free(json);

    printf("[CONFIG] Loaded configuration from %s\n", config_path);
    return 0;
}

int config_probe_video(StreamingConfig *config)
{
    if (strlen(config->video_path) == 0)
    {
        fprintf(stderr, "[CONFIG] No video path set\n");
        return -1;
    }

    // For a named pipe (FIFO), ffprobe would block waiting for data.
    // Use known camera parameters instead: picamera2 encodes 1080p at 30fps.
    if (config_is_fifo(config->video_path))
    {
        config->video_duration_sec = 0.0;
        config->video_fps          = 30.0;
        config->video_width        = 1920;
        config->video_height       = 1080;
        printf("[CONFIG] Live camera FIFO: %s (1080p H.264, 30fps, no duration)\n",
               config->video_path);
        return 0;
    }

    // Check if file exists
    if (access(config->video_path, R_OK) != 0)
    {
        fprintf(stderr, "[CONFIG] Video file not accessible: %s\n", config->video_path);
        return -1;
    }

    char command[MAX_PATH_LENGTH + 256];
    char output[1024];

    // Use ffprobe to get video information
    // Format: duration,r_frame_rate,width,height
    snprintf(command, sizeof(command),
        "ffprobe -v quiet -select_streams v:0 "
        "-show_entries format=duration:stream=r_frame_rate,width,height "
        "-of csv=p=0 \"%s\" 2>/dev/null",
        config->video_path);

    FILE *pipe = popen(command, "r");
    if (!pipe)
    {
        perror("[CONFIG] ffprobe popen failed");
        return -1;
    }

    // Read output lines
    double duration = 0.0;
    int width = 0, height = 0;
    char fps_str[32] = "";

    while (fgets(output, sizeof(output), pipe))
    {
        // Remove newline
        output[strcspn(output, "\n")] = '\0';

        // Try to parse different output formats
        // ffprobe outputs: width,height,fps (stream) and duration (format) on separate lines
        if (strchr(output, '/'))
        {
            // This line contains FPS (e.g., "1920,1080,30/1")
            sscanf(output, "%d,%d,%31s", &width, &height, fps_str);
        }
        else if (strchr(output, '.'))
        {
            // This line contains duration (e.g., "35.123456")
            sscanf(output, "%lf", &duration);
        }
    }

    int status = pclose(pipe);
    if (status != 0)
    {
        fprintf(stderr, "[CONFIG] ffprobe failed (is ffmpeg installed?)\n");
        return -1;
    }

    // Parse FPS from fraction (e.g., "30/1" or "30000/1001")
    double fps = 30.0;
    if (strlen(fps_str) > 0)
    {
        int num = 0, den = 1;
        if (sscanf(fps_str, "%d/%d", &num, &den) == 2 && den != 0)
        {
            fps = (double)num / (double)den;
        }
        else
        {
            fps = atof(fps_str);
        }
    }

    config->video_duration_sec = duration;
    config->video_fps = fps;
    config->video_width = width;
    config->video_height = height;

    printf("[CONFIG] Video probed: %.2fs, %.2ffps, %dx%d\n",
           config->video_duration_sec, config->video_fps,
           config->video_width, config->video_height);

    return 0;
}

void config_print(const StreamingConfig *config)
{
    printf("\n========== Configuration ==========\n");
    printf("  Video path:        %s\n", config->video_path);
    printf("  Video duration:    %.2f seconds\n", config->video_duration_sec);
    printf("  Video FPS:         %.2f\n", config->video_fps);
    printf("  Video dimensions:  %dx%d\n", config->video_width, config->video_height);
    printf("  Loop video:        %s\n", config->loop_video ? "yes" : "no");
    printf("  Detection interval: every %d frames\n", config->detection_frame_interval);
    printf("  mediaMTX path:     %s\n", config->mediamtx_path);
    printf("  mediaMTX config:   %s\n", config->mediamtx_config);
    printf("  WebSocket port:    %d\n", config->websocket_port);
    printf("  RTSP port:         %d\n", config->rtsp_port);
    printf("  RTSP stream:       %s\n", config->rtsp_stream_name);
    printf("====================================\n\n");
}

int config_validate(const StreamingConfig *config)
{
    int errors = 0;

    // Check video path
    if (strlen(config->video_path) == 0)
    {
        fprintf(stderr, "[CONFIG] Error: video_path is required\n");
        errors++;
    }
    else if (!config_is_fifo(config->video_path) && access(config->video_path, R_OK) != 0)
    {
        fprintf(stderr, "[CONFIG] Error: video file not readable: %s\n", config->video_path);
        errors++;
    }

    // Check mediaMTX binary
    if (access(config->mediamtx_path, X_OK) != 0)
    {
        fprintf(stderr, "[CONFIG] Error: mediaMTX not executable: %s\n", config->mediamtx_path);
        errors++;
    }

    // Check mediaMTX config
    if (access(config->mediamtx_config, R_OK) != 0)
    {
        fprintf(stderr, "[CONFIG] Error: mediaMTX config not readable: %s\n", config->mediamtx_config);
        errors++;
    }

    // Check ports
    if (config->websocket_port < 1024 || config->websocket_port > 65535)
    {
        fprintf(stderr, "[CONFIG] Error: invalid websocket_port: %d\n", config->websocket_port);
        errors++;
    }

    if (config->rtsp_port < 1024 || config->rtsp_port > 65535)
    {
        fprintf(stderr, "[CONFIG] Error: invalid rtsp_port: %d\n", config->rtsp_port);
        errors++;
    }

    if (config->detection_frame_interval < 1)
    {
        fprintf(stderr, "[CONFIG] Error: detection_frame_interval must be >= 1\n");
        errors++;
    }

    return (errors == 0) ? 0 : -1;
}