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

#include <uv.h>

#include "raylib.h"
#include "raymath.h"

#include "assets.h"
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
    uv_pipe_t control;
    uv_write_t controlWrite;
    int port;
    bool started;
    bool exited;
    bool controlOpen;
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
        local->exited = true;
        local->started = false;
        if (local->controlOpen) {
            uv_close ((uv_handle_t *)&local->control, NULL);
            local->controlOpen = false;
        }
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
    char *arguments[12];
    int count = 0;
    arguments[count++] = (char *)program;
    arguments[count++] = "--server";
    arguments[count++] = "--port";
    arguments[count++] = portText;
    arguments[count++] = "--control-stdin";
    if (loadPath != NULL && loadPath[0] != '\0') {
        arguments[count++] = "--load";
        arguments[count++] = (char *)loadPath;
    }
    if (savePath != NULL && savePath[0] != '\0') {
        arguments[count++] = "--save";
        arguments[count++] = (char *)savePath;
    }
    arguments[count] = NULL;

    if (uv_pipe_init (&appLoop, &local->control, 0) != 0) return false;
    local->controlOpen = true;

    uv_stdio_container_t stdio[3];
    memset (stdio, 0, sizeof (stdio));
    stdio[0].flags = UV_CREATE_PIPE | UV_READABLE_PIPE;
    stdio[0].data.stream = (uv_stream_t *)&local->control;
    stdio[1].flags = UV_IGNORE;
    stdio[2].flags = UV_IGNORE;

    uv_process_options_t options;
    memset (&options, 0, sizeof (options));
    options.exit_cb = LocalServerExited;
    options.file = program;
    options.args = arguments;
    options.stdio = stdio;
    options.stdio_count = 3;
#ifdef _WIN32
    options.flags |= UV_PROCESS_WINDOWS_HIDE;
#endif

    int result = uv_spawn (&appLoop, &local->process, &options);
    if (result != 0) {
        uv_close ((uv_handle_t *)&local->control, NULL);
        local->controlOpen = false;
        return false;
    }
    local->process.data = local;
    local->port = port;
    local->started = true;
    local->exited = false;
    return true;
}

