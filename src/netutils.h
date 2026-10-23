#ifndef NETUTILS_H
#define NETUTILS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct NetutilStream NetutilStream;
typedef struct NetutilServer NetutilServer;

#if defined(__EMSCRIPTEN__)
typedef struct {
    const char *url;
} NetutilAddress;
#else
typedef struct {
    const char *host;
    uint16_t port;
} NetutilAddress;
#endif

typedef void (*NetutilAcceptFn)(NetutilServer *server, NetutilStream *stream,
                                void *user);

typedef struct {
    void (*on_connected)(NetutilStream *stream, void *user);
    void (*on_read)(NetutilStream *stream, const unsigned char *data,
                    size_t size, void *user);
    void (*on_closed)(NetutilStream *stream, void *user);
    void (*on_error)(NetutilStream *stream, int code, void *user);
} NetutilEvents;

bool NetutilInit(void);
void NetutilShutdown(void);

/* Native TCP listener. Web keeps the function but returns false. */
bool NetutilListen(const char *bind, uint16_t port, NetutilAcceptFn accept,
                   void *user, NetutilServer **server);
void NetutilServerClose(NetutilServer *server);

bool NetutilConnect(const NetutilAddress *address, const NetutilEvents *events,
                    void *user, NetutilStream **stream);
void NetutilSetEvents(NetutilStream *stream, const NetutilEvents *events,
                      void *user);

bool NetutilWrite(NetutilStream *stream, const void *data, size_t size);
void NetutilClose(NetutilStream *stream);

/* Drive network events. Web implementations cannot block and return promptly. */
void NetutilPump(int timeout_ms);

#endif
