/* Web transport is implemented here later with Emscripten WebSocket APIs. */

bool NetutilInit (void) {
    return true;
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
    (void)server;
}

bool NetutilConnect (const NetutilAddress *address, const NetutilEvents *events,
                     void *user, NetutilStream **stream) {
    (void)address;
    (void)events;
    (void)user;
    if (stream != NULL) *stream = NULL;
    return false;
}

void NetutilSetEvents (NetutilStream *stream, const NetutilEvents *events,
                       void *user) {
    (void)stream;
    (void)events;
    (void)user;
}

bool NetutilWrite (NetutilStream *stream, const void *data, size_t size) {
    (void)stream;
    (void)data;
    (void)size;
    return false;
}

void NetutilClose (NetutilStream *stream) {
    (void)stream;
}

void NetutilPump (int timeout_ms) {
    (void)timeout_ms;
}
