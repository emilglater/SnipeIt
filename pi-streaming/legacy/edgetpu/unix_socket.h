/*
    Unix Domain Socket IPC implementation

    This module implements a Unix domain socket server that:
    - Accepts a connection from the Python object detection script
    - Sends START/STOP commands (JSON format) to Python
    - Receives detection JSON data from Python

    Based on Beej's Guide to Network Programming:
    - socket(), bind(), listen(), accept() for server setup
    - send(), recv() for data transfer
    - Uses AF_UNIX instead of AF_INET for local IPC
 */

#ifndef UNIX_SOCKET_H
#define UNIX_SOCKET_H

/* Standard Libraries */
#include <stddef.h>

// Socket path - both C and Python must use the same path
#define SOCKET_PATH "/tmp/detection.sock"

// Maximum message size (should be enough for JSON with multiple detections)
#define MAX_MSG_SIZE 4096

// Message buffer for receiving partial messages (increased for high throughput)
#define RECV_BUFFER_SIZE 65536

typedef struct
{
    int server_fd;                          // Server socket file descriptor (listens for connections)
    int client_fd;                          // Connected client file descriptor (Python script)
    char recv_buffer[RECV_BUFFER_SIZE];     // Buffer for partial message assembly
    size_t recv_len;                        // Current length of data in recv_buffer
} IPCConnection;

/**
 * @brief   Initialize the Unix domain socket server.
 * @details Creates and binds the socket, starts listening for connections.
 * @param   conn A pointer to IPCConnection structure to initialize.
 * @returns 0 on success, -1 on error (check errno).
 */
int ipc_server_init(IPCConnection *conn);

/**
 * @brief   Accept a client connection (blocking).
 * @details Waits for Python script to connect. This is a blocking call.
 * @param   conn A poiner to an initialized IPCConnection structure.
 * @returns 0 on success, -1 on error.
 */
int ipc_accept_client(IPCConnection *conn);

/**
 * @brief   Check if a client is connected.
 * @param   conn A poiner to an IPCConnection structure.
 * @returns 1 if client is connected, 0 if not.
 */
int ipc_check_client_connected(IPCConnection *conn);

/**
 * @brief   Send START command to Python process.
 * @details Sends JSON with video path and metadata for processing.
 * @param   conn A pointer to an IPCConnection structure.
 * @param   video_path The path to the video file to process.
 * @param   duration_sec Video duration in seconds.
 * @param   fps Video frames per second.
 * @param   loop Whether to loop the video.
 * @param   frame_interval Process every Nth frame.
 * @returns 0 on success, -1 on error.
 */
int ipc_send_start(IPCConnection *conn, const char *video_path, double duration_sec,
                    double fps, int loop, int frame_interval);

/**
 * @brief   Send STOP command to Python process.
 * @details Sends: {"cmd": "stop"}\n
 * @param   conn A poiner to an IPCConnection structure.
 * @returns 0 on success, -1 on error.
 */
int ipc_send_stop(IPCConnection *conn);

/**
 * @brief   Receive a message from Python process (non-blocking check).
 * @details Checks if a complete message (ending with \n) is available.
 *          Messages are newline-delimited JSON.
 * @param   conn A poiner to an IPCConnection structure.
 * @param   buffer A buffer to store the received message (without trailing \n).
 * @param   buffsize The size of the buffer (in bytes).
 * @returns > 0: Length of message received
 *          0: No complete message available yet
 *          -1: Error or client disconnected
 */
int ipc_recv_message(IPCConnection *conn, char *buffer, size_t buffsize);

/**
 * @brief   Get the server socket file descriptor.
 * @details Used by poll() to monitor for new client connections.
 * @param   conn A poiner to an IPCConnection structure.
 * @returns Server socket file descriptor, or -1 if not initialized.
 */
int ipc_get_server_fd(IPCConnection *conn);

/**
 * @brief   Get the client socket file descriptor.
 * @details Used by poll() to monitor for incoming messages.
 * @param   conn A poiner to an IPCConnection structure.
 * @returns Client socket file descriptor, or -1 if no client connected.
 */
int ipc_get_client_fd(IPCConnection *conn);

/**
 * @brief   Disconnect the current client.
 * @details Closes the client connection but keeps the server running.
 * @param   conn A poiner to an IPCConnection structure.
 */
void ipc_disconnect_client(IPCConnection *conn);

/**
 * @brief   Cleanup and close all sockets.
 * @details Closes both server and client sockets, removes the socket file.
 * @param   conn A poiner to an IPCConnection structure.
 */
void ipc_cleanup(IPCConnection *conn);

#endif