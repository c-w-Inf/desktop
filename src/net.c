#define _GNU_SOURCE

#include "net.h"
#include "ui.h"

#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <uv.h>

#include "assets.h"
#include "menu.h"
#include "object.h"
#include "entity.h"
#include "user.h"
#include "script.h"
#include "save.h"

/* 草稿协议固定值：不随协议改动递增。 */
#define NET_PROTOCOL_VERSION 1
#define NET_SNAPSHOT 1
#define NET_ASSET_ADD 2
#define NET_ASSET_RENAME 3
#define NET_ASSET_REPLACE 4
#define NET_ASSET_DELETE 5
#define NET_OBJECT_UPSERT 6
#define NET_OBJECT_DELETE 7
#define NET_HELLO 8
#define NET_REJECT 9
#define NET_USER_STATE 10
#define NET_USER_LEAVE 11
#define NET_CURSOR_STATE 12
#define NET_LOCK_STATE 13
#define NET_REQ_ASSET_ADD 20
#define NET_REQ_OBJECT_CREATE 21
#define NET_OBJECT_CREATED 22
#define NET_REQ_OBJECT_FRONT 23
#define NET_REQ_SCRIPT 24
#define NET_ERROR 25
#define NET_INFO 26
#define NET_MENU 27
#define NET_REQ_MENU 28
#define NET_REQ_EXPORT 29
#define NET_REQ_IMPORT 30
#define NET_DRAG_END 31
#define NET_LOCK_FAILED 32
#define NET_DRAG_UPDATE 33
#define NET_UNLOCK_NOTIFY 34
#define NET_CURSOR 14
#define NET_LOCK 15
#define NET_UNLOCK 16
#define NET_ASSET_DELETE_FAILED 17
#define NET_MESSAGE 18

#define SERVER_EVENT_DISCONNECT 255

#define NET_MAX_FRAME (256u * 1024u * 1024u)
#define NET_MAX_CLIENTS 16
#define NET_MAX_OUTPUT (512u * 1024u * 1024u)
#define NET_MAX_MESSAGE 256

typedef struct {
    unsigned char *data;
    size_t len;
    size_t cap;
} Buffer;

typedef struct {
    const unsigned char *data;
    size_t len;
    size_t pos;
} Reader;

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

static bool BufferReserve (Buffer *buffer, size_t extra) {
    if (extra > SIZE_MAX - buffer->len) return false;
    size_t needed = buffer->len + extra;
    if (needed <= buffer->cap) return true;

    size_t capacity = (buffer->cap == 0) ? 1024 : buffer->cap;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2) {
            capacity = needed;
            break;
        }
        capacity *= 2;
    }

    unsigned char *data = realloc (buffer->data, capacity);
    if (data == NULL) return false;

    buffer->data = data;
    buffer->cap = capacity;
    return true;
}

static bool BufferAppend (Buffer *buffer, const void *data, size_t size) {
    if (!BufferReserve (buffer, size)) return false;
    if (size > 0) memcpy (buffer->data + buffer->len, data, size);
    buffer->len += size;
    return true;
}

static bool BufferPutU8 (Buffer *buffer, uint8_t value) {
    return BufferAppend (buffer, &value, 1);
}

static bool BufferPutU16 (Buffer *buffer, uint16_t value) {
    unsigned char bytes[2] = {(unsigned char)(value & 0xFF), (unsigned char)(value >> 8)};
    return BufferAppend (buffer, bytes, 2);
}

static bool BufferPutU32 (Buffer *buffer, uint32_t value) {
    unsigned char bytes[4];
    for (int i = 0; i < 4; i++) bytes[i] = (unsigned char)((value >> (8 * i)) & 0xFF);
    return BufferAppend (buffer, bytes, 4);
}

static bool BufferPutU64 (Buffer *buffer, u64 value) {
    unsigned char bytes[8];
    for (int i = 0; i < 8; i++) bytes[i] = (unsigned char)((value >> (8 * i)) & 0xFF);
    return BufferAppend (buffer, bytes, 8);
}

static bool BufferPutF32 (Buffer *buffer, float value) {
    uint32_t bits;
    memcpy (&bits, &value, sizeof (bits));
    return BufferPutU32 (buffer, bits);
}

static bool BufferPutString (Buffer *buffer, const char *text) {
    size_t length = strlen (text);
    if (length > 0xFFFFu) length = 0xFFFFu;
    return BufferPutU16 (buffer, (uint16_t)length) &&
           BufferAppend (buffer, text, length);
}

static void BufferFree (Buffer *buffer) {
    free (buffer->data);
    buffer->data = NULL;
    buffer->len = 0;
    buffer->cap = 0;
}

static bool FrameMake (Buffer *frame, uint8_t type, const Buffer *payload) {
    bool ok = BufferPutU32 (frame, (uint32_t)(payload->len + 1)) &&
              BufferPutU8 (frame, type) &&
              BufferAppend (frame, payload->data, payload->len);
    return ok;
}

static bool ReaderAvailable (const Reader *reader, size_t size) {
    return reader->pos <= reader->len && size <= reader->len - reader->pos;
}

static bool ReaderBytes (Reader *reader, void *out, size_t size) {
    if (!ReaderAvailable (reader, size)) return false;
    if (out != NULL && size > 0) memcpy (out, reader->data + reader->pos, size);
    reader->pos += size;
    return true;
}

static bool ReaderU16 (Reader *reader, uint16_t *value) {
    unsigned char bytes[2];
    if (!ReaderBytes (reader, bytes, 2)) return false;
    *value = (uint16_t)(bytes[0] | ((uint16_t)bytes[1] << 8));
    return true;
}

