/**
 * acoustic_bridge.h
 *
 * Pi-side bridge that connects the SnipeIt acoustic detection pipeline to the
 * WebSocket server. Mirrors the DdlBridge pattern: owns its hardware (ALSA
 * capture, onset detector, GCC-PHAT, SRP-PHAT), runs the capture in its own
 * thread, and emits "acoustic_event" JSON messages via the shared
 * WebSocketServer when events fire.
 *
 * Driven from the main loop in src/main.c, next to ddl_bridge_tick().
 */

#ifndef ACOUSTIC_BRIDGE_H
#define ACOUSTIC_BRIDGE_H

#include "websocket_server.h"

typedef struct AcousticBridge AcousticBridge;

/**
 * @brief Initialize ALSA capture, onset detector, GCC-PHAT and SRP-PHAT, and
 *        spawn the audio capture thread.
 *
 * @param ws Reference to the running WebSocketServer. Non-owning. Must remain
 *           valid for the lifetime of the bridge.
 *
 * @returns A new AcousticBridge instance, or NULL on failure.
 */
AcousticBridge* acoustic_bridge_start(WebSocketServer* ws);

/**
 * @brief Service any pending acoustic events.
 *
 * @param b The bridge instance. May be NULL (no-op).
 *
 * @details Call from the main poll loop alongside ddl_bridge_tick(). Cheap
 *          when no event is pending.
 *
 * @returns The number of events processed (0 or 1 per call).
 */
int acoustic_bridge_tick(AcousticBridge* b);

/**
 * @brief Stop the capture thread and release all resources.
 *
 * @param b The bridge instance. May be NULL (no-op).
 */
void acoustic_bridge_stop(AcousticBridge* b);

#endif /* ACOUSTIC_BRIDGE_H */
