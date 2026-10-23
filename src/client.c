#define _GNU_SOURCE

#include "net.h"
#include "net_common.h"
#include "netutils.h"
#include "ui.h"
#include "utils.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "raylib.h"
#include "assets.h"
#include "menu.h"
#include "object.h"
#include "entity.h"
#include "user.h"

static NetutilStream *clientStream;
static bool clientActive;
static bool clientClosing;
static bool clientConnectDone;
static bool clientConnectOk;
static Buffer clientIn;
static int clientPendingFlags;
static u64 nextRequestId = 1;
static u64 objectRequestId;
static u64 clientUserId;
static u64 clientLockedId;
static u64 pendingLockId;
static NetLockDoneFn lockDone;
static int lockResult;
static double lockStarted;
static double lockTimeout;
static u64 lockLostId;
static u64 clientSceneVersion;

static bool ParseClientFrames (int *flags);
static void ClientCloseNow (void);
static void ClientOnConnected (NetutilStream *stream, void *user);
static void ClientOnRead (NetutilStream *stream, const unsigned char *data,
                          size_t size, void *user);
static void ClientOnClosed (NetutilStream *stream, void *user);

static const NetutilEvents clientEvents = {
    ClientOnConnected, ClientOnRead, ClientOnClosed, NULL
};

static void ClientPump (int milliseconds) {
    NetutilPump (milliseconds);
}

static void ClientOnConnected (NetutilStream *stream, void *user) {
    (void)stream;
    (void)user;
    clientConnectDone = true;
    clientConnectOk = true;
    clientActive = true;
    clientClosing = false;
}

static void ClientOnRead (NetutilStream *stream, const unsigned char *data,
                          size_t size, void *user) {
    (void)stream;
    (void)user;
    if (clientIn.len + size > NET_MAX_FRAME + 4 ||
        !BufferAppend (&clientIn, data, size)) {
        ClientCloseNow ();
        return;
    }
    int flags = NET_UPDATE_NONE;
    if (!ParseClientFrames (&flags)) {
        ClientCloseNow ();
        return;
    }
    clientPendingFlags |= flags;
    if (flags & NET_UPDATE_REJECTED) ClientCloseNow ();
}

static void ClientOnClosed (NetutilStream *stream, void *user) {
    (void)stream;
    (void)user;
    clientStream = NULL;
    clientActive = false;
    clientClosing = false;
    clientConnectDone = true;
    clientConnectOk = false;
    clientUserId = 0;
    clientLockedId = 0;
    pendingLockId = 0;
    objectRequestId = 0;
    lockDone = NULL;
    lockResult = 0;
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
    pendingLockId = 0;
    objectRequestId = 0;
    lockDone = NULL;
    lockResult = 0;
}

static void ClientWaitClosed (int milliseconds) {
    for (int elapsed = 0; elapsed < milliseconds && clientStream != NULL;
         elapsed++)
        NetutilPump (1);
}

u64 NetClientUserId (void) {
    return clientUserId;
}

u64 NetClientLockedObjectId (void) {
    return clientLockedId;
}

bool NetClientLockPending (void) {
    return lockDone != NULL;
}

u64 NetClientLockLostId (void) {
    return lockLostId;
}

u64 NetClientSceneVersion (void) {
    return clientSceneVersion;
}

static bool ClientSendFrame (Buffer *frame) {
    if (!clientActive || clientClosing || clientStream == NULL ||
        frame->len > NET_MAX_OUTPUT) {
        BufferFree (frame);
        return false;
    }
    bool ok = NetutilWrite (clientStream, frame->data, frame->len);
    BufferFree (frame);
    return ok;
}

bool NetClientConnect (const char *host, int port, const char *username) {
    if (username == NULL || username[0] == '\0') return false;
    ClientCloseNow ();
    ClientWaitClosed (1000);
    if (clientStream != NULL) return false;
    UserUnload ();
    clientPendingFlags = NET_UPDATE_NONE;
    nextRequestId = 1;
    objectRequestId = 0;
    pendingLockId = 0;
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
    if (!NetutilConnect (&address, &clientEvents, NULL, &clientStream))
        return false;

    for (int elapsed = 0; !clientConnectDone && elapsed < 5000; elapsed++)
        NetutilPump (1);
    if (!clientConnectDone || !clientConnectOk) {
        ClientCloseNow ();
        ClientWaitClosed (1000);
        return false;
    }

    clientActive = true;
    clientClosing = false;
    Buffer payload = {0};
    Buffer frame = {0};
    bool ok = BufferPutString (&payload, username) &&
              FrameMake (&frame, NET_HELLO, &payload);
    BufferFree (&payload);
    if (!ok || !ClientSendFrame (&frame)) {
        ClientCloseNow ();
        ClientWaitClosed (1000);
        return false;
    }
    return true;
}

