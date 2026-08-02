#include "ws_connection_manager.h"

#include <esp_http_server.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

static const char *TAG_WS = "OPC UA WS";

#define WS_CM_MAX_CONNECTIONS 4
#define WS_CM_QUEUE_LENGTH    16
/* connectionId = fd + WS_CM_ID_BASE. Keeps ids away from the reserved
 * listener id (1) and from 0, which UA_ConnectionManager callers treat as
 * "unset". */
#define WS_CM_ID_BASE         1000u
#define WS_CM_LISTENER_ID     1u

typedef enum {
    WS_CM_EVT_CONNECTED,
    WS_CM_EVT_MESSAGE,
    WS_CM_EVT_CLOSED
} WsCmEventType;

typedef struct {
    WsCmEventType type;
    uintptr_t connectionId;
    uint8_t *data;   /* heap-allocated; owned by the event until delivered */
    size_t dataLen;
} WsCmEvent;

typedef struct {
    bool used;
    uintptr_t connectionId;
    httpd_handle_t hd;
    int fd;
    void *context; /* per-connection context handed back through connectionCallback */
} WsCmConnection;

typedef struct {
    UA_ConnectionManager cm; /* First member: UA_ConnectionManager* == WsConnectionManagerImpl* */

    UA_ConnectionManager_connectionCallback connectionCallback;
    void *application;
    void *listenerContext;

    httpd_handle_t server;
    uint16_t port;
    char path[32];

    QueueHandle_t eventQueue;
    WsCmConnection connections[WS_CM_MAX_CONNECTIONS];
} WsConnectionManagerImpl;

static WsCmConnection *
wsConnFind(WsConnectionManagerImpl *wcm, uintptr_t connId) {
    for(size_t i = 0; i < WS_CM_MAX_CONNECTIONS; i++) {
        if(wcm->connections[i].used && wcm->connections[i].connectionId == connId)
            return &wcm->connections[i];
    }
    return NULL;
}

static WsCmConnection *
wsConnAcquire(WsConnectionManagerImpl *wcm, uintptr_t connId) {
    WsCmConnection *existing = wsConnFind(wcm, connId);
    if(existing)
        return existing;
    for(size_t i = 0; i < WS_CM_MAX_CONNECTIONS; i++) {
        if(!wcm->connections[i].used) {
            memset(&wcm->connections[i], 0, sizeof(WsCmConnection));
            wcm->connections[i].used = true;
            wcm->connections[i].connectionId = connId;
            return &wcm->connections[i];
        }
    }
    return NULL;
}

static void
wsConnRelease(WsConnectionManagerImpl *wcm, uintptr_t connId) {
    WsCmConnection *conn = wsConnFind(wcm, connId);
    if(conn)
        memset(conn, 0, sizeof(WsCmConnection));
}

/* WebSocket URI handler. Runs on esp_http_server's own worker task -- must
 * never call into UA_Server directly (see ws_connection_manager.h). Only
 * pushes events onto wcm->eventQueue for WsConnectionManager_poll() to
 * deliver later on the OPC UA thread. */
static esp_err_t
wsHandler(httpd_req_t *req) {
    WsConnectionManagerImpl *wcm = (WsConnectionManagerImpl *)req->user_ctx;
    int fd = httpd_req_to_sockfd(req);
    if(fd < 0)
        return ESP_FAIL;
    uintptr_t connId = (uintptr_t)fd + WS_CM_ID_BASE;

    if(req->method == HTTP_GET) {
        /* The connection itself is registered from open_fn (wsOpenHandler),
         * which -- unlike this method==HTTP_GET branch -- reliably fires for
         * every accepted session on this httpd instance. Nothing to do here. */
        return ESP_OK;
    }

    /* A WebSocket frame for an already-established connection */
    httpd_ws_frame_t wsPkt;
    memset(&wsPkt, 0, sizeof(wsPkt));
    wsPkt.type = HTTPD_WS_TYPE_BINARY;
    esp_err_t ret = httpd_ws_recv_frame(req, &wsPkt, 0);
    if(ret != ESP_OK) {
        ESP_LOGW(TAG_WS, "httpd_ws_recv_frame (length) failed for fd %d: %s", fd, esp_err_to_name(ret));
        return ret;
    }

    if(wsPkt.type == HTTPD_WS_TYPE_CLOSE) {
        /* httpd closes the socket; close_fn (wsCloseHandler) delivers the
         * CLOSED event once that happens. */
        return ESP_OK;
    }

    if(wsPkt.type != HTTPD_WS_TYPE_BINARY) {
        /* This is a binary-only OPC UA transport; ignore text/ping/pong. */
        return ESP_OK;
    }

    if(wsPkt.len == 0)
        return ESP_OK;

    uint8_t *buf = malloc(wsPkt.len);
    if(!buf)
        return ESP_ERR_NO_MEM;
    wsPkt.payload = buf;
    ret = httpd_ws_recv_frame(req, &wsPkt, wsPkt.len);
    if(ret != ESP_OK) {
        ESP_LOGW(TAG_WS, "httpd_ws_recv_frame (payload) failed for fd %d: %s", fd, esp_err_to_name(ret));
        free(buf);
        return ret;
    }

    WsCmEvent evt = {
        .type = WS_CM_EVT_MESSAGE, .connectionId = connId, .data = buf, .dataLen = wsPkt.len
    };
    if(xQueueSend(wcm->eventQueue, &evt, 0) != pdTRUE) {
        ESP_LOGW(TAG_WS, "WS event queue full, dropping message for fd %d", fd);
        free(buf);
    } else {
        ESP_LOGD(TAG_WS, "Queued %d-byte WS message for fd %d (connId %lu)", (int)wsPkt.len, fd,
                 (unsigned long)connId);
    }
    return ESP_OK;
}

