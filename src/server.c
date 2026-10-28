#define _GNU_SOURCE

#include "net.h"
#include "netutils.h"
#include "net_common.h"
#include "ui.h"
#include "utils.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "assets.h"
#include "control.h"
#include "control_signal.h"
#include "menu.h"
#include "object.h"
#include "entity.h"
#include "user.h"
#include "script.h"
#include "save.h"

typedef struct {
    int clientIndex;
    uint8_t type;
    unsigned char *payload;
    size_t length;
} ServerRequest;

static ServerRequest *serverRequests;
static int serverRequestCount;
static int serverRequestCapacity;


static u64 *deletedObjectIds;
static int deletedObjectCount;
static int deletedObjectCapacity;

static bool ServerObjectIdDeleted (u64 id) {
    for (int i = 0; i < deletedObjectCount; i++) {
        if (deletedObjectIds[i] == id) return true;
    }
    return false;
}

static bool ServerTombstoneObject (u64 id) {
    if (deletedObjectCount == deletedObjectCapacity) {
        int capacity = (deletedObjectCapacity == 0) ? 16 : deletedObjectCapacity * 2;
        u64 *ids = realloc (deletedObjectIds, (size_t)capacity * sizeof (u64));
        if (ids == NULL) return false;
        deletedObjectIds = ids;
        deletedObjectCapacity = capacity;
    }
    deletedObjectIds[deletedObjectCount++] = id;
    return true;
}


static Asset *ServerAssetFind (u64 id) {
    return AssetFindById (id);
}

static Object *ServerObjectFind (u64 id) {
    return ObjectFindById (id);
}

static u64 nextEntityId = 1;

static u64 ServerNewEntityId (void) {
    u64 id = nextEntityId++;
    if (nextEntityId == 0) nextEntityId = 1;
    return id;
}

static void ServerSetNextEntityId (u64 nextId) {
    nextEntityId = nextId != 0 ? nextId : 1;
}

u64 NetServerAllocateEntityId (void) {
    return ServerNewEntityId ();
}

static void ServerMakeUniqueName (const char *requested, char *name, size_t size,
                                  bool object, u64 excludeId) {
    const char *fallback = object ? "object" : "asset";
    if (requested == NULL || requested[0] == '\0') requested = fallback;
    snprintf (name, size, "%s", requested);

    for (int n = 2; n < 1000000; n++) {
        bool used;
        if (object) {
            Object *exclude = EntityFind (excludeId);
            used = ObjectNameUsed (name, exclude);
        } else {
            Asset *exclude = EntityFind (excludeId);
            used = AssetNameUsed (name, exclude);
        }
        if (!used) return;

        char suffix[16];
        int suffixLength = snprintf (suffix, sizeof (suffix), " %d", n);
        size_t requestedLength = strlen (requested);
        size_t suffixSize = suffixLength > 0 ? (size_t)suffixLength : 0;
        if (size <= suffixSize + 1) return;
        if (requestedLength > size - suffixSize - 1)
            requestedLength = size - suffixSize - 1;
        snprintf (name, size, "%.*s%s", (int)requestedLength, requested, suffix);
    }
}


typedef struct {
    NetutilStream *stream;
    Buffer in;
    bool active;
    bool closing;
    bool closeSent;
    bool joined;
    bool inputClosed;
    bool disconnectQueued;
    bool helloQueued;
    u64 userId;
    char username[128];
} ServerClient;

static ServerClient serverClients[NET_MAX_CLIENTS];
static ScriptEnv *serverScriptEnv;
static char serverSavePath[1024];
static int pendingLockClient = -1;
static u64 pendingLockObjectId;
static bool pendingLockFailureSent;
static bool serverSuppressObjectBroadcast;

static void ServerBeginDisconnect (ServerClient *client);
static void ServerClientMaybeClose (ServerClient *client);
static void ServerReadFrames (ServerClient *client, int clientIndex);
static void ServerQueueDisconnect (int clientIndex);
static void ServerProcessRequests (void);
static void ServerClientData (NetutilStream *stream, const unsigned char *data,
                              size_t size, void *user);
static void ServerClientOnClosed (NetutilStream *stream, void *user);
static const NetutilEvents serverClientEvents = {
    NULL, ServerClientData, ServerClientOnClosed, NULL
};

static void ServerSave (void) {
    if (serverScriptEnv == NULL || serverSavePath[0] == '\0') return;
    if (!SaveWrite (serverSavePath, serverScriptEnv))
        fprintf (stderr, "保存存档失败: %s\n", serverSavePath);
}

static u64 ServerNewUserId (void) {
    u64 id;
    bool used;
    do {
        id = ServerNewEntityId ();
        used = false;
        for (int i = 0; i < NET_MAX_CLIENTS; i++) {
            if (serverClients[i].userId == id) {
                used = true;
                break;
            }
        }
    } while (used);
    return id;
}

static bool ServerBuildSnapshot (Buffer *frame, u64 selfId) {
    Buffer payload = {0};
    IdGroup assetIds = AssetIds ();
    IdGroup objectIds = ObjectIds ();
    bool ok = BufferPutU32 (&payload, NET_PROTOCOL_VERSION) &&
              BufferPutU64 (&payload, selfId) &&
              BufferPutU32 (&payload, (uint32_t)assetIds.count);

    for (int i = 0; ok && i < assetIds.count; i++) {
        Asset *asset = EntityFind (assetIds.ids[i]);
        if (asset == NULL) {
            ok = false;
            break;
        }
        ok = BufferPutU64 (&payload, asset->id) &&
             BufferPutString (&payload, asset->name) &&
             BufferPutString (&payload, asset->ext) &&
             BufferPutU32 (&payload, (uint32_t)asset->fileSize) &&
             BufferAppend (&payload, asset->fileData, (size_t)asset->fileSize);
    }

    if (ok) ok = BufferPutU32 (&payload, (uint32_t)objectIds.count);
    for (int i = 0; ok && i < objectIds.count; i++) {
        Object *object = EntityFind (objectIds.ids[i]);
        if (object == NULL) {
            ok = false;
            break;
        }
        ok = BufferPutU64 (&payload, object->id) &&
             BufferPutString (&payload, object->name) &&
             BufferPutF32 (&payload, object->pos.x) &&
             BufferPutF32 (&payload, object->pos.y) &&
             BufferPutU32 (&payload, (uint32_t)object->layer) &&
             BufferPutU64 (&payload, object->timestamp) &&
             BufferPutU8 (&payload, ObjectIsHiddenForUser (object, selfId) ? 1 : 0) &&
             BufferPutU32 (&payload, (uint32_t)object->assets.count) &&
             BufferPutU32 (&payload, (uint32_t)object->assetIndex);
        for (int a = 0; ok && a < object->assets.count; a++)
            ok = BufferPutU64 (&payload, object->assets.ids[a]);
        if (ok) ok = BufferPutU64 (&payload, object->lockOwnerId);
    }

    if (ok) {
        IdGroup userIds = UserIds ();
        ok = BufferPutU32 (&payload, (uint32_t)userIds.count);
        for (int i = 0; ok && i < userIds.count; i++) {
            User *user = UserFind (userIds.ids[i]);
            if (user == NULL) {
                ok = false;
                break;
            }
            ok = BufferPutU64 (&payload, user->id) &&
                 BufferPutString (&payload, user->name) &&
                 BufferPutU32 (&payload, user->color) &&
                 BufferPutU8 (&payload, user->canscript ? 1 : 0) &&
                 BufferPutU8 (&payload, user->hasCursor ? 1 : 0) &&
                 BufferPutF32 (&payload, user->x) &&
                 BufferPutF32 (&payload, user->y);
        }
        IdGroupUnload (&userIds);
    }

    if (ok) ok = FrameMake (frame, NET_SNAPSHOT, &payload);
    BufferFree (&payload);
    IdGroupUnload (&assetIds);
    IdGroupUnload (&objectIds);
    return ok;
}


