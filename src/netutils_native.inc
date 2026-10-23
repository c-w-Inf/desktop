#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <uv.h>

#define NETUTIL_READ_SIZE 65536

typedef struct {
    unsigned char *data;
    size_t len;
    size_t cap;
} NetutilBuffer;

struct NetutilStream {
    uv_tcp_t handle;
    uv_write_t writeRequest;
    uv_connect_t connectRequest;
    uv_getaddrinfo_t resolveRequest;
    struct sockaddr_storage connectAddress;
    NetutilBuffer out;
    NetutilBuffer writeData;
    const NetutilEvents *events;
    void *user;
    char *host;
    bool handleInitialized;
    bool active;
    bool writing;
    bool closing;
    bool reading;
    bool resolving;
};

struct NetutilServer {
    uv_tcp_t handle;
    NetutilAcceptFn accept;
    void *user;
    bool closing;
};

static uv_loop_t netutilLoop;
static uv_timer_t netutilPumpTimer;
static bool netutilInitialized;

static bool BufferReserve (NetutilBuffer *buffer, size_t extra) {
    if (extra <= buffer->cap - buffer->len) return true;
    size_t needed = buffer->len + extra;
    size_t capacity = buffer->cap == 0 ? 4096 : buffer->cap;
    while (capacity < needed) capacity *= 2;
    unsigned char *data = realloc (buffer->data, capacity);
    if (data == NULL) return false;
    buffer->data = data;
    buffer->cap = capacity;
    return true;
}

static bool BufferAppend (NetutilBuffer *buffer, const void *data, size_t size) {
    if (!BufferReserve (buffer, size)) return false;
    memcpy (buffer->data + buffer->len, data, size);
    buffer->len += size;
    return true;
}

static void BufferFree (NetutilBuffer *buffer) {
    free (buffer->data);
    memset (buffer, 0, sizeof (*buffer));
}

static void NetutilStreamFree (NetutilStream *stream) {
    BufferFree (&stream->out);
    BufferFree (&stream->writeData);
    free (stream->host);
    free (stream);
}

static void NetutilStreamClosed (uv_handle_t *handle);
static void NetutilWriteDone (uv_write_t *request, int status);
static void NetutilReadData (uv_stream_t *handle, ssize_t nread,
                             const uv_buf_t *buffer);
static void NetutilAllocData (uv_handle_t *handle, size_t suggested,
                              uv_buf_t *buffer);

static void NetutilStreamClose (NetutilStream *stream) {
    if (stream == NULL || stream->closing) return;
    stream->closing = true;
    if (stream->handleInitialized &&
        !uv_is_closing ((uv_handle_t *)&stream->handle)) {
        uv_read_stop ((uv_stream_t *)&stream->handle);
        uv_close ((uv_handle_t *)&stream->handle, NetutilStreamClosed);
    } else if (!stream->handleInitialized && !stream->resolving) {
        NetutilStreamFree (stream);
    }
}

static void NetutilStreamClosed (uv_handle_t *handle) {
    NetutilStream *stream = handle->data;
    if (stream->events != NULL && stream->events->on_closed != NULL)
        stream->events->on_closed (stream, stream->user);
    NetutilStreamFree (stream);
}

static void NetutilAllocData (uv_handle_t *handle, size_t suggested,
                              uv_buf_t *buffer) {
    (void)handle;
    if (suggested > NETUTIL_READ_SIZE) suggested = NETUTIL_READ_SIZE;
    buffer->base = malloc (suggested);
    buffer->len = buffer->base != NULL ? suggested : 0;
}

static void NetutilReadData (uv_stream_t *handle, ssize_t nread,
                             const uv_buf_t *buffer) {
    NetutilStream *stream = handle->data;
    if (nread < 0) {
        free (buffer->base);
        NetutilStreamClose (stream);
        return;
    }
    if (nread > 0 && stream->events != NULL &&
        stream->events->on_read != NULL)
        stream->events->on_read (stream, (const unsigned char *)buffer->base,
                                 (size_t)nread, stream->user);
    free (buffer->base);
}