/* Custom session-open callback (httpd_config_t.open_fn). Fires exactly once
 * per accepted TCP session, before any URI handler dispatch -- unlike
 * checking req->method == HTTP_GET inside wsHandler, which turned out to
 * never actually fire on this ESP-IDF version (the framework completes the
 * WS handshake without ever invoking the registered URI handler for it), so
 * wcm->connections[] never got an entry and every subsequent message was
 * dropped as "unknown connId". This httpd instance only ever serves the one
 * WS URI, so treating every accepted session as a new WS connection here is
 * correct. Runs on the httpd worker task. */
static esp_err_t
wsOpenHandler(httpd_handle_t hd, int sockfd) {
    WsConnectionManagerImpl *wcm = (WsConnectionManagerImpl *)httpd_get_global_user_ctx(hd);
    uintptr_t connId = (uintptr_t)sockfd + WS_CM_ID_BASE;

    WsCmConnection *conn = wsConnAcquire(wcm, connId);
    if(!conn) {
        ESP_LOGW(TAG_WS, "Too many concurrent WebSocket connections, rejecting fd %d", sockfd);
        return ESP_FAIL;
    }
    conn->hd = hd;
    conn->fd = sockfd;

    WsCmEvent evt = { .type = WS_CM_EVT_CONNECTED, .connectionId = connId };
    if(xQueueSend(wcm->eventQueue, &evt, 0) != pdTRUE) {
        ESP_LOGW(TAG_WS, "WS event queue full, dropping CONNECTED event for fd %d", sockfd);
        wsConnRelease(wcm, connId);
        return ESP_FAIL;
    }
    ESP_LOGD(TAG_WS, "New session opened, queued CONNECTED for fd %d (connId %lu)", sockfd,
             (unsigned long)connId);
    return ESP_OK;
}

/* Custom session-close callback (httpd_config_t.close_fn). Per the
 * esp_http_server docs, once a custom close_fn is set, closing the socket
 * itself becomes our responsibility too. Runs on the httpd worker task. */
static void
wsCloseHandler(httpd_handle_t hd, int sockfd) {
    WsConnectionManagerImpl *wcm = (WsConnectionManagerImpl *)httpd_get_global_user_ctx(hd);
    uintptr_t connId = (uintptr_t)sockfd + WS_CM_ID_BASE;

    /* Only report a CLOSED event for sockets that actually completed a WS
     * handshake and were announced via CONNECTED -- close_fn also fires for
     * plain (non-WS) HTTP sessions and rejected handshakes. */
    if(wsConnFind(wcm, connId) != NULL) {
        WsCmEvent evt = { .type = WS_CM_EVT_CLOSED, .connectionId = connId };
        if(xQueueSend(wcm->eventQueue, &evt, 0) != pdTRUE)
            ESP_LOGW(TAG_WS, "WS event queue full, dropping CLOSED event for fd %d", sockfd);
    }

    close(sockfd);
}