static void ServerClientMaybeClose (ServerClient *client) {
    if (!client->active || !client->closing || client->closeSent) return;
    client->closeSent = true;
    NetutilClose (client->stream);
}

static void ServerClientOnClosed (NetutilStream *stream, void *user) {
    (void)stream;
    ServerClient *client = user;
    if (client->joined) ServerBeginDisconnect (client);
    client->stream = NULL;
    client->active = false;
    client->closing = false;
    client->closeSent = false;
    client->joined = false;
    client->inputClosed = false;
    client->disconnectQueued = false;
    client->helloQueued = false;
    client->userId = 0;
    client->username[0] = '\0';
    BufferFree (&client->in);
}

static void ServerClientEnqueue (ServerClient *client, const unsigned char *data,
                                 size_t size) {
    if (!client->active || client->closing || client->stream == NULL) return;
    if (size > NET_MAX_OUTPUT || !NetutilWrite (client->stream, data, size)) {
        client->closing = true;
        ServerClientMaybeClose (client);
    }
}

static void ServerClientData (NetutilStream *stream, const unsigned char *data,
                              size_t size, void *user) {
    (void)stream;
    ServerClient *client = user;
    int clientIndex = (int)(client - serverClients);
    if (!client->active || client->inputClosed) return;
    if (client->in.len + size > NET_MAX_FRAME + 4 ||
        !BufferAppend (&client->in, data, size)) {
        client->inputClosed = true;
    } else {
        ServerReadFrames (client, clientIndex);
    }
    if (client->inputClosed && !client->disconnectQueued)
        ServerQueueDisconnect (clientIndex);
    ServerProcessRequests ();
}

static bool ServerQueuePush (int clientIndex, uint8_t type,
                             const unsigned char *payload, size_t length) {
    if (serverRequestCount == serverRequestCapacity) {
        int capacity = (serverRequestCapacity == 0) ? 32 : serverRequestCapacity * 2;
        ServerRequest *requests = realloc (serverRequests,
                                           (size_t)capacity * sizeof (ServerRequest));
        if (requests == NULL) return false;
        serverRequests = requests;
        serverRequestCapacity = capacity;
    }

    unsigned char *copy = NULL;
    if (length > 0) {
        copy = malloc (length);
        if (copy == NULL) return false;
        memcpy (copy, payload, length);
    }
    ServerRequest *request = &serverRequests[serverRequestCount++];
    request->clientIndex = clientIndex;
    request->type = type;
    request->payload = copy;
    request->length = length;
    return true;
}

static void ServerCloseInput (int clientIndex) {
    ServerClient *client = &serverClients[clientIndex];
    if (!client->active) return;
    client->inputClosed = true;
}

static void ServerQueueDisconnect (int clientIndex) {
    ServerClient *client = &serverClients[clientIndex];
    if (!client->active || client->disconnectQueued) return;
    if (ServerQueuePush (clientIndex, SERVER_EVENT_DISCONNECT, NULL, 0)) {
        client->disconnectQueued = true;
    } else {
        client->closing = true;
    }
}

static void ServerBroadcast (const unsigned char *data, size_t size) {
    for (int i = 0; i < NET_MAX_CLIENTS; i++) {
        if (serverClients[i].joined)
            ServerClientEnqueue (&serverClients[i], data, size);
    }
}

static void ServerSendMenu (int clientIndex, u64 id) {
    Buffer frame = {0};
    if (FrameMenu (&frame, id))
        ServerClientEnqueue (&serverClients[clientIndex], frame.data, frame.len);
    BufferFree (&frame);
}

static void ServerSendAllMenus (int clientIndex) {
    ServerSendMenu (clientIndex, 0);
    IdGroup ids = ObjectIds ();
    for (int i = 0; i < ids.count; i++) {
        Object *object = EntityFind (ids.ids[i]);
        if (object != NULL) ServerSendMenu (clientIndex, object->id);
    }
    IdGroupUnload (&ids);
    ServerSendMenu (clientIndex, serverClients[clientIndex].userId);
}

static void ServerSendSnapshot (int clientIndex) {
    ServerClient *client = &serverClients[clientIndex];
    if (!client->active) return;

    Buffer frame = {0};
    if (!ServerBuildSnapshot (&frame, client->userId)) {
        client->closing = true;
        return;
    }
    ServerClientEnqueue (client, frame.data, frame.len);
    BufferFree (&frame);
    ServerSendAllMenus (clientIndex);
}

static void ServerBroadcastUserRecord (const User *user) {
    Buffer frame = {0};
    if (FrameUserState (&frame, user->id, user->name, user->color,
                        user->canscript, user->hasCursor, user->x, user->y))
        ServerBroadcast (frame.data, frame.len);
    BufferFree (&frame);
}

static void ServerBroadcastUserState (const ServerClient *client) {
    User *user = UserFind (client->userId);
    if (user != NULL) ServerBroadcastUserRecord (user);
}

void NetServerSetUserColor (u64 id, uint32_t color) {
    User *user = UserFind (id);
    if (user == NULL) return;
    UserSetColor (id, color);
    ServerBroadcastUserRecord (user);
}

void NetServerSetUserCanScript (u64 id, bool canscript) {
    User *user = UserFind (id);
    if (user == NULL) return;
    UserSetCanScript (id, canscript);
    ServerBroadcastUserRecord (user);
}

static void ServerBroadcastUserLeave (u64 userId) {
    Buffer frame = {0};
    if (FrameUserLeave (&frame, userId))
        ServerBroadcast (frame.data, frame.len);
    BufferFree (&frame);
}

static void ServerBroadcastLockState (u64 id, u64 ownerId) {
    Buffer frame = {0};
    if (FrameLockState (&frame, id, ownerId))
        ServerBroadcast (frame.data, frame.len);
    BufferFree (&frame);
}

