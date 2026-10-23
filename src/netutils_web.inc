#include <emscripten.h>
#include <emscripten/websocket.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    unsigned char *data;
    size_t len;
    size_t cap;
} WebBuffer;

struct NetutilStream {
    EMSCRIPTEN_WEBSOCKET_T socket;
    WebBuffer out;
    const NetutilEvents *events;
    void *user;
    bool opened;
    bool closing;
    bool closed;
};

struct NetutilServer {
    int unused;
};

static bool WebBufferAppend (WebBuffer *buffer, const void *data, size_t size) {
    if (size == 0) return true;
    if (size > SIZE_MAX - buffer->len) return false;
    size_t needed = buffer->len + size;
    if (needed > buffer->cap) {
        size_t capacity = buffer->cap == 0 ? 4096 : buffer->cap;
        while (capacity < needed) {
            if (capacity > SIZE_MAX / 2) { capacity = needed; break; }
            capacity *= 2;
        }
        unsigned char *grown = realloc (buffer->data, capacity);
        if (grown == NULL) return false;
        buffer->data = grown;
        buffer->cap = capacity;
    }
    memcpy (buffer->data + buffer->len, data, size);
    buffer->len = needed;
    return true;
}

static void WebStreamFree (NetutilStream *stream) {
    free (stream->out.data);
    free (stream);
}

static void WebNotifyClosed (NetutilStream *stream) {
    if (stream->closed) return;
    stream->closed = true;
    if (stream->events != NULL && stream->events->on_closed != NULL)
        stream->events->on_closed (stream, stream->user);
    if (stream->socket != 0) {
        emscripten_websocket_delete (stream->socket);
        stream->socket = 0;
    }
    WebStreamFree (stream);
}

static void WebFlush (NetutilStream *stream) {
    if (!stream->opened || stream->closing || stream->out.len == 0) return;
    EMSCRIPTEN_RESULT result = emscripten_websocket_send_binary (
        stream->socket, stream->out.data, (uint32_t)stream->out.len);
    if (result == EMSCRIPTEN_RESULT_SUCCESS) {
        stream->out.len = 0;
    } else {
        stream->closing = true;
        emscripten_websocket_close (stream->socket, 1000, "write failed");
    }
}

static EM_BOOL WebOnOpen (int eventType, const EmscriptenWebSocketOpenEvent *event,
                          void *user) {
    (void)eventType;
    (void)event;
    NetutilStream *stream = user;
    stream->opened = true;
    if (stream->closing) {
        emscripten_websocket_close (stream->socket, 1000, "closing");
        return EM_TRUE;
    }
    if (stream->events != NULL && stream->events->on_connected != NULL)
        stream->events->on_connected (stream, stream->user);
    WebFlush (stream);
    return EM_TRUE;
}

static EM_BOOL WebOnMessage (int eventType,
                             const EmscriptenWebSocketMessageEvent *event,
                             void *user) {
    (void)eventType;
    NetutilStream *stream = user;
    if (!stream->closing && event->numBytes > 0 && stream->events != NULL &&
        stream->events->on_read != NULL)
        stream->events->on_read (stream, event->data, event->numBytes,
                                 stream->user);
    return EM_TRUE;
}

static EM_BOOL WebOnError (int eventType,
                           const EmscriptenWebSocketErrorEvent *event,
                           void *user) {
    (void)eventType;
    (void)event;
    NetutilStream *stream = user;
    if (stream->events != NULL && stream->events->on_error != NULL)
        stream->events->on_error (stream, -1, stream->user);
    return EM_TRUE;
}

static EM_BOOL WebOnClose (int eventType,
                           const EmscriptenWebSocketCloseEvent *event,
                           void *user) {
    (void)eventType;
    (void)event;
    WebNotifyClosed ((NetutilStream *)user);
    return EM_TRUE;
}

bool NetutilInit (void) {
    return emscripten_websocket_is_supported () != 0;
}

void NetutilShutdown (void) {
}

bool NetutilListen (const char *bind, uint16_t port, NetutilAcceptFn accept,
                    void *user, NetutilServer **server) {
    (void)bind;
    (void)port;
    (void)accept;
    (void)user;
    if (server != NULL) *server = NULL;
    return false;
}

void NetutilServerClose (NetutilServer *server) {
    free (server);
}

bool NetutilConnect (const NetutilAddress *address, const NetutilEvents *events,
                     void *user, NetutilStream **stream) {
    if (address == NULL || address->url == NULL || stream == NULL) return false;
    *stream = NULL;

    NetutilStream *result = calloc (1, sizeof (*result));
    if (result == NULL) return false;
    result->events = events;
    result->user = user;

    EmscriptenWebSocketCreateAttributes attributes;
    memset (&attributes, 0, sizeof (attributes));
    attributes.url = address->url;
    attributes.createOnMainThread = EM_TRUE;
    result->socket = emscripten_websocket_new (&attributes);
    if (result->socket <= 0) {
        free (result);
        return false;
    }
    if (emscripten_websocket_set_onopen_callback (result->socket, result,
                                                  WebOnOpen) !=
            EMSCRIPTEN_RESULT_SUCCESS ||
        emscripten_websocket_set_onmessage_callback (result->socket, result,
                                                     WebOnMessage) !=
            EMSCRIPTEN_RESULT_SUCCESS ||
        emscripten_websocket_set_onerror_callback (result->socket, result,
                                                   WebOnError) !=
            EMSCRIPTEN_RESULT_SUCCESS ||
        emscripten_websocket_set_onclose_callback (result->socket, result,
                                                   WebOnClose) !=
            EMSCRIPTEN_RESULT_SUCCESS) {
        emscripten_websocket_delete (result->socket);
        free (result);
        return false;
    }
    *stream = result;
    return true;
}

void NetutilSetEvents (NetutilStream *stream, const NetutilEvents *events,
                       void *user) {
    if (stream == NULL) return;
    stream->events = events;
    stream->user = user;
}

bool NetutilWrite (NetutilStream *stream, const void *data, size_t size) {
    if (stream == NULL || stream->closing || stream->closed) return false;
    if (!WebBufferAppend (&stream->out, data, size)) {
        stream->closing = true;
        return false;
    }
    WebFlush (stream);
    return true;
}

void NetutilClose (NetutilStream *stream) {
    if (stream == NULL || stream->closing || stream->closed) return;
    stream->closing = true;
    if (stream->socket != 0) {
        emscripten_websocket_close (stream->socket, 1000, "closing");
    } else {
        WebNotifyClosed (stream);
    }
}

void NetutilPump (int timeout_ms) {
    if (timeout_ms > 0) emscripten_sleep ((unsigned int)timeout_ms);
}