static UA_StatusCode
WsCM_openConnection(UA_ConnectionManager *cm, const UA_KeyValueMap *params,
                     void *application, void *context,
                     UA_ConnectionManager_connectionCallback connectionCallback) {
    WsConnectionManagerImpl *wcm = (WsConnectionManagerImpl *)cm;

    const UA_Boolean *listen = (const UA_Boolean *)
        UA_KeyValueMap_getScalar(params, UA_QUALIFIEDNAME(0, "listen"), &UA_TYPES[UA_TYPES_BOOLEAN]);
    if(!listen || !*listen) {
        ESP_LOGE(TAG_WS, "Only listening (server) WebSocket connections are supported");
        return UA_STATUSCODE_BADNOTSUPPORTED;
    }

    const UA_Boolean *useSSL = (const UA_Boolean *)
        UA_KeyValueMap_getScalar(params, UA_QUALIFIEDNAME(0, "useSSL"), &UA_TYPES[UA_TYPES_BOOLEAN]);
    if(useSSL && *useSSL) {
        ESP_LOGE(TAG_WS, "opc.wss:// (TLS) is not supported by this WebSocket ConnectionManager");
        return UA_STATUSCODE_BADNOTSUPPORTED;
    }

    if(wcm->server != NULL) {
        ESP_LOGE(TAG_WS, "This WebSocket ConnectionManager only supports a single listener");
        return UA_STATUSCODE_BADALREADYEXISTS;
    }

    const UA_UInt16 *port = (const UA_UInt16 *)
        UA_KeyValueMap_getScalar(params, UA_QUALIFIEDNAME(0, "port"), &UA_TYPES[UA_TYPES_UINT16]);
    if(!port) {
        ESP_LOGE(TAG_WS, "No port given for the WebSocket listener");
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }

    const UA_String *path = (const UA_String *)
        UA_KeyValueMap_getScalar(params, UA_QUALIFIEDNAME(0, "path"), &UA_TYPES[UA_TYPES_STRING]);
    if(path && path->length > 0 && path->length < sizeof(wcm->path)) {
        memcpy(wcm->path, path->data, path->length);
        wcm->path[path->length] = '\0';
    } else {
        strcpy(wcm->path, "/");
    }

    wcm->connectionCallback = connectionCallback;
    wcm->application = application;
    wcm->port = *port;
    wcm->listenerContext = context;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = wcm->port;
    /* HTTPD_DEFAULT_CONFIG() leaves ctrl_port at the fixed
     * ESP_HTTPD_DEF_CTRL_PORT (32768). file_server (file_server.cpp) starts
     * its own httpd instance -- also on the default ctrl_port -- well before
     * forte_main() runs (see Application/main/main.c), and keeps its
     * loopback UDP control socket bound there for the device's entire
     * uptime. Reusing that same port here means every httpd_start() call
     * permanently fails to bind its control socket, so this needs its own,
     * distinct ctrl_port. */
    config.ctrl_port = ESP_HTTPD_DEF_CTRL_PORT + 1;
    /* httpd_start() itself already requires max_open_sockets + 3 <=
     * CONFIG_LWIP_MAX_SOCKETS for its own internal listen/control sockets
     * (see httpd_main.c) -- no extra headroom needed on top of that here. */
    config.max_open_sockets = WS_CM_MAX_CONNECTIONS;
    config.open_fn = wsOpenHandler;
    config.close_fn = wsCloseHandler;
    config.global_user_ctx = wcm;
    config.global_user_ctx_free_fn = NULL; /* wcm's lifetime is owned by us, not httpd */

    esp_err_t err = httpd_start(&wcm->server, &config);
    if(err != ESP_OK) {
        ESP_LOGE(TAG_WS, "Could not start httpd for %s: %s", wcm->path, esp_err_to_name(err));
        return UA_STATUSCODE_BADINTERNALERROR;
    }

    httpd_uri_t uriHandler = {
        .uri = wcm->path,
        .method = HTTP_GET,
        .handler = wsHandler,
        .user_ctx = wcm,
        .is_websocket = true,
        .supported_subprotocol = "opcua+uacp",
    };
    err = httpd_register_uri_handler(wcm->server, &uriHandler);
    if(err != ESP_OK) {
        ESP_LOGE(TAG_WS, "Could not register the WebSocket URI handler: %s", esp_err_to_name(err));
        httpd_stop(wcm->server);
        wcm->server = NULL;
        return UA_STATUSCODE_BADINTERNALERROR;
    }

    cm->eventSource.state = UA_EVENTSOURCESTATE_STARTED;

    /* Announce the listening "connection" itself, matching how the TCP
     * ConnectionManager announces its server socket. */
    UA_KeyValueMap empty = {0, NULL};
    connectionCallback(cm, WS_CM_LISTENER_ID, application, &wcm->listenerContext,
                       UA_CONNECTIONSTATE_ESTABLISHED, &empty, UA_BYTESTRING_NULL);

    ESP_LOGI(TAG_WS, "OPC UA WebSocket listener started on port %u, path %s", wcm->port, wcm->path);
    return UA_STATUSCODE_GOOD;
}