static void ServerSendLockState (int clientIndex, u64 id, u64 ownerId) {
    Buffer frame = {0};
    if (FrameLockState (&frame, id, ownerId))
        ServerClientEnqueue (&serverClients[clientIndex], frame.data, frame.len);
    BufferFree (&frame);
}

static void ServerSendObjectState (int clientIndex, const Object *object) {
    if (serverSuppressObjectBroadcast) return;
    Buffer frame = {0};
    bool hidden = ObjectIsHiddenForUser (object, serverClients[clientIndex].userId);
    if (FrameObjectUpsert (&frame, object->id, object->name, object->pos.x, object->pos.y,
                           object->layer, object->timestamp, hidden,
                           (uint32_t)object->assetIndex,
                           object->assets.ids, (uint32_t)object->assets.count))
        ServerClientEnqueue (&serverClients[clientIndex], frame.data, frame.len);
    BufferFree (&frame);
}

static void ServerBroadcastObjectStateForAll (const Object *object) {
    for (int i = 0; i < NET_MAX_CLIENTS; i++) {
        if (serverClients[i].joined) ServerSendObjectState (i, object);
    }
}

static void ServerRunObjectEvent (u64 id, const char *event, u64 userId);
static void ServerLockObject (int clientIndex, u64 id);

static void ServerReleaseLocks (u64 userId, bool notify) {
    IdGroup ids = ObjectIds ();
    for (int i = 0; i < ids.count; i++) {
        Object *object = ServerObjectFind (ids.ids[i]);
        if (object == NULL || object->lockOwnerId != userId) continue;

        if (notify) ServerRunObjectEvent (object->id, "touched", userId);
        object = ServerObjectFind (ids.ids[i]);
        if (object != NULL && object->lockOwnerId == userId) {
            object->lockOwnerId = 0;
            ServerBroadcastLockState (object->id, 0);
        }
    }
    IdGroupUnload (&ids);
}

static void ServerBeginDisconnect (ServerClient *client) {
    if (!client->active) return;
    client->closing = true;
    if (client->joined) {
        ServerReleaseLocks (client->userId, true);
        ServerBroadcastUserLeave (client->userId);
        UserRemove (client->userId);
        client->joined = false;
    }
}

static void ServerRejectClient (int clientIndex) {
    Buffer payload = {0};
    Buffer frame = {0};
    if (FrameMake (&frame, NET_REJECT, &payload))
        ServerClientEnqueue (&serverClients[clientIndex], frame.data, frame.len);
    BufferFree (&frame);
    BufferFree (&payload);
    serverClients[clientIndex].closing = true;
}

static bool ServerUsernameTaken (const char *name, int excludeIndex) {
    for (int i = 0; i < NET_MAX_CLIENTS; i++) {
        if (i == excludeIndex || !serverClients[i].joined || !serverClients[i].active) continue;
        if (strcmp (serverClients[i].username, name) == 0) return true;
    }
    return false;
}

static bool ServerHandleHello (ServerClient *client, int clientIndex,
                               const unsigned char *data, size_t length) {
    Reader reader = {data, length, 0};
    char name[128] = {0};
    if (!ReaderString (&reader, name, sizeof (name)) || name[0] == '\0' ||
        reader.pos != reader.len || ServerUsernameTaken (name, clientIndex)) {
        ServerRejectClient (clientIndex);
        printf ("client rejected: %s\n", name[0] != '\0' ? name : "(invalid username)");
        fflush (stdout);
        return false;
    }

    client->userId = ServerNewUserId ();
    if (UserAdd (client->userId, name) == NULL) {
        ServerRejectClient (clientIndex);
        return false;
    }
    client->joined = true;
    snprintf (client->username, sizeof (client->username), "%s", name);
    ServerSendSnapshot (clientIndex);
    ServerBroadcastUserState (client);
    printf ("client joined: %s\n", client->username);
    fflush (stdout);
    return true;
}

static bool ServerApplyObjectCreate (Reader *reader, int clientIndex);

static bool ServerCreateAsset (Reader *reader, int clientIndex,
                              u64 *requestIdOut, u64 *assetIdOut) {
    u64 requestId;
    char requested[256];
    char ext[8];
    uint32_t size;
    if (!ReaderU64 (reader, &requestId) ||
        !ReaderString (reader, requested, sizeof (requested)) ||
        !ReaderString (reader, ext, sizeof (ext)) ||
        !ReaderU32 (reader, &size) || size == 0 || size > NET_MAX_FRAME ||
        !ReaderAvailable (reader, size)) return false;

    u64 id = ServerNewEntityId ();
    char name[256];
    ServerMakeUniqueName (requested, name, sizeof (name), false, 0);

    const unsigned char *data = reader->data + reader->pos;
    reader->pos += size;
    Asset *asset = AssetAddRaw (name, ext, data, (int)size, id);
    if (asset == NULL) return false;

    Buffer frame = {0};
    if (!FrameAssetAdd (&frame, requestId, serverClients[clientIndex].userId,
                        id, asset->name, asset->ext, asset->fileData,
                        (uint32_t)asset->fileSize)) {
        BufferFree (&frame);
        return false;
    }
    ServerBroadcast (frame.data, frame.len);
    BufferFree (&frame);
    if (requestIdOut != NULL) *requestIdOut = requestId;
    if (assetIdOut != NULL) *assetIdOut = id;
    return true;
}

static bool ServerApplyAssetAdd (Reader *reader, int clientIndex) {
    return ServerCreateAsset (reader, clientIndex, NULL, NULL);
}

static bool ServerApplyAssetObject (Reader *reader, int clientIndex) {
    u64 requestId = 0;
    u64 assetId = 0;
    if (!ServerCreateAsset (reader, clientIndex, &requestId, &assetId))
        return false;

    char objectName[256];
    float x, y;
    uint32_t layer;
    u64 timestamp;
    if (!ReaderString (reader, objectName, sizeof (objectName)) ||
        !ReaderF32 (reader, &x) || !ReaderF32 (reader, &y) ||
        !ReaderU32 (reader, &layer) || !ReaderU64 (reader, &timestamp) ||
        reader->pos != reader->len) return false;

    Buffer payload = {0};
    Buffer frame = {0};
    bool ok = BufferPutU64 (&payload, requestId) &&
              BufferPutString (&payload, objectName) &&
              BufferPutF32 (&payload, x) && BufferPutF32 (&payload, y) &&
              BufferPutU32 (&payload, layer) &&
              BufferPutU64 (&payload, timestamp) &&
              BufferPutU32 (&payload, 1) && BufferPutU64 (&payload, assetId) &&
              FrameMake (&frame, NET_REQ_OBJECT_CREATE, &payload);
    BufferFree (&payload);
    if (!ok) {
        BufferFree (&frame);
        return false;
    }
    Reader objectReader = {frame.data + 5, frame.len - 5, 0};
    bool result = ServerApplyObjectCreate (&objectReader, clientIndex);
    BufferFree (&frame);
    return result;
}

