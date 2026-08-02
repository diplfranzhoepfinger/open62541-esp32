/* UA_ConnectionManager (protocol "websocket") bridging OPC UA Binary over
 * WebSocket (opc.ws://) on top of ESP-IDF's esp_http_server.
 *
 * open62541's own WebSocket ConnectionManager requires libwebsockets and
 * UA_ARCHITECTURE_POSIX, which is incompatible with the freertos-lwip
 * architecture used on ESP32. The server-side WS driver
 * (open62541/src/server/ua_server_ws.c) is otherwise transport-agnostic: it
 * looks up a UA_ConnectionManager named "websocket" in the EventLoop (see
 * open62541/CMakeLists.txt, UA_ENABLE_WEBSOCKET_TRANSPORT). This
 * implementation satisfies that interface on top of esp_http_server.
 */
#pragma once

/* Modular header (not the open62541.h amalgamation) so this file can be
 * included both from the ESP-IDF open62541 component (which also compiles
 * the amalgamation as a separate translation unit) and from FORTE's own
 * build of com/opc_ua, which already includes other modular open62541
 * headers -- including the amalgamation there too would double-define
 * every type it re-declares. */
#include <open62541/plugin/eventloop.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Allocates a new WebSocket ConnectionManager. Register it with
 * config->eventLoop->registerEventSource() before UA_Server_run_startup(). */
UA_ConnectionManager *WsConnectionManager_new(void);

/* Must be called regularly from the same thread that calls
 * UA_Server_run_iterate() (e.g. once per loop iteration). Delivers queued
 * WebSocket events (new connections, received messages, closed connections)
 * to the OPC UA server. open62541 has no locking on this architecture
 * (UA_MULTITHREADING < 100), so all UA_Server access must stay on that one
 * thread -- esp_http_server's own worker task only ever enqueues events,
 * it never calls into the server directly. */
void WsConnectionManager_poll(UA_ConnectionManager *cm);

#ifdef __cplusplus
}
#endif