static void NetutilFlush (NetutilStream *stream) {
    if (!stream->active || stream->writing || stream->out.len == 0) return;
    stream->writeData = stream->out;
    memset (&stream->out, 0, sizeof (stream->out));
    stream->writing = true;
    stream->writeRequest.data = stream;
    uv_buf_t buffer = uv_buf_init ((char *)stream->writeData.data,
                                   (unsigned int)stream->writeData.len);
    int result = uv_write (&stream->writeRequest,
                           (uv_stream_t *)&stream->handle, &buffer, 1,
                           NetutilWriteDone);
    if (result != 0) {
        stream->writing = false;
        BufferFree (&stream->writeData);
        NetutilStreamClose (stream);
    }
}

static void NetutilWriteDone (uv_write_t *request, int status) {
    NetutilStream *stream = request->data;
    stream->writing = false;
    BufferFree (&stream->writeData);
    if (status < 0) {
        NetutilStreamClose (stream);
        return;
    }
    NetutilFlush (stream);
}

static void NetutilStartRead (NetutilStream *stream) {
    if (!stream->active || stream->reading || stream->closing) return;
    if (uv_read_start ((uv_stream_t *)&stream->handle, NetutilAllocData,
                       NetutilReadData) == 0)
        stream->reading = true;
}

static void NetutilConnected (uv_connect_t *request, int status) {
    NetutilStream *stream = request->data;
    if (status < 0) {
        NetutilStreamClose (stream);
        return;
    }
    stream->active = true;
    NetutilStartRead (stream);
    if (stream->events != NULL && stream->events->on_connected != NULL)
        stream->events->on_connected (stream, stream->user);
}

static void NetutilResolved (uv_getaddrinfo_t *request, int status,
                             struct addrinfo *result) {
    NetutilStream *stream = request->data;
    stream->resolving = false;
    free (stream->host);
    stream->host = NULL;

    if (stream->closing) {
        if (result != NULL) uv_freeaddrinfo (result);
        NetutilStreamFree (stream);
        return;
    }

    const struct addrinfo *address = NULL;
    if (status == 0) {
        for (const struct addrinfo *entry = result; entry != NULL;
             entry = entry->ai_next) {
            if (entry->ai_family == AF_INET &&
                entry->ai_addrlen >= sizeof (struct sockaddr_in)) {
                address = entry;
                break;
            }
        }
    }
    if (address == NULL || uv_tcp_init (&netutilLoop, &stream->handle) != 0) {
        if (result != NULL) uv_freeaddrinfo (result);
        NetutilStreamClose (stream);
        return;
    }

    stream->handleInitialized = true;
    stream->handle.data = stream;
    stream->connectRequest.data = stream;
    memcpy (&stream->connectAddress, address->ai_addr, address->ai_addrlen);
    int connectResult = uv_tcp_connect (
        &stream->connectRequest, &stream->handle,
        (const struct sockaddr *)&stream->connectAddress, NetutilConnected);
    if (result != NULL) uv_freeaddrinfo (result);
    if (connectResult != 0) NetutilStreamClose (stream);
}

static void NetutilAccepted (uv_stream_t *listener, int status) {
    NetutilServer *server = listener->data;
    if (status < 0 || server->closing) return;

    NetutilStream *stream = calloc (1, sizeof (*stream));
    if (stream == NULL) return;
    if (uv_tcp_init (&netutilLoop, &stream->handle) != 0) {
        free (stream);
        return;
    }
    stream->handleInitialized = true;
    stream->handle.data = stream;
    stream->active = true;
    if (uv_accept (listener, (uv_stream_t *)&stream->handle) != 0) {
        NetutilStreamClose (stream);
        return;
    }
    if (server->accept != NULL)
        server->accept (server, stream, server->user);
    else
        NetutilStreamClose (stream);
}

static void NetutilServerClosed (uv_handle_t *handle) {
    free (handle->data);
}