static bool ServerApplyObjectCreate (Reader *reader, int clientIndex);

static bool ServerApplyAssetRename (Reader *reader, int clientIndex) {
    u64 id;
    char requested[256];
    if (!ReaderU64 (reader, &id) || !ReaderString (reader, requested, sizeof (requested)))
        return false;

    Asset *asset = ServerAssetFind (id);
    if (asset == NULL || requested[0] == '\0') {
        ServerSendSnapshot (clientIndex);
        return true;
    }
    if (strcmp (asset->name, requested) == 0) return true;

    char name[256];
    ServerMakeUniqueName (requested, name, sizeof (name), false, id);
    if (!AssetRename (asset, name)) return false;

    Buffer frame = {0};
    if (!FrameAssetRename (&frame, id, asset->name)) {
        BufferFree (&frame);
        return false;
    }
    ServerBroadcast (frame.data, frame.len);
    BufferFree (&frame);
    return true;
}

static bool ServerApplyAssetReplace (Reader *reader, int clientIndex) {
    u64 id;
    char ext[8];
    uint32_t size;
    if (!ReaderU64 (reader, &id) || !ReaderString (reader, ext, sizeof (ext)) ||
        !ReaderU32 (reader, &size) || size == 0 || size > NET_MAX_FRAME ||
        !ReaderAvailable (reader, size)) return false;

    Asset *asset = ServerAssetFind (id);
    if (asset == NULL) {
        ServerSendSnapshot (clientIndex);
        return true;
    }

    const unsigned char *data = reader->data + reader->pos;
    reader->pos += size;
    if (!AssetReplaceRaw (asset, ext, data, (int)size)) return false;

    Buffer frame = {0};
    if (!FrameAssetReplace (&frame, id, asset->ext, asset->fileData,
                            (uint32_t)asset->fileSize)) {
        BufferFree (&frame);
        return false;
    }
    ServerBroadcast (frame.data, frame.len);
    BufferFree (&frame);
    return true;
}

static int ServerAssetReferenceCount (u64 id) {
    int count = 0;
    IdGroup objectIds = ObjectIds ();
    for (int i = 0; i < objectIds.count; i++) {
        Object *object = EntityFind (objectIds.ids[i]);
        if (object == NULL) continue;
        for (int a = 0; a < object->assets.count; a++) {
            if (object->assets.ids[a] == id) count++;
        }
    }
    IdGroupUnload (&objectIds);
    return count;
}

static bool ServerApplyAssetDelete (Reader *reader, int clientIndex) {
    u64 id;
    if (!ReaderU64 (reader, &id)) return false;

    Asset *asset = ServerAssetFind (id);
    if (asset == NULL) return true;

    if (ServerAssetReferenceCount (id) > 0) {
        Buffer failed = {0};
        if (FrameAssetDeleteFailed (&failed, id))
            ServerClientEnqueue (&serverClients[clientIndex], failed.data, failed.len);
        BufferFree (&failed);
        return true;
    }

    if (!AssetDelete (asset)) return false;
    Buffer frame = {0};
    if (!FrameAssetDelete (&frame, id)) {
        BufferFree (&frame);
        return false;
    }
    ServerBroadcast (frame.data, frame.len);
    BufferFree (&frame);
    return true;
}

static bool ServerApplyObjectCreate (Reader *reader, int clientIndex) {
    u64 requestId;
    char requested[256];
    float x, y;
    uint32_t layer;
    u64 timestamp;
    uint32_t assetCount;
    if (!ReaderU64 (reader, &requestId) ||
        !ReaderString (reader, requested, sizeof (requested)) ||
        !ReaderF32 (reader, &x) || !ReaderF32 (reader, &y) ||
        !ReaderU32 (reader, &layer) || !ReaderU64 (reader, &timestamp) ||
        !ReaderU32 (reader, &assetCount) || assetCount > 100000 ||
        !ReaderAvailable (reader, (size_t)assetCount * 8)) return false;

    u64 *ids = NULL;
    if (assetCount > 0) {
        ids = malloc ((size_t)assetCount * sizeof (u64));
        if (ids == NULL) return false;
        for (uint32_t i = 0; i < assetCount; i++) {
            if (!ReaderU64 (reader, &ids[i]) || ServerAssetFind (ids[i]) == NULL) {
                free (ids);
                ServerSendSnapshot (clientIndex);
                return true;
            }
        }
    }

    u64 id = ServerNewEntityId ();
    char name[256];
    ServerMakeUniqueName (requested, name, sizeof (name), true, 0);

    Object *object = ObjectAdd (name, (Vector2){x, y}, (int32_t)layer, timestamp, id);
    if (object == NULL) {
        free (ids);
        return false;
    }
    if (!IdGroupCopy (ObjectAssets (object), ids, (int)assetCount)) {
        ObjectDelete (object);
        free (ids);
        return false;
    }
    free (ids);
    ObjectTouch (object);

    for (int i = 0; i < NET_MAX_CLIENTS; i++) {
        if (!serverClients[i].joined) continue;
        Buffer frame = {0};
        bool hidden = ObjectIsHiddenForUser (object, serverClients[i].userId);
        if (!FrameObjectCreated (&frame, requestId, serverClients[clientIndex].userId,
                                 id, object->name, object->pos.x, object->pos.y,
                                 object->layer, object->timestamp, hidden, 0,
                                 object->assets.ids, (uint32_t)object->assets.count)) {
            BufferFree (&frame);
            return false;
        }
        ServerClientEnqueue (&serverClients[i], frame.data, frame.len);
        BufferFree (&frame);
    }
    ServerLockObject (clientIndex, id);
    return true;
}

