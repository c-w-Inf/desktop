#include "control.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <unistd.h>
#endif

static ControlClient *activeClient;
static ControlServer *activeServer;

bool ControlPath (char *text, size_t size, int port) {
#ifdef _WIN32
    int needed = snprintf (text, size, "\\\\.\\pipe\\image-canvas-%ld-%d",
                           (long)uv_os_getpid (), port);
#else
    int needed = snprintf (text, size, "/tmp/image-canvas-%ld-%d.sock",
                           (long)uv_os_getpid (), port);
#endif
    return needed > 0 && (size_t)needed < size;
}

static void ControlAccepted (uv_stream_t *listener, int status) {
    ControlServer *server = listener->data;
    if (status < 0 || server->peerOpen) return;
    if (uv_pipe_init (server->loop, &server->peer, 0) != 0) return;
    server->peer.data = server;
    if (uv_accept (listener, (uv_stream_t *)&server->peer) != 0) {
        uv_close ((uv_handle_t *)&server->peer, NULL);
        return;
    }
    server->peerOpen = true;
}

bool ControlServerStart (ControlServer *server, uv_loop_t *loop,
                         const char *path) {
    if (server == NULL || loop == NULL || path == NULL || path[0] == '\0')
        return false;
    memset (server, 0, sizeof (*server));
    server->loop = loop;
    snprintf (server->path, sizeof (server->path), "%s", path);
#ifndef _WIN32
    unlink (server->path);
#endif
    if (uv_pipe_init (loop, &server->listener, 0) != 0) return false;
    server->listener.data = server;
    if (uv_pipe_bind (&server->listener, server->path) != 0 ||
        uv_listen ((uv_stream_t *)&server->listener, 4,
                   ControlAccepted) != 0) {
        uv_close ((uv_handle_t *)&server->listener, NULL);
        return false;
    }
    server->listenerOpen = true;
    activeServer = server;
    return true;
}

void ControlServerStop (ControlServer *server) {
    if (server == NULL) return;
    if (server->peerOpen) {
        uv_close ((uv_handle_t *)&server->peer, NULL);
        server->peerOpen = false;
    }
    if (server->listenerOpen) {
        uv_close ((uv_handle_t *)&server->listener, NULL);
        server->listenerOpen = false;
    }
#ifndef _WIN32
    if (server->path[0] != '\0') unlink (server->path);
#endif
    if (server->loop != NULL) uv_run (server->loop, UV_RUN_NOWAIT);
    if (activeServer == server) activeServer = NULL;
    server->path[0] = '\0';
}

static void ControlWriteDone (uv_write_t *request, int status) {
    (void)status;
    free (request->data);
    free (request);
}

bool ControlServerSendExport (const char *path) {
    if (activeServer == NULL || !activeServer->peerOpen ||
        path == NULL || path[0] == '\0') return false;
    size_t length = strlen (path);
    if (length > 1024) return false;
    char *message = malloc (length + 8);
    uv_write_t *request = calloc (1, sizeof (*request));
    if (message == NULL || request == NULL) {
        free (message);
        free (request);
        return false;
    }
    memcpy (message, "EXPORT ", 7);
    memcpy (message + 7, path, length);
    message[7 + length] = '\n';
    request->data = message;
    uv_buf_t buffer = uv_buf_init (message, (unsigned int)(length + 8));
    if (uv_write (request, (uv_stream_t *)&activeServer->peer,
                  &buffer, 1, ControlWriteDone) != 0) {
        free (message);
        free (request);
        return false;
    }
    return true;
}

typedef struct {
    uv_loop_t loop;
    uv_pipe_t pipe;
    uv_connect_t connect;
    ControlClient *client;
    char line[1024];
    size_t lineLen;
} ControlState;

