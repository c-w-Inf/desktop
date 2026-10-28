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
    enum NetPacketType type;
    unsigned char *payload;
    size_t length;
} ServerRequest;

static ServerRequest *serverRequests;
static int serverRequestCount;
static int serverRequestCapacity;




static Asset *ServerAssetFind (u64 id) {
    return EntityKindOf (id) == ENTITY_KIND_ASSET ? (Asset *)EntityFind (id) : NULL;
}

static Object *ServerObjectFind (u64 id) {
    return EntityKindOf (id) == ENTITY_KIND_OBJECT ? (Object *)EntityFind (id) : NULL;
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
            Asset *existing = AssetFind (name);
            used = existing != NULL && existing != exclude;
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

static bool ServerQueuePush (int clientIndex, enum NetPacketType type,
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

static void ServerBroadcastUserRecord (const User *user) {
    Buffer frame = {0};
    if (FrameUserState (&frame, user->id, user->name, user->color,
                        user->canscript, user->hasCursor, user->x, user->y))
        ServerBroadcast (frame.data, frame.len);
    BufferFree (&frame);
}

static void ServerBroadcastUserState (const ServerClient *client) {
    User *user = (User *)EntityFind (client->userId);
    if (user != NULL) ServerBroadcastUserRecord (user);
}

void NetServerSetUserColor (u64 id, uint32_t color) {
    if (EntityKindOf (id) != ENTITY_KIND_USER) return;
    User *user = (User *)EntityFind (id);
    if (user == NULL) return;
    UserSetColor (id, color);
    ServerBroadcastUserRecord (user);
}

void NetServerSetUserCanScript (u64 id, bool canscript) {
    if (EntityKindOf (id) != ENTITY_KIND_USER) return;
    User *user = (User *)EntityFind (id);
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
    if (FrameObjectUpsert (&frame, id, OBJECT_FIELD_LOCK_OWNER, NULL,
                           0.0f, 0.0f, 0, 0, false, 0, NULL, 0, ownerId))
        ServerBroadcast (frame.data, frame.len);
    BufferFree (&frame);
}

static void ServerSendLockState (int clientIndex, u64 id, u64 ownerId) {
    Buffer frame = {0};
    if (FrameObjectUpsert (&frame, id, OBJECT_FIELD_LOCK_OWNER, NULL,
                           0.0f, 0.0f, 0, 0, false, 0, NULL, 0, ownerId))
        ServerClientEnqueue (&serverClients[clientIndex], frame.data, frame.len);
    BufferFree (&frame);
}

static void ServerSendLockFailed (int clientIndex, u64 id) {
    Buffer frame = {0};
    if (FrameObjectLock (&frame, NET_LOCK_FAILED, id))
        ServerClientEnqueue (&serverClients[clientIndex], frame.data, frame.len);
    BufferFree (&frame);
}

static void ServerSendLockFailedToUser (u64 userId, u64 id) {
    if (userId == 0) return;
    for (int i = 0; i < NET_MAX_CLIENTS; i++) {
        if (serverClients[i].joined && serverClients[i].active &&
            serverClients[i].userId == userId)
            ServerSendLockFailed (i, id);
    }
}

static void ServerSendObjectState (int clientIndex, const Object *object) {
    if (serverSuppressObjectBroadcast) return;
    Buffer frame = {0};
    bool hidden = ObjectIsHiddenForUser (object, serverClients[clientIndex].userId);
    if (FrameObjectUpsert (&frame, object->id, OBJECT_FIELD_ALL,
                           object->name, object->pos.x, object->pos.y,
                           object->layer, object->timestamp, hidden,
                           (uint32_t)object->assetIndex, object->assets.ids,
                           (uint32_t)object->assets.count,
                           object->lockOwnerId))
        ServerClientEnqueue (&serverClients[clientIndex], frame.data, frame.len);
    BufferFree (&frame);
}

static void ServerBroadcastObjectStateForAll (const Object *object) {
    for (int i = 0; i < NET_MAX_CLIENTS; i++) {
        if (serverClients[i].joined) ServerSendObjectState (i, object);
    }
}

static void ServerBroadcastAsset (const Asset *asset) {
    RawFile file = AssetRawFile(asset);
    Buffer frame = {0};
    if (FrameAssetAdd (&frame, 0, 0, AssetId(asset), AssetName(asset),
                        file.ext, file.data, (uint32_t)file.size))
        ServerBroadcast (frame.data, frame.len);
    BufferFree (&frame);
}

static void ServerSendAsset (int clientIndex, const Asset *asset) {
    RawFile file = AssetRawFile(asset);
    Buffer frame = {0};
    if (FrameAssetAdd (&frame, 0, 0, AssetId(asset), AssetName(asset),
                        file.ext, file.data, (uint32_t)file.size))
        ServerClientEnqueue (&serverClients[clientIndex], frame.data, frame.len);
    BufferFree (&frame);
}

static void ServerSendUser (int clientIndex, const User *user) {
    Buffer frame = {0};
    if (FrameUserState (&frame, user->id, user->name, user->color,
                        user->canscript, user->hasCursor, user->x, user->y))
        ServerClientEnqueue (&serverClients[clientIndex], frame.data, frame.len);
    BufferFree (&frame);
}

static void ServerSendHeartbeat (int clientIndex) {
    Buffer frame = {0};
    if (FrameHeartbeat (&frame))
        ServerClientEnqueue (&serverClients[clientIndex], frame.data, frame.len);
    BufferFree (&frame);
}

static bool ServerSendWelcome (int clientIndex) {
    Buffer payload = {0};
    Buffer frame = {0};
    bool ok = BufferPutU64 (&payload, serverClients[clientIndex].userId) &&
              FrameMake (&frame, NET_HELLO, &payload);
    if (ok)
        ServerClientEnqueue (&serverClients[clientIndex], frame.data, frame.len);
    BufferFree (&payload);
    BufferFree (&frame);
    return ok;
}

static void ServerSendJoinState (int clientIndex) {
    if (!ServerSendWelcome (clientIndex)) {
        serverClients[clientIndex].closing = true;
        return;
    }
    ServerSendHeartbeat (clientIndex);

    IdGroup assets = AssetIds ();
    for (int i = 0; i < assets.count; i++) {
        Asset *asset = EntityFind (assets.ids[i]);
        if (asset != NULL) ServerSendAsset (clientIndex, asset);
    }
    IdGroupUnload (&assets);

    IdGroup users = UserIds ();
    for (int i = 0; i < users.count; i++) {
        User *user = (User *)EntityFind (users.ids[i]);
        if (user != NULL) ServerSendUser (clientIndex, user);
    }
    IdGroupUnload (&users);

    IdGroup objects = ObjectIds ();
    for (int i = 0; i < objects.count; i++) {
        Object *object = EntityFind (objects.ids[i]);
        if (object != NULL) ServerSendObjectState (clientIndex, object);
    }
    IdGroupUnload (&objects);

    ServerSendAllMenus (clientIndex);
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
            ServerSendLockFailedToUser (userId, object->id);
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
                               ServerPacket *packet) {
    const char *name = packet->u.hello.username;
    if (name[0] == '\0' || ServerUsernameTaken (name, clientIndex)) {
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
    ServerSendJoinState (clientIndex);
    ServerBroadcastUserState (client);
    printf ("client joined: %s\n", client->username);
    fflush (stdout);
    return true;
}

static bool ServerCreateObject (int clientIndex, u64 requestId,
                                const char *requested, float x, float y,
                                uint32_t layer, const u64 *ids,
                                uint32_t assetCount) {
    for (uint32_t i = 0; i < assetCount; i++) {
        if (ServerAssetFind (ids[i]) == NULL) return true;
    }
    u64 id = ServerNewEntityId ();
    char name[256];
    ServerMakeUniqueName (requested, name, sizeof (name), true, 0);
    Object *object = ObjectAdd (name, (Vector2){x, y}, (int32_t)layer, 0, id);
    if (object == NULL) return false;
    if (!IdGroupCopy (ObjectAssets (object), ids, (int)assetCount)) {
        ObjectDelete (object);
        return false;
    }
    ObjectTouch (object);
    for (int i = 0; i < NET_MAX_CLIENTS; i++) {
        if (!serverClients[i].joined) continue;
        Buffer frame = {0};
        bool hidden = ObjectIsHiddenForUser (object, serverClients[i].userId);
        if (!FrameObjectCreated (&frame, requestId,
                                 serverClients[clientIndex].userId, id,
                                 object->name, object->pos.x, object->pos.y,
                                 object->layer, object->timestamp, hidden, 0,
                                 object->assets.ids,
                                 (uint32_t)object->assets.count)) {
            BufferFree (&frame);
            return false;
        }
        ServerClientEnqueue (&serverClients[i], frame.data, frame.len);
        BufferFree (&frame);
    }
    ServerLockObject (clientIndex, id);
    return true;
}

static bool ServerCreateAsset (const char *requested, const char *ext,
                               const unsigned char *data, size_t size,
                               u64 requestId, int clientIndex,
                               u64 *assetIdOut) {
    u64 id = ServerNewEntityId ();
    char name[256];
    ServerMakeUniqueName (requested, name, sizeof (name), false, 0);
    Asset *asset = AssetCreate (name, id);
    RawFile input = {(unsigned char *)data, size, ""};
    snprintf (input.ext, sizeof (input.ext), "%s", ext);
    if (asset == NULL || !AssetLoad (asset, input)) {
        if (asset != NULL) AssetDelete (asset);
        return false;
    }
    RawFile stored = AssetRawFile(asset);
    Buffer frame = {0};
    if (!FrameAssetAdd (&frame, requestId, serverClients[clientIndex].userId,
                        id, AssetName(asset), stored.ext, stored.data,
                        (uint32_t)stored.size)) {
        BufferFree (&frame);
        return false;
    }
    ServerBroadcast (frame.data, frame.len);
    BufferFree (&frame);
    if (assetIdOut != NULL) *assetIdOut = id;
    return true;
}

static bool ServerApplyAssetAdd (ServerPacket *packet, int clientIndex) {
    ServerAssetAddPacket *p = &packet->u.assetAdd;
    u64 assetId = 0;
    if (!ServerCreateAsset (p->name, p->ext, p->data, p->size,
                            p->requestId, clientIndex, &assetId))
        return false;
    if (!p->createObject) return true;
    return ServerCreateObject (clientIndex, p->requestId, p->objectName,
                               p->x, p->y, p->layer, &assetId, 1);
}

static bool ServerApplyAssetRename (ServerPacket *packet) {
    ServerAssetRenamePacket *p = &packet->u.assetRename;
    if (p->name[0] == '\0') return true;
    Asset *asset = ServerAssetFind (p->id);
    if (asset == NULL) return true;
    if (strcmp (AssetName(asset), p->name) == 0) return true;

    char name[256];
    ServerMakeUniqueName (p->name, name, sizeof (name), false, p->id);
    if (!AssetRename (asset, name)) return false;

    Buffer frame = {0};
    if (!FrameAssetRename (&frame, p->id, AssetName(asset))) {
        BufferFree (&frame);
        return false;
    }
    ServerBroadcast (frame.data, frame.len);
    BufferFree (&frame);
    return true;
}

static bool ServerApplyAssetReplace (ServerPacket *packet) {
    ServerAssetReplacePacket *p = &packet->u.assetReplace;
    Asset *asset = ServerAssetFind (p->id);
    if (asset == NULL) return true;

    RawFile input = {p->data, p->size, ""};
    snprintf (input.ext, sizeof (input.ext), "%s", p->ext);
    if (!AssetLoad (asset, input)) return false;

    RawFile stored = AssetRawFile(asset);
    Buffer frame = {0};
    if (!FrameAssetReplace (&frame, p->id, stored.ext, stored.data,
                            (uint32_t)stored.size)) {
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

static bool ServerApplyAssetDelete (ServerPacket *packet, int clientIndex) {
    u64 id = packet->u.id.id;
    Asset *asset = ServerAssetFind (id);
    if (asset == NULL) return true;

    if (ServerAssetReferenceCount (id) > 0) {
        NetServerSendError (serverClients[clientIndex].userId,
                            "素材正在被物件使用，无法删除");
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

static bool ServerApplyObjectCreate (ServerPacket *packet, int clientIndex) {
    ServerObjectCreatePacket *p = &packet->u.objectCreate;
    return ServerCreateObject (clientIndex, p->requestId, p->name,
                               p->x, p->y, p->layer,
                               p->assetIds, p->assetCount);
}

static bool ServerApplyObjectUpsert (ServerPacket *packet, int clientIndex) {
    ServerObjectUpsertPacket *p = &packet->u.objectUpsert;
    const uint32_t required = OBJECT_FIELD_NAME | OBJECT_FIELD_POSITION |
                              OBJECT_FIELD_LAYER | OBJECT_FIELD_ASSETS |
                              OBJECT_FIELD_ASSET_INDEX;
    if ((p->fields & required) != required || p->name == NULL) return true;
    Object *object = ServerObjectFind (p->id);
    if (object == NULL) return true;
    if (object->lockOwnerId == 0 ||
        object->lockOwnerId != serverClients[clientIndex].userId) {
        ServerSendObjectState (clientIndex, object);
        ServerSendLockState (clientIndex, object->id, object->lockOwnerId);
        return true;
    }
    for (uint32_t i = 0; i < p->assetCount; i++) {
        if (ServerAssetFind (p->assetIds[i]) == NULL) return true;
    }
    char name[256];
    ServerMakeUniqueName (p->name, name, sizeof (name), true, p->id);
    if (!ObjectRename (object, name) ||
        !IdGroupCopy (ObjectAssets (object), p->assetIds,
                      (int)p->assetCount))
        return false;
    object->pos = (Vector2){p->x, p->y};
    object->layer = (int32_t)p->layer;
    object->assetIndex = p->assetIndex < (uint32_t)object->assets.count
                             ? (int)p->assetIndex : 0;
    ObjectTouch (object);
    ServerBroadcastObjectStateForAll (object);
    return true;
}

static bool ServerApplyDragEnd (ServerPacket *packet, int clientIndex) {
    u64 id = packet->u.id.id;
    Object *object = ServerObjectFind (id);
    if (object == NULL) return true;
    u64 userId = serverClients[clientIndex].userId;
    if (object->lockOwnerId == userId)
        ServerRunObjectEvent (id, "dragged", userId);
    return true;
}

static bool ServerApplyDragUpdate (ServerPacket *packet, int clientIndex) {
    u64 id = packet->u.drag.id;
    float dx = packet->u.drag.dx;
    float dy = packet->u.drag.dy;
    Object *object = ServerObjectFind (id);
    if (object == NULL) return true;
    u64 userId = serverClients[clientIndex].userId;
    if (object->lockOwnerId != userId) return true;

    Vector2 moved = {object->pos.x + dx, object->pos.y + dy};
    object->pos = moved;
    ObjectTouch (object);

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

static bool ServerApplyObjectDelete (ServerPacket *packet, int clientIndex) {
    u64 id = packet->u.id.id;
    Object *object = ServerObjectFind (id);
    if (object == NULL) return true;
    if (object->lockOwnerId == 0 ||
        object->lockOwnerId != serverClients[clientIndex].userId) {
        ServerSendObjectState (clientIndex, object);
        ServerSendLockState (clientIndex, id, object->lockOwnerId);
        return true;
    }
    if (!ObjectDelete (object)) return false;

    Buffer frame = {0};
    if (!FrameObjectDelete (&frame, id)) {
        BufferFree (&frame);
        return false;
    }
    ServerBroadcast (frame.data, frame.len);
    BufferFree (&frame);
    return true;
}

static bool ServerApplyCursor (ServerPacket *packet, int clientIndex) {
    float x = packet->u.cursor.x;
    float y = packet->u.cursor.y;
    ServerClient *client = &serverClients[clientIndex];
    UserSetCursor (client->userId, true, x, y);

    Buffer frame = {0};
    if (!FrameCursorState (&frame, client->userId, x, y)) {
        BufferFree (&frame);
        return false;
    }
    ServerBroadcast (frame.data, frame.len);
    BufferFree (&frame);
    ServerSendHeartbeat (clientIndex);
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
    if (object == NULL) {
        ServerSendLockFailed (clientIndex, id);
        return;
    }

    u64 userId = serverClients[clientIndex].userId;
    if (object->lockOwnerId != 0 && object->lockOwnerId != userId) {
        ServerSendLockFailed (clientIndex, id);
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
    ServerRunObjectEvent (id, "ontouch", userId);

    Object *current = ServerObjectFind (id);
    if (current != NULL) {
        if (current->lockOwnerId == userId)
            ServerBroadcastLockState (id, userId);
        else
            ServerSendLockFailed (clientIndex, id);
    }
}

static bool ServerApplyLock (ServerPacket *packet, int clientIndex,
                             bool unlock, bool notify) {
    u64 id = packet->u.id.id;
    Object *object = ServerObjectFind (id);
    if (object == NULL) {
        if (!unlock) ServerSendLockFailed (clientIndex, id);
        return true;
    }

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

static bool ServerApplyScript (ServerPacket *packet, int clientIndex) {
    User *user = (User *)EntityFind (serverClients[clientIndex].userId);
    if (user != NULL && !user->canscript) {
        NetServerSendError (serverClients[clientIndex].userId,
                            "当前用户不允许提交脚本");
        return true;
    }

    const char *source = packet->u.script.source;
    const char *selfExpr = packet->u.script.selfExpr;
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

static bool ServerApplyMenu (ServerPacket *packet, int clientIndex) {
    u64 id = packet->u.menu.id;
    uint32_t rawIndex = packet->u.menu.index;
    const char *name = packet->u.menu.name;
    if (rawIndex > (uint32_t)INT_MAX) {
        NetServerSendError (serverClients[clientIndex].userId, "菜单项不存在");
        return true;
    }

    if (id != 0 && EntityKindOf (id) == ENTITY_KIND_USER &&
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

static bool IdGroupHas (const IdGroup *group, u64 id) {
    for (int i = 0; i < group->count; i++) {
        if (group->ids[i] == id) return true;
    }
    return false;
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

static bool ServerApplyImport (ServerPacket *packet, int clientIndex) {
    size_t size = packet->u.import.size;
    const unsigned char *data = packet->u.import.data;
    u64 userId = serverClients[clientIndex].userId;
    IdGroup beforeAssets = AssetIds ();
    IdGroup beforeObjects = ObjectIds ();
    int beforeMenuCount = MenuCount (0);
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

    if (ok) {
        IdGroup assets = AssetIds ();
        for (int i = 0; i < assets.count; i++) {
            if (!IdGroupHas (&beforeAssets, assets.ids[i])) {
                Asset *asset = EntityFind (assets.ids[i]);
                if (asset != NULL) ServerBroadcastAsset (asset);
            }
        }
        IdGroupUnload (&assets);

        IdGroup objects = ObjectIds ();
        for (int i = 0; i < objects.count; i++) {
            if (!IdGroupHas (&beforeObjects, objects.ids[i])) {
                Object *object = EntityFind (objects.ids[i]);
                if (object != NULL) ServerBroadcastObjectStateForAll (object);
            }
        }
        IdGroupUnload (&objects);

        if (MenuCount (0) != beforeMenuCount) NetServerBroadcastMenu (0);
    }
    IdGroupUnload (&beforeAssets);
    IdGroupUnload (&beforeObjects);
    TextBufferFree (&conflicts);
    return true;
}


static bool ServerImportAllowed (int clientIndex) {
    User *user = (User *)EntityFind (serverClients[clientIndex].userId);
    if (user != NULL && !user->canscript) {
        NetServerSendError (serverClients[clientIndex].userId,
                            "当前用户不允许导入");
        return false;
    }
    return true;
}


static bool ServerApplyFrame (ServerPacket *packet, int clientIndex) {
    switch (packet->type) {
        case NET_REQ_ASSET_ADD:
            if (!ServerImportAllowed (clientIndex)) return true;
            return ServerApplyAssetAdd (packet, clientIndex);
        case NET_ASSET_RENAME:
            return ServerApplyAssetRename (packet);
        case NET_ASSET_REPLACE:
            return ServerApplyAssetReplace (packet);
        case NET_ASSET_DELETE:
            return ServerApplyAssetDelete (packet, clientIndex);
        case NET_REQ_OBJECT_CREATE:
            return ServerApplyObjectCreate (packet, clientIndex);
        case NET_OBJECT_UPSERT:
            return ServerApplyObjectUpsert (packet, clientIndex);
        case NET_REQ_SCRIPT:
            return ServerApplyScript (packet, clientIndex);
        case NET_REQ_MENU:
            return ServerApplyMenu (packet, clientIndex);
        case NET_REQ_IMPORT:
            if (!ServerImportAllowed (clientIndex)) return true;
            return ServerApplyImport (packet, clientIndex);
        case NET_DRAG_UPDATE:
            return ServerApplyDragUpdate (packet, clientIndex);
        case NET_DRAG_END:
            return ServerApplyDragEnd (packet, clientIndex);
        case NET_OBJECT_DELETE:
            return ServerApplyObjectDelete (packet, clientIndex);
        case NET_CURSOR:
            return ServerApplyCursor (packet, clientIndex);
        case NET_LOCK:
            return ServerApplyLock (packet, clientIndex, false, false);
        case NET_UNLOCK:
            return ServerApplyLock (packet, clientIndex, true, false);
        case NET_UNLOCK_NOTIFY:
            return ServerApplyLock (packet, clientIndex, true, true);
        default:
            return false;
    }
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

        enum NetPacketType type = (enum NetPacketType)data[4];
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

        } else if (request->type == NET_HELLO) {
            ServerPacket packet = ServerPacketParse (
                request->type, request->payload, request->length);
            if (client->joined || packet.type == NET_NONE) {
                ServerBeginDisconnect (client);
            } else if (!ServerHandleHello (client, request->clientIndex, &packet)) {
                ServerBeginDisconnect (client);
            }
            ServerPacketFree (&packet);
        } else if (!client->joined) {
            ServerBeginDisconnect (client);
        } else {
            ServerPacket packet = ServerPacketParse (
                request->type, request->payload, request->length);
            if (packet.type == NET_NONE ||
                !ServerApplyFrame (&packet, request->clientIndex))
                ServerBeginDisconnect (client);
            ServerPacketFree (&packet);
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


static void ServerSendMessageType (u64 target, const char *message, enum NetPacketType type) {
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
    if (id != 0 && EntityKindOf (id) == ENTITY_KIND_USER) {
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
    ServerSendLockFailedToUser (ownerId, id);
    ServerBroadcastLockState (id, 0);
}

bool NetServerDeleteObject (u64 id) {
    Object *object = ServerObjectFind (id);
    if (object == NULL) return false;
    if (object->lockOwnerId != 0) NetServerReleaseObjectLock (id);

    if (!ObjectDelete (object)) return false;

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
