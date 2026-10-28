#include "client.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "assets.h"
#include "cutils.h"
#include "entity.h"
#include "menu.h"
#include "net.h"
#include "net_common.h"
#include "netclient.h"
#include "netutils.h"
#include "object.h"
#include "ui.h"
#include "user.h"
#include "view.h"
#if !defined(__EMSCRIPTEN__)
#include "control.h"
#include "save.h"
#endif
#include "utils.h"

static int localPort;
static const char* localAddress;
static Object* chosen;
static bool dragged;
static bool dragMoved;
static Vector2 lastMouse;
static Vector2 dragPreviewPos;

static bool ObjectLockedBySelf (const Object* object) {
    if (object == NULL || object->lockOwnerId == 0) return false;
    return object->lockOwnerId == NetClientUserId ();
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

static bool SendObjectState (const Object* object, const char* name, int layer, const IdGroup* assets, int assetIndex) {
    Object copy = *object;
    if (name != NULL) snprintf (copy.name, sizeof (copy.name), "%s", name);
    copy.layer = layer;
    copy.assets = *assets;
    copy.assetIndex = assetIndex;
    ObjectTouch (&copy);
    return NetPublishObjectUpsert (&copy);
}

static bool SendObjectAssetAdd (const Object* object, u64 assetId) {
    IdGroup assets = IdGroupCreate ();
    if (!IdGroupCopy (&assets, object->assets.ids, object->assets.count) || !IdGroupAdd (&assets, assetId)) {
        IdGroupUnload (&assets);
        return false;
    }
    bool ok = SendObjectState (object, NULL, object->layer, &assets, object->assetIndex);
    IdGroupUnload (&assets);
    return ok;
}

static bool SendObjectAssetRemove (const Object* object, int index) {
    IdGroup assets = IdGroupCreate ();
    if (!IdGroupCopy (&assets, object->assets.ids, object->assets.count) || !IdGroupRemoveAt (&assets, index)) {
        IdGroupUnload (&assets);
        return false;
    }
    int assetIndex = AssetIndexAfterRemove (object->assetIndex, index, object->assets.count);
    bool ok = SendObjectState (object, NULL, object->layer, &assets, assetIndex);
    IdGroupUnload (&assets);
    return ok;
}

static bool SendObjectAssetMove (const Object* object, int from, int to) {
    IdGroup assets = IdGroupCreate ();
    if (!IdGroupCopy (&assets, object->assets.ids, object->assets.count) || !IdGroupMove (&assets, from, to)) {
        IdGroupUnload (&assets);
        return false;
    }
    bool ok = SendObjectState (object, NULL, object->layer, &assets, object->assetIndex);
    IdGroupUnload (&assets);
    return ok;
}

static void ImportChosenFile (const char* path) {
    if (path == NULL || path[0] == '\0') return;
    if (PathHasExtension (path, ".dsk")) {
        if (!NetRequestImport (path)) UiShowError ("导入请求失败");
    } else if (PathIsImage (path)) {
        if (!NetRequestAssetAdd (path))
            UiShowError ("导入素材失败");
        else
            UiOpenAssetManager ();
    } else {
        UiShowError ("不支持的文件");
    }
}

void ClientInit (bool localMode, int serverPort, const char* serverAddress) {
    localPort = serverPort;
    localAddress = serverAddress;
    chosen = NULL;
    dragged = false;
    dragMoved = false;
    lastMouse = (Vector2){0};
    dragPreviewPos = (Vector2){0};
    CutilsSetFileHandler (ImportChosenFile);
    UiSetLocalMode (localMode);
    if (!localMode) {
        UiSetConnectMenuVisible (false);
    }
}

static NetutilStream* clientStream;
static bool clientActive;
static bool clientClosing;
static bool clientConnectDone;
static bool clientConnectOk;
static Buffer clientIn;
static u64 nextRequestId = 1;
static u64 clientUserId;
static u64 clientLockedId;

static void ClientCloseNow (void);
static void ClientOnConnected (NetutilStream* stream, void* user);
static void ClientOnRead (NetutilStream* stream, const unsigned char* data, size_t size, void* user);
static void ClientOnClosed (NetutilStream* stream, void* user);

static const NetutilEvents clientEvents = {ClientOnConnected, ClientOnRead, ClientOnClosed, NULL};

static void ClientOnConnected (NetutilStream* stream, void* user) {
    (void)stream;
    (void)user;
    clientConnectDone = true;
    clientConnectOk = true;
    clientActive = true;
    clientClosing = false;
}

static void ClientOnRead (NetutilStream* stream, const unsigned char* data, size_t size, void* user) {
    (void)stream;
    (void)user;
    if (clientIn.len + size > NET_MAX_FRAME + 4 || !BufferAppend (&clientIn, data, size)) {
        ClientCloseNow ();
    }
}

static void ClientOnClosed (NetutilStream* stream, void* user) {
    (void)stream;
    (void)user;
    clientStream = NULL;
    clientActive = false;
    clientClosing = false;
    clientConnectDone = true;
    clientConnectOk = false;
    clientUserId = 0;
    clientLockedId = 0;
    BufferFree (&clientIn);
}

static void ClientCloseNow (void) {
    if (clientStream != NULL) {
        clientClosing = true;
        NetutilClose (clientStream);
    } else {
        clientActive = false;
        clientClosing = false;
        BufferFree (&clientIn);
    }
    clientUserId = 0;
}

static void ClientWaitClosed (int milliseconds) {
    for (int elapsed = 0; elapsed < milliseconds && clientStream != NULL; elapsed++) NetutilPump (1);
}

u64 NetClientUserId (void) { return clientUserId; }

u64 NetClientLockedObjectId (void) { return clientLockedId; }

static bool ClientSendFrame (Buffer* frame) {
    if (!clientActive || clientClosing || clientStream == NULL || frame->len > NET_MAX_OUTPUT) {
        BufferFree (frame);
        return false;
    }
    bool ok = NetutilWrite (clientStream, frame->data, frame->len);
    BufferFree (frame);
    return ok;
}

bool NetClientConnect (const char* host, int port, const char* username) {
    if (username == NULL || username[0] == '\0') return false;
    ClientCloseNow ();
    ClientWaitClosed (1000);
    if (clientStream != NULL) return false;
    UserUnload ();
    nextRequestId = 1;
    clientUserId = 0;
    clientLockedId = 0;

    if (!NetutilInit ()) return false;
#if defined(__EMSCRIPTEN__)
    NetutilAddress address = {host};
#else
    NetutilAddress address = {host, (uint16_t)port};
#endif
    clientConnectDone = false;
    clientConnectOk = false;
    if (!NetutilConnect (&address, &clientEvents, NULL, &clientStream)) return false;

    for (int elapsed = 0; !clientConnectDone && elapsed < 5000; elapsed++) NetutilPump (1);
    if (!clientConnectDone || !clientConnectOk) {
        ClientCloseNow ();
        ClientWaitClosed (1000);
        return false;
    }

    clientActive = true;
    clientClosing = false;
    Buffer payload = {0};
    Buffer frame = {0};
    bool ok = BufferPutString (&payload, username) && FrameMake (&frame, NET_HELLO, &payload);
    BufferFree (&payload);
    if (!ok || !ClientSendFrame (&frame)) {
        ClientCloseNow ();
        ClientWaitClosed (1000);
        return false;
    }
    return true;
}

bool NetClientConnected (void) { return clientStream != NULL && clientActive && !clientClosing; }

void NetClientDisconnect (void) { ClientCloseNow (); }

bool NetRequestObjectCreate (Vector2 center) {
    if (clientLockedId != 0) NetUnlockObject (clientLockedId);
    u64 requestId = nextRequestId++;

    Buffer payload = {0};
    Buffer frame = {0};
    bool ok = BufferPutU64 (&payload, requestId) && BufferPutString (&payload, "object") &&
              BufferPutF32 (&payload, center.x) && BufferPutF32 (&payload, center.y) && BufferPutU32 (&payload, 0) &&
              BufferPutU32 (&payload, 0) && FrameMake (&frame, NET_REQ_OBJECT_CREATE, &payload);
    BufferFree (&payload);
    if (!ok) {
        BufferFree (&frame);
        return false;
    }
    if (!ClientSendFrame (&frame)) {
        return false;
    }
    return true;
}

bool NetRequestAssetRename (u64 id, const char* name) {
    if (id == 0 || name == NULL || name[0] == '\0') return false;
    Buffer frame = {0};
    if (!FrameAssetRename (&frame, id, name)) {
        BufferFree (&frame);
        return false;
    }
    return ClientSendFrame (&frame);
}

bool NetRequestAssetReplace (u64 id, RawFile file) {
    if (id == 0 || file.ext[0] == '\0' || file.data == NULL || file.size == 0 || file.size > NET_MAX_FRAME ||
        file.size > INT_MAX)
        return false;

    Buffer frame = {0};
    if (!FrameAssetReplace (&frame, id, file.ext, file.data, (uint32_t)file.size)) {
        BufferFree (&frame);
        return false;
    }
    return ClientSendFrame (&frame);
}

bool NetRequestAssetDelete (u64 id) {
    Buffer frame = {0};
    if (!FrameAssetDelete (&frame, id)) {
        BufferFree (&frame);
        return false;
    }
    return ClientSendFrame (&frame);
}

bool NetPublishObjectUpsert (const Object* object) {
    if (object == NULL) return false;

    Buffer frame = {0};
    bool ok = FrameObjectUpsert (
        &frame, object->id, OBJECT_FIELD_ALL & ~(OBJECT_FIELD_LOCK_OWNER | OBJECT_FIELD_TIMESTAMP), object->name,
        object->pos.x, object->pos.y, object->layer, object->timestamp, object->hidden, (uint32_t)object->assetIndex,
        object->assets.ids, (uint32_t)object->assets.count, 0);
    if (!ok) {
        BufferFree (&frame);
        return false;
    }
    return ClientSendFrame (&frame);
}

bool NetRequestScript (const char* source, const char* selfExpr) {
    if (source == NULL) return false;
    if (selfExpr == NULL) selfExpr = "";
    if (strlen (source) > 0xFFFFu || strlen (selfExpr) > 0xFFFFu) return false;

    Buffer payload = {0};
    Buffer frame = {0};
    bool ok = BufferPutString (&payload, source) && BufferPutString (&payload, selfExpr) &&
              FrameMake (&frame, NET_REQ_SCRIPT, &payload);
    BufferFree (&payload);
    if (!ok) {
        BufferFree (&frame);
        return false;
    }
    return ClientSendFrame (&frame);
}

bool NetRequestMenu (u64 id, int index, const char* name) {
    if (index < 0 || name == NULL || strlen (name) > 0xFFFFu) return false;

    Buffer frame = {0};
    if (!FrameMenuRequest (&frame, id, (uint32_t)index, name)) {
        BufferFree (&frame);
        return false;
    }
    return ClientSendFrame (&frame);
}

bool NetRequestImport (const char* path) {
    if (path == NULL || path[0] == '\0') return false;

    RawFile file = {0};
#if !defined(__EMSCRIPTEN__)
    if (PathIsDirectory (path))
        file = SavePackFolderData (path);
    else
#endif
        file = RawFileRead (path);
    if (file.data == NULL || file.size == 0 || file.size > UINT32_MAX) {
        RawFileFree (&file);
        return false;
    }

    Buffer payload = {0};
    Buffer frame = {0};
    bool ok = BufferPutU32 (&payload, (uint32_t)file.size) && BufferAppend (&payload, file.data, file.size) &&
              FrameMake (&frame, NET_REQ_IMPORT, &payload);
    BufferFree (&payload);
    RawFileFree (&file);
    if (!ok) {
        BufferFree (&frame);
        return false;
    }
    return ClientSendFrame (&frame);
}

bool NetRequestDragUpdate (u64 id, Vector2 delta) {
    if (id == 0) return false;
    Buffer frame = {0};
    if (!FrameObjectDragUpdate (&frame, id, delta.x, delta.y)) {
        BufferFree (&frame);
        return false;
    }
    return ClientSendFrame (&frame);
}

bool NetRequestDragEnd (u64 id) {
    if (id == 0) return false;
    Buffer frame = {0};
    if (!FrameObjectLock (&frame, NET_DRAG_END, id)) {
        BufferFree (&frame);
        return false;
    }
    return ClientSendFrame (&frame);
}

bool NetRequestObjectDelete (u64 id) {
    Buffer frame = {0};
    if (!FrameObjectDelete (&frame, id)) {
        BufferFree (&frame);
        return false;
    }
    return ClientSendFrame (&frame);
}

bool NetRequestAssetAdd (const char* path) {
    RawFile file = RawFileRead (path);
    if (file.data == NULL || file.size == 0 || file.size > INT_MAX) {
        RawFileFree (&file);
        return false;
    }

    u64 requestId = nextRequestId++;

    Buffer payload = {0};
    Buffer frame = {0};
    bool ok = BufferPutU64 (&payload, requestId) && BufferPutU8 (&payload, 0) && BufferPutString (&payload, "asset") &&
              BufferPutString (&payload, file.ext) && BufferPutU32 (&payload, (uint32_t)file.size) &&
              BufferAppend (&payload, file.data, file.size) && FrameMake (&frame, NET_REQ_ASSET_ADD, &payload);
    BufferFree (&payload);
    RawFileFree (&file);
    if (!ok) {
        BufferFree (&frame);
        return false;
    }
    if (!ClientSendFrame (&frame)) {
        return false;
    }
    return true;
}

bool NetRequestAssetObject (const char* path, Vector2 position) {
    if (clientLockedId != 0) NetUnlockObject (clientLockedId);
    RawFile file = RawFileRead (path);
    if (file.data == NULL || file.size == 0 || file.size > INT_MAX) {
        RawFileFree (&file);
        return false;
    }

    u64 requestId = nextRequestId++;

    Buffer payload = {0};
    Buffer frame = {0};
    bool ok = BufferPutU64 (&payload, requestId) && BufferPutU8 (&payload, 1) && BufferPutString (&payload, "asset") &&
              BufferPutString (&payload, file.ext) && BufferPutU32 (&payload, (uint32_t)file.size) &&
              BufferAppend (&payload, file.data, file.size) && BufferPutString (&payload, "object") &&
              BufferPutF32 (&payload, position.x) && BufferPutF32 (&payload, position.y) &&
              BufferPutU32 (&payload, 0) && FrameMake (&frame, NET_REQ_ASSET_ADD, &payload);
    BufferFree (&payload);
    RawFileFree (&file);
    if (!ok) {
        BufferFree (&frame);
        return false;
    }
    if (!ClientSendFrame (&frame)) {
        return false;
    }
    return true;
}

bool NetPublishCursor (Vector2 worldPos) {
    Buffer frame = {0};
    if (!FrameCursor (&frame, worldPos.x, worldPos.y)) {
        BufferFree (&frame);
        return false;
    }
    return ClientSendFrame (&frame);
}

bool NetRequestLock (u64 id) {
    u64 selfId = NetClientUserId ();
    if (selfId == 0 || id == 0) return false;

    Object* object = EntityFind (id);
    if (object == NULL) return false;
    if (object->lockOwnerId == selfId) {
        clientLockedId = id;
        return true;
    }
    if (object->lockOwnerId != 0) return false;
    if (clientLockedId != 0 && clientLockedId != id) NetUnlockObject (clientLockedId);

    Buffer frame = {0};
    if (!FrameObjectLock (&frame, NET_LOCK, id)) {
        BufferFree (&frame);
        return false;
    }
    if (!ClientSendFrame (&frame)) return false;
    return true;
}

static bool SendUnlockObject (u64 id, enum NetPacketType type, bool clearLocal) {
    if (clearLocal) {
        Object* object = EntityFind (id);
        if (object != NULL && object->lockOwnerId == NetClientUserId ()) object->lockOwnerId = 0;
        if (id == clientLockedId) clientLockedId = 0;
    }

    Buffer frame = {0};
    if (!FrameObjectLock (&frame, type, id)) {
        BufferFree (&frame);
        return false;
    }
    return ClientSendFrame (&frame);
}

bool NetUnlockObject (u64 id) { return SendUnlockObject (id, NET_UNLOCK_NOTIFY, true); }

static bool ApplyClientPacket (ClientPacket* packet, Vector2 cursor) {
    bool ok = true;
    ClientObjectPacket* objectPacket = NULL;

    switch (packet->type) {
        case NET_HEARTBEAT:
            ok = NetPublishCursor (cursor);
            break;
        case NET_HELLO:
            clientUserId = packet->u.hello.selfId;
            clientLockedId = 0;
            break;
        case NET_ASSET_ADD: {
            ClientAssetAddPacket* p = &packet->u.assetAdd;
            Asset* asset = EntityFind (p->id);
            if (asset == NULL) {
                asset = AssetCreate (p->name, p->id);
                if (asset != NULL) {
                    RawFile file = {p->data, p->size, ""};
                    snprintf (file.ext, sizeof (file.ext), "%s", p->ext);
                    AssetLoad (asset, file);
                }
            }
        } break;
        case NET_ASSET_RENAME: {
            ClientAssetRenamePacket* p = &packet->u.assetRename;
            Asset* asset = EntityFind (p->id);
            if (asset != NULL && strcmp (AssetName (asset), p->name) != 0) AssetRename (asset, p->name);
        } break;
        case NET_ASSET_REPLACE: {
            ClientAssetReplacePacket* p = &packet->u.assetReplace;
            Asset* asset = EntityFind (p->id);
            if (asset != NULL) {
                RawFile file = {p->data, p->size, ""};
                snprintf (file.ext, sizeof (file.ext), "%s", p->ext);
                AssetLoad (asset, file);
            }
        } break;
        case NET_ASSET_DELETE: {
            u64 id = packet->u.id.id;
            Asset* asset = EntityFind (id);
            if (asset != NULL) {
                IdGroup ids = ObjectIds ();
                for (int i = 0; i < ids.count; i++) {
                    Object* object = EntityFind (ids.ids[i]);
                    if (object == NULL) continue;
                    for (int a = object->assets.count - 1; a >= 0; a--) {
                        if (object->assets.ids[a] == id) IdGroupRemoveAt (&object->assets, a);
                    }
                }
                IdGroupUnload (&ids);
                AssetDelete (asset);
            }
        } break;
        case NET_OBJECT_CREATED:
            objectPacket = &packet->u.objectCreated.object;
            break;
        case NET_OBJECT_UPSERT:
            objectPacket = &packet->u.object;
            break;
        case NET_LOCK_FAILED: {
            u64 id = packet->u.id.id;
            if (chosen != NULL && chosen->id == id) {
                chosen = NULL;
                dragged = false;
                dragMoved = false;
                ViewClearDragPreview ();
            }
            if (uiSelectedObject != NULL && uiSelectedObject->id == id) UiSelectObject (NULL);
            if (id == clientLockedId) clientLockedId = 0;
        } break;
        case NET_OBJECT_DELETE: {
            u64 id = packet->u.id.id;
            Object* object = EntityFind (id);
            if (id == clientLockedId) clientLockedId = 0;
            if (object != NULL) ObjectDelete (object);
        } break;
        case NET_USER_STATE: {
            ClientUserPacket* p = &packet->u.user;
            UserAdd (p->id, p->username);
            UserSetColor (p->id, p->color);
            UserSetCanScript (p->id, p->canscript);
            UserSetCursor (p->id, p->hasCursor, p->x, p->y);
        } break;
        case NET_USER_LEAVE: {
            u64 id = packet->u.id.id;
            UserRemove (id);
            if (id == clientUserId) clientLockedId = 0;
            IdGroup ids = ObjectIds ();
            for (int i = 0; i < ids.count; i++) {
                Object* object = EntityFind (ids.ids[i]);
                if (object != NULL && object->lockOwnerId == id) object->lockOwnerId = 0;
            }
            IdGroupUnload (&ids);
        } break;
        case NET_CURSOR_STATE: {
            ClientCursorPacket* p = &packet->u.cursor;
            UserSetCursor (p->id, true, p->x, p->y);
        } break;
        case NET_MENU: {
            ClientMenuPacket* p = &packet->u.menu;
            ok = MenuSetNames (p->id, (const char* const*)p->names, (int)p->count);
        } break;
        case NET_MESSAGE:
            UiShowMessageTextKind (packet->u.message.text, UI_MESSAGE_SCRIPT);
            break;
        case NET_ERROR:
            UiShowMessageTextKind (packet->u.message.text, UI_MESSAGE_ERROR);
            break;
        case NET_INFO:
            UiShowMessageTextKind (packet->u.message.text, UI_MESSAGE_INFO);
            break;
        default:
            ok = false;
            break;
    }

    if (objectPacket != NULL) {
        ClientObjectPacket* p = objectPacket;
        Object* object = EntityFind (p->id);
        if (object == NULL &&
            (p->fields & (OBJECT_FIELD_NAME | OBJECT_FIELD_POSITION | OBJECT_FIELD_LAYER | OBJECT_FIELD_TIMESTAMP)) ==
                (OBJECT_FIELD_NAME | OBJECT_FIELD_POSITION | OBJECT_FIELD_LAYER | OBJECT_FIELD_TIMESTAMP)) {
            object = ObjectAdd (p->name, (Vector2){p->x, p->y}, (int)(int32_t)p->layer, p->timestamp, p->id);
        }
        if (object != NULL) {
            if ((p->fields & OBJECT_FIELD_NAME) && strcmp (object->name, p->name) != 0) ObjectRename (object, p->name);
            if (p->fields & OBJECT_FIELD_POSITION) object->pos = (Vector2){p->x, p->y};
            if (p->fields & OBJECT_FIELD_LAYER) object->layer = (int)(int32_t)p->layer;
            if (p->fields & OBJECT_FIELD_TIMESTAMP) object->timestamp = p->timestamp;
            if (p->fields & OBJECT_FIELD_HIDDEN) object->hidden = p->hidden;
            if (p->fields & OBJECT_FIELD_ASSETS) {
                IdGroupClear (&object->assets);
                for (uint32_t i = 0; i < p->assetCount; i++) {
                    if (EntityFind (p->assetIds[i]) != NULL) IdGroupAdd (&object->assets, p->assetIds[i]);
                }
            }
            if (p->fields & OBJECT_FIELD_ASSET_INDEX) {
                object->assetIndex = p->assetIndex < (uint32_t)object->assets.count ? (int)p->assetIndex : 0;
            }
            if (p->fields & OBJECT_FIELD_LOCK_OWNER) {
                object->lockOwnerId = p->lockOwnerId;
                if (p->lockOwnerId == clientUserId)
                    clientLockedId = p->id;
                else if (p->id == clientLockedId)
                    clientLockedId = 0;
            }
        }
    }

    ClientPacketFree (packet);
    return ok;
}

static bool ApplyUiEvent (UiEvent* event, Vector2 world) {
    if (chosen != NULL && !ObjectContains (chosen)) {
        chosen = NULL;
        dragged = false;
        dragMoved = false;
        ViewClearDragPreview ();
    }

    switch (event->action) {
        case UI_ACTION_CANVAS_ZOOM:
            ViewZoomAt (event->mousePosition, event->u.canvasZoom.wheel);
            break;
        case UI_ACTION_CLOSE:
            return false;
        case UI_ACTION_LOCK_OBJECT: {
            Object* object = event->u.object.object;
            chosen = NULL;
            dragged = false;
            if (!ObjectLockedBySelf (object) && !NetRequestLock (object->id)) UiShowError ("锁定物件失败");
            break;
        }
        case UI_ACTION_RUN_MENU: {
            Object* object = event->u.objectMenu.object;
            int index = event->u.objectMenu.index;
            const char* name = MenuName (object->id, index);
            if (name == NULL || !NetRequestMenu (object->id, index, name)) UiShowError ("执行菜单失败");
            break;
        }
        case UI_ACTION_UNLOCK_OBJECT: {
            Object* object = event->u.object.object;
            if (chosen == object) chosen = NULL;
            dragged = false;
            NetUnlockObject (object->id);
            break;
        }
        case UI_ACTION_CREATE_OBJECT: {
            Object* previous = uiSelectedObject;
            if (previous != NULL) {
                NetUnlockObject (previous->id);
                if (chosen == previous) chosen = NULL;
                uiSelectedObject = NULL;
            }
            Vector2 center = viewCam.target;
            if (!NetRequestObjectCreate (center)) UiShowError ("新建物件失败");
            break;
        }
        case UI_ACTION_RENAME_OBJECT: {
            Object* object = event->u.objectName.object;
            if (!ObjectLockedBySelf (object)) break;
            if (ObjectRename (object, event->u.objectName.name))
                NetPublishObjectUpsert (object);
            else
                UiShowError ("重命名失败，名称已存在");
            break;
        }
        case UI_ACTION_DELETE_OBJECT: {
            Object* object = event->u.object.object;
            if (!ObjectLockedBySelf (object)) break;
            u64 id = object->id;
            if (chosen == object) chosen = NULL;
            dragged = false;
            if (ObjectDelete (object)) {
                UiObjectDeleted ();
                NetRequestObjectDelete (id);
            } else {
                UiShowError ("删除物件失败");
            }
            break;
        }
        case UI_ACTION_ADD_OBJECT_ASSET: {
            Object* object = event->u.objectAsset.object;
            Asset* asset = event->u.objectAsset.asset;
            if (ObjectLockedBySelf (object)) {
                IdGroup* assets = ObjectAssets (object);
                if (assets != NULL && IdGroupAdd (assets, AssetId (asset))) {
                    ObjectTouch (object);
                    NetPublishObjectUpsert (object);
                } else {
                    UiShowError ("添加素材失败");
                }
            } else if (!SendObjectAssetAdd (object, AssetId (asset))) {
                UiShowError ("添加素材失败");
            }
            break;
        }
        case UI_ACTION_USE_OBJECT_ASSET: {
            Object* object = event->u.objectAssets.object;
            int index = event->u.objectAssets.index;
            if (index < 0 || index >= object->assets.count) break;
            if (!ObjectLockedBySelf (object)) {
                UiShowError ("物件未锁定，无法切换素材");
                break;
            }
            object->assetIndex = index;
            if (!NetPublishObjectUpsert (object)) UiShowError ("切换素材失败");
            break;
        }
        case UI_ACTION_REMOVE_OBJECT_ASSET: {
            Object* object = event->u.objectAssets.object;
            int index = event->u.objectAssets.index;
            if (ObjectLockedBySelf (object)) {
                IdGroup* assets = ObjectAssets (object);
                int assetIndex = AssetIndexAfterRemove (object->assetIndex, index, object->assets.count);
                if (assets != NULL && IdGroupRemoveAt (assets, index)) {
                    object->assetIndex = assetIndex;
                    ObjectTouch (object);
                    NetPublishObjectUpsert (object);
                } else {
                    UiShowError ("移除素材失败");
                }
            } else if (!SendObjectAssetRemove (object, index)) {
                UiShowError ("移除素材失败");
            }
            break;
        }
        case UI_ACTION_MOVE_OBJECT_ASSET: {
            Object* object = event->u.objectAssets.object;
            int index = event->u.objectAssets.index;
            int target = event->u.objectAssets.target;
            if (ObjectLockedBySelf (object)) {
                IdGroup* assets = ObjectAssets (object);
                if (assets != NULL && IdGroupMove (assets, index, target)) {
                    ObjectTouch (object);
                    NetPublishObjectUpsert (object);
                } else {
                    UiShowError ("排序素材失败");
                }
            } else if (!SendObjectAssetMove (object, index, target)) {
                UiShowError ("排序素材失败");
            }
            break;
        }
        case UI_ACTION_SET_OBJECT_LAYER: {
            Object* object = event->u.objectLayer.object;
            if (!ObjectLockedBySelf (object)) break;
            if (ObjectSetLayer (object, event->u.objectLayer.layer))
                NetPublishObjectUpsert (object);
            else
                UiShowError ("修改层数失败");
            break;
        }
        case UI_ACTION_RUN_GLOBAL_MENU: {
            int index = event->u.menu.index;
            const char* name = MenuName (0, index);
            if (name == NULL || !NetRequestMenu (0, index, name)) UiShowError ("执行菜单失败");
            break;
        }
        case UI_ACTION_RUN_USER_MENU: {
            u64 userId = NetClientUserId ();
            int index = event->u.menu.index;
            const char* name = MenuName (userId, index);
            if (userId == 0 || name == NULL || !NetRequestMenu (userId, index, name)) UiShowError ("执行菜单失败");
            break;
        }
        case UI_ACTION_OPEN_SERVER: {
            if (localPort == 0) {
                UiShowError ("当前没有本地服务");
                break;
            }
            char message[160];
            snprintf (message, sizeof (message), "已开放 %s:%d", localAddress, localPort);
            UiShowMessage (message);
            UiSetConnectMenuVisible (false);
            break;
        }
        case UI_ACTION_CONNECT:
            if (!CutilsOpenRemote (event->u.connect.host, event->u.connect.port, event->u.connect.username)) {
                UiShowError ("打开新窗口失败");
            } else if (SceneIsEmpty ()) {
                return false;
            }
            break;
        case UI_ACTION_IMPORT_SAVE:
            if (!NetRequestImport (event->u.path.path)) UiShowError ("导入请求失败");
            break;
        case UI_ACTION_EXPORT_SAVE: {
            User* self = (User*)EntityFind (NetClientUserId ());
            if (self != NULL && !self->canscript) {
                UiShowError ("当前用户不允许导出");
                break;
            }
#if defined(__EMSCRIPTEN__)
            UiShowError ("当前客户端没有本地服务端，无法导出");
#else
            if (!ControlServerSendExport (event->u.path.path)) UiShowError ("当前客户端没有本地服务端，无法导出");
#endif
            break;
        }
        case UI_ACTION_IMPORT:
            if (!NetRequestAssetAdd (event->u.path.path)) {
                UiShowError ("导入素材失败");
            } else {
                UiOpenAssetManager ();
            }
            break;
        case UI_ACTION_RENAME_ASSET:
            if (!NetRequestAssetRename (AssetId (event->u.assetName.asset), event->u.assetName.name))
                UiShowError ("重命名失败");
            break;
        case UI_ACTION_REPLACE_ASSET: {
            RawFile file = RawFileRead (event->u.assetPath.path);
            bool sent =
                file.data != NULL && file.size > 0 && NetRequestAssetReplace (AssetId (event->u.assetPath.asset), file);
            RawFileFree (&file);
            if (!sent) UiShowError ("更换素材失败");
            break;
        }
        case UI_ACTION_DELETE_ASSET:
            if (!NetRequestAssetDelete (AssetId (event->u.asset.asset))) UiShowError ("删除素材失败");
            break;
        case UI_ACTION_SET_BACKGROUND:
            ViewSetBackground (event->u.background.style);
            break;
        case UI_ACTION_VIEW_ORIGIN:
            ViewSetOrigin ();
            break;
        case UI_ACTION_SET_ZOOM:
            ViewSetZoom (event->u.zoom.value);
            break;
        case UI_ACTION_RUN_SCRIPT: {
            User* self = (User*)EntityFind (NetClientUserId ());
            if (self != NULL && !self->canscript) {
                UiShowError ("当前用户不允许提交脚本");
                break;
            }
            if (NetRequestScript (uiScriptText.data != NULL ? uiScriptText.data : "", uiScriptSelf)) {
                uiScriptOpen = false;
                uiScriptScroll = 0.0f;
                TextBufferFree (&uiScriptText);
                UiShowMessage ("脚本已提交");
            } else {
                UiShowError ("脚本提交失败");
            }
            break;
        }
        case UI_ACTION_CANVAS_CLICK: {
            Object* picked = ObjectPick (world);
            lastMouse = world;
            dragMoved = false;
            ViewClearDragPreview ();

            if (picked == NULL) {
                if (chosen != NULL) NetUnlockObject (chosen->id);
                chosen = NULL;
                dragged = true;
                break;
            }
            if (picked->lockOwnerId != 0 && picked->lockOwnerId != NetClientUserId ()) {
                chosen = NULL;
                dragged = false;
                break;
            }

            chosen = picked;
            dragged = true;
            dragPreviewPos = picked->pos;
            if (!ObjectLockedBySelf (picked) && !NetRequestLock (picked->id)) {
                chosen = NULL;
                dragged = false;
            }
        } break;
        case UI_ACTION_CANVAS_DRAG: {
            if (!dragged) break;

            Vector2 delta = {world.x - lastMouse.x, world.y - lastMouse.y};
            lastMouse = world;
            if (chosen == NULL) {
                ViewPan (event->mouseDelta);
                break;
            }
            if (chosen->lockOwnerId != 0 && !ObjectLockedBySelf (chosen)) {
                dragged = false;
                dragMoved = false;
                ViewClearDragPreview ();
                break;
            }

            dragPreviewPos.x += delta.x;
            dragPreviewPos.y += delta.y;
            dragMoved = true;
            NetRequestDragUpdate (chosen->id, delta);
            ViewSetDragPreview (chosen, dragPreviewPos);
        } break;
        case UI_ACTION_CANVAS_RELEASE:
        case UI_ACTION_CANVAS_MOVE: {
            if (dragged && chosen != NULL && dragMoved) NetRequestDragEnd (chosen->id);
            dragged = false;
            dragMoved = false;
            ViewClearDragPreview ();
        } break;
        case UI_ACTION_CANVAS_RIGHT_CLICK: {
            Object* object = ObjectPick (world);
            if (dragged && chosen != NULL && dragMoved) NetRequestDragEnd (chosen->id);
            dragMoved = false;
            dragged = false;
            ViewClearDragPreview ();
            if (object != NULL && ObjectLockedBySelf (object)) {
                chosen = object;
                UiOpenObjectContext (object, event->mousePosition);
            } else {
                UiOpenGlobalContext (event->mousePosition);
            }
        } break;

        case UI_ACTION_NONE:
        default:
            break;
    }

    if (event->action != UI_ACTION_NONE && event->action < UI_ACTION_CANVAS_MOVE) {
        if (dragged && chosen != NULL && dragMoved) NetRequestDragEnd (chosen->id);
        dragMoved = false;
        dragged = false;
        ViewClearDragPreview ();
    }

    if (event->files.count > 0) {
        int failed = 0;
        for (unsigned int i = 0; i < event->files.count; i++) {
            const char* path = event->files.paths[i];
            if (PathHasExtension (path, ".dsk") || PathIsDirectory (path)) {
                if (!NetRequestImport (path)) failed++;
                continue;
            }
            if (!PathIsImage (path)) {
                failed++;
                continue;
            }

            Vector2 position = {world.x + i * 20.0f, world.y + i * 20.0f};
            if (!NetRequestAssetObject (path, position)) failed++;
        }
        UnloadDroppedFiles (event->files);
        if (failed > 0) UiShowError ("有文件导入失败");
    }

    return true;
}

bool ClientFrame (void) {
    if (!NetClientConnected ()) return false;

    CutilsPump ();
    CutilsMouseReset ();
    CutilsKeysReset ();
    ViewUpdateViewport ();

    UiEvent event = UiUpdate ();
    Vector2 world = ViewScreenToWorld (event.mousePosition);

    if (!ApplyUiEvent (&event, world)) return false;

    BeginDrawing ();
    ClearBackground (WHITE);
    ViewDraw (chosen);
    UiDraw ();
    EndDrawing ();

    NetutilPump (0);
    for (;;) {
        ClientPacket packet;
        if (!ClientPacketRead (&clientIn, &packet)) {
            ClientCloseNow ();
            return false;
        }
        if (packet.type == NET_NONE) break;
        if (!ApplyClientPacket (&packet, world)) {
            ClientCloseNow ();
            return false;
        }
    }

    return true;
}
