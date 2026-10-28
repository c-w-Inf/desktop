#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "raylib.h"
#include "raymath.h"

#include <uv.h>

#include "assets.h"
#include "control.h"
#include "cutils.h"
#include "entity.h"
#include "menu.h"
#include "client.h"
#include "netclient.h"
#include "object.h"
#include "user.h"
#include "ui.h"
#include "view.h"



static void PrintUsage (const char *program) {
    fprintf (stderr,
             "用法:\n"
             "  %s [--load 存档] [--save 存档]  本地模式（自动启动本地服务端）\n"
             "  %s <ip> <port> <username>  远程客户端\n",
             program, program);
}

typedef struct {
    uv_process_t process;
    int port;
    bool started;
    ControlServer control;
    char controlPath[256];
} LocalServer;

static uv_loop_t *appLoop;

static int PickFreePort (void) {
    uv_tcp_t probe;
    if (uv_tcp_init (appLoop, &probe) != 0) return 0;

    struct sockaddr_in address;
    uv_ip4_addr ("127.0.0.1", 0, &address);
    int port = 0;
    if (uv_tcp_bind (&probe, (const struct sockaddr *)&address, 0) == 0) {
        struct sockaddr_storage name;
        int length = sizeof (name);
        if (uv_tcp_getsockname (&probe, (struct sockaddr *)&name, &length) == 0) {
            const struct sockaddr_in *bound = (const struct sockaddr_in *)&name;
            port = ntohs (bound->sin_port);
        }
    }
    uv_close ((uv_handle_t *)&probe, NULL);
    uv_run (appLoop, UV_RUN_DEFAULT);
    return port;
}

static void GetLocalAddress (char *text, int size) {
    snprintf (text, (size_t)size, "127.0.0.1");

    uv_interface_address_t *interfaces = NULL;
    int count = 0;
    if (uv_interface_addresses (&interfaces, &count) != 0) return;
    for (int i = 0; i < count; i++) {
        if (interfaces[i].is_internal) continue;
        if (interfaces[i].address.address4.sin_family != AF_INET) continue;
        char address[64];
        if (uv_ip4_name (&interfaces[i].address.address4, address,
                         sizeof (address)) == 0) {
            snprintf (text, (size_t)size, "%s", address);
            break;
        }
    }
    uv_free_interface_addresses (interfaces, count);
}

static void LocalServerExited (uv_process_t *process, int64_t status,
                               int signal) {
    (void)status;
    (void)signal;
    LocalServer *local = process->data;
    if (local != NULL) {
        local->started = false;
    }
    uv_close ((uv_handle_t *)process, NULL);
}

static bool SelfPath (char *text, size_t size) {
    size_t length = size;
    if (uv_exepath (text, &length) != 0 || length >= size) return false;
    text[length] = '\0';
    return true;
}

static void ServerProgram (const char *program, char *text, size_t size) {
    const char *slash = strrchr (program, '/');
#ifdef _WIN32
    const char *backslash = strrchr (program, '\\');
    if (backslash != NULL && (slash == NULL || backslash > slash))
        slash = backslash;
#endif
    const char *base = slash != NULL ? slash + 1 : program;
    const char *dot = strrchr (base, '.');
    size_t directory = slash != NULL ? (size_t)(base - program) : 0;
    size_t name = dot != NULL ? (size_t)(dot - base) : strlen (base);
    snprintf (text, size, "%.*s%.*s_server%s", (int)directory, program,
              (int)name, base, dot != NULL ? dot : "");
}

static bool StartLocalServer (LocalServer *local, const char *loadPath,
                              const char *savePath) {
    if (local->started) return true;

    int port = PickFreePort ();
    if (port == 0) return false;

    char portText[16];
    snprintf (portText, sizeof (portText), "%d", port);
    char clientProgram[1024];
    if (!SelfPath (clientProgram, sizeof (clientProgram))) return false;
    char serverProgram[1024];
    ServerProgram (clientProgram, serverProgram, sizeof (serverProgram));
    char *arguments[16];
    int count = 0;
    arguments[count++] = serverProgram;
    arguments[count++] = "--server";
    arguments[count++] = "--port";
    arguments[count++] = portText;
    if (!ControlPath (local->controlPath, sizeof (local->controlPath), port) ||
        !ControlServerStart (&local->control, appLoop, local->controlPath))
        return false;
    arguments[count++] = "--control-pipe";
    arguments[count++] = local->controlPath;
    if (loadPath != NULL && loadPath[0] != '\0') {
        arguments[count++] = "--load";
        arguments[count++] = (char *)loadPath;
    }
    if (savePath != NULL && savePath[0] != '\0') {
        arguments[count++] = "--save";
        arguments[count++] = (char *)savePath;
    }
    arguments[count] = NULL;

    uv_process_options_t options;
    memset (&options, 0, sizeof (options));
    options.exit_cb = LocalServerExited;
    options.file = serverProgram;
    options.args = arguments;
    options.stdio = NULL;
    options.stdio_count = 0;
#ifdef _WIN32
    options.flags |= UV_PROCESS_WINDOWS_HIDE;
#endif

    local->process.data = local;
    int result = uv_spawn (appLoop, &local->process, &options);
    if (result != 0) {
        ControlServerStop (&local->control);
        return false;
    }
    local->port = port;
    local->started = true;
    return true;
}