bool NetClientConnected (void) {
    return clientStream != NULL && clientActive && !clientClosing;
}

void NetClientDisconnect (void) {
    ClientCloseNow ();
}

bool NetRequestObjectCreate (Vector2 center, NetLockDoneFn done) {
    if (clientLockedId != 0) NetUnlockObject (clientLockedId);
    u64 requestId = nextRequestId++;
    objectRequestId = requestId;
    lockDone = done;
    lockResult = 0;
    pendingLockId = 0;
    lockStarted = GetTime ();
    lockTimeout = 30.0;

    u64 timestamp = (u64)(GetTime () * 1000000.0);
    if (timestamp == 0) timestamp = 1;

    Buffer payload = {0};
    Buffer frame = {0};
    bool ok = BufferPutU64 (&payload, requestId) &&
              BufferPutString (&payload, "object") &&
              BufferPutF32 (&payload, center.x) &&
              BufferPutF32 (&payload, center.y) &&
              BufferPutU32 (&payload, 0) &&
              BufferPutU64 (&payload, timestamp) &&
              BufferPutU32 (&payload, 0) &&
              FrameMake (&frame, NET_REQ_OBJECT_CREATE, &payload);
    BufferFree (&payload);
    if (!ok) {
        BufferFree (&frame);
        lockDone = NULL;
        return false;
    }
    if (!ClientSendFrame (&frame)) {
        lockDone = NULL;
        return false;
    }
    return true;
}

bool NetRequestAssetRename (u64 id, const char *name) {
    if (id == 0 || name == NULL || name[0] == '\0') return false;
    Buffer frame = {0};
    if (!FrameAssetRename (&frame, id, name)) {
        BufferFree (&frame);
        return false;
    }
    return ClientSendFrame (&frame);
}

