// Image Canvas
//   拖入图片 = 添加素材 + 创建引用该素材的物件
//   顶栏 导入：只添加素材
//   顶栏 素材：素材管理
//   顶栏 物件：物件管理（新建 / 重命名 / 管理素材数组 / 删除）
//   顶栏 连接：输入用户名 / IP / 端口，确定后启动新的远程客户端窗口
//   左键点击选中物件，按住拖动；选中物件显示 alpha 兼容的阴影 shader
//   拖动空白处平移 / 滚轮缩放

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
#include "client.h"
#include "control.h"
#include "entity.h"
#include "menu.h"
#include "net.h"
#include "object.h"
#include "user.h"
#include "ui.h"
#include "view.h"



static void PrintUsage (const char *program) {
    fprintf (stderr,
             "用法:\n"
             "  %s [--load 存档] [--save 存档]  本地模式（自动启动本地服务端）\n"
             "  %s --server [--port 7777] [--load 存档] [--save 存档]  无头服务器\n"
             "  %s <ip> <port> <username>  远程客户端\n",
             program, program, program);
}

typedef struct {
    uv_process_t process;
    int port;
    bool started;
    ControlServer control;
    char controlPath[256];
} LocalServer;

static uv_loop_t appLoop;
static bool appLoopInitialized;

static void AppLoopPump (void) {
    if (appLoopInitialized) uv_run (&appLoop, UV_RUN_NOWAIT);
}

static int PickFreePort (void) {
    uv_tcp_t probe;
    if (uv_tcp_init (&appLoop, &probe) != 0) return 0;

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
    uv_run (&appLoop, UV_RUN_DEFAULT);
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

static bool StartLocalServer (LocalServer *local, const char *program,
                              const char *loadPath, const char *savePath) {
    if (local->started) return true;

    int port = PickFreePort ();
    if (port == 0) return false;

    char portText[16];
    snprintf (portText, sizeof (portText), "%d", port);
    char *arguments[16];
    int count = 0;
    arguments[count++] = (char *)program;
    arguments[count++] = "--server";
    arguments[count++] = "--port";
    arguments[count++] = portText;
    if (!ControlPath (local->controlPath, sizeof (local->controlPath), port) ||
        !ControlServerStart (&local->control, &appLoop, local->controlPath))
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
    options.file = program;
    options.args = arguments;
    options.stdio = NULL;
    options.stdio_count = 0;
#ifdef _WIN32
    options.flags |= UV_PROCESS_WINDOWS_HIDE;
#endif

    local->process.data = local;
    int result = uv_spawn (&appLoop, &local->process, &options);
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
        AppLoopPump ();
        uv_sleep (20);
    }
    if (local->started)
        uv_process_kill (&local->process, SIGTERM);
    for (int i = 0; i < 100 && local->started; i++) {
        AppLoopPump ();
        uv_sleep (20);
    }
    if (local->started) {
        uv_process_kill (&local->process, SIGTERM);
        for (int i = 0; i < 100 && local->started; i++) {
            AppLoopPump ();
            uv_sleep (20);
        }
    }
    local->started = false;
}

static bool ConnectLocalSession (LocalServer *local, const char *program,
                                 const char *loadPath, const char *savePath) {
    if (!StartLocalServer (local, program, loadPath, savePath)) return false;
    for (int i = 0; i < 100; i++) {
        AppLoopPump ();
        if (!local->started) break;
        if (NetClientConnect ("127.0.0.1", local->port, "local")) return true;
        uv_sleep (20);
    }
    StopLocalServer (local);
    return false;
}

static void RemoteProcessClosed (uv_handle_t *handle) {
    free (handle);
}

static void RemoteWindowExited (uv_process_t *process, int64_t status,
                                int signal) {
    (void)status;
    (void)signal;
    uv_close ((uv_handle_t *)process, RemoteProcessClosed);
}