typedef struct {
    httpd_handle_t hd;
    int fd;
    uint8_t *data;
    size_t len;
} WsCmSendWork;

/* Runs on the httpd worker task (queued via httpd_queue_work), never called
 * directly from the OPC UA thread -- httpd_ws_send_frame_async() is only
 * safe from within httpd's own task context or via this queueing API. */
static void
wsSendWorkFn(void *arg) {
    WsCmSendWork *work = (WsCmSendWork *)arg;
    httpd_ws_frame_t wsPkt;
    memset(&wsPkt, 0, sizeof(wsPkt));
    wsPkt.type = HTTPD_WS_TYPE_BINARY;
    wsPkt.payload = work->data;
    wsPkt.len = work->len;
    esp_err_t err = httpd_ws_send_frame_async(work->hd, work->fd, &wsPkt);
    if(err != ESP_OK)
        ESP_LOGW(TAG_WS, "httpd_ws_send_frame_async failed for fd %d: %s", work->fd, esp_err_to_name(err));
    free(work->data);
    free(work);
}

static UA_StatusCode
WsCM_sendWithConnection(UA_ConnectionManager *cm, uintptr_t connectionId,
                         const UA_KeyValueMap *params, UA_ByteString *buf) {
    (void)params;
    WsConnectionManagerImpl *wcm = (WsConnectionManagerImpl *)cm;
    WsCmConnection *conn = wsConnFind(wcm, connectionId);
    if(!conn) {
        ESP_LOGW(TAG_WS, "UA_Server tried to send %u bytes to unknown connId %lu",
                 (unsigned)buf->length, (unsigned long)connectionId);
        UA_ByteString_clear(buf);
        return UA_STATUSCODE_BADCONNECTIONCLOSED;
    }
    ESP_LOGD(TAG_WS, "UA_Server sending %u bytes to connId %lu", (unsigned)buf->length,
             (unsigned long)connectionId);

    WsCmSendWork *work = malloc(sizeof(WsCmSendWork));
    if(!work) {
        UA_ByteString_clear(buf);
        return UA_STATUSCODE_BADOUTOFMEMORY;
    }
    work->hd = conn->hd;
    work->fd = conn->fd;
    work->data = buf->data; /* take ownership; freed in wsSendWorkFn */
    work->len = buf->length;
    buf->data = NULL;
    buf->length = 0;

    esp_err_t err = httpd_queue_work(conn->hd, wsSendWorkFn, work);
    if(err != ESP_OK) {
        free(work->data);
        free(work);
        return UA_STATUSCODE_BADINTERNALERROR;
    }
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode
WsCM_closeConnection(UA_ConnectionManager *cm, uintptr_t connectionId) {
    WsConnectionManagerImpl *wcm = (WsConnectionManagerImpl *)cm;
    WsCmConnection *conn = wsConnFind(wcm, connectionId);
    if(!conn)
        return UA_STATUSCODE_BADCONNECTIONCLOSED;
    /* Triggers close_fn (wsCloseHandler) on the httpd worker task, which
     * delivers the CLOSED event through the queue and closes the socket. */
    httpd_sess_trigger_close(conn->hd, conn->fd);
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode
WsCM_allocNetworkBuffer(UA_ConnectionManager *cm, uintptr_t connectionId,
                         UA_ByteString *buf, size_t bufSize) {
    (void)cm; (void)connectionId;
    return UA_ByteString_allocBuffer(buf, bufSize);
}

static void
WsCM_freeNetworkBuffer(UA_ConnectionManager *cm, uintptr_t connectionId, UA_ByteString *buf) {
    (void)cm; (void)connectionId;
    UA_ByteString_clear(buf);
}

static UA_StatusCode
WsCM_eventSourceStart(UA_EventSource *es) {
    es->state = UA_EVENTSOURCESTATE_STARTED;
    return UA_STATUSCODE_GOOD;
}

static void
WsCM_eventSourceStop(UA_EventSource *es) {
    WsConnectionManagerImpl *wcm = (WsConnectionManagerImpl *)es;
    if(wcm->server) {
        httpd_stop(wcm->server);
        wcm->server = NULL;
    }
    es->state = UA_EVENTSOURCESTATE_STOPPED;
}

static UA_StatusCode
WsCM_eventSourceFree(UA_EventSource *es) {
    WsConnectionManagerImpl *wcm = (WsConnectionManagerImpl *)es;
    if(wcm->server)
        httpd_stop(wcm->server);
    if(wcm->eventQueue) {
        WsCmEvent evt;
        while(xQueueReceive(wcm->eventQueue, &evt, 0) == pdTRUE) {
            if(evt.type == WS_CM_EVT_MESSAGE)
                free(evt.data);
        }
        vQueueDelete(wcm->eventQueue);
    }
    free(wcm);
    return UA_STATUSCODE_GOOD;
}

UA_ConnectionManager *
WsConnectionManager_new(void) {
    WsConnectionManagerImpl *wcm = calloc(1, sizeof(WsConnectionManagerImpl));
    if(!wcm)
        return NULL;

    wcm->eventQueue = xQueueCreate(WS_CM_QUEUE_LENGTH, sizeof(WsCmEvent));
    if(!wcm->eventQueue) {
        free(wcm);
        return NULL;
    }

    wcm->cm.eventSource.eventSourceType = UA_EVENTSOURCETYPE_CONNECTIONMANAGER;
    wcm->cm.eventSource.name = UA_STRING("opc-ws-httpd"); /* string literal, no allocation/cleanup needed */
    wcm->cm.eventSource.state = UA_EVENTSOURCESTATE_FRESH;
    wcm->cm.eventSource.start = WsCM_eventSourceStart;
    wcm->cm.eventSource.stop = WsCM_eventSourceStop;
    wcm->cm.eventSource.free = WsCM_eventSourceFree;

    wcm->cm.protocol = UA_STRING("websocket"); /* matched by src/server/ua_server_ws.c */
    wcm->cm.openConnection = WsCM_openConnection;
    wcm->cm.sendWithConnection = WsCM_sendWithConnection;
    wcm->cm.closeConnection = WsCM_closeConnection;
    wcm->cm.allocNetworkBuffer = WsCM_allocNetworkBuffer;
    wcm->cm.freeNetworkBuffer = WsCM_freeNetworkBuffer;

    return &wcm->cm;
}

void
WsConnectionManager_poll(UA_ConnectionManager *cm) {
    WsConnectionManagerImpl *wcm = (WsConnectionManagerImpl *)cm;
    if(!wcm->eventQueue)
        return;

    UA_KeyValueMap empty = {0, NULL};
    WsCmEvent evt;
    while(xQueueReceive(wcm->eventQueue, &evt, 0) == pdTRUE) {
        WsCmConnection *conn = wsConnFind(wcm, evt.connectionId);

        switch(evt.type) {
            case WS_CM_EVT_CONNECTED:
                if(conn) {
                    conn->context = NULL;
                    ESP_LOGD(TAG_WS, "Delivering CONNECTED for connId %lu to UA_Server",
                             (unsigned long)evt.connectionId);
                    wcm->connectionCallback(cm, evt.connectionId, wcm->application, &conn->context,
                                            UA_CONNECTIONSTATE_ESTABLISHED, &empty, UA_BYTESTRING_NULL);
                } else {
                    ESP_LOGW(TAG_WS, "Dropping CONNECTED for unknown connId %lu",
                             (unsigned long)evt.connectionId);
                }
                break;
            case WS_CM_EVT_MESSAGE:
                if(conn) {
                    ESP_LOGD(TAG_WS, "Delivering %u-byte message for connId %lu to UA_Server",
                             (unsigned)evt.dataLen, (unsigned long)evt.connectionId);
                    UA_ByteString msg = { evt.dataLen, evt.data };
                    wcm->connectionCallback(cm, evt.connectionId, wcm->application, &conn->context,
                                            UA_CONNECTIONSTATE_ESTABLISHED, &empty, msg);
                } else {
                    ESP_LOGW(TAG_WS, "Dropping %u-byte message for unknown connId %lu",
                             (unsigned)evt.dataLen, (unsigned long)evt.connectionId);
                }
                free(evt.data);
                break;
            case WS_CM_EVT_CLOSED:
                if(conn) {
                    wcm->connectionCallback(cm, evt.connectionId, wcm->application, &conn->context,
                                            UA_CONNECTIONSTATE_CLOSING, &empty, UA_BYTESTRING_NULL);
                    wsConnRelease(wcm, evt.connectionId);
                }
                break;
        }
    }
}
