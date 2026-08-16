#include "websocket_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libwebsockets.h>

// Per-session data (one per connected client)
typedef struct
{
    WebSocketServer *server;    // Back-reference to server
} SessionData;

// Protocol callback function
static int ws_callback(struct lws *wsi, enum lws_callback_reasons reason,
                       void *user, void *in, size_t len)
{
    SessionData *session = (SessionData *)user;
    WebSocketServer *ws = NULL;

    // Get server reference from protocol user data
    if (wsi)
    {
        const struct lws_protocols *protocol = lws_get_protocol(wsi);
        if (protocol && protocol->user)
        {
            ws = (WebSocketServer *)protocol->user;
        }
    }

    switch (reason)
    {
        case LWS_CALLBACK_PROTOCOL_INIT:
            printf("[WS] Protocol initialized\n");
            break;

        case LWS_CALLBACK_ESTABLISHED:
            printf("[WS] Client connected\n");
            if (ws)
            {
                // Single-client app.  If a client is still registered when a new
                // one arrives, it is almost always a stale half-open socket from
                // the same Android app reconnecting before libwebsockets has
                // detected the old TCP close.  Rejecting the new connection here
                // was the cause of intermittent black screens: on_connect never
                // fired, so FFmpeg was never (re)started.  Instead, evict the old
                // client and let the new one take over.
                if (ws->client_connected && ws->client_wsi && ws->client_wsi != wsi)
                {
                    struct lws *old_wsi = ws->client_wsi;
                    printf("[WS] Existing client present - evicting it for the new connection\n");

                    // Detach the old client from server state *before* tearing
                    // down, so the old socket's eventual CLOSED callback is a
                    // no-op (it's guarded by client_wsi == wsi).
                    ws->client_wsi = NULL;
                    ws->client_connected = false;
                    ws->queue_head = 0;
                    ws->queue_tail = 0;
                    ws->queue_count = 0;

                    // Tear down the old streaming session (stop FFmpeg, STOP to
                    // Python) so the new connection starts a clean stream.
                    if (ws->on_disconnect)
                    {
                        ws->on_disconnect(ws->callback_user_data);
                    }

                    // Force the stale socket closed asynchronously.
                    lws_set_timeout(old_wsi, PENDING_TIMEOUT_CLOSE_SEND, LWS_TO_KILL_ASYNC);
                }

                ws->client_wsi = wsi;
                ws->client_connected = true;

                // Store server reference in session
                if (session)
                {
                    session->server = ws;
                }

                // Call user callback
                if (ws->on_connect)
                {
                    ws->on_connect(ws->callback_user_data);
                }
            }
            break;

        case LWS_CALLBACK_CLOSED:
            printf("[WS] Client disconnected\n");
            if (ws && ws->client_wsi == wsi)
            {
                ws->client_wsi = NULL;
                ws->client_connected = false;

                // Clear message queue
                ws->queue_head = 0;
                ws->queue_tail = 0;
                ws->queue_count = 0;

                // Call user callback
                if (ws->on_disconnect)
                {
                    ws->on_disconnect(ws->callback_user_data);
                }
            }
            break;

        case LWS_CALLBACK_SERVER_WRITEABLE:
            if (ws && ws->queue_count > 0)
            {
                // Get message from queue
                WsQueuedMessage *msg = &ws->queue[ws->queue_tail];

                // Prepare buffer with LWS_PRE padding
                unsigned char buf[LWS_PRE + WS_MAX_MSG_SIZE];
                size_t msg_len = msg->len;

                if (msg_len > WS_MAX_MSG_SIZE - LWS_PRE)
                {
                    msg_len = WS_MAX_MSG_SIZE - LWS_PRE;
                }

                memcpy(&buf[LWS_PRE], msg->data, msg_len);

                // Send as text message
                int written = lws_write(wsi, &buf[LWS_PRE], msg_len, LWS_WRITE_TEXT);

                if (written < (int)msg_len)
                {
                    fprintf(stderr, "[WS] Write error: wrote %d of %zu bytes\n",
                            written, msg_len);
                }

                // Remove message from queue
                ws->queue_tail = (ws->queue_tail + 1) % WS_QUEUE_SIZE;
                ws->queue_count--;

                // If more messages in queue, request another callback
                if (ws->queue_count > 0)
                {
                    lws_callback_on_writable(wsi);
                }
            }
            break;

        case LWS_CALLBACK_RECEIVE:
            if (ws && ws->on_command && in && len > 0)
            {
                // Command frames are small (<256B) and fit in a single WS
                // frame. If you ever send larger payloads you'd reassemble
                // here with lws_is_final_fragment() before dispatching.
                ws->on_command((const char *)in, len, ws->callback_user_data);
            }
            break;

        default:
            break;
    }

    return 0;
}