static bool ServerApplyObjectUpsert (Reader *reader, int clientIndex) {
    u64 id;
    char requested[256];
    float x, y;
    uint32_t layer;
    u64 timestamp;
    uint8_t hidden;
    uint32_t assetIndex;
    uint32_t assetCount;
    if (!ReaderU64 (reader, &id) || !ReaderString (reader, requested, sizeof (requested)) ||
        !ReaderF32 (reader, &x) || !ReaderF32 (reader, &y) ||
        !ReaderU32 (reader, &layer) || !ReaderU64 (reader, &timestamp) ||
        !ReaderBytes (reader, &hidden, 1) ||
        !ReaderU32 (reader, &assetCount) || !ReaderU32 (reader, &assetIndex) ||
        id == 0 ||
        assetCount > 100000 || !ReaderAvailable (reader, (size_t)assetCount * 8))
        return false;
    (void)hidden;

    Object *object = ServerObjectFind (id);
    if (object == NULL) {
        if (ServerObjectIdDeleted (id)) return true;
        ServerSendSnapshot (clientIndex);
        return true;
    }
    if (object->lockOwnerId == 0 ||
        object->lockOwnerId != serverClients[clientIndex].userId) {
        ServerSendObjectState (clientIndex, object);
        ServerSendLockState (clientIndex, object->id, object->lockOwnerId);
        return true;
    }

    u64 *ids = NULL;
    if (assetCount > 0) {
        ids = malloc ((size_t)assetCount * sizeof (u64));
        if (ids == NULL) return false;
        for (uint32_t i = 0; i < assetCount; i++) {
            if (!ReaderU64 (reader, &ids[i]) || ServerAssetFind (ids[i]) == NULL) {
                free (ids);
                ServerSendSnapshot (clientIndex);
                return true;
            }
        }
    }

    char name[256];
    ServerMakeUniqueName (requested, name, sizeof (name), true, id);

    if (!ObjectRename (object, name) ||
        !IdGroupCopy (ObjectAssets (object), ids, (int)assetCount)) {
        free (ids);
        return false;
    }
    free (ids);
    object->pos = (Vector2){x, y};
    object->layer = (int32_t)layer;
    object->assetIndex = assetIndex < (uint32_t)object->assets.count
                             ? (int)assetIndex : 0;
    // 时间戳只增不减，避免置顶广播后到达的旧更新把物件压回去。
    if (timestamp > object->timestamp) object->timestamp = timestamp;

    ServerBroadcastObjectStateForAll (object);
    return true;
}

static bool ServerApplyObjectFront (Reader *reader, int clientIndex) {
    u64 id;
    if (!ReaderU64 (reader, &id)) return false;

    Object *object = ServerObjectFind (id);
    if (object == NULL) return true;
    if (object->lockOwnerId != serverClients[clientIndex].userId) {
        ServerSendObjectState (clientIndex, object);
        ServerSendLockState (clientIndex, id, object->lockOwnerId);
        return true;
    }

    ObjectTouch (object);
    ServerBroadcastObjectStateForAll (object);
    return true;
}

static bool ServerApplyDragEnd (Reader *reader, int clientIndex) {
    u64 id;
    if (!ReaderU64 (reader, &id) || reader->pos != reader->len) return false;
    Object *object = ServerObjectFind (id);
    if (object == NULL) return true;
    u64 userId = serverClients[clientIndex].userId;
    if (object->lockOwnerId == userId)
        ServerRunObjectEvent (id, "dragged", userId);
    return true;
}

static bool ServerApplyDragUpdate (Reader *reader, int clientIndex) {
    u64 id;
    float dx, dy;
    if (!ReaderU64 (reader, &id) || !ReaderF32 (reader, &dx) ||
        !ReaderF32 (reader, &dy) || reader->pos != reader->len) return false;
    Object *object = ServerObjectFind (id);
    if (object == NULL) return true;
    u64 userId = serverClients[clientIndex].userId;
    if (object->lockOwnerId != userId) return true;

    Vector2 moved = {object->pos.x + dx, object->pos.y + dy};
    object->pos = moved;
    ObjectTouch (object);
    // 脚本可能再次移动 self；拖动期间暂不广播中间状态，最后统一广播。
    serverSuppressObjectBroadcast = true;
    ServerRunObjectEvent (id, "ondrag", userId);
    serverSuppressObjectBroadcast = false;
    object = ServerObjectFind (id);
    if (object != NULL) {
        object->pos = moved;
        ObjectTouch (object);
    }
    IdGroup objectIds = ObjectIds ();
    for (int i = 0; i < objectIds.count; i++) {
        Object *current = EntityFind (objectIds.ids[i]);
        if (current != NULL) ServerBroadcastObjectStateForAll (current);
    }
    IdGroupUnload (&objectIds);
    return true;
}

static bool ServerApplyObjectDelete (Reader *reader, int clientIndex) {
    u64 id;
    if (!ReaderU64 (reader, &id)) return false;

    Object *object = ServerObjectFind (id);
    if (object == NULL) return true;
    if (object->lockOwnerId == 0 ||
        object->lockOwnerId != serverClients[clientIndex].userId) {
        ServerSendObjectState (clientIndex, object);
        ServerSendLockState (clientIndex, id, object->lockOwnerId);
        return true;
    }
    if (!ObjectDelete (object)) return false;
    if (!ServerTombstoneObject (id)) return false;

    Buffer frame = {0};
    if (!FrameObjectDelete (&frame, id)) {
        BufferFree (&frame);
        return false;
    }
    ServerBroadcast (frame.data, frame.len);
    BufferFree (&frame);
    return true;
}

static bool ServerApplyCursor (Reader *reader, int clientIndex) {
    float x, y;
    if (!ReaderF32 (reader, &x) || !ReaderF32 (reader, &y)) return false;
    ServerClient *client = &serverClients[clientIndex];
    UserSetCursor (client->userId, true, x, y);

    Buffer frame = {0};
    if (!FrameCursorState (&frame, client->userId, x, y)) {
        BufferFree (&frame);
        return false;
    }
    ServerBroadcast (frame.data, frame.len);
    BufferFree (&frame);
    return true;
}

static void ServerRunObjectEvent (u64 id, const char *event, u64 userId) {
    if (serverScriptEnv == NULL) return;
    TextBuffer error = ScriptRunObjectEvent (id, event, userId, serverScriptEnv);
    if (error.length > 0) {
        fprintf (stderr, "物件事件 %s 失败: %s\n", event, error.data);
        NetServerSendError (0, error.data);
    }
    TextBufferFree (&error);
}

static void ServerLockObject (int clientIndex, u64 id) {
    Object *object = ServerObjectFind (id);
    if (object == NULL) return;

    u64 userId = serverClients[clientIndex].userId;
    if (object->lockOwnerId != 0 && object->lockOwnerId != userId) {
        ServerSendLockState (clientIndex, id, object->lockOwnerId);
        return;
    }
    if (object->lockOwnerId == userId && userId != 0) {
        ServerSendLockState (clientIndex, id, object->lockOwnerId);
        return;
    }

    ServerReleaseLocks (userId, false);
    object = ServerObjectFind (id);
    if (object == NULL) return;
    object->lockOwnerId = userId;
    pendingLockClient = clientIndex;
    pendingLockObjectId = id;
    pendingLockFailureSent = false;
    ServerRunObjectEvent (id, "ontouch", userId);
    bool failureSent = pendingLockFailureSent;
    pendingLockClient = -1;
    pendingLockObjectId = 0;
    pendingLockFailureSent = false;

    Object *current = ServerObjectFind (id);
    if (current != NULL && current->lockOwnerId == userId) {
        ServerBroadcastLockState (id, userId);
    } else if (!failureSent) {
        Buffer frame = {0};
        if (FrameObjectLock (&frame, NET_LOCK_FAILED, id))
            ServerClientEnqueue (&serverClients[clientIndex], frame.data, frame.len);
        BufferFree (&frame);
    }
}