static void ControlRead (uv_stream_t *stream, ssize_t nread,
                         const uv_buf_t *buffer) {
    ControlState *state = stream->data;
    if (nread > 0) {
        const char *data = buffer->base;
        for (ssize_t i = 0; i < nread; i++) {
            char ch = data[i];
            if (ch == '\n') {
                state->line[state->lineLen] = '\0';
                size_t length = state->lineLen;
                if (length > 0 && state->line[length - 1] == '\r')
                    state->line[length - 1] = '\0';
                if (strncmp (state->line, "EXPORT ", 7) == 0 &&
                    state->line[7] != '\0') {
                    ControlClient *client = state->client;
                    if (client->commandMutexReady) {
                        uv_mutex_lock (&client->commandMutex);
                        snprintf (client->exportPath, sizeof (client->exportPath),
                                  "%s", state->line + 7);
                        client->exportReady = true;
                        uv_mutex_unlock (&client->commandMutex);
                    }
                }
                state->lineLen = 0;
            } else if (state->lineLen + 1 < sizeof (state->line)) {
                state->line[state->lineLen++] = ch;
            }
        }
    }
    free (buffer->base);
    if (nread < 0) {
        state->client->closed = true;
        uv_read_stop (stream);
    }
}

static void ControlAlloc (uv_handle_t *handle, size_t suggested,
                          uv_buf_t *buffer) {
    (void)handle;
    if (suggested > 4096) suggested = 4096;
    buffer->base = malloc (suggested);
    buffer->len = buffer->base != NULL ? suggested : 0;
}

static void ControlConnected (uv_connect_t *request, int status) {
    ControlState *state = request->data;
    if (status < 0) {
        state->client->failed = true;
        return;
    }
    uv_read_start ((uv_stream_t *)&state->pipe, ControlAlloc, ControlRead);
}

static void ControlThread (void *argument) {
    ControlState *state = argument;
    ControlClient *client = state->client;
    if (uv_loop_init (&state->loop) != 0) {
        client->failed = true;
        free (state);
        return;
    }
    if (uv_pipe_init (&state->loop, &state->pipe, 0) != 0) {
        uv_loop_close (&state->loop);
        client->failed = true;
        free (state);
        return;
    }
    state->pipe.data = state;
    state->connect.data = state;
    uv_pipe_connect (&state->connect, &state->pipe, client->path,
                     ControlConnected);
    while (!client->stop && !client->closed && !client->failed) {
        uv_run (&state->loop, UV_RUN_NOWAIT);
        if (!client->stop && !client->closed && !client->failed)
            uv_sleep (5);
    }
    if (!uv_is_closing ((uv_handle_t *)&state->pipe))
        uv_close ((uv_handle_t *)&state->pipe, NULL);
    uv_run (&state->loop, UV_RUN_DEFAULT);
    uv_loop_close (&state->loop);
    free (state);
}

bool ControlClientStart (ControlClient *client, const char *path) {
    if (client == NULL || path == NULL || path[0] == '\0') return false;
    memset (client, 0, sizeof (*client));
    if (uv_mutex_init (&client->commandMutex) != 0) return false;
    client->commandMutexReady = true;
    snprintf (client->path, sizeof (client->path), "%s", path);
    ControlState *state = calloc (1, sizeof (*state));
    if (state == NULL) {
        uv_mutex_destroy (&client->commandMutex);
        client->commandMutexReady = false;
        return false;
    }
    state->client = client;
    if (uv_thread_create (&client->thread, ControlThread, state) != 0) {
        free (state);
        uv_mutex_destroy (&client->commandMutex);
        client->commandMutexReady = false;
        return false;
    }
    client->started = true;
    activeClient = client;
    return true;
}

bool ControlClientClosed (void) {
    return activeClient != NULL && activeClient->closed;
}

bool ControlClientTakeExport (char *path, size_t size) {
    ControlClient *client = activeClient;
    if (client == NULL || !client->commandMutexReady || size == 0) return false;
    uv_mutex_lock (&client->commandMutex);
    bool ready = client->exportReady;
    if (ready) {
        snprintf (path, size, "%s", client->exportPath);
        client->exportReady = false;
    }
    uv_mutex_unlock (&client->commandMutex);
    return ready;
}

void ControlClientStop (ControlClient *client) {
    if (client == NULL || !client->started) return;
    client->stop = true;
    uv_thread_join (&client->thread);
    client->started = false;
    if (activeClient == client) activeClient = NULL;
    if (client->commandMutexReady) {
        uv_mutex_destroy (&client->commandMutex);
        client->commandMutexReady = false;
    }
}