// Protocol definition
static struct lws_protocols protocols[] = {
    {
        .name = "detection-protocol",
        .callback = ws_callback,
        .per_session_data_size = sizeof(SessionData),
        .rx_buffer_size = WS_MAX_MSG_SIZE,
        .user = NULL  // Will be set to WebSocketServer pointer
    },
    { NULL, NULL, 0, 0, 0, NULL, 0 } // Terminator
};

int ws_init(WebSocketServer *ws, int port)
{
    *ws = (WebSocketServer){
        .context = NULL,
        .client_wsi = NULL,
        .port = port,
        .running = false,
        .client_connected = false,
        .on_connect = NULL,
        .on_disconnect = NULL,
        .on_command = NULL,
        .callback_user_data = NULL,
        .queue = NULL,
        .queue_head = 0,
        .queue_tail = 0,
        .queue_count = 0,
        .queue_dropped = 0
    };

    // Allocate message queue
    ws->queue = (WsQueuedMessage *)calloc(WS_QUEUE_SIZE, sizeof(WsQueuedMessage));
    if (!ws->queue)
    {
        fprintf(stderr, "[WS] Failed to allocate message queue\n");
        return -1;
    }

    // Set protocol user data to point to our server struct
    protocols[0].user = ws;

    // Configure libwebsockets
    struct lws_context_creation_info info;
    memset(&info, 0, sizeof(info));

    info.port = port;
    info.protocols = protocols;
    info.gid = -1;
    info.uid = -1;
    info.options = LWS_SERVER_OPTION_VALIDATE_UTF8;

    // Reduce logging verbosity
    lws_set_log_level(LLL_ERR | LLL_WARN, NULL);

    // Create context
    ws->context = lws_create_context(&info);
    if (!ws->context)
    {
        fprintf(stderr, "[WS] Failed to create libwebsockets context\n");
        return -1;
    }

    ws->running = true;
    printf("[WS] WebSocket server listening on port %d\n", port);

    return 0;
}

void ws_set_callbacks(WebSocketServer *ws,
                      ws_connect_callback on_connect,
                      ws_disconnect_callback on_disconnect,
                      void *user_data)
{
    ws->on_connect = on_connect;
    ws->on_disconnect = on_disconnect;
    ws->callback_user_data = user_data;
}

void ws_set_command_callback(WebSocketServer *ws, ws_command_callback on_command)
{
    ws->on_command = on_command;
}

int ws_service(WebSocketServer *ws, int timeout_ms)
{
    if (!ws->running || !ws->context)
    {
        return -1;
    }

    return lws_service(ws->context, timeout_ms);
}

bool ws_is_client_connected(WebSocketServer *ws)
{
    return ws->client_connected;
}

// Internal: add message to queue (non-blocking)
static int ws_queue_message(WebSocketServer *ws, const char *data, size_t len)
{
    if (!ws->client_connected || !ws->client_wsi || !ws->queue)
    {
        return -1;
    }

    if (len >= WS_MAX_MSG_SIZE)
    {
        fprintf(stderr, "[WS] Message too large: %zu bytes\n", len);
        return -1;
    }

    // Check if queue is full
    if (ws->queue_count >= WS_QUEUE_SIZE)
    {
        // Drop oldest message to make room (or could drop this new one)
        ws->queue_tail = (ws->queue_tail + 1) % WS_QUEUE_SIZE;
        ws->queue_count--;
        ws->queue_dropped++;
    }

    // Add to queue
    WsQueuedMessage *msg = &ws->queue[ws->queue_head];
    memcpy(msg->data, data, len);
    msg->len = len;

    ws->queue_head = (ws->queue_head + 1) % WS_QUEUE_SIZE;
    ws->queue_count++;

    // Request callback to send (if not already requested)
    lws_callback_on_writable(ws->client_wsi);

    return 0;
}

int ws_send(WebSocketServer *ws, const char *message)
{
    return ws_queue_message(ws, message, strlen(message));
}

int ws_send_json(WebSocketServer *ws, const char *json, size_t len)
{
    return ws_queue_message(ws, json, len);
}

int ws_get_poll_fd(WebSocketServer *ws)
{
    // libwebsockets manages its own event loop internally
    // For integration with external poll(), we'd need to use
    // the external poll API, which is more complex.
    // For now, return -1 to indicate we use lws_service() instead.
    (void)ws;
    return -1;
}

void ws_cleanup(WebSocketServer *ws)
{
    if (ws->context)
    {
        lws_context_destroy(ws->context);
        ws->context = NULL;
    }

    // Free message queue
    if (ws->queue)
    {
        free(ws->queue);
        ws->queue = NULL;
    }

    // Report dropped messages if any
    if (ws->queue_dropped > 0)
    {
        printf("[WS] Warning: %d messages were dropped due to full queue\n", ws->queue_dropped);
    }

    ws->running = false;
    ws->client_connected = false;
    ws->client_wsi = NULL;
    ws->queue_head = 0;
    ws->queue_tail = 0;
    ws->queue_count = 0;

    printf("[WS] Cleanup complete\n");
}