static bool ServerApplyLock (Reader *reader, int clientIndex, bool unlock,
                             bool notify) {
    u64 id;
    if (!ReaderU64 (reader, &id)) return false;

    Object *object = ServerObjectFind (id);
    if (object == NULL) return true;

    u64 userId = serverClients[clientIndex].userId;

    if (unlock) {
        if (object->lockOwnerId == userId) {
            if (notify) ServerRunObjectEvent (id, "touched", userId);
            object = ServerObjectFind (id);
            if (object != NULL && object->lockOwnerId == userId) {
                object->lockOwnerId = 0;
                ServerBroadcastLockState (id, 0);
            }
        }
        return true;
    }

    ServerLockObject (clientIndex, id);
    return true;
}

static bool ServerApplyScript (Reader *reader, int clientIndex) {
    User *user = UserFind (serverClients[clientIndex].userId);
    if (user != NULL && !user->canscript) {
        NetServerSendError (serverClients[clientIndex].userId,
                            "当前用户不允许提交脚本");
        return true;
    }

    char source[65536];
    char selfExpr[768];
    if (!ReaderString (reader, source, sizeof (source)) ||
        !ReaderString (reader, selfExpr, sizeof (selfExpr)) ||
        reader->pos != reader->len)
        return false;
    if (serverScriptEnv == NULL) {
        NetServerSendError (serverClients[clientIndex].userId, "脚本环境不可用");
        return true;
    }

    u64 targetSelf = 0;
    if (selfExpr[0] != '\0') {
        char expression[1024];
        int needed = snprintf (expression, sizeof (expression), "return (%s)\n", selfExpr);
        if (needed < 0 || (size_t)needed >= sizeof (expression)) {
            NetServerSendError (serverClients[clientIndex].userId, "self 表达式过长");
            return true;
        }

        char *selfResult = NULL;
        TextBuffer selfError = ScriptRun (serverClients[clientIndex].userId, expression,
                                          serverScriptEnv, &selfResult);
        if (selfError.length > 0) {
            NetServerSendError (0, selfError.data);
            TextBufferFree (&selfError);
            free (selfResult);
            return true;
        }
        TextBufferFree (&selfError);

        bool valid = selfResult != NULL;
        unsigned long long value = 0;
        if (valid && selfResult[0] != '\0') {
            for (const char *digit = selfResult; *digit != '\0'; digit++) {
                if (*digit < '0' || *digit > '9') {
                    valid = false;
                    break;
                }
            }
        } else {
            valid = false;
        }
        if (valid) {
            errno = 0;
            char *end = NULL;
            value = strtoull (selfResult, &end, 10);
            if (errno == ERANGE || end == selfResult || *end != '\0')
                valid = false;
        }
        free (selfResult);
        if (!valid) {
            NetServerSendError (serverClients[clientIndex].userId,
                                "self 表达式必须返回 int");
            return true;
        }
        targetSelf = (u64)value;
    }

    char *result = NULL;
    TextBuffer error = ScriptRun (targetSelf, source, serverScriptEnv, &result);
    if (error.length > 0) {
        NetServerSendError (0, error.data);
    } else if (result != NULL) {
        char message[NET_MAX_MESSAGE + 1];
        snprintf (message, sizeof (message), "脚本已返回 %s", result);
        NetServerSendInfo (serverClients[clientIndex].userId, message);
    }
    TextBufferFree (&error);
    free (result);
    return true;
}

static bool ServerApplyMenu (Reader *reader, int clientIndex) {
    u64 id;
    uint32_t rawIndex;
    char name[256];
    if (!ReaderU64 (reader, &id) || !ReaderU32 (reader, &rawIndex) ||
        !ReaderString (reader, name, sizeof (name)) || reader->pos != reader->len)
        return false;
    if (rawIndex > (uint32_t)INT_MAX) {
        NetServerSendError (serverClients[clientIndex].userId, "菜单项不存在");
        return true;
    }

    if (id != 0 && UserFind (id) != NULL &&
        id != serverClients[clientIndex].userId) {
        NetServerSendError (serverClients[clientIndex].userId,
                            "无权执行其他用户的菜单");
        return true;
    }

    int index = (int)rawIndex;
    const char *menuName = MenuName (id, index);
    const char *func = MenuFunc (id, index);
    if (menuName == NULL || func == NULL || strcmp (menuName, name) != 0) {
        NetServerSendError (serverClients[clientIndex].userId, "菜单项不存在或已更新");
        return true;
    }
    if (serverScriptEnv == NULL) {
        NetServerSendError (serverClients[clientIndex].userId, "脚本环境不可用");
        return true;
    }

    char *result = NULL;
    TextBuffer error = ScriptRunFunction (id, func, serverScriptEnv,
                                          serverClients[clientIndex].userId, &result);
    if (error.length > 0) {
        NetServerSendError (0, error.data);
    } else if (result != NULL) {
        char message[NET_MAX_MESSAGE + 1];
        snprintf (message, sizeof (message), "脚本已返回 %s", result);
        NetServerSendInfo (serverClients[clientIndex].userId, message);
    }
    TextBufferFree (&error);
    free (result);
    return true;
}

static u64 ServerImportAllocateId (void *context) {
    (void)context;
    return ServerNewEntityId ();
}

static void SendImportConflicts (u64 userId, const TextBuffer *conflicts) {
    const char *line = conflicts->data;
    while (line != NULL && *line != '\0') {
        const char *end = strchr (line, '\n');
        size_t length = end != NULL ? (size_t)(end - line) : strlen (line);
        if (length > 0) {
            char message[NET_MAX_MESSAGE + 1];
            snprintf (message, sizeof (message), "导入冲突，未替换: %.*s",
                      (int)length, line);
            NetServerSendError (userId, message);
        }
        if (end == NULL) break;
        line = end + 1;
    }
}

static bool ServerMergeImportData (const unsigned char *data, size_t size,
                                  ScriptEnv *globalEnv, TextBuffer *conflicts) {
    char directory[1024];
    size_t directorySize = sizeof (directory);
    if (uv_os_tmpdir (directory, &directorySize) != 0) return false;
    char path[1024];
    int needed = snprintf (path, sizeof (path),
                           "%s/image-canvas-import-XXXXXX", directory);
    if (needed < 0 || (size_t)needed >= sizeof (path)) return false;

    uv_fs_t request;
    int fd = uv_fs_mkstemp (NULL, &request, path, NULL);
    uv_fs_req_cleanup (&request);
    if (fd < 0) return false;

    if (uv_fs_close (NULL, &request, fd, NULL) < 0) return false;
    uv_fs_req_cleanup (&request);

    bool ok = false;
    FILE *file = fopen (path, "wb");
    if (file != NULL) {
        ok = fwrite (data, 1, size, file) == size;
        if (fclose (file) != 0) ok = false;
    }
    if (ok) ok = SaveMerge (path, globalEnv, ServerImportAllocateId, NULL,
                            conflicts);
    uv_fs_unlink (NULL, &request, path, NULL);
    uv_fs_req_cleanup (&request);
    return ok;
}

