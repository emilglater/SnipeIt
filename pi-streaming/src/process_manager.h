/*
    Process Manager for FFmpeg and mediaMTX

    This module handles starting and stopping external processes:
    - mediaMTX RTSP server
    - FFmpeg video streaming

    Uses fork/exec to spawn processes and manages their lifecycle.
 */

#ifndef PROCESS_MANAGER_H
#define PROCESS_MANAGER_H

/* Standard Libraries */
#include <stdbool.h>
#include <sys/types.h>

#include "config.h"

typedef struct
{
    pid_t mediamtx_pid;     // PID of mediaMTX process (0 if not running)
    pid_t ffmpeg_pid;       // PID of FFmpeg process (0 if not running)
    bool mediamtx_running;  // Whether mediaMTX is running
    bool ffmpeg_running;    // Whether FFmpeg is running
} ProcessManager;

/**
 * @brief   Initialize the process manager.
 * @param   pm A pointer to ProcessManager structure.
 */
void pm_init(ProcessManager *pm);

/**
 * @brief   Start the mediaMTX RTSP server.
 * @param   pm A pointer to ProcessManager structure.
 * @param   config A pointer to StreamingConfig with mediaMTX paths.
 * @returns 0 on success, -1 on error.
 */
int pm_start_mediamtx(ProcessManager *pm, const StreamingConfig *config);

/**
 * @brief   Stop the mediaMTX server.
 * @param   pm A pointer to ProcessManager structure.
 */
void pm_stop_mediamtx(ProcessManager *pm);

/**
 * @brief   Check if mediaMTX is running.
 * @param   pm A pointer to ProcessManager structure.
 * @returns true if running, false otherwise.
 */
bool pm_is_mediamtx_running(ProcessManager *pm);

/**
 * @brief   Start FFmpeg to stream video to mediaMTX.
 * @param   pm A pointer to ProcessManager structure.
 * @param   config A pointer to StreamingConfig with video and RTSP settings.
 * @returns 0 on success, -1 on error.
 */
int pm_start_ffmpeg(ProcessManager *pm, const StreamingConfig *config);

/**
 * @brief   Stop FFmpeg streaming.
 * @param   pm A pointer to ProcessManager structure.
 */
void pm_stop_ffmpeg(ProcessManager *pm);

/**
 * @brief   Check if FFmpeg is running.
 * @param   pm A pointer to ProcessManager structure.
 * @returns true if running, false otherwise.
 */
bool pm_is_ffmpeg_running(ProcessManager *pm);

/**
 * @brief   Check status of child processes (non-blocking).
 * @details Should be called periodically to detect if processes have exited.
 * @param   pm A pointer to ProcessManager structure.
 */
void pm_check_processes(ProcessManager *pm);

/**
 * @brief   Stop all running processes and cleanup.
 * @param   pm A pointer to ProcessManager structure.
 */
void pm_cleanup(ProcessManager *pm);

/**
 * @brief   Wait for mediaMTX to be ready (listening on port).
 * @param   config A pointer to StreamingConfig with RTSP port.
 * @param   timeout_sec Maximum time to wait in seconds.
 * @returns 0 if ready, -1 if timeout.
 */
int pm_wait_for_mediamtx_ready(const StreamingConfig *config, int timeout_sec);

#endif