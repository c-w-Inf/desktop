#include "client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "raylib.h"
#include "raymath.h"
#include "assets.h"
#include "entity.h"
#include "menu.h"
#include "net.h"
#include "object.h"
#include "user.h"
#include "cutils.h"
#include "utils.h"

static void ReleaseOwnedObjectLock (bool notify) {
    u64 id = NetClientLockedObjectId ();
    if (id == 0) return;
    if (notify) NetUnlockObject (id);
    else NetUnlockObjectSilent (id);
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

typedef struct {
    char *path;
    Vector2 position;
} DropItem;

static DropItem *dropItems;
static int dropCount;
static int dropCapacity;
static bool dropInProgress;
static bool lockEventReady;
static bool lockEventAcquired;

static void OnLockDone (bool acquired) {
    lockEventReady = true;
    lockEventAcquired = acquired;
}

static void DropQueueClear (void) {
    for (int i = 0; i < dropCount; i++) free (dropItems[i].path);
    free (dropItems);
    dropItems = NULL;
    dropCount = 0;
    dropCapacity = 0;
}

static bool DropQueuePush (const char *path, Vector2 position) {
    if (dropCount == dropCapacity) {
        int capacity = dropCapacity == 0 ? 8 : dropCapacity * 2;
        DropItem *items = realloc (dropItems, (size_t)capacity * sizeof (*items));
        if (items == NULL) return false;
        dropItems = items;
        dropCapacity = capacity;
    }
    dropItems[dropCount].path = StrDup (path);
    if (dropItems[dropCount].path == NULL) return false;
    dropItems[dropCount].position = position;
    dropCount++;
    return true;
}

static void DrawBusySpinner (const Ui *ui) {
    float x = GetScreenWidth () - 34.0f;
    float y = GetScreenHeight () - 38.0f;
    float angle = (float)(GetTime () * 240.0);
    DrawRing ((Vector2){x, y}, 11.0f, 14.0f, angle, angle + 260.0f,
              24, (Color){40, 90, 180, 255});
    const char *text = "处理中";
    Vector2 size = MeasureTextEx (ui->font, text, 16.0f, 0.0f);
    DrawTextEx (ui->font, text, (Vector2){x - size.x - 10.0f, y - size.y * 0.5f},
                16.0f, 0.0f, (Color){40, 90, 180, 255});
}

static void StartNextDrop (void) {
    while (dropCount > 0) {
        DropItem item = dropItems[0];
        dropItems[0] = dropItems[--dropCount];
        if (NetRequestAssetObject (item.path, item.position, OnLockDone)) {
            free (item.path);
            dropInProgress = true;
            return;
        }
        free (item.path);
    }
    dropInProgress = false;
}

static void DrawBusyFrame (Ui *ui, View *view, Object *chosen) {
    BeginDrawing ();
    ClearBackground (WHITE);
    ViewDraw (view, chosen, ui->font);
    DrawNetCursors (ui, view);
    UiDraw (ui);
    DrawBusySpinner (ui);
    EndDrawing ();
}



static Ui *importUi;

static void ImportChosenFile (const char *path) {
    if (path == NULL || path[0] == '\0') return;
    if (PathHasExtension (path, ".dsk")) {
        if (!NetRequestImport (path))
            UiShowError (importUi, "导入请求失败");
    } else if (PathIsImage (path)) {
        if (!NetRequestAssetAdd (path))
            UiShowError (importUi, "导入素材失败");
        else
            UiOpenAssetManager (importUi);
    } else {
        UiShowError (importUi, "不支持的文件");
    }
}

void ClientFrameInit (ClientFrameState *state, Ui *ui, View *view,
                      bool localMode, int localPort, const char *localAddress,
                      ClientPumpFn pump, ClientOpenRemoteFn openRemote,
                      const char *program) {
    memset (state, 0, sizeof (*state));
    state->ui = ui;
    state->view = view;
    state->localMode = localMode;
    state->localPort = localPort;
    state->localAddress = localAddress;
    state->pump = pump;
    state->openRemote = openRemote;
    state->program = program;
    state->sceneVersion = NetClientSceneVersion ();
    importUi = ui;
    CutilsSetFileHandler (ImportChosenFile);
    DropQueueClear ();
    if (!localMode) {
        UiSetConnectMenuVisible (ui, false);
        UiSetFileMenuVisible (ui, false);
    }
}

bool ClientFrame (ClientFrameState *state) {
    Ui *ui = state->ui;
    View *view = state->view;
    int localPort = state->localPort;
    const char *localAddress = state->localAddress;
    ClientPumpFn pump = state->pump;
    ClientOpenRemoteFn openRemote = state->openRemote;
    const char *program = state->program;
#define chosen state->chosen
#define dragged state->dragged
#define dragMoved state->dragMoved
#define suppressCanvasInput state->suppressCanvasInput
#define grab state->grab
#define lastCursorSend state->lastCursorSend
#define lastObjectSync state->lastObjectSync
#define lastCanvasClickId state->lastCanvasClickId
#define lastCanvasClickTime state->lastCanvasClickTime
#define closeAfterConnect state->closeAfterConnect
#define sceneVersion state->sceneVersion

        bool closeRequested = WindowShouldClose ();
        pump ();

        ViewUpdateViewport (view);

        bool wasPending = NetClientLockPending ();
        int update = NetClientUpdate ();
        if (update & NET_UPDATE_REJECTED) {
            return false;
        }
        if (update & NET_UPDATE_SCENE) {
            // 全量快照会重建所有物件，旧指针立即失效
            ResetClientSceneState (&chosen, &dragged, &dragMoved,
                                   &suppressCanvasInput, &lastCanvasClickId,
                                   &lastCanvasClickTime, ui);
            sceneVersion = NetClientSceneVersion ();
        } else {
            if (update & NET_UPDATE_ASSET_DELETE_FAILED)
                UiShowError (ui, "素材删除失败");
            if (update & NET_UPDATE_ASSET_DELETED)
                UiAssetDeleted (ui);
            if (update & NET_UPDATE_DELETED) {
                if (chosen != NULL && !ObjectContains (chosen)) {
                    chosen = NULL;
                    dragged = false;
                    suppressCanvasInput = IsMouseButtonDown (MOUSE_BUTTON_LEFT);
                }
                UiObjectDeleted (ui);
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
        if (!NetClientConnected ()) return false;

        if (lockEventReady) {
            lockEventReady = false;
            if (dropInProgress) {
                dropInProgress = false;
                StartNextDrop ();
            } else if (lockEventAcquired) {
                u64 lockedId = NetClientLockedObjectId ();
                Object *locked = EntityFind (lockedId);
                if (locked != NULL) {
                    chosen = locked;
                    suppressCanvasInput = IsMouseButtonDown (MOUSE_BUTTON_LEFT);
                    if (ui->objectManagerOpen) UiSelectObject (ui, locked);
                    else BringObjectToFront (locked);
                }
            }
        }
        if (wasPending) {
            if (IsFileDropped ()) {
                FilePathList files = LoadDroppedFiles ();
                UnloadDroppedFiles (files);
            }
            DrawBusyFrame (ui, view, chosen);
            return true;
        }
        if (closeRequested) return false;

        bool wasModal = UiIsModal (ui);
        UiEvent event = UiUpdate (ui);
        bool isModal = UiIsModal (ui);

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
                    if (!NetRequestLock (event.object->id, OnLockDone))
                        UiShowError (ui, "锁定物件失败");
                } else if (!ui->objectManagerOpen) {
                    BringObjectToFront (event.object);
                }
                break;
            }
            case UI_ACTION_RUN_MENU:
                if (event.object == NULL) break;
                {
                    const char *name = MenuName (event.object->id, event.menuIndex);
                    if (name == NULL ||
                        !NetRequestMenu (event.object->id, event.menuIndex, name))
                        UiShowError (ui, "执行菜单失败");
                }
                break;
            case UI_ACTION_RUN_GLOBAL_MENU:
                {
                    const char *name = MenuName (0, event.menuIndex);
                    if (name == NULL || !NetRequestMenu (0, event.menuIndex, name))
                        UiShowError (ui, "执行菜单失败");
                }
                break;
            case UI_ACTION_RUN_USER_MENU:
                {
                    u64 userId = NetClientUserId ();
                    const char *name = MenuName (userId, event.menuIndex);
                    if (userId == 0 || name == NULL ||
                        !NetRequestMenu (userId, event.menuIndex, name))
                        UiShowError (ui, "执行菜单失败");
                }
                break;
            case UI_ACTION_UNLOCK_OBJECT:
                if (event.object == NULL) break;
                if (chosen == event.object) chosen = NULL;
                dragged = false;
                NetUnlockObject (event.object->id);
                break;
            case UI_ACTION_OPEN_SERVER: {
                if (localPort == 0) {
                    UiShowError (ui, "当前没有本地服务");
                    break;
                }
                char message[160];
                snprintf (message, sizeof (message), "已开放 %s:%d",
                          localAddress, localPort);
                UiShowMessage (ui, message);
                UiSetConnectMenuVisible (ui, false);
                break;
            }
            case UI_ACTION_CONNECT:
                if (!openRemote (program, event.host, event.port, event.username)) {
                    UiShowError (ui, "打开新窗口失败");
                } else if (SceneIsEmpty ()) {
                    // 当前窗口没有素材或物件，连接后不再保留旧窗口
                    closeAfterConnect = true;
                }
                break;
            case UI_ACTION_IMPORT_SAVE:
                if (!NetRequestImport (event.path))
                    UiShowError (ui, "导入请求失败");
                break;
            case UI_ACTION_EXPORT_SAVE:
                if (!NetRequestExport (event.path))
                    UiShowError (ui, "导出存档失败");
                break;
            case UI_ACTION_IMPORT:
                if (!NetRequestAssetAdd (event.path)) {
                    UiShowError (ui, "导入素材失败");
                } else {
                    UiOpenAssetManager (ui);
                }
                break;
            case UI_ACTION_CREATE_OBJECT: {
                Object *previous = ui->selectedObject;
                if (previous != NULL) {
                    NetUnlockObject (previous->id);
                    if (chosen == previous) chosen = NULL;
                    ui->selectedObject = NULL;
                }
                Vector2 center = view->cam.target;
                if (!NetRequestObjectCreate (center, OnLockDone))
                    UiShowError (ui, "新建物件失败");
                break;
            }
            case UI_ACTION_RENAME_OBJECT: {
                if (!ObjectLockedBySelf (event.object)) break;
                if (ObjectRename (event.object, event.objectName))
                    NetPublishObjectUpsert (event.object);
                else
                    UiShowError (ui, "重命名失败，名称已存在");
                break;
            }
            case UI_ACTION_DELETE_OBJECT: {
                if (!ObjectLockedBySelf (event.object)) break;
                u64 id = event.object->id;
                if (chosen == event.object) chosen = NULL;
                dragged = false;
                if (ObjectDelete (event.object)) {
                    UiObjectDeleted (ui);
                    NetRequestObjectDelete (id);
                } else {
                    UiShowError (ui, "删除物件失败");
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
                        UiShowError (ui, "添加素材失败");
                    }
                } else if (!SendObjectAssetAdd (event.object, event.asset->id)) {
                    UiShowError (ui, "添加素材失败");
                }
                break;
            case UI_ACTION_USE_OBJECT_ASSET:
                if (event.object == NULL || event.assetIndex < 0 ||
                    event.assetIndex >= event.object->assets.count) break;
                if (!ObjectLockedBySelf (event.object)) {
                    UiShowError (ui, "物件未锁定，无法切换素材");
                    break;
                }
                event.object->assetIndex = event.assetIndex;
                if (!NetPublishObjectUpsert (event.object))
                    UiShowError (ui, "切换素材失败");
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
                        UiShowError (ui, "移除素材失败");
                    }
                } else if (!SendObjectAssetRemove (event.object, event.assetIndex)) {
                    UiShowError (ui, "移除素材失败");
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
                        UiShowError (ui, "排序素材失败");
                    }
                } else if (!SendObjectAssetMove (event.object, event.assetIndex,
                                                 event.assetTargetIndex)) {
                    UiShowError (ui, "排序素材失败");
                }
                break;
            case UI_ACTION_SET_OBJECT_LAYER:
                if (!ObjectLockedBySelf (event.object)) break;
                if (ObjectSetLayer (event.object, event.layer))
                    NetPublishObjectUpsert (event.object);
                else
                    UiShowError (ui, "修改层数失败");
                break;
            case UI_ACTION_RENAME_ASSET:
                if (!NetRequestAssetRename (event.asset->id, event.assetName))
                    UiShowError (ui, "重命名失败");
                break;
            case UI_ACTION_REPLACE_ASSET: {
                int fileSize = 0;
                unsigned char *fileData = ReadFileBytes (event.path, &fileSize);
                bool sent = fileData != NULL && fileSize > 0 &&
                            NetRequestAssetReplace (event.asset->id,
                                                    GetFileExtension (event.path),
                                                    fileData, fileSize);
                free (fileData);
                if (!sent) UiShowError (ui, "更换素材失败");
                break;
            }
            case UI_ACTION_DELETE_ASSET:
                if (!NetRequestAssetDelete (event.asset->id))
                    UiShowError (ui, "删除素材失败");
                break;
            case UI_ACTION_SET_BACKGROUND:
                ViewSetBackground (view, event.background);
                break;
            case UI_ACTION_VIEW_ORIGIN:
                ViewSetOrigin (view);
                break;
            case UI_ACTION_SET_ZOOM:
                ViewSetZoom (view, event.zoom);
                break;
            case UI_ACTION_RUN_SCRIPT: {
                User *self = UserFind (NetClientUserId ());
                if (self != NULL && !self->canscript) {
                    UiShowError (ui, "当前用户不允许提交脚本");
                    break;
                }
                if (NetRequestScript (ui->scriptText.data != NULL ? ui->scriptText.data : "",
                                      ui->scriptSelf)) {
                    ui->scriptOpen = false;
                    ui->scriptScroll = 0.0f;
                    TextBufferFree (&ui->scriptText);
                    UiShowMessage (ui, "脚本已提交");
                } else {
                    UiShowError (ui, "脚本提交失败");
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
                                   &lastCanvasClickTime, ui);
            sceneVersion = NetClientSceneVersion ();
        }

        if (closeAfterConnect) return false;

        Vector2 mouse = GetMousePosition ();

        if (!event.mouseCaptured) {
            float wheel = GetMouseWheelMove ();
            if (wheel != 0.0f) ViewZoomAt (view, mouse, wheel);
        }

        Vector2 world = ViewScreenToWorld (view, mouse);

        // 拖入图片：素材、物件和锁定由服务端一次完成
        if (IsFileDropped ()) {
            FilePathList files = LoadDroppedFiles ();

            if (!wasModal && !isModal) {
                int failed = 0;
                for (unsigned int i = 0; i < files.count; i++) {
                    const char *path = files.paths[i];
                    if (PathHasExtension (path, ".dsk") ||
                        PathIsDirectory (path)) {
                        if (!NetRequestImport (path)) failed++;
                        return true;
                    }
                    if (!PathIsImage (path)) {
                        failed++;
                        return true;
                    }

                    Vector2 position = {world.x + i * 20.0f, world.y + i * 20.0f};
                    if (!DropQueuePush (path, position)) failed++;
                }
                if (!dropInProgress && !NetClientLockPending ())
                    StartNextDrop ();
                if (failed > 0) UiShowError (ui, "有文件导入失败");
            } else {
                UiShowError (ui, "弹窗打开时不能拖入文件");
            }

            UnloadDroppedFiles (files);
        }

        if (NetClientSceneVersion () != sceneVersion) {
            ResetClientSceneState (&chosen, &dragged, &dragMoved,
                                   &suppressCanvasInput, &lastCanvasClickId,
                                   &lastCanvasClickTime, ui);
            sceneVersion = NetClientSceneVersion ();
        }

        bool wasDraggingObject = dragged && chosen != NULL;
        bool leftDown = IsMouseButtonDown (MOUSE_BUTTON_LEFT);

        if (!event.mouseCaptured && !suppressCanvasInput) {
            if (IsMouseButtonPressed (MOUSE_BUTTON_LEFT)) {
                Object *picked = ObjectPick (world);
                if (picked == NULL) {
                    if (chosen != NULL) NetUnlockObject (chosen->id);
                    chosen = NULL;
                    dragged = true;
                    dragMoved = false;
                } else if (picked->lockOwnerId != 0 &&
                           picked->lockOwnerId != NetClientUserId ()) {
                    suppressCanvasInput = true;
                } else if (ObjectLockedBySelf (picked)) {
                    double now = GetTime ();
                    if (picked->id == lastCanvasClickId &&
                        now - lastCanvasClickTime <= 0.35) {
                        chosen = picked;
                        UiOpenObjectAssets (ui, picked);
                        suppressCanvasInput = true;
                        lastCanvasClickId = 0;
                        lastCanvasClickTime = 0.0;
                    } else {
                        chosen = picked;
                        grab = Vector2Subtract (world, picked->pos);
                        dragged = true;
                        dragMoved = false;
                        lastCanvasClickId = picked->id;
                        lastCanvasClickTime = now;
                        BringObjectToFront (picked);
                    }
                } else if (!NetRequestLock (picked->id, OnLockDone)) {
                    suppressCanvasInput = true;
                } else {
                    chosen = NULL;
                    dragged = false;
                    suppressCanvasInput = true;
                }
            }

            if (!suppressCanvasInput) {
                bool canDrag = chosen == NULL || ObjectLockedBySelf (chosen);
                if (dragged && leftDown && canDrag) {
                    if (chosen != NULL) {
                        Vector2 oldPosition = chosen->pos;
                        ObjectMove (chosen, Vector2Subtract (world, grab));
                        if (chosen->pos.x != oldPosition.x || chosen->pos.y != oldPosition.y)
                            dragMoved = true;
                    } else {
                        ViewPan (view, GetMouseDelta ());
                    }
                } else {
                    dragged = false;
                }

                if (IsMouseButtonPressed (MOUSE_BUTTON_RIGHT)) {
                    Object *object = ObjectPick (world);
                    if (object == NULL) {
                        UiOpenGlobalContext (ui, mouse);
                    } else if (object->lockOwnerId != 0 &&
                               object->lockOwnerId != NetClientUserId ()) {
                        /* 被其他客户端占用：无操作 */
                    } else if (ObjectLockedBySelf (object)) {
                        chosen = object;
                        BringObjectToFront (object);
                        UiOpenObjectContext (ui, object, mouse);
                    } else if (!NetRequestLock (object->id, OnLockDone)) {
                        suppressCanvasInput = true;
                    } else {
                        chosen = NULL;
                        suppressCanvasInput = true;
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
                                   &lastCanvasClickTime, ui);
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
                NetPublishCursor (ViewScreenToWorld (view, mouse));
            }
        }

        if (!NetClientConnected ()) return false;

        BeginDrawing ();
        ClearBackground (WHITE);
        ViewDraw (view, chosen, ui->font);
        DrawNetCursors (ui, view);
        UiDraw (ui);
#if defined(__EMSCRIPTEN__)
        static Texture2D webDebugTexture = {0};
        if (webDebugTexture.id == 0) {
            Image webDebugImage = GenImageColor (64, 64, MAGENTA);
            webDebugTexture = LoadTextureFromImage (webDebugImage);
            UnloadImage (webDebugImage);
        }
        DrawTexture (webDebugTexture, 400, 400, WHITE);
        DrawRectangle (400, 450, 50, 50, BLUE);
        DrawTextureRec (ui->font.texture, (Rectangle){0, 0, 128, 128},
                        (Vector2){400, 300}, WHITE);
        DrawTextEx (ui->font, "ABC", (Vector2){540, 300}, 32, 0, RED);
#endif
        EndDrawing ();
#undef chosen
#undef dragged
#undef dragMoved
#undef suppressCanvasInput
#undef grab
#undef lastCursorSend
#undef lastObjectSync
#undef lastCanvasClickId
#undef lastCanvasClickTime
#undef closeAfterConnect
#undef sceneVersion
    return true;
}

int ClientRun (Ui *ui, View *view, bool localMode, int localPort,
               const char *localAddress, ClientPumpFn pump,
               ClientOpenRemoteFn openRemote, const char *program) {
    ClientFrameState state;
    ClientFrameInit (&state, ui, view, localMode, localPort, localAddress,
                     pump, openRemote, program);
    while (ClientFrame (&state)) {
    }
    ClientFrameUnload (&state);
    return 0;
}

void ClientFrameUnload (ClientFrameState *state) {
    (void)state;
    DropQueueClear ();
}