static bool ServerApplyImport (Reader *reader, int clientIndex) {
    uint32_t size = 0;
    if (!ReaderU32 (reader, &size) || size == 0 ||
        !ReaderAvailable (reader, size)) return false;
    const unsigned char *data = reader->data + reader->pos;
    reader->pos += size;
    if (reader->pos != reader->len) return false;

    u64 userId = serverClients[clientIndex].userId;
    TextBuffer conflicts = {0};
    bool ok = ServerMergeImportData (data, size, serverScriptEnv, &conflicts);
    if (!ok) {
        fprintf (stderr, "增量导入失败\n");
        NetServerSendError (userId, "增量导入失败");
    } else if (conflicts.length > 0) {
        fprintf (stderr, "增量导入冲突（未替换）:\n%s", conflicts.data);
        SendImportConflicts (userId, &conflicts);
    } else {
        fprintf (stderr, "增量导入完成\n");
        NetServerSendInfo (userId, "增量导入完成");
    }

    for (int i = 0; i < NET_MAX_CLIENTS; i++) {
        if (serverClients[i].joined) ServerSendSnapshot (i);
    }
    TextBufferFree (&conflicts);
    return true;
}


static bool ServerImportAllowed (int clientIndex) {
    User *user = UserFind (serverClients[clientIndex].userId);
    if (user != NULL && !user->canscript) {
        NetServerSendError (serverClients[clientIndex].userId,
                            "当前用户不允许导入");
        return false;
    }
    return true;
}


static bool ServerApplyFrame (uint8_t type, const unsigned char *data, size_t length,
                              int clientIndex) {
    Reader reader = {data, length, 0};
    bool ok = false;

    switch (type) {
        case NET_REQ_ASSET_ADD:
            if (!ServerImportAllowed (clientIndex)) { ok = true; break; }
            ok = ServerApplyAssetAdd (&reader, clientIndex);
            break;
        case NET_REQ_ASSET_OBJECT:
            if (!ServerImportAllowed (clientIndex)) { ok = true; break; }
            ok = ServerApplyAssetObject (&reader, clientIndex);
            break;
        case NET_ASSET_RENAME:
            ok = ServerApplyAssetRename (&reader, clientIndex);
            break;
        case NET_ASSET_REPLACE:
            ok = ServerApplyAssetReplace (&reader, clientIndex);
            break;
        case NET_ASSET_DELETE:
            ok = ServerApplyAssetDelete (&reader, clientIndex);
            break;
        case NET_REQ_OBJECT_CREATE:
            ok = ServerApplyObjectCreate (&reader, clientIndex);
            break;
        case NET_OBJECT_UPSERT:
            ok = ServerApplyObjectUpsert (&reader, clientIndex);
            break;
        case NET_REQ_OBJECT_FRONT:
            ok = ServerApplyObjectFront (&reader, clientIndex);
            break;
        case NET_REQ_SCRIPT:
            ok = ServerApplyScript (&reader, clientIndex);
            break;
        case NET_REQ_MENU:
            ok = ServerApplyMenu (&reader, clientIndex);
            break;
        case NET_REQ_IMPORT:
            if (!ServerImportAllowed (clientIndex)) { ok = true; break; }
            ok = ServerApplyImport (&reader, clientIndex);
            break;
        case NET_DRAG_UPDATE:
            ok = ServerApplyDragUpdate (&reader, clientIndex);
            break;
        case NET_DRAG_END:
            ok = ServerApplyDragEnd (&reader, clientIndex);
            break;
        case NET_OBJECT_DELETE:
            ok = ServerApplyObjectDelete (&reader, clientIndex);
            break;
        case NET_CURSOR:
            ok = ServerApplyCursor (&reader, clientIndex);
            break;
        case NET_LOCK:
            ok = ServerApplyLock (&reader, clientIndex, false, false);
            break;
        case NET_UNLOCK:
            ok = ServerApplyLock (&reader, clientIndex, true, false);
            break;
        case NET_UNLOCK_NOTIFY:
            ok = ServerApplyLock (&reader, clientIndex, true, true);
            break;
        default:
            return false;
    }

    return ok;
}