static void StopLocalServer (LocalServer *local) {
    ControlServerStop (&local->control);
    if (!local->started) return;

    for (int i = 0; i < 100 && local->started; i++) {
        CutilsPump ();
        uv_sleep (20);
    }
    if (local->started)
        uv_process_kill (&local->process, SIGTERM);
    for (int i = 0; i < 100 && local->started; i++) {
        CutilsPump ();
        uv_sleep (20);
    }
    if (local->started) {
        uv_process_kill (&local->process, SIGTERM);
        for (int i = 0; i < 100 && local->started; i++) {
            CutilsPump ();
            uv_sleep (20);
        }
    }
    local->started = false;
}

static bool ConnectLocalSession (LocalServer *local, const char *loadPath,
                                 const char *savePath) {
    if (!StartLocalServer (local, loadPath, savePath)) return false;
    for (int i = 0; i < 100; i++) {
        CutilsPump ();
        if (!local->started) break;
        if (NetClientConnect ("127.0.0.1", local->port, "local")) return true;
        uv_sleep (20);
    }
    StopLocalServer (local);
    return false;
}

int main (int argc, char **argv) {
    const char *host = NULL;
    const char *username = NULL;
    const char *loadPath = NULL;
    const char *savePath = NULL;
    int port = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp (argv[i], "--port") == 0 && i + 1 < argc) {
            port = atoi (argv[++i]);
        } else if ((strcmp (argv[i], "--name") == 0 ||
                    strcmp (argv[i], "--username") == 0) && i + 1 < argc) {
            username = argv[++i];
        } else if (strcmp (argv[i], "--load") == 0 && i + 1 < argc) {
            loadPath = argv[++i];
        } else if (strcmp (argv[i], "--save") == 0 && i + 1 < argc) {
            savePath = argv[++i];
        } else if (host == NULL) {
            host = argv[i];
        } else if (port == 0) {
            port = atoi (argv[i]);
        } else if (username == NULL) {
            username = argv[i];
        } else {
            PrintUsage (argv[0]);
            return 1;
        }
    }

    if ((host == NULL) != (port == 0) || (host == NULL && username != NULL) ||
        (host != NULL && (username == NULL || username[0] == '\0')) ||
        (port != 0 && (port < 1 || port > 65535))) {
        PrintUsage (argv[0]);
        return 1;
    }

    SetConfigFlags (FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT);
    InitWindow (1000, 700, "desktop");
    if (!IsWindowReady ()) {
        NetClientDisconnect ();
        return 1;
    }

    AssetInit ();
    ObjectInit ();
    UserInit ();
    appLoop = uv_default_loop ();

    UiInit ();

    ViewInit ();

    LocalServer localServer = {0};
    bool connected = (host != NULL)
                         ? NetClientConnect (host, port, username)
                         : ConnectLocalSession (&localServer, loadPath,
                                                savePath);
    if (!connected) {
        fprintf (stderr, host != NULL ? "连接服务器失败\n" : "启动本地服务失败\n");
        NetClientDisconnect ();
        ViewUnload ();
        UserUnload ();
        ObjectUnload ();
        AssetUnload ();
        EntityUnload ();
        UiUnload ();
        CloseWindow ();
        return 1;
    }

    char localAddress[64] = "";
    if (host == NULL) GetLocalAddress (localAddress, (int)sizeof (localAddress));
    ClientInit (host == NULL, localServer.port, localAddress);
    while (ClientFrame ()) {
    }

    NetClientDisconnect ();
    StopLocalServer (&localServer);
    ViewUnload ();
    UserUnload ();
    ObjectUnload ();
    AssetUnload ();
    EntityUnload ();
    UiUnload ();
    CloseWindow ();
    return 0;
}