static bool ReaderU32 (Reader *reader, uint32_t *value) {
    unsigned char bytes[4];
    if (!ReaderBytes (reader, bytes, 4)) return false;
    *value = (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
             ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
    return true;
}

static bool ReaderU64 (Reader *reader, u64 *value) {
    unsigned char bytes[8];
    if (!ReaderBytes (reader, bytes, 8)) return false;
    *value = 0;
    for (int i = 0; i < 8; i++) *value |= (u64)bytes[i] << (8 * i);
    return true;
}

static bool ReaderF32 (Reader *reader, float *value) {
    uint32_t bits;
    if (!ReaderU32 (reader, &bits)) return false;
    memcpy (value, &bits, sizeof (bits));
    return true;
}

static bool ReaderString (Reader *reader, char *text, size_t size) {
    uint16_t length;
    if (!ReaderU16 (reader, &length)) return false;
    if (!ReaderAvailable (reader, length) || size == 0) return false;

    size_t copy = (length < size - 1) ? length : size - 1;
    memcpy (text, reader->data + reader->pos, copy);
    text[copy] = '\0';
    reader->pos += length;
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

static bool FrameAssetAdd (Buffer *frame, u64 requestId, u64 requesterId,
                           u64 id, const char *name, const char *ext,
                           const unsigned char *data, uint32_t size) {
    Buffer payload = {0};
    bool ok = BufferPutU64 (&payload, requestId) &&
              BufferPutU64 (&payload, requesterId) &&
              BufferPutU64 (&payload, id) &&
              BufferPutString (&payload, name) &&
              BufferPutString (&payload, ext) &&
              BufferPutU32 (&payload, size) &&
              BufferAppend (&payload, data, (size_t)size) &&
              FrameMake (frame, NET_ASSET_ADD, &payload);
    BufferFree (&payload);
    return ok;
}

static bool FrameAssetRename (Buffer *frame, u64 id, const char *name) {
    Buffer payload = {0};
    bool ok = BufferPutU64 (&payload, id) &&
              BufferPutString (&payload, name) &&
              FrameMake (frame, NET_ASSET_RENAME, &payload);
    BufferFree (&payload);
    return ok;
}

static bool FrameAssetReplace (Buffer *frame, u64 id, const char *ext,
                               const unsigned char *data, uint32_t size) {
    Buffer payload = {0};
    bool ok = BufferPutU64 (&payload, id) &&
              BufferPutString (&payload, ext) &&
              BufferPutU32 (&payload, size) &&
              BufferAppend (&payload, data, (size_t)size) &&
              FrameMake (frame, NET_ASSET_REPLACE, &payload);
    BufferFree (&payload);
    return ok;
}

static bool FrameAssetDelete (Buffer *frame, u64 id) {
    Buffer payload = {0};
    bool ok = BufferPutU64 (&payload, id) &&
              FrameMake (frame, NET_ASSET_DELETE, &payload);
    BufferFree (&payload);
    return ok;
}

static bool FrameAssetDeleteFailed (Buffer *frame, u64 id) {
    Buffer payload = {0};
    bool ok = BufferPutU64 (&payload, id) &&
              FrameMake (frame, NET_ASSET_DELETE_FAILED, &payload);
    BufferFree (&payload);
    return ok;
}

static bool FrameMessage (Buffer *frame, uint8_t type, const char *message) {
    Buffer payload = {0};
    bool ok = BufferPutString (&payload, message) &&
              FrameMake (frame, type, &payload);
    BufferFree (&payload);
    return ok;
}

static bool FrameObjectUpsert (Buffer *frame, u64 id, const char *name,
                               float x, float y, int32_t layer, u64 timestamp,
                               bool hidden, uint32_t assetIndex,
                               const u64 *assetIds, uint32_t assetCount) {
    Buffer payload = {0};
    bool ok = BufferPutU64 (&payload, id) &&
              BufferPutString (&payload, name) &&
              BufferPutF32 (&payload, x) &&
              BufferPutF32 (&payload, y) &&
              BufferPutU32 (&payload, (uint32_t)layer) &&
              BufferPutU64 (&payload, timestamp) &&
              BufferPutU8 (&payload, hidden ? 1 : 0) &&
              BufferPutU32 (&payload, assetCount) &&
              BufferPutU32 (&payload, assetIndex);
    for (uint32_t i = 0; ok && i < assetCount; i++)
        ok = BufferPutU64 (&payload, assetIds[i]);
    if (ok) ok = FrameMake (frame, NET_OBJECT_UPSERT, &payload);
    BufferFree (&payload);
    return ok;
}

static bool FrameObjectCreated (Buffer *frame, u64 requestId,
                                u64 requesterId, u64 id,
                                const char *name, float x, float y, int32_t layer,
                                u64 timestamp, bool hidden, uint32_t assetIndex,
                                const u64 *assetIds, uint32_t assetCount) {
    Buffer payload = {0};
    bool ok = BufferPutU64 (&payload, requestId) &&
              BufferPutU64 (&payload, requesterId) &&
              BufferPutU64 (&payload, id) &&
              BufferPutString (&payload, name) &&
              BufferPutF32 (&payload, x) &&
              BufferPutF32 (&payload, y) &&
              BufferPutU32 (&payload, (uint32_t)layer) &&
              BufferPutU64 (&payload, timestamp) &&
              BufferPutU8 (&payload, hidden ? 1 : 0) &&
              BufferPutU32 (&payload, assetCount) &&
              BufferPutU32 (&payload, assetIndex);
    for (uint32_t i = 0; ok && i < assetCount; i++)
        ok = BufferPutU64 (&payload, assetIds[i]);
    if (ok) ok = FrameMake (frame, NET_OBJECT_CREATED, &payload);
    BufferFree (&payload);
    return ok;
}

static bool FrameMenuRequest (Buffer *frame, u64 id, uint32_t index,
                              const char *name) {
    Buffer payload = {0};
    bool ok = BufferPutU64 (&payload, id) &&
              BufferPutU32 (&payload, index) &&
              BufferPutString (&payload, name) &&
              FrameMake (frame, NET_REQ_MENU, &payload);
    BufferFree (&payload);
    return ok;
}

static bool FrameObjectDelete (Buffer *frame, u64 id) {
    Buffer payload = {0};
    bool ok = BufferPutU64 (&payload, id) &&
              FrameMake (frame, NET_OBJECT_DELETE, &payload);
    BufferFree (&payload);
    return ok;
}

static bool FrameMenu (Buffer *frame, u64 id) {
    Buffer payload = {0};
    int count = MenuCount (id);
    bool ok = BufferPutU64 (&payload, id) &&
              BufferPutU32 (&payload, (uint32_t)count);
    for (int i = 0; ok && i < count; i++) {
        const char *name = MenuName (id, i);
        ok = BufferPutString (&payload, name != NULL ? name : "");
    }
    if (ok) ok = FrameMake (frame, NET_MENU, &payload);
    BufferFree (&payload);
    return ok;
}

static bool FrameUserState (Buffer *frame, u64 id, const char *username,
                            uint32_t color, bool canscript,
                            bool hasCursor, float x, float y) {
    Buffer payload = {0};
    bool ok = BufferPutU64 (&payload, id) &&
              BufferPutString (&payload, username) &&
              BufferPutU32 (&payload, color) &&
              BufferPutU8 (&payload, canscript ? 1 : 0) &&
              BufferPutU8 (&payload, hasCursor ? 1 : 0) &&
              BufferPutF32 (&payload, x) &&
              BufferPutF32 (&payload, y) &&
              FrameMake (frame, NET_USER_STATE, &payload);
    BufferFree (&payload);
    return ok;
}

static bool FrameUserLeave (Buffer *frame, u64 id) {
    Buffer payload = {0};
    bool ok = BufferPutU64 (&payload, id) &&
              FrameMake (frame, NET_USER_LEAVE, &payload);
    BufferFree (&payload);
    return ok;
}

static bool FrameCursorState (Buffer *frame, u64 id, float x, float y) {
    Buffer payload = {0};
    bool ok = BufferPutU64 (&payload, id) &&
              BufferPutF32 (&payload, x) &&
              BufferPutF32 (&payload, y) &&
              FrameMake (frame, NET_CURSOR_STATE, &payload);
    BufferFree (&payload);
    return ok;
}

static bool FrameLockState (Buffer *frame, u64 id, u64 ownerId) {
    Buffer payload = {0};
    bool ok = BufferPutU64 (&payload, id) &&
              BufferPutU64 (&payload, ownerId) &&
              FrameMake (frame, NET_LOCK_STATE, &payload);
    BufferFree (&payload);
    return ok;
}

static bool FrameCursor (Buffer *frame, float x, float y) {
    Buffer payload = {0};
    bool ok = BufferPutF32 (&payload, x) &&
              BufferPutF32 (&payload, y) &&
              FrameMake (frame, NET_CURSOR, &payload);
    BufferFree (&payload);
    return ok;
}

static bool FrameObjectLock (Buffer *frame, uint8_t type, u64 id) {
    Buffer payload = {0};
    bool ok = BufferPutU64 (&payload, id) &&
              FrameMake (frame, type, &payload);
    BufferFree (&payload);
    return ok;
}

typedef struct {
    uv_tcp_t handle;
    uv_write_t writeRequest;
    Buffer in;
    Buffer out;
    Buffer writeData;
    bool active;
    bool writing;
    bool closing;
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
static volatile sig_atomic_t serverStopRequested;
static int pendingLockClient = -1;
static u64 pendingLockObjectId;
static bool pendingLockFailureSent;
static uv_loop_t *serverLoop;
static uv_timer_t serverTimer;
static uv_signal_t serverSignals[3];
static int serverSignalCount;
static uv_pipe_t serverControl;
static bool serverControlEnabled;
static bool serverControlOpen;

static void ServerStopSignal (uv_signal_t *handle, int number) {
    (void)handle;
    (void)number;
    serverStopRequested = 1;
}

static void ServerInstallStopHandlers (void) {
    uv_signal_init (serverLoop, &serverSignals[serverSignalCount]);
    uv_signal_start (&serverSignals[serverSignalCount], ServerStopSignal, SIGINT);
    serverSignalCount++;
#ifndef _WIN32
    uv_signal_init (serverLoop, &serverSignals[serverSignalCount]);
    uv_signal_start (&serverSignals[serverSignalCount], ServerStopSignal, SIGTERM);
    serverSignalCount++;
    uv_signal_init (serverLoop, &serverSignals[serverSignalCount]);
    uv_signal_start (&serverSignals[serverSignalCount], ServerStopSignal, SIGHUP);
    serverSignalCount++;
#else
    uv_signal_init (serverLoop, &serverSignals[serverSignalCount]);
    uv_signal_start (&serverSignals[serverSignalCount], ServerStopSignal, SIGBREAK);
    serverSignalCount++;
#endif
}

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


static void ServerClientWriteDone (uv_write_t *request, int status);
static void ServerClientClosed (uv_handle_t *handle);
static void ServerBeginDisconnect (ServerClient *client);

static void ServerClientFlush (ServerClient *client) {
    if (!client->active || client->writing || client->out.len == 0) return;

    client->writeData = client->out;
    memset (&client->out, 0, sizeof (client->out));
    client->writeRequest.data = client;
    client->writing = true;

    uv_buf_t buffer = uv_buf_init ((char *)client->writeData.data,
                                   (unsigned int)client->writeData.len);
    int result = uv_write (&client->writeRequest,
                           (uv_stream_t *)&client->handle, &buffer, 1,
                           ServerClientWriteDone);
    if (result != 0) {
        client->writing = false;
        BufferFree (&client->writeData);
        client->closing = true;
    }
}

static void ServerClientMaybeClose (ServerClient *client) {
    if (!client->active || !client->closing) return;
    ServerClientFlush (client);
    if (client->writing || client->out.len > 0) return;
    if (uv_is_closing ((uv_handle_t *)&client->handle)) return;

    uv_read_stop ((uv_stream_t *)&client->handle);
    BufferFree (&client->in);
    BufferFree (&client->out);
    BufferFree (&client->writeData);
    uv_close ((uv_handle_t *)&client->handle, ServerClientClosed);
}

static void ServerClientWriteDone (uv_write_t *request, int status) {
    ServerClient *client = request->data;
    client->writing = false;
    BufferFree (&client->writeData);
    if (status < 0 && !client->closing) {
        if (client->joined) ServerBeginDisconnect (client);
        else client->closing = true;
    }
    if (!client->closing) ServerClientFlush (client);
    ServerClientMaybeClose (client);
}

static void ServerClientClosed (uv_handle_t *handle) {
    ServerClient *client = handle->data;
    client->active = false;
    client->closing = false;
    client->joined = false;
    client->inputClosed = false;
    client->disconnectQueued = false;
    client->helloQueued = false;
    client->userId = 0;
    client->username[0] = '\0';
    BufferFree (&client->in);
    BufferFree (&client->out);
    BufferFree (&client->writeData);
}

static void ServerClientEnqueue (ServerClient *client, const unsigned char *data, size_t size) {
    if (!client->active) return;
    if (client->out.len + size > NET_MAX_OUTPUT ||
        !BufferAppend (&client->out, data, size)) {
        client->closing = true;
    } else {
        ServerClientFlush (client);
    }
    ServerClientMaybeClose (client);
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
    uv_read_stop ((uv_stream_t *)&client->handle);
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

static bool ServerApplyAssetAdd (Reader *reader, int clientIndex) {
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
    return true;
}

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
    if (!ReaderU64 (reader, &id) || reader->pos != reader->len) return false;
    Object *object = ServerObjectFind (id);
    if (object == NULL) return true;
    u64 userId = serverClients[clientIndex].userId;
    if (object->lockOwnerId == userId)
        ServerRunObjectEvent (id, "ondrag", userId);
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

    if (object->lockOwnerId != 0 && object->lockOwnerId != userId) {
        ServerSendLockState (clientIndex, id, object->lockOwnerId);
        return true;
    }
    if (object->lockOwnerId == userId && userId != 0) {
        ServerSendLockState (clientIndex, id, object->lockOwnerId);
        return true;
    }

    ServerReleaseLocks (userId, false);
    object = ServerObjectFind (id);
    if (object == NULL) return true;
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

static bool ServerApplyImport (Reader *reader, int clientIndex) {
    char path[1024];
    if (!ReaderString (reader, path, sizeof (path)) || path[0] == '\0' ||
        reader->pos != reader->len)
        return false;

    u64 userId = serverClients[clientIndex].userId;
    TextBuffer conflicts = {0};
    bool ok = SaveMerge (path, serverScriptEnv, ServerImportAllocateId, NULL,
                         &conflicts);
    if (!ok) {
        fprintf (stderr, "增量导入失败: %s\n", path);
        NetServerSendError (userId, "增量导入失败");
    } else if (conflicts.length > 0) {
        fprintf (stderr, "增量导入冲突（未替换）:\n%s", conflicts.data);
        SendImportConflicts (userId, &conflicts);
    } else {
        fprintf (stderr, "增量导入完成: %s\n", path);
        NetServerSendInfo (userId, "增量导入完成");
    }

    for (int i = 0; i < NET_MAX_CLIENTS; i++) {
        if (serverClients[i].joined) ServerSendSnapshot (i);
    }
    TextBufferFree (&conflicts);
    return true;
}

static bool ServerApplyExport (Reader *reader, int clientIndex) {
    char path[1024];
    if (!ReaderString (reader, path, sizeof (path)) || path[0] == '\0' ||
        reader->pos != reader->len)
        return false;

    u64 userId = serverClients[clientIndex].userId;
    if (!SaveWrite (path, serverScriptEnv)) {
        NetServerSendError (userId, "导出存档失败");
    } else {
        NetServerSendInfo (userId, "导出存档已完成");
    }
    return true;
}

static bool ServerApplyFrame (uint8_t type, const unsigned char *data, size_t length,
                              int clientIndex) {
    Reader reader = {data, length, 0};
    bool ok = false;

    switch (type) {
        case NET_REQ_ASSET_ADD:
            ok = ServerApplyAssetAdd (&reader, clientIndex);
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
        case NET_REQ_EXPORT:
            ok = ServerApplyExport (&reader, clientIndex);
            break;
        case NET_REQ_IMPORT:
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
static uv_tcp_t serverListener;

static void NetAllocBuffer (uv_handle_t *handle, size_t suggested,
                               uv_buf_t *buffer) {
    (void)handle;
    if (suggested > 65536) suggested = 65536;
    buffer->base = malloc (suggested);
    buffer->len = buffer->base != NULL ? suggested : 0;
}

static void ServerClientRead (uv_stream_t *stream, ssize_t nread,
                              const uv_buf_t *buffer) {
    ServerClient *client = stream->data;
    int clientIndex = (int)(client - serverClients);
    if (nread < 0) {
        free (buffer->base);
        ServerCloseInput (clientIndex);
        ServerQueueDisconnect (clientIndex);
        ServerProcessRequests ();
        return;
    }
    if (nread == 0) {
        free (buffer->base);
        return;
    }
    if (client->in.len + (size_t)nread > NET_MAX_FRAME + 4 ||
        !BufferAppend (&client->in, buffer->base, (size_t)nread)) {
        free (buffer->base);
        ServerCloseInput (clientIndex);
        ServerQueueDisconnect (clientIndex);
        ServerProcessRequests ();
        return;
    }
    free (buffer->base);
    ServerReadFrames (client, clientIndex);
    if (client->inputClosed && !client->disconnectQueued)
        ServerQueueDisconnect (clientIndex);
    ServerProcessRequests ();
}

static void ServerClientAccepted (uv_stream_t *server, int status) {
    (void)server;
    if (status < 0) return;

    int slot = -1;
    for (int i = 0; i < NET_MAX_CLIENTS; i++) {
        if (!serverClients[i].active) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        uv_tcp_t rejected;
        uv_tcp_init (serverLoop, &rejected);
        if (uv_accept ((uv_stream_t *)&serverListener,
                       (uv_stream_t *)&rejected) == 0)
            uv_close ((uv_handle_t *)&rejected, NULL);
        return;
    }

    ServerClient *client = &serverClients[slot];
    uv_tcp_init (serverLoop, &client->handle);
    client->handle.data = client;
    client->active = true;
    client->closing = false;
    client->joined = false;
    client->inputClosed = false;
    client->disconnectQueued = false;
    client->helloQueued = false;
    client->userId = 0;
    client->username[0] = '\0';
    memset (&client->in, 0, sizeof (client->in));
    memset (&client->out, 0, sizeof (client->out));
    memset (&client->writeData, 0, sizeof (client->writeData));
    client->writing = false;

    if (uv_accept ((uv_stream_t *)&serverListener,
                   (uv_stream_t *)&client->handle) != 0 ||
        uv_read_start ((uv_stream_t *)&client->handle, NetAllocBuffer,
                       ServerClientRead) != 0) {
        client->closing = true;
        ServerClientMaybeClose (client);
        return;
    }
    printf ("client connected\n");
    fflush (stdout);
}

void NetServerEnableStdinControl (void) {
    serverControlEnabled = true;
}

static void ServerControlBuffer (uv_handle_t *handle, size_t suggested,
                                 uv_buf_t *buffer) {
    (void)handle;
    if (suggested > 4096) suggested = 4096;
    buffer->base = malloc (suggested);
    buffer->len = buffer->base != NULL ? suggested : 0;
}

static void ServerControlRead (uv_stream_t *stream, ssize_t nread,
                               const uv_buf_t *buffer) {
    (void)stream;
    if (nread > 0 && buffer->base != NULL) {
        size_t length = (size_t)nread;
        for (size_t i = 0; i + 4 <= length; i++) {
            if (memcmp (buffer->base + i, "stop", 4) == 0) {
                serverStopRequested = 1;
                break;
            }
        }
    }
    free (buffer->base);
}

static void ServerTimerTick (uv_timer_t *timer) {
    (void)timer;
    if (serverStopRequested) {
        uv_stop (serverLoop);
        return;
    }
    time_t now = time (NULL);
    if (now - serverLastSave >= SAVE_INTERVAL_SECONDS) {
        serverLastSave = now;
        ServerSave ();
    }
}

static void ServerClientForceClose (ServerClient *client) {
    if (!client->active || uv_is_closing ((uv_handle_t *)&client->handle)) return;
    client->closing = true;
    uv_read_stop ((uv_stream_t *)&client->handle);
    uv_close ((uv_handle_t *)&client->handle, ServerClientClosed);
}

int NetRunServer (int port, const char *bindAddress,
                  const char *loadPath, const char *savePath) {
    uv_loop_t loop;
    if (uv_loop_init (&loop) != 0) return 1;
    serverLoop = &loop;
    serverStopRequested = 0;
    serverLastSave = time (NULL);
    serverSignalCount = 0;
    serverSavePath[0] = '\0';
    if (savePath != NULL && savePath[0] != '\0')
        snprintf (serverSavePath, sizeof (serverSavePath), "%s", savePath);

    for (int i = 0; i < NET_MAX_CLIENTS; i++) {
        serverClients[i].active = false;
        serverClients[i].closing = false;
    }
    if (uv_tcp_init (&loop, &serverListener) != 0) {
        uv_loop_close (&loop);
        serverLoop = NULL;
        return 1;
    }

    struct sockaddr_in address;
    const char *bind = bindAddress != NULL && bindAddress[0] != '\0'
                           ? bindAddress : "0.0.0.0";
    int addressResult = uv_ip4_addr (bind, port, &address);
    if (addressResult != 0) {
        fprintf (stderr, "无效的绑定地址: %s (%s)\n", bind,
                 uv_strerror (addressResult));
        uv_close ((uv_handle_t *)&serverListener, NULL);
        uv_run (&loop, UV_RUN_DEFAULT);
        uv_loop_close (&loop);
        serverLoop = NULL;
        return 1;
    }
    if (uv_tcp_bind (&serverListener, (const struct sockaddr *)&address, 0) != 0 ||
        uv_listen ((uv_stream_t *)&serverListener, NET_MAX_CLIENTS,
                   ServerClientAccepted) != 0) {
        fprintf (stderr, "无法监听端口 %d\n", port);
        uv_close ((uv_handle_t *)&serverListener, NULL);
        uv_run (&loop, UV_RUN_DEFAULT);
        uv_loop_close (&loop);
        serverLoop = NULL;
        return 1;
    }

    serverScriptEnv = ScriptEnvCreate ();
    if (serverScriptEnv == NULL) {
        fprintf (stderr, "创建脚本环境失败\n");
        uv_close ((uv_handle_t *)&serverListener, NULL);
        uv_run (&loop, UV_RUN_DEFAULT);
        uv_loop_close (&loop);
        serverLoop = NULL;
        return 1;
    }
    if (loadPath != NULL && loadPath[0] != '\0') {
        u64 nextId = 1;
        if (!SaveRead (loadPath, serverScriptEnv, &nextId)) {
            fprintf (stderr, "导入存档失败: %s\n", loadPath);
            ScriptEnvDestroy (serverScriptEnv);
            serverScriptEnv = NULL;
            uv_close ((uv_handle_t *)&serverListener, NULL);
            uv_run (&loop, UV_RUN_DEFAULT);
            uv_loop_close (&loop);
            serverLoop = NULL;
            return 1;
        }
        ServerSetNextEntityId (nextId);
    }

    ServerInstallStopHandlers ();
    uv_timer_init (&loop, &serverTimer);
    uv_timer_start (&serverTimer, ServerTimerTick, 500, 500);
    if (serverControlEnabled) {
        uv_pipe_init (&loop, &serverControl, 0);
        if (uv_pipe_open (&serverControl, 0) == 0 &&
            uv_read_start ((uv_stream_t *)&serverControl, ServerControlBuffer,
                           ServerControlRead) == 0) {
            serverControlOpen = true;
        }
    }
    printf ("Image Canvas server listening on port %d\n", port);
    fflush (stdout);
    uv_run (&loop, UV_RUN_DEFAULT);

    ServerSave ();
    uv_close ((uv_handle_t *)&serverTimer, NULL);
    for (int i = 0; i < serverSignalCount; i++)
        uv_close ((uv_handle_t *)&serverSignals[i], NULL);
    uv_close ((uv_handle_t *)&serverListener, NULL);
    if (serverControlOpen) {
        uv_close ((uv_handle_t *)&serverControl, NULL);
        serverControlOpen = false;
    }
    for (int i = 0; i < NET_MAX_CLIENTS; i++)
        ServerClientForceClose (&serverClients[i]);
    uv_run (&loop, UV_RUN_DEFAULT);
    uv_loop_close (&loop);
    serverLoop = NULL;

    if (serverScriptEnv != NULL) ScriptEnvDestroy (serverScriptEnv);
    serverScriptEnv = NULL;
    return 0;
}

static uv_loop_t clientLoop;
static bool clientLoopInitialized;
static uv_tcp_t clientHandle;
static uv_connect_t clientConnectRequest;
static bool clientHandleInitialized;
static bool clientActive;
static bool clientClosing;
static bool clientConnectDone;
static bool clientConnectOk;
static Buffer clientIn;
static Buffer clientOut;
static Buffer clientWriteData;
static uv_write_t clientWriteRequest;
static bool clientWriting;
static int clientPendingFlags;
static u64 nextRequestId = 1;
static u64 waitingRequestId;
static u64 responseAssetId;
static u64 responseObjectId;
static bool responseReady;
static u64 clientUserId;
static u64 pendingLockId;
static bool lockRequestFailed;
static u64 lockLostId;
static u64 clientSceneVersion;

static bool ParseClientFrames (int *flags);
static bool WaitForResponse (const char *what, char *error, int errorSize);
static void ClientCloseNow (void);
static void ClientFlush (void);
static void ClientOnWriteDone (uv_write_t *request, int status);
static void ClientOnClosed (uv_handle_t *handle);

static void ClientPump (int milliseconds) {
    int elapsed = 0;
    do {
        if (!clientLoopInitialized) return;
        uv_run (&clientLoop, UV_RUN_NOWAIT);
        if (milliseconds <= 0) break;
        uv_sleep (1);
        elapsed++;
    } while (elapsed < milliseconds);
}

static void ClientOnRead (uv_stream_t *stream, ssize_t nread,
                          const uv_buf_t *buffer) {
    (void)stream;
    if (nread < 0) {
        free (buffer->base);
        ClientCloseNow ();
        return;
    }
    if (nread == 0) {
        free (buffer->base);
        return;
    }
    if (clientIn.len + (size_t)nread > NET_MAX_FRAME + 4 ||
        !BufferAppend (&clientIn, buffer->base, (size_t)nread)) {
        free (buffer->base);
        ClientCloseNow ();
        return;
    }
    free (buffer->base);

    int flags = NET_UPDATE_NONE;
    if (!ParseClientFrames (&flags)) {
        ClientCloseNow ();
        return;
    }
    clientPendingFlags |= flags;
    if (flags & NET_UPDATE_REJECTED) ClientCloseNow ();
}

static void ClientCloseNow (void);

static void ClientOnWriteDone (uv_write_t *request, int status) {
    (void)request;
    clientWriting = false;
    BufferFree (&clientWriteData);
    if (status < 0) {
        ClientCloseNow ();
        return;
    }
    ClientFlush ();
}

static void ClientOnClosed (uv_handle_t *handle) {
    (void)handle;
    clientHandleInitialized = false;
    clientActive = false;
    clientClosing = false;
    clientUserId = 0;
    pendingLockId = 0;
    lockRequestFailed = false;
    BufferFree (&clientIn);
    BufferFree (&clientOut);
    BufferFree (&clientWriteData);
}

static void ClientFlush (void) {
    if (!clientActive || clientClosing || clientWriting || clientOut.len == 0)
        return;

    clientWriteData = clientOut;
    memset (&clientOut, 0, sizeof (clientOut));
    clientWriteRequest.data = NULL;
    clientWriting = true;
    uv_buf_t buffer = uv_buf_init ((char *)clientWriteData.data,
                                   (unsigned int)clientWriteData.len);
    int result = uv_write (&clientWriteRequest, (uv_stream_t *)&clientHandle,
                           &buffer, 1, ClientOnWriteDone);
    if (result != 0) {
        clientWriting = false;
        BufferFree (&clientWriteData);
        ClientCloseNow ();
    }
}

static void ClientCloseNow (void) {
    if (clientHandleInitialized && !uv_is_closing ((uv_handle_t *)&clientHandle)) {
        clientClosing = true;
        uv_read_stop ((uv_stream_t *)&clientHandle);
        uv_close ((uv_handle_t *)&clientHandle, ClientOnClosed);
    } else if (!clientHandleInitialized) {
        clientActive = false;
        clientClosing = false;
        BufferFree (&clientIn);
        BufferFree (&clientOut);
        BufferFree (&clientWriteData);
    }
    clientUserId = 0;
    pendingLockId = 0;
    lockRequestFailed = false;
}

static void ClientWaitClosed (int milliseconds) {
    for (int elapsed = 0; elapsed < milliseconds && clientHandleInitialized;
         elapsed++) {
        uv_run (&clientLoop, UV_RUN_NOWAIT);
        uv_sleep (1);
    }
}

u64 NetClientUserId (void) {
    return clientUserId;
}

u64 NetClientLockLostId (void) {
    return lockLostId;
}

u64 NetClientSceneVersion (void) {
    return clientSceneVersion;
}

static bool ClientSendFrame (Buffer *frame) {
    if (!clientActive || clientClosing || clientOut.len + frame->len > NET_MAX_OUTPUT) {
        BufferFree (frame);
        return false;
    }
    bool ok = BufferAppend (&clientOut, frame->data, frame->len);
    BufferFree (frame);
    if (ok) ClientFlush ();
    return ok;
}

typedef struct {
    bool done;
    bool ok;
    struct sockaddr_in address;
} ClientResolve;

static void ClientResolved (uv_getaddrinfo_t *request, int status,
                            struct addrinfo *result) {
    ClientResolve *resolve = request->data;
    resolve->done = true;
    resolve->ok = status == 0 && result != NULL &&
                  result->ai_family == AF_INET &&
                  result->ai_addrlen >= sizeof (struct sockaddr_in);
    if (resolve->ok)
        memcpy (&resolve->address, result->ai_addr, sizeof (resolve->address));
    uv_freeaddrinfo (result);
}

static void ClientConnected (uv_connect_t *request, int status) {
    (void)request;
    clientConnectDone = true;
    clientConnectOk = status == 0;
}

bool NetClientConnect (const char *host, int port, const char *username) {
    if (username == NULL || username[0] == '\0') return false;
    ClientCloseNow ();
    ClientWaitClosed (1000);
    UserUnload ();
    clientPendingFlags = NET_UPDATE_NONE;
    nextRequestId = 1;
    waitingRequestId = 0;
    responseReady = false;

    if (!clientLoopInitialized) {
        if (uv_loop_init (&clientLoop) != 0) return false;
        clientLoopInitialized = true;
    }

    struct addrinfo hints;
    memset (&hints, 0, sizeof (hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    ClientResolve resolve;
    memset (&resolve, 0, sizeof (resolve));
    uv_getaddrinfo_t resolveRequest;
    resolveRequest.data = &resolve;
    if (uv_getaddrinfo (&clientLoop, &resolveRequest, ClientResolved, host,
                        NULL, &hints) != 0)
        return false;
    while (!resolve.done) uv_run (&clientLoop, UV_RUN_ONCE);
    if (!resolve.ok) return false;
    resolve.address.sin_port = htons ((uint16_t)port);

    if (uv_tcp_init (&clientLoop, &clientHandle) != 0) return false;
    clientHandleInitialized = true;
    clientConnectDone = false;
    clientConnectOk = false;
    if (uv_tcp_connect (&clientConnectRequest, &clientHandle,
                        (const struct sockaddr *)&resolve.address,
                        ClientConnected) != 0) {
        ClientCloseNow ();
        ClientWaitClosed (1000);
        return false;
    }
    while (!clientConnectDone) uv_run (&clientLoop, UV_RUN_ONCE);
    if (!clientConnectOk) {
        ClientCloseNow ();
        ClientWaitClosed (1000);
        return false;
    }

    clientActive = true;
    clientClosing = false;
    clientUserId = 0;
    if (uv_read_start ((uv_stream_t *)&clientHandle, NetAllocBuffer,
                       ClientOnRead) != 0) {
        ClientCloseNow ();
        ClientWaitClosed (1000);
        return false;
    }

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
    return clientHandleInitialized && clientActive && !clientClosing;
}

void NetClientDisconnect (void) {
    ClientCloseNow ();
}
Object *NetCreateObject (Vector2 center) {
    u64 requestId = nextRequestId++;
    waitingRequestId = requestId;
    responseReady = false;
    responseAssetId = 0;
    responseObjectId = 0;

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
        waitingRequestId = 0;
        return NULL;
    }
    if (!ClientSendFrame (&frame)) {
        waitingRequestId = 0;
        return NULL;
    }
    if (!WaitForResponse ("等待物件响应", NULL, 0)) {
        waitingRequestId = 0;
        return NULL;
    }
    waitingRequestId = 0;
    return EntityFind (responseObjectId);
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

static bool WaitForResponse (const char *what, char *error, int errorSize) {
    int elapsed = 0;
    while (elapsed < 30000) {
        if (responseReady) return true;
        if (!NetClientConnected ()) {
            if (error != NULL && errorSize > 0)
                snprintf (error, (size_t)errorSize, "服务器连接已断开");
            return false;
        }
        ClientPump (50);
        elapsed += 50;
    }
    if (error != NULL && errorSize > 0)
        snprintf (error, (size_t)errorSize, "%s超时", what);
    return false;
}

Asset *NetCreateAsset (const char *path) {
    int size = 0;
    unsigned char *data = LoadFileData (path, &size);
    if (data == NULL || size <= 0) {
        UnloadFileData (data);
        return NULL;
    }

    const char *ext = GetFileExtension (path);
    Image image = LoadImageFromMemory (ext, data, size);
    if (image.data == NULL) {
        UnloadFileData (data);
        return NULL;
    }
    UnloadImage (image);

    u64 requestId = nextRequestId++;
    waitingRequestId = requestId;
    responseReady = false;
    responseAssetId = 0;
    responseObjectId = 0;

    Buffer payload = {0};
    Buffer frame = {0};
    bool ok = BufferPutU64 (&payload, requestId) &&
              BufferPutString (&payload, "asset") &&
              BufferPutString (&payload, ext) &&
              BufferPutU32 (&payload, (uint32_t)size) &&
              BufferAppend (&payload, data, (size_t)size) &&
              FrameMake (&frame, NET_REQ_ASSET_ADD, &payload);
    BufferFree (&payload);
    UnloadFileData (data);
    if (!ok) {
        BufferFree (&frame);
        waitingRequestId = 0;
        return NULL;
    }
    if (!ClientSendFrame (&frame)) {
        waitingRequestId = 0;
        return NULL;
    }
    if (!WaitForResponse ("等待素材响应", NULL, 0)) {
        waitingRequestId = 0;
        return NULL;
    }
    waitingRequestId = 0;
    return EntityFind (responseAssetId);
}

bool NetPublishCursor (Vector2 worldPos) {
    Buffer frame = {0};
    if (!FrameCursor (&frame, worldPos.x, worldPos.y)) {
        BufferFree (&frame);
        return false;
    }
    return ClientSendFrame (&frame);
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

bool NetLockObject (u64 id) {
    u64 selfId = NetClientUserId ();
    if (selfId == 0 || id == 0) return false;

    Object *object = EntityFind (id);
    if (object == NULL) return false;
    if (object->lockOwnerId == selfId) return true;
    if (object->lockOwnerId != 0) return false;

    Buffer frame = {0};
    if (!FrameObjectLock (&frame, NET_LOCK, id)) {
        BufferFree (&frame);
        return false;
    }
    pendingLockId = id;
    lockRequestFailed = false;
    if (!ClientSendFrame (&frame)) {
        pendingLockId = 0;
        return false;
    }

    int elapsed = 0;
    while (elapsed < 2000) {
        if (lockRequestFailed) {
            pendingLockId = 0;
            return false;
        }
        object = EntityFind (id);
        if (object == NULL) {
            pendingLockId = 0;
            return false;
        }
        if (object->lockOwnerId == selfId) {
            pendingLockId = 0;
            return true;
        }
        if (object->lockOwnerId != 0) {
            pendingLockId = 0;
            return false;
        }
        if (!NetClientConnected ()) {
            pendingLockId = 0;
            return false;
        }

        ClientPump (10);
        elapsed += 10;
    }
    pendingLockId = 0;
    return false;
}

static bool SendUnlockObject (u64 id, uint8_t type, bool clearLocal) {
    if (clearLocal) {
        Object *object = EntityFind (id);
        if (object != NULL && object->lockOwnerId == NetClientUserId ())
            object->lockOwnerId = 0;
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

bool NetUnlockObjectAndWait (u64 id) {
    u64 selfId = NetClientUserId ();
    Object *object = EntityFind (id);
    if (selfId == 0 || id == 0 || object == NULL ||
        object->lockOwnerId != selfId)
        return true;

    if (!SendUnlockObject (id, NET_UNLOCK_NOTIFY, false)) return false;

    int elapsed = 0;
    while (elapsed < 2000) {
        if (!NetClientConnected ()) return false;
        object = EntityFind (id);
        if (object == NULL || object->lockOwnerId != selfId) return true;

        ClientPump (10);
        elapsed += 10;
    }
    return false;
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

    if (requestId != 0 && requestId == waitingRequestId && existing != NULL &&
        requesterId == NetClientUserId ()) {
        responseAssetId = id;
        responseReady = true;
    }
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

static bool ApplyObjectCreatedDelta (Reader *reader) {
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

    if (requestId != 0 && requestId == waitingRequestId && object != NULL &&
        requesterId == NetClientUserId ()) {
        responseObjectId = id;
        responseReady = true;
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

static bool ApplyLockDelta (Reader *reader) {
    u64 id;
    u64 ownerId;
    if (!ReaderU64 (reader, &id) || !ReaderU64 (reader, &ownerId))
        return false;

    Object *object = EntityFind (id);
    if (object != NULL) object->lockOwnerId = ownerId;
    return true;
}

static bool ApplyLockFailedDelta (Reader *reader, int *flags) {
    u64 id;
    if (!ReaderU64 (reader, &id) || reader->pos != reader->len) return false;
    if (id == pendingLockId) {
        lockRequestFailed = true;
    } else {
        lockLostId = id;
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
            ok = ApplyObjectCreatedDelta (&reader);
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
            ok = ApplyLockDelta (&reader);
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
    if (clientLoopInitialized) ClientPump (0);
    int flags = clientPendingFlags;
    clientPendingFlags = NET_UPDATE_NONE;
    if (flags & NET_UPDATE_REJECTED) ClientCloseNow ();
    return flags;
}