static void ServerReadFrames (ServerClient *client, int clientIndex) {
    size_t offset = 0;

    while (client->in.len - offset >= 5) {
        const unsigned char *data = client->in.data + offset;
        uint32_t payloadSize = (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
                               ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
        if (payloadSize == 0 || payloadSize > NET_MAX_FRAME) {
            ServerCloseInput (clientIndex);
            break;
        }

        size_t frameSize = 4 + (size_t)payloadSize;
        if (client->in.len - offset < frameSize) break;

        uint8_t type = data[4];
        const unsigned char *payload = data + 5;
        size_t payloadLength = (size_t)payloadSize - 1;

        if (!client->helloQueued) {
            if (type != NET_HELLO) {
                ServerCloseInput (clientIndex);
                break;
            }
            client->helloQueued = true;
        }

        if (type == SERVER_EVENT_DISCONNECT) {
            client->inputClosed = true;
            break;
        }
        if (!ServerQueuePush (clientIndex, type, payload, payloadLength)) {
            client->inputClosed = true;
            break;
        }
        offset += frameSize;
    }

    if (offset > 0) {
        memmove (client->in.data, client->in.data + offset, client->in.len - offset);
        client->in.len -= offset;
    }
}

static void ServerProcessRequests (void) {
    for (int i = 0; i < serverRequestCount; i++) {
        ServerRequest *request = &serverRequests[i];
        ServerClient *client = &serverClients[request->clientIndex];

        if (request->type == SERVER_EVENT_DISCONNECT) {
            ServerBeginDisconnect (client);
        } else if (!client->active || client->closing) {
            /* The connection was already rejected or closed. */
        } else if (request->type == NET_HELLO) {
            if (client->joined) {
                ServerBeginDisconnect (client);
            } else if (!ServerHandleHello (client, request->clientIndex,
                                           request->payload, request->length)) {
                ServerBeginDisconnect (client);
            }
        } else if (!client->joined) {
            ServerBeginDisconnect (client);
        } else if (!ServerApplyFrame (request->type, request->payload,
                                      request->length, request->clientIndex)) {
            ServerBeginDisconnect (client);
        }

        free (request->payload);
    }
    serverRequestCount = 0;
    for (int i = 0; i < NET_MAX_CLIENTS; i++)
        ServerClientMaybeClose (&serverClients[i]);
}

static time_t serverLastSave;
static NetutilServer *serverListener;

static void ServerClientAccepted (NetutilServer *server, NetutilStream *stream,
                                  void *user) {
    (void)server;
    (void)user;
    int slot = -1;
    for (int i = 0; i < NET_MAX_CLIENTS; i++) {
        if (!serverClients[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        NetutilClose (stream);
        return;
    }

    ServerClient *client = &serverClients[slot];
    memset (client, 0, sizeof (*client));
    client->stream = stream;
    client->active = true;
    NetutilSetEvents (stream, &serverClientEvents, client);
    printf ("client connected\n");
    fflush (stdout);
}

static void ServerClientForceClose (ServerClient *client) {
    if (!client->active) return;
    client->closing = true;
    ServerClientMaybeClose (client);
}

int NetRunServer (int port, const char *bindAddress,
                  const char *loadPath, const char *savePath) {
    if (!NetutilInit ()) return 1;
    serverLastSave = time (NULL);
    serverSavePath[0] = '\0';
    if (savePath != NULL && savePath[0] != '\0')
        snprintf (serverSavePath, sizeof (serverSavePath), "%s", savePath);
    for (int i = 0; i < NET_MAX_CLIENTS; i++)
        memset (&serverClients[i], 0, sizeof (serverClients[i]));

    const char *bind = bindAddress != NULL && bindAddress[0] != '\0'
                           ? bindAddress : "0.0.0.0";
    if (!NetutilListen (bind, (uint16_t)port, ServerClientAccepted, NULL,
                        &serverListener)) {
        fprintf (stderr, "无法监听端口 %d\n", port);
        NetutilShutdown ();
        return 1;
    }

    serverScriptEnv = ScriptEnvCreate ();
    if (serverScriptEnv == NULL) {
        fprintf (stderr, "创建脚本环境失败\n");
        NetutilServerClose (serverListener);
        NetutilShutdown ();
        return 1;
    }
    if (loadPath != NULL && loadPath[0] != '\0') {
        u64 nextId = 1;
        if (!SaveRead (loadPath, serverScriptEnv, &nextId)) {
            fprintf (stderr, "导入存档失败: %s\n", loadPath);
            ScriptEnvDestroy (serverScriptEnv);
            serverScriptEnv = NULL;
            NetutilServerClose (serverListener);
            NetutilShutdown ();
            return 1;
        }
        ServerSetNextEntityId (nextId);
    }

    UtilsInstallStopHandlers ();
    printf ("Image Canvas server listening on port %d\n", port);
    fflush (stdout);
    char exportPath[1024];
    while (!UtilsStopRequested () && !ControlClientClosed ()) {
        if (ControlClientTakeExport (exportPath, sizeof (exportPath))) {
            if (serverScriptEnv != NULL && SaveWrite (exportPath, serverScriptEnv))
                NetServerSendInfo (0, "导出存档已完成");
            else
                NetServerSendError (0, "导出存档失败");
        }
        NetutilPump (500);
        time_t now = time (NULL);
        if (now - serverLastSave >= SAVE_INTERVAL_SECONDS) {
            serverLastSave = now;
            ServerSave ();
        }
    }

    ServerSave ();
    NetutilServerClose (serverListener);
    for (int i = 0; i < NET_MAX_CLIENTS; i++)
        ServerClientForceClose (&serverClients[i]);
    NetutilShutdown ();
    serverListener = NULL;

    if (serverScriptEnv != NULL) ScriptEnvDestroy (serverScriptEnv);
    serverScriptEnv = NULL;
    return 0;
}


static void ServerSendMessageType (u64 target, const char *message, uint8_t type) {
    if (message == NULL || message[0] == '\0') return;

    char text[NET_MAX_MESSAGE + 1];
    snprintf (text, sizeof (text), "%s", message);

    Buffer frame = {0};
    if (!FrameMessage (&frame, type, text)) {
        BufferFree (&frame);
        return;
    }
    if (target == 0) {
        ServerBroadcast (frame.data, frame.len);
    } else {
        for (int i = 0; i < NET_MAX_CLIENTS; i++) {
            ServerClient *client = &serverClients[i];
            if (client->joined && client->active && client->userId == target)
                ServerClientEnqueue (client, frame.data, frame.len);
        }
    }
    BufferFree (&frame);
}

void NetServerBroadcastMessage (const char *message) {
    ServerSendMessageType (0, message, NET_MESSAGE);
}

void NetServerSendMessage (u64 target, const char *message) {
    ServerSendMessageType (target, message, NET_MESSAGE);
}

void NetServerSendError (u64 target, const char *message) {
    ServerSendMessageType (target, message, NET_ERROR);
}

void NetServerSendInfo (u64 target, const char *message) {
    ServerSendMessageType (target, message, NET_INFO);
}
void NetServerBroadcastMenu (u64 id) {
    Buffer frame = {0};
    if (!FrameMenu (&frame, id)) {
        BufferFree (&frame);
        return;
    }
    if (id != 0 && UserFind (id) != NULL) {
        for (int i = 0; i < NET_MAX_CLIENTS; i++) {
            if (serverClients[i].joined && serverClients[i].userId == id)
                ServerClientEnqueue (&serverClients[i], frame.data, frame.len);
        }
    } else {
        ServerBroadcast (frame.data, frame.len);
    }
    BufferFree (&frame);
}

void NetServerReleaseObjectLock (u64 id) {
    Object *object = ServerObjectFind (id);
    if (object == NULL || object->lockOwnerId == 0) return;
    u64 ownerId = object->lockOwnerId;
    object->lockOwnerId = 0;
    ServerBroadcastLockState (id, 0);

    for (int i = 0; i < NET_MAX_CLIENTS; i++) {
        if (serverClients[i].joined && serverClients[i].userId == ownerId) {
            Buffer frame = {0};
            if (FrameObjectLock (&frame, NET_LOCK_FAILED, id))
                ServerClientEnqueue (&serverClients[i], frame.data, frame.len);
            BufferFree (&frame);
            break;
        }
    }
    if (id == pendingLockObjectId) pendingLockFailureSent = true;
}

bool NetServerDeleteObject (u64 id) {
    Object *object = ServerObjectFind (id);
    if (object == NULL) return false;
    if (object->lockOwnerId != 0) NetServerReleaseObjectLock (id);

    if (!ObjectDelete (object)) return false;
    if (!ServerTombstoneObject (id)) return false;

    Buffer frame = {0};
    if (!FrameObjectDelete (&frame, id)) {
        BufferFree (&frame);
        return false;
    }
    ServerBroadcast (frame.data, frame.len);
    BufferFree (&frame);
    return true;
}

void NetServerBroadcastObjectState (const Object *object) {
    if (object == NULL) return;
    ServerBroadcastObjectStateForAll (object);
}


