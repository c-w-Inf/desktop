#ifndef CONTROL_H
#define CONTROL_H

#include <stdbool.h>
#include <stddef.h>

#include <uv.h>

typedef struct {
    uv_loop_t *loop;
    uv_pipe_t listener;
    uv_pipe_t peer;
    bool listenerOpen;
    bool peerOpen;
    char path[256];
} ControlServer;

typedef struct {
    uv_thread_t thread;
    volatile bool started;
    volatile bool stop;
    volatile bool closed;
    volatile bool failed;
    char path[256];
} ControlClient;

bool ControlPath (char *text, size_t size, int port);
bool ControlServerStart (ControlServer *server, uv_loop_t *loop,
                         const char *path);
void ControlServerStop (ControlServer *server);

bool ControlClientStart (ControlClient *client, const char *path);
bool ControlClientClosed (void);
void ControlClientStop (ControlClient *client);

#endif