static bool OpenRemoteWindow (const char *program, const char *host, int port,
                              const char *username) {
    char portText[16];
    snprintf (portText, sizeof (portText), "%d", port);

    uv_process_t *process = calloc (1, sizeof (*process));
    if (process == NULL) return false;

    char *arguments[] = {(char *)program, (char *)host, portText,
                         "--name", (char *)username, NULL};
    uv_process_options_t options;
    memset (&options, 0, sizeof (options));
    options.exit_cb = RemoteWindowExited;
    options.file = program;
    options.args = arguments;
    int result = uv_spawn (&appLoop, process, &options);
    if (result != 0) {
        free (process);
        return false;
    }
    return true;
}

static void CloseProcessHandle (uv_handle_t *handle, void *data) {
    (void)data;
    if (handle->type == UV_PROCESS && !uv_is_closing (handle)) {
        if (handle->data == NULL)
            uv_close (handle, RemoteProcessClosed);
        else
            uv_close (handle, NULL);
    }
}

static void CloseAppLoop (void) {
    if (!appLoopInitialized) return;
    uv_walk (&appLoop, CloseProcessHandle, NULL);
    uv_run (&appLoop, UV_RUN_DEFAULT);
    uv_loop_close (&appLoop);
    appLoopInitialized = false;
}

int main (int argc, char **argv) {
    bool serverMode = false;
    const char *host = NULL;
    const char *username = NULL;
    const char *loadPath = NULL;
    const char *savePath = NULL;
    const char *controlPath = NULL;
    const char *bindAddress = NULL;
    int port = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp (argv[i], "--server") == 0) {
            serverMode = true;
        } else if (strcmp (argv[i], "--port") == 0 && i + 1 < argc) {
            port = atoi (argv[++i]);
        } else if ((strcmp (argv[i], "--name") == 0 ||
                    strcmp (argv[i], "--username") == 0) && i + 1 < argc) {
            username = argv[++i];
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
        } else if (username == NULL) {
            username = argv[i];
        } else {
            PrintUsage (argv[0]);
            return 1;
        }
    }

    if (serverMode) {
        if (port == 0) {
            if (host != NULL) port = atoi (host);
            if (port == 0) port = 7777;
        }
        if (port < 1 || port > 65535) {
            PrintUsage (argv[0]);
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

    if ((host == NULL) != (port == 0) || (host == NULL && username != NULL) ||
        (host != NULL && (username == NULL || username[0] == '\0')) ||
        (port != 0 && (port < 1 || port > 65535))) {
        PrintUsage (argv[0]);
        return 1;
    }

    SetConfigFlags (FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT);
    InitWindow (1000, 700, "Image Canvas");
    if (!IsWindowReady ()) {
        NetClientDisconnect ();
        return 1;
    }

    AssetInit ();
    ObjectInit ();
    UserInit ();
    if (uv_loop_init (&appLoop) != 0) {
        UserUnload ();
        ObjectUnload ();
        AssetUnload ();
        EntityUnload ();
        CloseWindow ();
        return 1;
    }
    appLoopInitialized = true;

    Ui ui;
    UiInit (&ui);

    View view;
    ViewInit (&view);

    LocalServer localServer = {0};
    bool connected = (host != NULL)
                         ? NetClientConnect (host, port, username)
                         : ConnectLocalSession (&localServer, argv[0],
                                                loadPath, savePath);
    if (!connected) {
        fprintf (stderr, host != NULL ? "连接服务器失败\n" : "启动本地服务失败\n");
        NetClientDisconnect ();
        CloseAppLoop ();
        ViewUnload (&view);
        UserUnload ();
        ObjectUnload ();
        AssetUnload ();
        EntityUnload ();
        UiUnload (&ui);
        CloseWindow ();
        return 1;
    }

    char localAddress[64] = "";
    if (host == NULL) GetLocalAddress (localAddress, (int)sizeof (localAddress));
    ClientRun (&ui, &view, host == NULL, localServer.port, localAddress,
               AppLoopPump, OpenRemoteWindow, argv[0]);

    NetClientDisconnect ();
    StopLocalServer (&localServer);
    CloseAppLoop ();
    ViewUnload (&view);
    UserUnload ();
    ObjectUnload ();
    AssetUnload ();
    EntityUnload ();
    UiUnload (&ui);
    CloseWindow ();
    return 0;
}