bool NetutilInit (void) {
    if (netutilInitialized) return true;
    if (uv_loop_init (&netutilLoop) != 0) return false;
    if (uv_timer_init (&netutilLoop, &netutilPumpTimer) != 0) {
        uv_loop_close (&netutilLoop);
        return false;
    }
    netutilInitialized = true;
    return true;
}

void NetutilShutdown (void) {
    if (!netutilInitialized) return;
    uv_close ((uv_handle_t *)&netutilPumpTimer, NULL);
    uv_run (&netutilLoop, UV_RUN_DEFAULT);
    uv_loop_close (&netutilLoop);
    netutilInitialized = false;
}

bool NetutilListen (const char *bind, uint16_t port, NetutilAcceptFn accept,
                    void *user, NetutilServer **server) {
    if (!netutilInitialized || server == NULL) return false;
    *server = NULL;

    char addressText[16];
    snprintf (addressText, sizeof (addressText), "%s",
              bind != NULL && bind[0] != '\0' ? bind : "0.0.0.0");
    struct sockaddr_in address;
    if (uv_ip4_addr (addressText, port, &address) != 0) return false;

    NetutilServer *listener = calloc (1, sizeof (*listener));
    if (listener == NULL) return false;
    listener->accept = accept;
    listener->user = user;
    if (uv_tcp_init (&netutilLoop, &listener->handle) != 0) {
        free (listener);
        return false;
    }
    listener->handle.data = listener;
    if (uv_tcp_bind (&listener->handle, (const struct sockaddr *)&address, 0) != 0 ||
        uv_listen ((uv_stream_t *)&listener->handle, 16, NetutilAccepted) != 0) {
        uv_close ((uv_handle_t *)&listener->handle, NetutilServerClosed);
        return false;
    }
    *server = listener;
    return true;
}

void NetutilServerClose (NetutilServer *server) {
    if (server == NULL || server->closing) return;
    server->closing = true;
    if (!uv_is_closing ((uv_handle_t *)&server->handle))
        uv_close ((uv_handle_t *)&server->handle, NetutilServerClosed);
}

bool NetutilConnect (const NetutilAddress *address, const NetutilEvents *events,
                     void *user, NetutilStream **stream) {
    if (!netutilInitialized || address == NULL || address->host == NULL ||
        stream == NULL)
        return false;
    *stream = NULL;

    NetutilStream *result = calloc (1, sizeof (*result));
    if (result == NULL) return false;
    result->host = strdup (address->host);
    if (result->host == NULL) {
        free (result);
        return false;
    }
    result->events = events;
    result->user = user;
    result->resolving = true;
    result->resolveRequest.data = result;

    char service[16];
    snprintf (service, sizeof (service), "%u", (unsigned)address->port);
    struct addrinfo hints;
    memset (&hints, 0, sizeof (hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    if (uv_getaddrinfo (&netutilLoop, &result->resolveRequest, NetutilResolved,
                        result->host, service, &hints) != 0) {
        free (result->host);
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
    NetutilStartRead (stream);
}

bool NetutilWrite (NetutilStream *stream, const void *data, size_t size) {
    if (stream == NULL || stream->closing || !stream->active)
        return false;
    if (size > 0 && !BufferAppend (&stream->out, data, size)) return false;
    NetutilFlush (stream);
    return true;
}

void NetutilClose (NetutilStream *stream) {
    NetutilStreamClose (stream);
}

static void NetutilPumpTimeout (uv_timer_t *timer) {
    (void)timer;
}

void NetutilPump (int timeout_ms) {
    if (!netutilInitialized) return;
    if (timeout_ms <= 0) {
        uv_run (&netutilLoop, UV_RUN_NOWAIT);
        return;
    }
    uv_timer_start (&netutilPumpTimer, NetutilPumpTimeout,
                    (uint64_t)timeout_ms, 0);
    uv_run (&netutilLoop, UV_RUN_ONCE);
    uv_timer_stop (&netutilPumpTimer);
}
