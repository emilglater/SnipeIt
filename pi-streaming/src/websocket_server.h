/*
    WebSocket Server using libwebsockets

    This module implements a WebSocket server for the Android app:
    - Listens for WebSocket connections on a configurable port
    - Notifies when clients connect/disconnect
    - Sends JSON to the client: sensor_data, target_detection, acoustic events,
      stream_ready
    - Receives command frames from the app (dispatched via on_command)
 */

#ifndef WEBSOCKET_SERVER_H
#define WEBSOCKET_SERVER_H

/* Standard Libraries */
#include <stdbool.h>
#include <stddef.h>

// Forward declaration (libwebsockets types)
struct lws_context;

// Maximum message size for WebSocket
#define WS_MAX_MSG_SIZE 4096

// Message queue settings
#define WS_QUEUE_SIZE 256  // Number of messages in queue (ring buffer)

// Callback function types
typedef void (*ws_connect_callback)(void *user_data);
typedef void (*ws_disconnect_callback)(void *user_data);
typedef void (*ws_command_callback)(const char *payload, size_t len, void *user_data);
// Single queued message
typedef struct
{
    char data[WS_MAX_MSG_SIZE];
    size_t len;
} WsQueuedMessage;

typedef struct
{
    struct lws_context *context;            // libwebsockets context
    struct lws *client_wsi;                 // Connected client (NULL if none)
    int port;                               // Listening port
    bool running;                           // Whether server is running
    bool client_connected;                  // Whether a client is connected

    // Callbacks
    ws_connect_callback on_connect;         // Called when client connects
    ws_disconnect_callback on_disconnect;   // Called when client disconnects
    ws_command_callback on_command;         // Called when client sends a message
    void *callback_user_data;               // User data passed to callbacks

    // Message queue (ring buffer) for non-blocking sends
    WsQueuedMessage *queue;                 // Dynamically allocated queue
    int queue_head;                         // Next position to write
    int queue_tail;                         // Next position to read
    int queue_count;                        // Number of messages in queue
    int queue_dropped;                      // Count of dropped messages (for stats)
} WebSocketServer;

/**
 * @brief   Initialize the WebSocket server.
 * @param   ws A pointer to WebSocketServer structure.
 * @param   port Port number to listen on.
 * @returns 0 on success, -1 on error.
 */
int ws_init(WebSocketServer *ws, int port);

/**
 * @brief   Set connection callbacks.
 * @param   ws A pointer to WebSocketServer structure.
 * @param   on_connect Callback when client connects (can be NULL).
 * @param   on_disconnect Callback when client disconnects (can be NULL).
 * @param   user_data User data passed to callbacks.
 */
void ws_set_callbacks(WebSocketServer *ws,
                      ws_connect_callback on_connect,
                      ws_disconnect_callback on_disconnect,
                      void *user_data);

void ws_set_command_callback(WebSocketServer *ws, ws_command_callback on_command);

/**
 * @brief   Run one libwebsockets service pass. BLOCKS.
 * @details libwebsockets >= 3.2 IGNORES timeout_ms and blocks inside poll() on
 *          its own internal timeout (~30 s on an idle link). The caller must
 *          have another thread calling lws_cancel_service() to bound this - see
 *          the waker thread in main.c. Do not assume this returns promptly, and
 *          do not remove the waker.
 * @param   ws A pointer to WebSocketServer structure.
 * @param   timeout_ms Passed to lws_service(); currently ignored by lws.
 * @returns 0 on success, -1 if the server is not running.
 */
int ws_service(WebSocketServer *ws, int timeout_ms);

/**
 * @brief   Check if a client is connected.
 * @param   ws A pointer to WebSocketServer structure.
 * @returns true if a client is connected, false otherwise.
 */
bool ws_is_client_connected(WebSocketServer *ws);

/**
 * @brief   Send a text message to the connected client.
 * @param   ws A pointer to WebSocketServer structure.
 * @param   message The message to send (null-terminated string).
 * @returns 0 on success, -1 on error (no client connected).
 */
int ws_send(WebSocketServer *ws, const char *message);

/**
 * @brief   Send a JSON message to the connected client.
 * @details Convenience wrapper that handles serialization.
 * @param   ws A pointer to WebSocketServer structure.
 * @param   json The JSON string to send.
 * @param   len Length of the JSON string.
 * @returns 0 on success, -1 on error.
 */
int ws_send_json(WebSocketServer *ws, const char *json, size_t len);

/**
 * @brief   Cleanup and shutdown the WebSocket server.
 * @param   ws A pointer to WebSocketServer structure.
 */
void ws_cleanup(WebSocketServer *ws);



#endif