bool NetRequestAssetReplace (u64 id, const char *ext,
                             const unsigned char *data, int size) {
    if (id == 0 || ext == NULL || ext[0] == '\0' || data == NULL ||
        size <= 0 || (uint32_t)size > NET_MAX_FRAME) return false;

    Image image = LoadImageFromMemory (ext, data, size);
    if (!IsImageValid (image)) {
        UnloadImage (image);
        return false;
    }
    UnloadImage (image);

    Buffer frame = {0};
    if (!FrameAssetReplace (&frame, id, ext, data, (uint32_t)size)) {
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

bool NetPublishObjectUpsert (const Object *object) {
    if (object == NULL) return false;

    Buffer frame = {0};
    bool ok = FrameObjectUpsert (&frame, object->id, object->name, object->pos.x, object->pos.y,
                                 object->layer, object->timestamp, object->hidden,
                                 (uint32_t)object->assetIndex,
                                 object->assets.ids, (uint32_t)object->assets.count);
    if (!ok) {
        BufferFree (&frame);
        return false;
    }
    return ClientSendFrame (&frame);
}

bool NetRequestScript (const char *source, const char *selfExpr) {
    if (source == NULL) return false;
    if (selfExpr == NULL) selfExpr = "";
    if (strlen (source) > 0xFFFFu || strlen (selfExpr) > 0xFFFFu) return false;

    Buffer payload = {0};
    Buffer frame = {0};
    bool ok = BufferPutString (&payload, source) &&
              BufferPutString (&payload, selfExpr) &&
              FrameMake (&frame, NET_REQ_SCRIPT, &payload);
    BufferFree (&payload);
    if (!ok) {
        BufferFree (&frame);
        return false;
    }
    return ClientSendFrame (&frame);
}

bool NetRequestMenu (u64 id, int index, const char *name) {
    if (index < 0 || name == NULL || strlen (name) > 0xFFFFu)
        return false;

    Buffer frame = {0};
    if (!FrameMenuRequest (&frame, id, (uint32_t)index, name)) {
        BufferFree (&frame);
        return false;
    }
    return ClientSendFrame (&frame);
}

bool NetRequestImport (const char *path) {
    if (path == NULL || path[0] == '\0' || strlen (path) > 0xFFFFu)
        return false;

    Buffer payload = {0};
    Buffer frame = {0};
    bool ok = BufferPutString (&payload, path) &&
              FrameMake (&frame, NET_REQ_IMPORT, &payload);
    BufferFree (&payload);
    if (!ok) {
        BufferFree (&frame);
        return false;
    }
    return ClientSendFrame (&frame);
}

bool NetRequestExport (const char *path) {
    if (path == NULL || path[0] == '\0' || strlen (path) > 0xFFFFu)
        return false;

    Buffer payload = {0};
    Buffer frame = {0};
    bool ok = BufferPutString (&payload, path) &&
              FrameMake (&frame, NET_REQ_EXPORT, &payload);
    BufferFree (&payload);
    if (!ok) {
        BufferFree (&frame);
        return false;
    }
    return ClientSendFrame (&frame);
}

bool NetRequestObjectFront (u64 id) {
    Object *object = EntityFind (id);
    if (object == NULL || object->lockOwnerId != NetClientUserId ()) return false;

    Buffer frame = {0};
    if (!FrameObjectLock (&frame, NET_REQ_OBJECT_FRONT, id)) {
        BufferFree (&frame);
        return false;
    }
    return ClientSendFrame (&frame);
}

bool NetRequestDragUpdate (u64 id) {
    if (id == 0) return false;
    Buffer frame = {0};
    if (!FrameObjectLock (&frame, NET_DRAG_UPDATE, id)) {
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


bool NetRequestAssetAdd (const char *path) {
    int size = 0;
    unsigned char *data = ReadFileBytes (path, &size);
    if (data == NULL || size <= 0) {
        free (data);
        return false;
    }

    const char *ext = GetFileExtension (path);
    Image image = LoadImageFromMemory (ext, data, size);
    if (image.data == NULL) {
        free (data);
        return false;
    }
    UnloadImage (image);

    u64 requestId = nextRequestId++;

    Buffer payload = {0};
    Buffer frame = {0};
    bool ok = BufferPutU64 (&payload, requestId) &&
              BufferPutString (&payload, "asset") &&
              BufferPutString (&payload, ext) &&
              BufferPutU32 (&payload, (uint32_t)size) &&
              BufferAppend (&payload, data, (size_t)size) &&
              FrameMake (&frame, NET_REQ_ASSET_ADD, &payload);
    BufferFree (&payload);
    free (data);
    if (!ok) {
        BufferFree (&frame);
        return false;
    }
    if (!ClientSendFrame (&frame)) {
        return false;
    }
    return true;
}

bool NetRequestAssetObject (const char *path, Vector2 position,
                            NetLockDoneFn done) {
    if (clientLockedId != 0) NetUnlockObject (clientLockedId);
    int size = 0;
    unsigned char *data = ReadFileBytes (path, &size);
    if (data == NULL || size <= 0) {
        free (data);
        return false;
    }

    const char *ext = GetFileExtension (path);
    Image image = LoadImageFromMemory (ext, data, size);
    if (image.data == NULL) {
        free (data);
        return false;
    }
    UnloadImage (image);

    u64 requestId = nextRequestId++;
    objectRequestId = requestId;
    lockDone = done;
    lockResult = 0;
    pendingLockId = 0;
    lockStarted = GetTime ();
    lockTimeout = 30.0;

    u64 timestamp = (u64)(GetTime () * 1000000.0);
    if (timestamp == 0) timestamp = 1;

    Buffer payload = {0};
    Buffer frame = {0};
    bool ok = BufferPutU64 (&payload, requestId) &&
              BufferPutString (&payload, "asset") &&
              BufferPutString (&payload, ext) &&
              BufferPutU32 (&payload, (uint32_t)size) &&
              BufferAppend (&payload, data, (size_t)size) &&
              BufferPutString (&payload, "object") &&
              BufferPutF32 (&payload, position.x) &&
              BufferPutF32 (&payload, position.y) &&
              BufferPutU32 (&payload, 0) &&
              BufferPutU64 (&payload, timestamp) &&
              FrameMake (&frame, NET_REQ_ASSET_OBJECT, &payload);
    BufferFree (&payload);
    free (data);
    if (!ok) {
        BufferFree (&frame);
        lockDone = NULL;
        return false;
    }
    if (!ClientSendFrame (&frame)) {
        lockDone = NULL;
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


bool NetRequestLock (u64 id, NetLockDoneFn done) {
    u64 selfId = NetClientUserId ();
    if (selfId == 0 || id == 0) return false;

    Object *object = EntityFind (id);
    if (object == NULL) return false;
    if (object->lockOwnerId == selfId) {
        clientLockedId = id;
        lockDone = done;
        pendingLockId = id;
        lockResult = 1;
        lockStarted = GetTime ();
        lockTimeout = 0.0;
        return true;
    }
    if (object->lockOwnerId != 0) return false;
    if (clientLockedId != 0 && clientLockedId != id)
        NetUnlockObject (clientLockedId);

    Buffer frame = {0};
    if (!FrameObjectLock (&frame, NET_LOCK, id)) {
        BufferFree (&frame);
        return false;
    }
    pendingLockId = id;
    lockDone = done;
    lockResult = 0;
    lockStarted = GetTime ();
    lockTimeout = 2.0;
    if (!ClientSendFrame (&frame)) {
        pendingLockId = 0;
        lockDone = NULL;
        return false;
    }
    return true;
}

static bool SendUnlockObject (u64 id, uint8_t type, bool clearLocal) {
    if (clearLocal) {
        Object *object = EntityFind (id);
        if (object != NULL && object->lockOwnerId == NetClientUserId ())
            object->lockOwnerId = 0;
        if (id == clientLockedId) clientLockedId = 0;
    }

    Buffer frame = {0};
    if (!FrameObjectLock (&frame, type, id)) {
        BufferFree (&frame);
        return false;
    }
    return ClientSendFrame (&frame);
}

bool NetUnlockObject (u64 id) {
    return SendUnlockObject (id, NET_UNLOCK_NOTIFY, true);
}

bool NetUnlockObjectSilent (u64 id) {
    return SendUnlockObject (id, NET_UNLOCK, true);
}


static bool ApplySnapshot (const unsigned char *data, size_t length) {
    Reader reader = {data, length, 0};
    uint32_t version;
    u64 selfId;
    if (!ReaderU32 (&reader, &version) || version != NET_PROTOCOL_VERSION ||
        !ReaderU64 (&reader, &selfId)) return false;
    clientUserId = selfId;
    clientSceneVersion++;

    AssetUnload ();
    ObjectUnload ();
    UserUnload ();
    MenuSetNames (0, NULL, 0);
    AssetInit ();
    ObjectInit ();
    clientLockedId = 0;

    uint32_t assetCount;
    if (!ReaderU32 (&reader, &assetCount)) return false;

    for (uint32_t i = 0; i < assetCount; i++) {
        u64 id;
        char name[256];
        char ext[8];
        uint32_t size;
        if (!ReaderU64 (&reader, &id) || !ReaderString (&reader, name, sizeof (name)) ||
            !ReaderString (&reader, ext, sizeof (ext)) || !ReaderU32 (&reader, &size) ||
            size == 0 || size > NET_MAX_FRAME || !ReaderAvailable (&reader, size))
            return false;

        AssetAdd (name, ext, reader.data + reader.pos, (int)size, id);
        reader.pos += size;
    }

    uint32_t objectCount;
    if (!ReaderU32 (&reader, &objectCount)) return false;

    for (uint32_t i = 0; i < objectCount; i++) {
        u64 id;
        char name[256];
        float x, y;
        uint32_t layer;
        u64 timestamp;
        uint8_t hidden;
        uint32_t objectAssetCount;
        uint32_t assetIndex;
        if (!ReaderU64 (&reader, &id) || !ReaderString (&reader, name, sizeof (name)) ||
            !ReaderF32 (&reader, &x) || !ReaderF32 (&reader, &y) ||
            !ReaderU32 (&reader, &layer) || !ReaderU64 (&reader, &timestamp) ||
            !ReaderBytes (&reader, &hidden, 1) ||
            !ReaderU32 (&reader, &objectAssetCount) ||
            !ReaderU32 (&reader, &assetIndex) ||
            !ReaderAvailable (&reader, (size_t)objectAssetCount * 8)) return false;

        Object *object = ObjectAdd (
            name, (Vector2){x, y}, (int)(int32_t)layer, timestamp, id);
        for (uint32_t a = 0; a < objectAssetCount; a++) {
            u64 assetId;
            if (!ReaderU64 (&reader, &assetId)) return false;
            if (object != NULL) IdGroupAdd (&object->assets, assetId);
        }

        u64 lockOwnerId;
        if (!ReaderU64 (&reader, &lockOwnerId)) return false;
        if (object != NULL) {
            object->timestamp = timestamp;
            object->hidden = hidden != 0;
            object->lockOwnerId = lockOwnerId;
            object->assetIndex = (assetIndex < objectAssetCount) ? (int)assetIndex : 0;
            if (lockOwnerId == clientUserId) clientLockedId = id;
        }
    }

    uint32_t userCount;
    if (!ReaderU32 (&reader, &userCount)) return false;
    for (uint32_t i = 0; i < userCount; i++) {
        u64 id;
        char username[128];
        uint32_t color;
        uint8_t canscript;
        uint8_t hasCursor;
        float x, y;
        if (!ReaderU64 (&reader, &id) ||
            !ReaderString (&reader, username, sizeof (username)) ||
            !ReaderU32 (&reader, &color) ||
            !ReaderBytes (&reader, &canscript, 1) ||
            !ReaderBytes (&reader, &hasCursor, 1) ||
            !ReaderF32 (&reader, &x) || !ReaderF32 (&reader, &y)) return false;
        UserAdd (id, username);
        UserSetColor (id, color);
        UserSetCanScript (id, canscript != 0);
        UserSetCursor (id, hasCursor != 0, x, y);
    }

    return reader.pos == reader.len;
}

static bool ApplyAssetAddDelta (Reader *reader) {
    u64 requestId;
    u64 requesterId;
    u64 id;
    char name[256];
    char ext[8];
    uint32_t size;
    if (!ReaderU64 (reader, &requestId) ||
        !ReaderU64 (reader, &requesterId) ||
        !ReaderU64 (reader, &id) ||
        !ReaderString (reader, name, sizeof (name)) ||
        !ReaderString (reader, ext, sizeof (ext)) || !ReaderU32 (reader, &size) ||
        size == 0 || size > NET_MAX_FRAME || !ReaderAvailable (reader, size))
        return false;

    Asset *existing = EntityFind (id);
    if (existing == NULL)
        existing = AssetAdd (name, ext, reader->data + reader->pos, (int)size, id);
    reader->pos += size;

    return true;
}

static bool ApplyAssetRenameDelta (Reader *reader) {
    u64 id;
    char name[256];
    if (!ReaderU64 (reader, &id) || !ReaderString (reader, name, sizeof (name)))
        return false;

    Asset *asset = EntityFind (id);
    if (asset != NULL && strcmp (asset->name, name) != 0)
        AssetRename (asset, name);
    return true;
}

static bool ApplyAssetReplaceDelta (Reader *reader) {
    u64 id;
    char ext[8];
    uint32_t size;
    if (!ReaderU64 (reader, &id) || !ReaderString (reader, ext, sizeof (ext)) ||
        !ReaderU32 (reader, &size) || size == 0 || size > NET_MAX_FRAME ||
        !ReaderAvailable (reader, size)) return false;

    Asset *asset = EntityFind (id);
    if (asset != NULL) {
        bool same = (asset->fileSize == (int)size) && (strcmp (asset->ext, ext) == 0) &&
                    asset->fileData != NULL &&
                    memcmp (asset->fileData, reader->data + reader->pos, size) == 0;
        if (!same) AssetReplace (asset, ext, reader->data + reader->pos, (int)size);
    }
    reader->pos += size;
    return true;
}

static bool ApplyAssetDeleteFailedDelta (Reader *reader, int *flags) {
    u64 id;
    if (!ReaderU64 (reader, &id)) return false;
    (void)id;
    *flags |= NET_UPDATE_ASSET_DELETE_FAILED;
    return true;
}

static bool ApplyAssetDeleteDelta (Reader *reader, int *flags) {
    u64 id;
    if (!ReaderU64 (reader, &id)) return false;

    Asset *asset = EntityFind (id);
    if (asset != NULL) {
        IdGroup ids = ObjectIds ();
        for (int i = 0; i < ids.count; i++) {
            Object *object = EntityFind (ids.ids[i]);
            if (object == NULL) continue;
            for (int a = object->assets.count - 1; a >= 0; a--) {
                if (object->assets.ids[a] == id) IdGroupRemoveAt (&object->assets, a);
            }
        }
        IdGroupUnload (&ids);
        if (AssetDelete (asset)) *flags |= NET_UPDATE_ASSET_DELETED;
    }
    return true;
}

static bool ApplyObjectState (Reader *reader, u64 id, const char *name,
                              float x, float y, uint32_t layer, u64 timestamp,
                              bool hidden, uint32_t assetIndex, uint32_t assetCount,
                              Object **result) {
    Object *object = EntityFind (id);
    if (object == NULL) {
        object = ObjectAdd (name, (Vector2){x, y},
                            (int)(int32_t)layer, timestamp, id);
    } else {
        if (strcmp (object->name, name) != 0) ObjectRename (object, name);
        object->pos = (Vector2){x, y};
        object->layer = (int)(int32_t)layer;
    }

    if (object != NULL) {
        IdGroupClear (&object->assets);
        for (uint32_t i = 0; i < assetCount; i++) {
            u64 assetId;
            if (!ReaderU64 (reader, &assetId)) return false;
            if (EntityFind (assetId) != NULL) IdGroupAdd (&object->assets, assetId);
        }
        object->timestamp = timestamp;
        object->hidden = hidden;
        object->assetIndex = (assetIndex < (uint32_t)object->assets.count)
                                 ? (int)assetIndex : 0;
    } else {
        for (uint32_t i = 0; i < assetCount; i++) {
            u64 assetId;
            if (!ReaderU64 (reader, &assetId)) return false;
        }
    }
    *result = object;
    return true;
}

static bool ApplyObjectCreatedDelta (Reader *reader, int *flags) {
    (void)flags;
    u64 requestId;
    u64 requesterId;
    u64 id;
    char name[256];
    float x, y;
    uint32_t layer;
    u64 timestamp;
    uint8_t hidden;
    uint32_t assetIndex;
    uint32_t assetCount;
    if (!ReaderU64 (reader, &requestId) ||
        !ReaderU64 (reader, &requesterId) ||
        !ReaderU64 (reader, &id) ||
        !ReaderString (reader, name, sizeof (name)) ||
        !ReaderF32 (reader, &x) || !ReaderF32 (reader, &y) ||
        !ReaderU32 (reader, &layer) || !ReaderU64 (reader, &timestamp) ||
        !ReaderBytes (reader, &hidden, 1) ||
        !ReaderU32 (reader, &assetCount) || !ReaderU32 (reader, &assetIndex) ||
        !ReaderAvailable (reader, (size_t)assetCount * 8)) return false;

    Object *object = NULL;
    if (!ApplyObjectState (reader, id, name, x, y, layer, timestamp,
                           hidden != 0, assetIndex, assetCount, &object)) return false;

    if (requestId != 0 && requestId == objectRequestId && object != NULL &&
        requesterId == NetClientUserId ()) {
        objectRequestId = 0;
        if (lockDone != NULL) pendingLockId = id;
    }
    return true;
}

static bool ApplyObjectUpsertDelta (Reader *reader) {
    u64 id;
    char name[256];
    float x, y;
    uint32_t layer;
    u64 timestamp;
    uint8_t hidden;
    uint32_t assetIndex;
    uint32_t assetCount;
    if (!ReaderU64 (reader, &id) || !ReaderString (reader, name, sizeof (name)) ||
        !ReaderF32 (reader, &x) || !ReaderF32 (reader, &y) ||
        !ReaderU32 (reader, &layer) || !ReaderU64 (reader, &timestamp) ||
        !ReaderBytes (reader, &hidden, 1) ||
        !ReaderU32 (reader, &assetCount) || !ReaderU32 (reader, &assetIndex) ||
        !ReaderAvailable (reader, (size_t)assetCount * 8)) return false;

    Object *object = NULL;
    return ApplyObjectState (reader, id, name, x, y, layer, timestamp,
                             hidden != 0, assetIndex, assetCount, &object);
}

static bool ApplyObjectDeleteDelta (Reader *reader, int *flags) {
    u64 id;
    if (!ReaderU64 (reader, &id)) return false;

    Object *object = EntityFind (id);
    if (id == clientLockedId) clientLockedId = 0;
    if (object != NULL && ObjectDelete (object)) *flags |= NET_UPDATE_DELETED;
    return true;
}

static bool ApplyUserStateDelta (Reader *reader) {
    u64 id;
    char username[128];
    uint32_t color;
    uint8_t canscript;
    uint8_t hasCursor;
    float x, y;
    if (!ReaderU64 (reader, &id) ||
        !ReaderString (reader, username, sizeof (username)) ||
        !ReaderU32 (reader, &color) ||
        !ReaderBytes (reader, &canscript, 1) ||
        !ReaderBytes (reader, &hasCursor, 1) ||
        !ReaderF32 (reader, &x) || !ReaderF32 (reader, &y)) return false;
    UserAdd (id, username);
    UserSetColor (id, color);
    UserSetCanScript (id, canscript != 0);
    UserSetCursor (id, hasCursor != 0, x, y);
    return true;
}

static bool ApplyUserLeaveDelta (Reader *reader) {
    u64 id;
    if (!ReaderU64 (reader, &id)) return false;
    UserRemove (id);
    if (id == clientUserId) clientLockedId = 0;

    IdGroup ids = ObjectIds ();
    for (int i = 0; i < ids.count; i++) {
        Object *object = EntityFind (ids.ids[i]);
        if (object != NULL && object->lockOwnerId == id)
            object->lockOwnerId = 0;
    }
    IdGroupUnload (&ids);
    return true;
}

static bool ApplyCursorDelta (Reader *reader) {
    u64 id;
    float x, y;
    if (!ReaderU64 (reader, &id) || !ReaderF32 (reader, &x) || !ReaderF32 (reader, &y))
        return false;
    UserSetCursor (id, true, x, y);
    return true;
}

static bool ApplyLockDelta (Reader *reader, int *flags) {
    (void)flags;
    u64 id;
    u64 ownerId;
    if (!ReaderU64 (reader, &id) || !ReaderU64 (reader, &ownerId))
        return false;

    Object *object = EntityFind (id);
    if (object != NULL) object->lockOwnerId = ownerId;
    if (ownerId == clientUserId) {
        clientLockedId = id;
        if (lockDone != NULL && id == pendingLockId) lockResult = 1;
    } else if (id == clientLockedId) {
        clientLockedId = 0;
    }
    return true;
}

static bool ApplyLockFailedDelta (Reader *reader, int *flags) {
    u64 id;
    if (!ReaderU64 (reader, &id) || reader->pos != reader->len) return false;
    if (lockDone != NULL && id == pendingLockId) {
        lockResult = -1;
    } else {
        lockLostId = id;
        if (id == clientLockedId) clientLockedId = 0;
        *flags |= NET_UPDATE_LOCK_LOST;
    }
    return true;
}

static bool ApplyMenuDelta (Reader *reader) {
    u64 id;
    uint32_t count;
    if (!ReaderU64 (reader, &id) || !ReaderU32 (reader, &count) ||
        count > 100000) return false;

    char **names = NULL;
    if (count > 0) {
        names = calloc (count, sizeof (char *));
        if (names == NULL) return false;
        for (uint32_t i = 0; i < count; i++) {
            char name[256];
            if (!ReaderString (reader, name, sizeof (name)) ||
                (names[i] = strdup (name)) == NULL) {
                for (uint32_t j = 0; j < i; j++) free (names[j]);
                free (names);
                return false;
            }
        }
    }

    bool ok = MenuSetNames (id, (const char *const *)names, (int)count);

    for (uint32_t i = 0; i < count; i++) free (names[i]);
    free (names);
    return ok;
}

static bool ApplyMessageDelta (Reader *reader, UiMessageKind kind) {
    char message[NET_MAX_MESSAGE + 1];
    if (!ReaderString (reader, message, sizeof (message)) || message[0] == '\0' ||
        reader->pos != reader->len) return false;
    UiShowMessageTextKind (message, kind);
    return true;
}

static bool ApplyDelta (uint8_t type, const unsigned char *data, size_t length, int *flags) {
    Reader reader = {data, length, 0};
    bool ok = false;

    switch (type) {
        case NET_ASSET_ADD:
            ok = ApplyAssetAddDelta (&reader);
            break;
        case NET_ASSET_RENAME:
            ok = ApplyAssetRenameDelta (&reader);
            break;
        case NET_ASSET_REPLACE:
            ok = ApplyAssetReplaceDelta (&reader);
            break;
        case NET_ASSET_DELETE:
            ok = ApplyAssetDeleteDelta (&reader, flags);
            break;
        case NET_ASSET_DELETE_FAILED:
            ok = ApplyAssetDeleteFailedDelta (&reader, flags);
            break;
        case NET_OBJECT_CREATED:
            ok = ApplyObjectCreatedDelta (&reader, flags);
            break;
        case NET_OBJECT_UPSERT:
            ok = ApplyObjectUpsertDelta (&reader);
            break;
        case NET_OBJECT_DELETE:
            ok = ApplyObjectDeleteDelta (&reader, flags);
            break;
        case NET_USER_STATE:
            ok = ApplyUserStateDelta (&reader);
            break;
        case NET_USER_LEAVE:
            ok = ApplyUserLeaveDelta (&reader);
            break;
        case NET_CURSOR_STATE:
            ok = ApplyCursorDelta (&reader);
            break;
        case NET_LOCK_STATE:
            ok = ApplyLockDelta (&reader, flags);
            break;
        case NET_LOCK_FAILED:
            ok = ApplyLockFailedDelta (&reader, flags);
            break;
        case NET_MESSAGE:
            ok = ApplyMessageDelta (&reader, UI_MESSAGE_SCRIPT);
            break;
        case NET_ERROR:
            ok = ApplyMessageDelta (&reader, UI_MESSAGE_ERROR);
            break;
        case NET_INFO:
            ok = ApplyMessageDelta (&reader, UI_MESSAGE_INFO);
            break;
        case NET_MENU:
            ok = ApplyMenuDelta (&reader);
            break;
        default:
            return false;
    }

    return ok && reader.pos == reader.len;
}

static bool ParseClientFrames (int *flags) {
    size_t offset = 0;

    while (clientIn.len - offset >= 5) {
        const unsigned char *data = clientIn.data + offset;
        uint32_t payloadSize = (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
                               ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
        if (payloadSize == 0 || payloadSize > NET_MAX_FRAME) return false;

        size_t frameSize = 4 + (size_t)payloadSize;
        if (clientIn.len - offset < frameSize) break;

        uint8_t type = data[4];
        const unsigned char *payload = data + 5;
        size_t payloadLength = (size_t)payloadSize - 1;

        if (type == NET_REJECT) {
            *flags |= NET_UPDATE_REJECTED;
            return true;
        }
        if (type == NET_SNAPSHOT) {
            if (!ApplySnapshot (payload, payloadLength)) return false;
            *flags |= NET_UPDATE_SCENE;
        } else if (!ApplyDelta (type, payload, payloadLength, flags)) {
            return false;
        }

        offset += frameSize;
    }

    if (offset > 0) {
        memmove (clientIn.data, clientIn.data + offset, clientIn.len - offset);
        clientIn.len -= offset;
    }
    return true;
}

int NetClientUpdate (void) {
    ClientPump (0);

    if (lockDone != NULL) {
        bool timeout = lockTimeout > 0.0 &&
                       GetTime () - lockStarted >= lockTimeout;
        if (lockResult != 0 || timeout) {
            NetLockDoneFn done = lockDone;
            bool acquired = lockResult > 0;
            lockDone = NULL;
            lockResult = 0;
            pendingLockId = 0;
            done (acquired);
        }
    }

    int flags = clientPendingFlags;
    clientPendingFlags = NET_UPDATE_NONE;
    if (flags & NET_UPDATE_REJECTED) ClientCloseNow ();
    return flags;
}
