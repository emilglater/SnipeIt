#ifndef DDL_BRIDGE_H
#define DDL_BRIDGE_H

#include <stdint.h>

#include "websocket_server.h"

typedef struct DdlBridge DdlBridge;

/**
 * @brief Start the sensor pipeline and bridge it to the WebSocket server.
 * @details Runs the DDL boot sequence (log/hal/event_bus/app init + scheduler
 *          start) without blocking on app_join(). After this returns, the DDL
 *          active objects run on their own threads and the broadcaster
 *          refreshes the snapshot frame every ~2 s.
 *
 * @param ws        Already-initialised WebSocket server. The bridge does not
 *                  take ownership; the caller still owns it.
 * @param period_ms Minimum interval between sensor_data emissions.
 *                  The underlying snapshot only refreshes every ~2 s, so
 *                  values below 2000 will repeat data.
 * @returns Opaque bridge handle on success, NULL on failure (the bridge has
 *          fully cleaned up after itself on failure).
 */
DdlBridge* ddl_bridge_start(WebSocketServer* ws, unsigned int period_ms);

/**
 * @brief Called from the pi-streaming main loop. Cheap when nothing to do.
 * @details If a client is connected and at least period_ms has elapsed since
 *          the last emission, reads the snapshot, serialises it to JSON, and
 *          queues the message on the WS server. Otherwise returns quickly.
 *
 * @param b Bridge handle from ddl_bridge_start(). May be NULL (no-op).
 */
void ddl_bridge_tick(DdlBridge* b);

/**
 * @brief Stop the sensor pipeline and free the bridge.
 *
 * @param b Bridge handle from ddl_bridge_start(). May be NULL (no-op).
 */
void ddl_bridge_stop(DdlBridge* b);

/**
 * @brief Handle one inbound command message from the app.
 * @details Called from the WebSocket command callback on the main thread.
 *          Recognises "set_servo_angles" (clamped to the servo's mechanical
 *          range, and it cancels any active lock-on) and "select_target".
 *          Anything else, including the app's keepalive ping, is ignored.
 *
 * @param b    Bridge handle from ddl_bridge_start().
 * @param json NUL-terminated JSON payload. The parser uses strstr(), so a
 *             non-terminated buffer will read past the end.
 * @param len  Ignored. Present for call-site symmetry with the WS callback.
 */
void ddl_bridge_handle_command(DdlBridge* b, const char* json, size_t len);

/**
 * @brief Forward any queued Orin detections to the app. Call from the main loop.
 * @details The Orin receiver runs on its own thread and cannot touch the
 *          WebSocket (all ws_send must stay on the main thread). So the receiver
 *          callback serialises each detection message into a small internal
 *          queue; this drains that queue and sends each message to the app in
 *          the exact "target_detection" schema the app already expects. Cheap
 *          when the queue is empty.
 *
 * @param b Bridge handle from ddl_bridge_start(). May be NULL (no-op).
 */
void ddl_bridge_pump_detections(DdlBridge* b);

/**
 * @brief Record the capture-time servo pose for an outgoing frame_id.
 * @details The frame sender calls this once per emitted frame, at the
 *          instant of capture; the bridge reads the current servo pose and
 *          stores it in the frame_id->pose ring so a returning detection can
 *          be joined back to the pose the camera held when the frame was
 *          taken. No-op if the receiver pipeline didn't start. Thread-safe.
 *
 * @param b        Bridge handle from ddl_bridge_start(). May be NULL (no-op).
 * @param frame_id The frame_id tagged into the outgoing frame's SEI.
 */
void ddl_bridge_record_capture_pose(DdlBridge* b, uint32_t frame_id);

#endif