static void StopLocalServer (LocalServer *local) {
    if (!local->started) return;

    static const char command[] = "stop\n";
    if (local->controlOpen) {
        uv_buf_t buffer = uv_buf_init ((char *)command, sizeof (command) - 1);
        uv_write (&local->controlWrite, (uv_stream_t *)&local->control,
                  &buffer, 1, NULL);
    }
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
    if (local->controlOpen) {
        uv_close ((uv_handle_t *)&local->control, NULL);
        local->controlOpen = false;
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

static void RemoteWindowExited (uv_process_t *process, int64_t status,
                                int signal) {
    (void)status;
    (void)signal;
    uv_close ((uv_handle_t *)process, NULL);
    free (process);
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
    if (handle->type == UV_PROCESS && !uv_is_closing (handle))
        uv_close (handle, NULL);
}

static void CloseAppLoop (void) {
    if (!appLoopInitialized) return;
    uv_walk (&appLoop, CloseProcessHandle, NULL);
    uv_run (&appLoop, UV_RUN_DEFAULT);
    uv_loop_close (&appLoop);
    appLoopInitialized = false;
}

static void ReleaseOwnedObjectLock (bool notify) {
    u64 selfId = NetClientUserId ();
    if (selfId == 0) return;

    IdGroup ids = ObjectIds ();
    for (int i = 0; i < ids.count; i++) {
        Object *object = EntityFind (ids.ids[i]);
        if (object == NULL || object->lockOwnerId != selfId) continue;
        if (notify) NetUnlockObject (object->id);
        else NetUnlockObjectSilent (object->id);
    }
    IdGroupUnload (&ids);
}

static void ReleaseOwnedObjectLocksAndWait (void) {
    u64 selfId = NetClientUserId ();
    if (selfId == 0) return;

    IdGroup ids = ObjectIds ();
    for (int i = 0; i < ids.count; i++) {
        Object *object = EntityFind (ids.ids[i]);
        if (object != NULL && object->lockOwnerId == selfId)
            NetUnlockObjectAndWait (object->id);
    }
    IdGroupUnload (&ids);
}

static void ResetClientSceneState (Object **chosen, bool *dragged,
                                    bool *dragMoved, bool *suppress,
                                    u64 *lastClickId, double *lastClickTime,
                                    Ui *ui) {
    ReleaseOwnedObjectLock (false);
    *chosen = NULL;
    *dragged = false;
    *dragMoved = false;
    *suppress = IsMouseButtonDown (MOUSE_BUTTON_LEFT);
    *lastClickId = 0;
    *lastClickTime = 0.0;
    UiResetSceneState (ui);
}

static bool ObjectLockedBySelf (const Object *object) {
    if (object == NULL || object->lockOwnerId == 0) return false;
    return object->lockOwnerId == NetClientUserId ();
}

static void BringObjectToFront (Object *object) {
    if (object == NULL || !ObjectLockedBySelf (object)) return;
    NetRequestObjectFront (object->id);
}

static Object *AcquireObjectLock (Object *object, bool *acquired) {
    *acquired = false;
    if (ObjectLockedBySelf (object)) return object;

    u64 id = object->id;
    bool locked = NetLockObject (id);
    Object *current = EntityFind (id);
    if (!locked) return NULL;

    *acquired = true;
    if (current == NULL) {
        NetUnlockObjectSilent (id);
        return NULL;
    }
    return current;
}

static bool SceneIsEmpty (void) {
    IdGroup assets = AssetIds ();
    IdGroup objects = ObjectIds ();
    bool empty = assets.count == 0 && objects.count == 0;
    IdGroupUnload (&assets);
    IdGroupUnload (&objects);
    return empty;
}

static int AssetIndexAfterRemove (int current, int removed, int count) {
    if (removed < current) return current - 1;
    if (removed > current) return current;
    if (current >= count - 1) return count > 1 ? count - 2 : 0;
    return current;
}

static bool SendObjectState (const Object *object, const char *name, int layer,
                             const IdGroup *assets, int assetIndex) {
    Object copy = *object;
    if (name != NULL) snprintf (copy.name, sizeof (copy.name), "%s", name);
    copy.layer = layer;
    copy.assets = *assets;
    copy.assetIndex = assetIndex;
    ObjectTouch (&copy);
    return NetPublishObjectUpsert (&copy);
}


static bool SendObjectAssetAdd (const Object *object, u64 assetId) {
    IdGroup assets = IdGroupCreate ();
    if (!IdGroupCopy (&assets, object->assets.ids, object->assets.count) ||
        !IdGroupAdd (&assets, assetId)) {
        IdGroupUnload (&assets);
        return false;
    }
    bool ok = SendObjectState (object, NULL, object->layer, &assets,
                               object->assetIndex);
    IdGroupUnload (&assets);
    return ok;
}

static bool SendObjectAssetRemove (const Object *object, int index) {
    IdGroup assets = IdGroupCreate ();
    if (!IdGroupCopy (&assets, object->assets.ids, object->assets.count) ||
        !IdGroupRemoveAt (&assets, index)) {
        IdGroupUnload (&assets);
        return false;
    }
    int assetIndex = AssetIndexAfterRemove (object->assetIndex, index,
                                            object->assets.count);
    bool ok = SendObjectState (object, NULL, object->layer, &assets, assetIndex);
    IdGroupUnload (&assets);
    return ok;
}

static bool SendObjectAssetMove (const Object *object, int from, int to) {
    IdGroup assets = IdGroupCreate ();
    if (!IdGroupCopy (&assets, object->assets.ids, object->assets.count) ||
        !IdGroupMove (&assets, from, to)) {
        IdGroupUnload (&assets);
        return false;
    }
    bool ok = SendObjectState (object, NULL, object->layer, &assets,
                               object->assetIndex);
    IdGroupUnload (&assets);
    return ok;
}

static void DrawNetCursors (const Ui *ui, const View *view) {
    u64 selfId = NetClientUserId ();
    IdGroup ids = UserIds ();
    for (int i = 0; i < ids.count; i++) {
        User *user = EntityFind (ids.ids[i]);
        if (user == NULL || !user->hasCursor || user->id == selfId) continue;

        Vector2 screen = GetWorldToScreen2D ((Vector2){user->x, user->y}, view->cam);
        Color color = UserColor (user);
        DrawCircleV (screen, 4.0f, color);
        DrawTextEx (ui->font, user->name, (Vector2){screen.x + 7.0f, screen.y + 4.0f},
                    20.0f, 0.0f, color);
    }
    IdGroupUnload (&ids);
}

int main (int argc, char **argv) {
    bool serverMode = false;
    bool controlStdin = false;
    const char *host = NULL;
    const char *username = NULL;
    const char *loadPath = NULL;
    const char *savePath = NULL;
    int port = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp (argv[i], "--server") == 0) {
            serverMode = true;
        } else if (strcmp (argv[i], "--port") == 0 && i + 1 < argc) {
            port = atoi (argv[++i]);
        } else if (strcmp (argv[i], "--control-stdin") == 0) {
            controlStdin = true;
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
        if (controlStdin) NetServerEnableStdinControl ();
        int result = NetRunServer (port, "0.0.0.0", loadPath, savePath);
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

    if (host != NULL) {
        UiSetConnectMenuVisible (&ui, false);
        UiSetFileMenuVisible (&ui, false);
    }

    Object *chosen = NULL;
    bool dragged = false;
    bool dragMoved = false;
    bool suppressCanvasInput = false;
    Vector2 grab = {0};
    double lastCursorSend = 0.0;
    double lastObjectSync = 0.0;
    u64 lastCanvasClickId = 0;
    double lastCanvasClickTime = 0.0;
    bool closeAfterConnect = false;
    u64 sceneVersion = NetClientSceneVersion ();

    while (!WindowShouldClose ()) {
        AppLoopPump ();

        ViewUpdateViewport (&view);

        int update = NetClientUpdate ();
        if (update & NET_UPDATE_REJECTED) {
            break;
        }
        if (update & NET_UPDATE_SCENE) {
            // 全量快照会重建所有物件，旧指针立即失效
            ResetClientSceneState (&chosen, &dragged, &dragMoved,
                                   &suppressCanvasInput, &lastCanvasClickId,
                                   &lastCanvasClickTime, &ui);
            sceneVersion = NetClientSceneVersion ();
        } else {
            if (update & NET_UPDATE_ASSET_DELETE_FAILED)
                UiShowError (&ui, "素材删除失败");
            if (update & NET_UPDATE_ASSET_DELETED)
                UiAssetDeleted (&ui);
            if (update & NET_UPDATE_DELETED) {
                if (chosen != NULL && !ObjectContains (chosen)) {
                    chosen = NULL;
                    dragged = false;
                    suppressCanvasInput = IsMouseButtonDown (MOUSE_BUTTON_LEFT);
                }
                UiObjectDeleted (&ui);
            }
            if (update & NET_UPDATE_LOCK_LOST) {
                if (chosen != NULL && chosen->id == NetClientLockLostId ()) {
                    chosen = NULL;
                    dragged = false;
                    suppressCanvasInput = IsMouseButtonDown (MOUSE_BUTTON_LEFT);
                }
            }
        }
        if (chosen != NULL && chosen->lockOwnerId != 0 &&
            chosen->lockOwnerId != NetClientUserId ()) {
            chosen = NULL;
            dragged = false;
            suppressCanvasInput = IsMouseButtonDown (MOUSE_BUTTON_LEFT);
        }
        if (!NetClientConnected ()) break;

        bool wasModal = UiIsModal (&ui);
        UiEvent event = UiUpdate (&ui);
        bool isModal = UiIsModal (&ui);

        if (event.deselectObject) {
            if (chosen != NULL) NetUnlockObject (chosen->id);
            chosen = NULL;
            dragged = false;
            suppressCanvasInput = IsMouseButtonDown (MOUSE_BUTTON_LEFT);
        }

        switch (event.action) {
            case UI_ACTION_LOCK_OBJECT: {
                if (event.object == NULL) break;
                chosen = NULL;
                dragged = false;
                if (!ObjectLockedBySelf (event.object)) {
                    ReleaseOwnedObjectLock (true);
                    bool acquired = false;
                    event.object = AcquireObjectLock (event.object, &acquired);
                    if (event.object == NULL) {
                        UiObjectDeleted (&ui);
                        break;
                    }
                }
                // 物件管理器里选中只锁定；画布/右键选中才置顶。
                if (!ui.objectManagerOpen)
                    BringObjectToFront (event.object);
                break;
            }
            case UI_ACTION_RUN_MENU:
                if (event.object == NULL) break;
                {
                    const char *name = MenuName (event.object->id, event.menuIndex);
                    if (name == NULL ||
                        !NetRequestMenu (event.object->id, event.menuIndex, name))
                        UiShowError (&ui, "执行菜单失败");
                }
                break;
            case UI_ACTION_RUN_GLOBAL_MENU:
                {
                    const char *name = MenuName (0, event.menuIndex);
                    if (name == NULL || !NetRequestMenu (0, event.menuIndex, name))
                        UiShowError (&ui, "执行菜单失败");
                }
                break;
            case UI_ACTION_RUN_USER_MENU:
                {
                    u64 userId = NetClientUserId ();
                    const char *name = MenuName (userId, event.menuIndex);
                    if (userId == 0 || name == NULL ||
                        !NetRequestMenu (userId, event.menuIndex, name))
                        UiShowError (&ui, "执行菜单失败");
                }
                break;
            case UI_ACTION_UNLOCK_OBJECT:
                if (event.object == NULL) break;
                if (chosen == event.object) chosen = NULL;
                dragged = false;
                NetUnlockObject (event.object->id);
                break;
            case UI_ACTION_OPEN_SERVER: {
                if (!localServer.started) {
                    UiShowError (&ui, "当前没有本地服务");
                    break;
                }
                char address[64];
                GetLocalAddress (address, (int)sizeof (address));
                char message[160];
                snprintf (message, sizeof (message), "已开放 %s:%d", address, localServer.port);
                UiShowMessage (&ui, message);
                UiSetConnectMenuVisible (&ui, false);
                break;
            }
            case UI_ACTION_CONNECT:
                if (!OpenRemoteWindow (argv[0], event.host, event.port, event.username)) {
                    UiShowError (&ui, "打开新窗口失败");
                } else if (SceneIsEmpty ()) {
                    // 当前窗口没有素材或物件，连接后不再保留旧窗口
                    closeAfterConnect = true;
                }
                break;
            case UI_ACTION_IMPORT_SAVE:
                if (!NetRequestImport (event.path))
                    UiShowError (&ui, "导入请求失败");
                break;
            case UI_ACTION_EXPORT_SAVE:
                if (!NetRequestExport (event.path))
                    UiShowError (&ui, "导出存档失败");
                break;
            case UI_ACTION_IMPORT: {
                Asset *asset = NetCreateAsset (event.path);
                if (asset != NULL) {
                    UiOpenAssetManager (&ui);
                    UiSelectAsset (&ui, asset);
                } else {
                    UiShowError (&ui, "导入素材失败");
                }
                break;
            }
            case UI_ACTION_CREATE_OBJECT: {
                Object *previous = ui.selectedObject;
                if (previous != NULL) {
                    NetUnlockObject (previous->id);
                    if (chosen == previous) chosen = NULL;
                    ui.selectedObject = NULL;
                }
                Vector2 center = view.cam.target;
                Object *object = NetCreateObject (center);
                if (object != NULL)
                    UiSelectObject (&ui, object);
                else
                    UiShowError (&ui, "新建物件失败");
                break;
            }
            case UI_ACTION_RENAME_OBJECT: {
                u64 id = event.object->id;
                bool acquired = false;
                event.object = AcquireObjectLock (event.object, &acquired);
                if (event.object == NULL) break;
                if (ObjectRename (event.object, event.objectName))
                    NetPublishObjectUpsert (event.object);
                else
                    UiShowError (&ui, "重命名失败，名称已存在");
                if (acquired) NetUnlockObjectSilent (id);
                break;
            }
            case UI_ACTION_DELETE_OBJECT: {
                u64 id = event.object->id;
                bool acquired = false;
                event.object = AcquireObjectLock (event.object, &acquired);
                if (event.object == NULL) break;
                if (chosen == event.object) chosen = NULL;
                dragged = false;
                if (ObjectDelete (event.object)) {
                    UiObjectDeleted (&ui);
                    NetRequestObjectDelete (id);
                } else {
                    UiShowError (&ui, "删除物件失败");
                    if (acquired) NetUnlockObjectSilent (id);
                }
                break;
            }
            case UI_ACTION_ADD_OBJECT_ASSET:
                if (ObjectLockedBySelf (event.object)) {
                    IdGroup *assets = ObjectAssets (event.object);
                    if (assets != NULL && IdGroupAdd (assets, event.asset->id)) {
                        ObjectTouch (event.object);
                        NetPublishObjectUpsert (event.object);
                    } else {
                        UiShowError (&ui, "添加素材失败");
                    }
                } else if (!SendObjectAssetAdd (event.object, event.asset->id)) {
                    UiShowError (&ui, "添加素材失败");
                }
                break;
            case UI_ACTION_USE_OBJECT_ASSET:
                if (event.object == NULL || event.assetIndex < 0 ||
                    event.assetIndex >= event.object->assets.count) break;
                if (!ObjectLockedBySelf (event.object)) {
                    UiShowError (&ui, "物件未锁定，无法切换素材");
                    break;
                }
                event.object->assetIndex = event.assetIndex;
                if (!NetPublishObjectUpsert (event.object))
                    UiShowError (&ui, "切换素材失败");
                break;
            case UI_ACTION_REMOVE_OBJECT_ASSET:
                if (ObjectLockedBySelf (event.object)) {
                    IdGroup *assets = ObjectAssets (event.object);
                    int assetIndex = AssetIndexAfterRemove (
                        event.object->assetIndex, event.assetIndex,
                        event.object->assets.count);
                    if (assets != NULL && IdGroupRemoveAt (assets, event.assetIndex)) {
                        event.object->assetIndex = assetIndex;
                        ObjectTouch (event.object);
                        NetPublishObjectUpsert (event.object);
                    } else {
                        UiShowError (&ui, "移除素材失败");
                    }
                } else if (!SendObjectAssetRemove (event.object, event.assetIndex)) {
                    UiShowError (&ui, "移除素材失败");
                }
                break;
            case UI_ACTION_MOVE_OBJECT_ASSET:
                if (ObjectLockedBySelf (event.object)) {
                    IdGroup *assets = ObjectAssets (event.object);
                    if (assets != NULL && IdGroupMove (assets, event.assetIndex,
                                                       event.assetTargetIndex)) {
                        ObjectTouch (event.object);
                        NetPublishObjectUpsert (event.object);
                    } else {
                        UiShowError (&ui, "排序素材失败");
                    }
                } else if (!SendObjectAssetMove (event.object, event.assetIndex,
                                                 event.assetTargetIndex)) {
                    UiShowError (&ui, "排序素材失败");
                }
                break;
            case UI_ACTION_SET_OBJECT_LAYER: {
                u64 id = event.object->id;
                bool acquired = false;
                event.object = AcquireObjectLock (event.object, &acquired);
                if (event.object == NULL) break;
                if (ObjectSetLayer (event.object, event.layer))
                    NetPublishObjectUpsert (event.object);
                else
                    UiShowError (&ui, "修改层数失败");
                if (acquired) NetUnlockObjectSilent (id);
                break;
            }
            case UI_ACTION_RENAME_ASSET:
                if (!NetRequestAssetRename (event.asset->id, event.assetName))
                    UiShowError (&ui, "重命名失败");
                break;
            case UI_ACTION_REPLACE_ASSET: {
                int fileSize = 0;
                unsigned char *fileData = LoadFileData (event.path, &fileSize);
                bool sent = fileData != NULL && fileSize > 0 &&
                            NetRequestAssetReplace (event.asset->id,
                                                    GetFileExtension (event.path),
                                                    fileData, fileSize);
                UnloadFileData (fileData);
                if (!sent) UiShowError (&ui, "更换素材失败");
                break;
            }
            case UI_ACTION_DELETE_ASSET:
                if (!NetRequestAssetDelete (event.asset->id))
                    UiShowError (&ui, "删除素材失败");
                break;
            case UI_ACTION_SET_BACKGROUND:
                ViewSetBackground (&view, event.background);
                break;
            case UI_ACTION_VIEW_ORIGIN:
                ViewSetOrigin (&view);
                break;
            case UI_ACTION_SET_ZOOM:
                ViewSetZoom (&view, event.zoom);
                break;
            case UI_ACTION_RUN_SCRIPT: {
                User *self = UserFind (NetClientUserId ());
                if (self != NULL && !self->canscript) {
                    UiShowError (&ui, "当前用户不允许提交脚本");
                    break;
                }
                if (NetRequestScript (ui.scriptText.data != NULL ? ui.scriptText.data : "",
                                      ui.scriptSelf)) {
                    ui.scriptOpen = false;
                    ui.scriptScroll = 0.0f;
                    TextBufferFree (&ui.scriptText);
                    UiShowMessage (&ui, "脚本已提交");
                } else {
                    UiShowError (&ui, "脚本提交失败");
                }
                break;
            }
            case UI_ACTION_NONE:
            default:
                break;
        }

        if (NetClientSceneVersion () != sceneVersion) {
            ResetClientSceneState (&chosen, &dragged, &dragMoved,
                                   &suppressCanvasInput, &lastCanvasClickId,
                                   &lastCanvasClickTime, &ui);
            sceneVersion = NetClientSceneVersion ();
        }

        if (closeAfterConnect) break;

        Vector2 mouse = GetMousePosition ();

        if (!event.mouseCaptured) {
            float wheel = GetMouseWheelMove ();
            if (wheel != 0.0f) ViewZoomAt (&view, mouse, wheel);
        }

        Vector2 world = ViewScreenToWorld (&view, mouse);

        // 拖入图片：正常时添加素材并创建物件；弹窗打开时拒绝并提示
        if (IsFileDropped ()) {
            FilePathList files = LoadDroppedFiles ();

            if (!wasModal && !isModal) {
                int failed = 0;
                for (unsigned int i = 0; i < files.count; i++) {
                    const char *path = files.paths[i];
                    if (PathHasExtension (path, ".dsk") ||
                        PathIsDirectory (path)) {
                        if (!NetRequestImport (path)) failed++;
                        continue;
                    }
                    if (!PathIsImage (path)) {
                        failed++;
                        continue;
                    }

                    Vector2 position = {world.x + i * 20.0f, world.y + i * 20.0f};
                    Asset *asset = NetCreateAsset (path);
                    if (asset == NULL) {
                        failed++;
                        continue;
                    }
                    Object *object = NetCreateObject (position);
                    if (object == NULL) {
                        failed++;
                        continue;
                    }
                    if (!NetLockObject (object->id)) {
                        failed++;
                        continue;
                    }
                    if (!SendObjectAssetAdd (object, asset->id)) failed++;
                    NetUnlockObjectSilent (object->id);
                }
                if (failed > 0) UiShowError (&ui, "有文件导入失败");
            } else {
                UiShowError (&ui, "弹窗打开时不能拖入文件");
            }

            UnloadDroppedFiles (files);
        }

        if (NetClientSceneVersion () != sceneVersion) {
            ResetClientSceneState (&chosen, &dragged, &dragMoved,
                                   &suppressCanvasInput, &lastCanvasClickId,
                                   &lastCanvasClickTime, &ui);
            sceneVersion = NetClientSceneVersion ();
        }

        bool wasDraggingObject = dragged && chosen != NULL;
        bool leftDown = IsMouseButtonDown (MOUSE_BUTTON_LEFT);

        if (!event.mouseCaptured && !suppressCanvasInput) {
            // 只有在画布上按下左键才开始拖动；弹窗按钮残留的 leftDown 不能触发拖动
            if (IsMouseButtonPressed (MOUSE_BUTTON_LEFT)) {
                Object *picked = ObjectPick (world);
                bool lockedByOther = picked != NULL && picked->lockOwnerId != 0 &&
                                     picked->lockOwnerId != NetClientUserId ();
                if (lockedByOther) {
                    suppressCanvasInput = true;
                    lastCanvasClickId = 0;
                } else {
                    double now = GetTime ();
                    bool doubleClick = picked != NULL &&
                                       picked->id == lastCanvasClickId &&
                                       now - lastCanvasClickTime <= 0.35;
                    if (doubleClick) {
                        if (chosen != NULL && chosen != picked)
                            NetUnlockObject (chosen->id);
                        chosen = picked;

                        bool opened = true;
                        if (!ObjectLockedBySelf (chosen)) {
                            ReleaseOwnedObjectLock (true);
                            bool acquired = false;
                            chosen = AcquireObjectLock (chosen, &acquired);
                            if (chosen == NULL) opened = false;
                        }

                        if (opened) {
                            UiOpenObjectAssets (&ui, chosen);
                            BringObjectToFront (chosen);
                        }
                        suppressCanvasInput = true;
                        lastCanvasClickId = 0;
                        lastCanvasClickTime = 0.0;
                    } else {
                        if (picked != chosen) {
                            if (chosen != NULL) NetUnlockObject (chosen->id);
                            chosen = picked;
                        }

                        if (chosen != NULL) {
                            bool acquired = false;
                            chosen = AcquireObjectLock (chosen, &acquired);
                            if (chosen == NULL) {
                                suppressCanvasInput = true;
                            } else {
                                BringObjectToFront (chosen);
                            }
                        }

                        if (chosen != NULL)
                            grab = Vector2Subtract (world, chosen->pos);
                        lastCanvasClickId = (chosen != NULL && picked != NULL)
                                                ? picked->id : 0;
                        lastCanvasClickTime = now;
                    }
                }
            }

            if (!suppressCanvasInput) {
                if (IsMouseButtonPressed (MOUSE_BUTTON_LEFT)) {
                    dragged = true;
                    dragMoved = false;
                }

                bool canDrag = chosen == NULL || ObjectLockedBySelf (chosen);
                if (dragged && leftDown && canDrag) {
                    if (chosen != NULL) {
                        Vector2 oldPosition = chosen->pos;
                        ObjectMove (chosen, Vector2Subtract (world, grab));
                        if (chosen->pos.x != oldPosition.x || chosen->pos.y != oldPosition.y)
                            dragMoved = true;
                    } else {
                        ViewPan (&view, GetMouseDelta ());
                    }
                } else {
                    dragged = false;
                }

                if (IsMouseButtonPressed (MOUSE_BUTTON_RIGHT)) {
                    Object *object = ObjectPick (world);
                    bool lockedByOther = object != NULL && object->lockOwnerId != 0 &&
                                         object->lockOwnerId != NetClientUserId ();
                    if (lockedByOther) {
                        // 被其他客户端占用：保持无操作，不提示
                    } else if (object != NULL) {
                        if (object != chosen) {
                            if (chosen != NULL) NetUnlockObject (chosen->id);
                            chosen = object;
                        }

                        if (chosen != NULL) {
                            bool acquired = false;
                            chosen = AcquireObjectLock (chosen, &acquired);
                        }

                        if (chosen != NULL) {
                            BringObjectToFront (chosen);
                            UiOpenObjectContext (&ui, chosen, mouse);
                        }
                    } else {
                        UiOpenGlobalContext (&ui, mouse);
                    }
                }
            } else {
                dragged = false;
            }
        } else {
            dragged = false;
        }

        if (NetClientSceneVersion () != sceneVersion) {
            ResetClientSceneState (&chosen, &dragged, &dragMoved,
                                   &suppressCanvasInput, &lastCanvasClickId,
                                   &lastCanvasClickTime, &ui);
            sceneVersion = NetClientSceneVersion ();
        }

        if (suppressCanvasInput && !IsMouseButtonDown (MOUSE_BUTTON_LEFT))
            suppressCanvasInput = false;

        // 拖动期间定时广播物件位置，让其他客户端看到连续移动
        if (dragged && chosen != NULL && chosen->lockOwnerId != 0 &&
            chosen->lockOwnerId == NetClientUserId ()) {
            double now = GetTime ();
            if (now - lastObjectSync >= 0.03) {
                lastObjectSync = now;
                NetPublishObjectUpsert (chosen);
                NetRequestDragUpdate (chosen->id);
            }
        }

        if (wasDraggingObject && !dragged && chosen != NULL) {
            NetPublishObjectUpsert (chosen);
            if (dragMoved) {
                NetRequestDragUpdate (chosen->id);
                NetRequestDragEnd (chosen->id);
            }
            dragMoved = false;
        }

        {
            double now = GetTime ();
            if (now - lastCursorSend >= 0.05) {
                lastCursorSend = now;
                NetPublishCursor (ViewScreenToWorld (&view, mouse));
            }
        }

        if (!NetClientConnected ()) break;

        BeginDrawing ();
        ClearBackground (WHITE);
        ViewDraw (&view, chosen, ui.font);
        DrawNetCursors (&ui, &view);
        UiDraw (&ui);
        EndDrawing ();
    }

    if (NetClientConnected ()) ReleaseOwnedObjectLocksAndWait ();
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
