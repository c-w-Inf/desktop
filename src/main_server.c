#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "assets.h"
#include "control.h"
#include "entity.h"
#include "net.h"
#include "object.h"
#include "user.h"

static void ServerUsage (const char *program) {
    fprintf (stderr,
             "用法:\n"
             "  %s [--port 7777] [--bind 地址] [--load 存档] [--save 存档]\n",
             program);
}

int main (int argc, char **argv) {
    const char *loadPath = NULL;
    const char *savePath = NULL;
    const char *controlPath = NULL;
    const char *bindAddress = NULL;
    const char *host = NULL;
    int port = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp (argv[i], "--server") == 0) {
            continue;
        } else if (strcmp (argv[i], "--port") == 0 && i + 1 < argc) {
            port = atoi (argv[++i]);
        } else if (strcmp (argv[i], "--load") == 0 && i + 1 < argc) {
            loadPath = argv[++i];
        } else if (strcmp (argv[i], "--save") == 0 && i + 1 < argc) {
            savePath = argv[++i];
        } else if (strcmp (argv[i], "--control-pipe") == 0 && i + 1 < argc) {
            controlPath = argv[++i];
        } else if (strcmp (argv[i], "--bind") == 0 && i + 1 < argc) {
            bindAddress = argv[++i];
        } else if (host == NULL) {
            host = argv[i];
        } else if (port == 0) {
            port = atoi (argv[i]);
        } else {
            ServerUsage (argv[0]);
            return 1;
        }
    }

    if (port == 0) {
        if (host != NULL) port = atoi (host);
        if (port == 0) port = 7777;
    }
    if (port < 1 || port > 65535) {
        ServerUsage (argv[0]);
        return 1;
    }

    AssetInit ();
    ObjectInit ();
    UserInit ();
    ControlClient control = {0};
    if (controlPath != NULL && controlPath[0] != '\0')
        ControlClientStart (&control, controlPath);
    int result = NetRunServer (port,
                               bindAddress != NULL ? bindAddress : "0.0.0.0",
                               loadPath, savePath);
    ControlClientStop (&control);
    UserUnload ();
    ObjectUnload ();
    AssetUnload ();
    EntityUnload ();
    return result;
}
