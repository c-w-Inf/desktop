#include "net.h"

#include <stdlib.h>
#include <string.h>

#include "net_common.h"
#include "menu.h"

static char *ReadAllocString (Reader *reader) {
    char text[256];
    if (!ReaderString (reader, text, sizeof (text))) return NULL;
    return StrDup (text);
}

static bool ReadAllocBytes (Reader *reader, size_t size,
                            unsigned char **data) {
    if (size == 0 || size > NET_MAX_FRAME || !ReaderAvailable (reader, size))
        return false;
    unsigned char *copy = malloc (size);
    if (copy == NULL) return false;
    memcpy (copy, reader->data + reader->pos, size);
    reader->pos += size;
    *data = copy;
    return true;
}

static bool ReadObject (Reader *reader, ClientObjectPacket *object) {
    object->fields = OBJECT_FIELD_ALL & ~OBJECT_FIELD_LOCK_OWNER;
    if (!ReaderU64 (reader, &object->id) ||
        (object->name = ReadAllocString (reader)) == NULL ||
        !ReaderF32 (reader, &object->x) ||
        !ReaderF32 (reader, &object->y) ||
        !ReaderU32 (reader, &object->layer) ||
        !ReaderU64 (reader, &object->timestamp) ||
        !ReaderBytes (reader, &object->hidden, 1) ||
        !ReaderU32 (reader, &object->assetCount) ||
        !ReaderU32 (reader, &object->assetIndex) ||
        object->assetCount > 100000 ||
        !ReaderAvailable (reader, (size_t)object->assetCount * 8))
        return false;

    if (object->assetCount > 0) {
        object->assetIds = malloc (object->assetCount * sizeof (u64));
        if (object->assetIds == NULL) return false;
        for (uint32_t i = 0; i < object->assetCount; i++) {
            if (!ReaderU64 (reader, &object->assetIds[i])) return false;
        }
    }
    return true;
}

static bool ReadObjectUpsert (Reader *reader, ClientObjectPacket *object) {
    if (!ReaderU32 (reader, &object->fields) ||
        !ReaderU64 (reader, &object->id) ||
        (object->fields & ~OBJECT_FIELD_ALL) != 0)
        return false;

    if ((object->fields & OBJECT_FIELD_NAME) &&
        (object->name = ReadAllocString (reader)) == NULL)
        return false;
    if ((object->fields & OBJECT_FIELD_POSITION) &&
        (!ReaderF32 (reader, &object->x) || !ReaderF32 (reader, &object->y)))
        return false;
    if ((object->fields & OBJECT_FIELD_LAYER) &&
        !ReaderU32 (reader, &object->layer))
        return false;
    if ((object->fields & OBJECT_FIELD_TIMESTAMP) &&
        !ReaderU64 (reader, &object->timestamp))
        return false;
    if ((object->fields & OBJECT_FIELD_HIDDEN) &&
        !ReaderBytes (reader, &object->hidden, 1))
        return false;
    if (object->fields & OBJECT_FIELD_ASSETS) {
        if (!ReaderU32 (reader, &object->assetCount) ||
            object->assetCount > 100000 ||
            !ReaderAvailable (reader, (size_t)object->assetCount * 8))
            return false;
        if (object->assetCount > 0) {
            object->assetIds = malloc (object->assetCount * sizeof (u64));
            if (object->assetIds == NULL) return false;
            for (uint32_t i = 0; i < object->assetCount; i++) {
                if (!ReaderU64 (reader, &object->assetIds[i])) return false;
            }
        }
    }
    if ((object->fields & OBJECT_FIELD_ASSET_INDEX) &&
        !ReaderU32 (reader, &object->assetIndex))
        return false;
    if ((object->fields & OBJECT_FIELD_LOCK_OWNER) &&
        !ReaderU64 (reader, &object->lockOwnerId))
        return false;
    return true;
}

static bool ParsePacket (ClientPacket *packet, Reader *reader) {
    switch (packet->type) {
    case NET_HELLO:
        return ReaderU64 (reader, &packet->u.hello.selfId);
    case NET_HEARTBEAT:
        return true;
    case NET_ASSET_ADD: {
        ClientAssetAddPacket *p = &packet->u.assetAdd;
        uint32_t size;
        if (!ReaderU64 (reader, &p->requestId) ||
            !ReaderU64 (reader, &p->requesterId) ||
            !ReaderU64 (reader, &p->id) ||
            (p->name = ReadAllocString (reader)) == NULL ||
            (p->ext = ReadAllocString (reader)) == NULL ||
            !ReaderU32 (reader, &size) ||
            !ReadAllocBytes (reader, size, &p->data))
            return false;
        p->size = size;
        return true;
    }
    case NET_ASSET_RENAME: {
        ClientAssetRenamePacket *p = &packet->u.assetRename;
        return ReaderU64 (reader, &p->id) &&
               (p->name = ReadAllocString (reader)) != NULL;
    }
    case NET_ASSET_REPLACE: {
        ClientAssetReplacePacket *p = &packet->u.assetReplace;
        uint32_t size;
        if (!ReaderU64 (reader, &p->id) ||
            (p->ext = ReadAllocString (reader)) == NULL ||
            !ReaderU32 (reader, &size) ||
            !ReadAllocBytes (reader, size, &p->data))
            return false;
        p->size = size;
        return true;
    }
    case NET_ASSET_DELETE:
    case NET_OBJECT_DELETE:
    case NET_USER_LEAVE:
    case NET_LOCK_FAILED:
        return ReaderU64 (reader, &packet->u.id.id);
    case NET_OBJECT_CREATED: {
        ClientObjectCreatedPacket *p = &packet->u.objectCreated;
        return ReaderU64 (reader, &p->requestId) &&
               ReaderU64 (reader, &p->requesterId) &&
               ReadObject (reader, &p->object);
    }
    case NET_OBJECT_UPSERT:
        return ReadObjectUpsert (reader, &packet->u.object);
    case NET_USER_STATE: {
        ClientUserPacket *p = &packet->u.user;
        return ReaderU64 (reader, &p->id) &&
               (p->username = ReadAllocString (reader)) != NULL &&
               ReaderU32 (reader, &p->color) &&
               ReaderBytes (reader, &p->canscript, 1) &&
               ReaderBytes (reader, &p->hasCursor, 1) &&
               ReaderF32 (reader, &p->x) &&
               ReaderF32 (reader, &p->y);
    }
    case NET_CURSOR_STATE: {
        ClientCursorPacket *p = &packet->u.cursor;
        return ReaderU64 (reader, &p->id) &&
               ReaderF32 (reader, &p->x) &&
               ReaderF32 (reader, &p->y);
    }
    case NET_MENU: {
        ClientMenuPacket *p = &packet->u.menu;
        if (!ReaderU64 (reader, &p->id) ||
            !ReaderU32 (reader, &p->count) || p->count > 100000)
            return false;
        if (p->count > 0) {
            p->names = calloc (p->count, sizeof (char *));
            if (p->names == NULL) return false;
            for (uint32_t i = 0; i < p->count; i++) {
                if ((p->names[i] = ReadAllocString (reader)) == NULL)
                    return false;
            }
        }
        return true;
    }
    case NET_MESSAGE:
    case NET_ERROR:
    case NET_INFO:
        return (packet->u.message.text = ReadAllocString (reader)) != NULL;
    default:
        return false;
    }
}

ClientPacket ClientPacketParse (enum NetPacketType type,
                                const unsigned char *data, size_t length) {
    ClientPacket packet = {0};
    packet.type = type;

    Reader reader = {data, length, 0};
    if (!ParsePacket (&packet, &reader) || reader.pos != reader.len) {
        ClientPacketFree (&packet);
        return (ClientPacket){0};
    }
    return packet;
}

bool ClientPacketRead (Buffer *in, ClientPacket *packet) {
    *packet = (ClientPacket){0};
    if (in->len < 5) return true;

    const unsigned char *data = in->data;
    uint32_t payloadSize = (uint32_t)data[0] |
                           ((uint32_t)data[1] << 8) |
                           ((uint32_t)data[2] << 16) |
                           ((uint32_t)data[3] << 24);
    if (payloadSize == 0 || payloadSize > NET_MAX_FRAME) return false;

    size_t frameSize = 4 + (size_t)payloadSize;
    if (in->len < frameSize) return true;

    *packet = ClientPacketParse (data[4], data + 5,
                                 (size_t)payloadSize - 1);
    if (packet->type == NET_NONE) return false;

    memmove (in->data, in->data + frameSize, in->len - frameSize);
    in->len -= frameSize;
    return true;
}

void ClientPacketFree (ClientPacket *packet) {
    if (packet == NULL) return;
    switch (packet->type) {
    case NET_ASSET_ADD:
        free (packet->u.assetAdd.name);
        free (packet->u.assetAdd.ext);
        free (packet->u.assetAdd.data);
        break;
    case NET_ASSET_RENAME:
        free (packet->u.assetRename.name);
        break;
    case NET_ASSET_REPLACE:
        free (packet->u.assetReplace.ext);
        free (packet->u.assetReplace.data);
        break;
    case NET_OBJECT_CREATED:
        free (packet->u.objectCreated.object.name);
        free (packet->u.objectCreated.object.assetIds);
        break;
    case NET_OBJECT_UPSERT:
        free (packet->u.object.name);
        free (packet->u.object.assetIds);
        break;
    case NET_USER_STATE:
        free (packet->u.user.username);
        break;
    case NET_MENU:
        for (uint32_t i = 0; i < packet->u.menu.count; i++)
            free (packet->u.menu.names[i]);
        free (packet->u.menu.names);
        break;
    case NET_MESSAGE:
    case NET_ERROR:
    case NET_INFO:
        free (packet->u.message.text);
        break;
    default:
        break;
    }
}

static bool ReadServerObject (Reader *reader, ServerObjectUpsertPacket *p) {
    if (!ReaderU32 (reader, &p->fields) ||
        !ReaderU64 (reader, &p->id) ||
        (p->fields & ~OBJECT_FIELD_ALL) != 0)
        return false;
    if ((p->fields & OBJECT_FIELD_NAME) &&
        (p->name = ReadAllocString (reader)) == NULL)
        return false;
    if ((p->fields & OBJECT_FIELD_POSITION) &&
        (!ReaderF32 (reader, &p->x) || !ReaderF32 (reader, &p->y)))
        return false;
    if ((p->fields & OBJECT_FIELD_LAYER) &&
        !ReaderU32 (reader, &p->layer))
        return false;
    if ((p->fields & OBJECT_FIELD_TIMESTAMP) &&
        !ReaderU64 (reader, &p->timestamp))
        return false;
    if ((p->fields & OBJECT_FIELD_HIDDEN) &&
        !ReaderBytes (reader, &p->hidden, 1))
        return false;
    if (p->fields & OBJECT_FIELD_ASSETS) {
        if (!ReaderU32 (reader, &p->assetCount) ||
            p->assetCount > 100000 ||
            !ReaderAvailable (reader, (size_t)p->assetCount * 8))
            return false;
        if (p->assetCount > 0) {
            p->assetIds = malloc (p->assetCount * sizeof (u64));
            if (p->assetIds == NULL) return false;
            for (uint32_t i = 0; i < p->assetCount; i++) {
                if (!ReaderU64 (reader, &p->assetIds[i])) return false;
            }
        }
    }
    if ((p->fields & OBJECT_FIELD_ASSET_INDEX) &&
        !ReaderU32 (reader, &p->assetIndex))
        return false;
    if ((p->fields & OBJECT_FIELD_LOCK_OWNER) &&
        !ReaderU64 (reader, &p->lockOwnerId))
        return false;
    return true;
}

static bool ParseServerPacket (ServerPacket *packet, Reader *reader) {
    switch (packet->type) {
    case NET_HELLO:
        return (packet->u.hello.username = ReadAllocString (reader)) != NULL;
    case NET_REQ_ASSET_ADD: {
        ServerAssetAddPacket *p = &packet->u.assetAdd;
        uint32_t size;
        if (!ReaderU64 (reader, &p->requestId) ||
            !ReaderBytes (reader, &p->createObject, 1) ||
            (p->name = ReadAllocString (reader)) == NULL ||
            (p->ext = ReadAllocString (reader)) == NULL ||
            !ReaderU32 (reader, &size) ||
            !ReadAllocBytes (reader, size, &p->data))
            return false;
        p->size = size;
        if (!p->createObject) return true;
        return (p->objectName = ReadAllocString (reader)) != NULL &&
               ReaderF32 (reader, &p->x) &&
               ReaderF32 (reader, &p->y) &&
               ReaderU32 (reader, &p->layer);
    }
    case NET_REQ_OBJECT_CREATE: {
        ServerObjectCreatePacket *p = &packet->u.objectCreate;
        if (!ReaderU64 (reader, &p->requestId) ||
            (p->name = ReadAllocString (reader)) == NULL ||
            !ReaderF32 (reader, &p->x) ||
            !ReaderF32 (reader, &p->y) ||
            !ReaderU32 (reader, &p->layer) ||
            !ReaderU32 (reader, &p->assetCount) ||
            p->assetCount > 100000 ||
            !ReaderAvailable (reader, (size_t)p->assetCount * 8))
            return false;
        if (p->assetCount > 0) {
            p->assetIds = malloc (p->assetCount * sizeof (u64));
            if (p->assetIds == NULL) return false;
            for (uint32_t i = 0; i < p->assetCount; i++) {
                if (!ReaderU64 (reader, &p->assetIds[i])) return false;
            }
        }
        return true;
    }
    case NET_REQ_SCRIPT: {
        ServerScriptPacket *p = &packet->u.script;
        return (p->source = ReadAllocString (reader)) != NULL &&
               (p->selfExpr = ReadAllocString (reader)) != NULL;
    }
    case NET_REQ_MENU: {
        ServerMenuPacket *p = &packet->u.menu;
        return ReaderU64 (reader, &p->id) &&
               ReaderU32 (reader, &p->index) &&
               (p->name = ReadAllocString (reader)) != NULL;
    }
    case NET_REQ_IMPORT: {
        ServerImportPacket *p = &packet->u.import;
        uint32_t size;
        if (!ReaderU32 (reader, &size) || !ReadAllocBytes (reader, size, &p->data))
            return false;
        p->size = size;
        return true;
    }
    case NET_DRAG_UPDATE: {
        ServerDragPacket *p = &packet->u.drag;
        return ReaderU64 (reader, &p->id) &&
               ReaderF32 (reader, &p->dx) &&
               ReaderF32 (reader, &p->dy);
    }
    case NET_CURSOR: {
        ServerCursorPacket *p = &packet->u.cursor;
        return ReaderF32 (reader, &p->x) && ReaderF32 (reader, &p->y);
    }
    case NET_DRAG_END:
    case NET_LOCK:
    case NET_UNLOCK:
    case NET_UNLOCK_NOTIFY:
    case NET_ASSET_DELETE:
    case NET_OBJECT_DELETE:
        return ReaderU64 (reader, &packet->u.id.id);
    case NET_ASSET_RENAME: {
        ServerAssetRenamePacket *p = &packet->u.assetRename;
        return ReaderU64 (reader, &p->id) &&
               (p->name = ReadAllocString (reader)) != NULL;
    }
    case NET_ASSET_REPLACE: {
        ServerAssetReplacePacket *p = &packet->u.assetReplace;
        uint32_t size;
        if (!ReaderU64 (reader, &p->id) ||
            (p->ext = ReadAllocString (reader)) == NULL ||
            !ReaderU32 (reader, &size) ||
            !ReadAllocBytes (reader, size, &p->data))
            return false;
        p->size = size;
        return true;
    }
    case NET_OBJECT_UPSERT:
        return ReadServerObject (reader, &packet->u.objectUpsert);
    default:
        return false;
    }
}

ServerPacket ServerPacketParse (enum NetPacketType type,
                                const unsigned char *data, size_t length) {
    ServerPacket packet = {0};
    packet.type = type;

    Reader reader = {data, length, 0};
    if (!ParseServerPacket (&packet, &reader) || reader.pos != reader.len) {
        ServerPacketFree (&packet);
        return (ServerPacket){0};
    }
    return packet;
}

void ServerPacketFree (ServerPacket *packet) {
    if (packet == NULL) return;
    switch (packet->type) {
    case NET_HELLO:
        free (packet->u.hello.username);
        break;
    case NET_REQ_ASSET_ADD:
        free (packet->u.assetAdd.name);
        free (packet->u.assetAdd.ext);
        free (packet->u.assetAdd.data);
        free (packet->u.assetAdd.objectName);
        break;
    case NET_REQ_OBJECT_CREATE:
        free (packet->u.objectCreate.name);
        free (packet->u.objectCreate.assetIds);
        break;
    case NET_REQ_SCRIPT:
        free (packet->u.script.source);
        free (packet->u.script.selfExpr);
        break;
    case NET_REQ_MENU:
        free (packet->u.menu.name);
        break;
    case NET_REQ_IMPORT:
        free (packet->u.import.data);
        break;
    case NET_ASSET_RENAME:
        free (packet->u.assetRename.name);
        break;
    case NET_ASSET_REPLACE:
        free (packet->u.assetReplace.ext);
        free (packet->u.assetReplace.data);
        break;
    case NET_OBJECT_UPSERT:
        free (packet->u.objectUpsert.name);
        free (packet->u.objectUpsert.assetIds);
        break;
    default:
        break;
    }
}








bool FrameMake (Buffer *frame, enum NetPacketType type, const Buffer *payload) {
    bool ok = BufferPutU32 (frame, (uint32_t)(payload->len + 1)) &&
              BufferPutU8 (frame, (uint8_t)type) &&
              BufferAppend (frame, payload->data, payload->len);
    return ok;
}













bool FrameAssetAdd (Buffer *frame, u64 requestId, u64 requesterId,
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








bool FrameAssetRename (Buffer *frame, u64 id, const char *name) {
    Buffer payload = {0};
    bool ok = BufferPutU64 (&payload, id) &&
              BufferPutString (&payload, name) &&
              FrameMake (frame, NET_ASSET_RENAME, &payload);
    BufferFree (&payload);
    return ok;
}










bool FrameAssetReplace (Buffer *frame, u64 id, const char *ext,
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







bool FrameAssetDelete (Buffer *frame, u64 id) {
    Buffer payload = {0};
    bool ok = BufferPutU64 (&payload, id) &&
              FrameMake (frame, NET_ASSET_DELETE, &payload);
    BufferFree (&payload);
    return ok;
}








bool FrameMessage (Buffer *frame, enum NetPacketType type, const char *message) {
    Buffer payload = {0};
    bool ok = BufferPutString (&payload, message) &&
              FrameMake (frame, type, &payload);
    BufferFree (&payload);
    return ok;
}


















bool FrameObjectUpsert (Buffer *frame, u64 id, uint32_t fields,
                               const char *name, float x, float y,
                               int32_t layer, u64 timestamp, bool hidden,
                               uint32_t assetIndex, const u64 *assetIds,
                               uint32_t assetCount, u64 lockOwnerId) {
    Buffer payload = {0};
    bool ok = BufferPutU32 (&payload, fields) && BufferPutU64 (&payload, id);
    if (ok && (fields & OBJECT_FIELD_NAME))
        ok = BufferPutString (&payload, name);
    if (ok && (fields & OBJECT_FIELD_POSITION))
        ok = BufferPutF32 (&payload, x) && BufferPutF32 (&payload, y);
    if (ok && (fields & OBJECT_FIELD_LAYER))
        ok = BufferPutU32 (&payload, (uint32_t)layer);
    if (ok && (fields & OBJECT_FIELD_TIMESTAMP))
        ok = BufferPutU64 (&payload, timestamp);
    if (ok && (fields & OBJECT_FIELD_HIDDEN))
        ok = BufferPutU8 (&payload, hidden ? 1 : 0);
    if (ok && (fields & OBJECT_FIELD_ASSETS)) {
        ok = BufferPutU32 (&payload, assetCount);
        for (uint32_t i = 0; ok && i < assetCount; i++)
            ok = BufferPutU64 (&payload, assetIds[i]);
    }
    if (ok && (fields & OBJECT_FIELD_ASSET_INDEX))
        ok = BufferPutU32 (&payload, assetIndex);
    if (ok && (fields & OBJECT_FIELD_LOCK_OWNER))
        ok = BufferPutU64 (&payload, lockOwnerId);
    if (ok) ok = FrameMake (frame, NET_OBJECT_UPSERT, &payload);
    BufferFree (&payload);
    return ok;
}


















bool FrameObjectCreated (Buffer *frame, u64 requestId,
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









bool FrameMenuRequest (Buffer *frame, u64 id, uint32_t index,
                              const char *name) {
    Buffer payload = {0};
    bool ok = BufferPutU64 (&payload, id) &&
              BufferPutU32 (&payload, index) &&
              BufferPutString (&payload, name) &&
              FrameMake (frame, NET_REQ_MENU, &payload);
    BufferFree (&payload);
    return ok;
}







bool FrameObjectDelete (Buffer *frame, u64 id) {
    Buffer payload = {0};
    bool ok = BufferPutU64 (&payload, id) &&
              FrameMake (frame, NET_OBJECT_DELETE, &payload);
    BufferFree (&payload);
    return ok;
}







bool FrameMenu (Buffer *frame, u64 id) {
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













bool FrameUserState (Buffer *frame, u64 id, const char *username,
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







bool FrameUserLeave (Buffer *frame, u64 id) {
    Buffer payload = {0};
    bool ok = BufferPutU64 (&payload, id) &&
              FrameMake (frame, NET_USER_LEAVE, &payload);
    BufferFree (&payload);
    return ok;
}









bool FrameCursorState (Buffer *frame, u64 id, float x, float y) {
    Buffer payload = {0};
    bool ok = BufferPutU64 (&payload, id) &&
              BufferPutF32 (&payload, x) &&
              BufferPutF32 (&payload, y) &&
              FrameMake (frame, NET_CURSOR_STATE, &payload);
    BufferFree (&payload);
    return ok;
}








bool FrameCursor (Buffer *frame, float x, float y) {
    Buffer payload = {0};
    bool ok = BufferPutF32 (&payload, x) &&
              BufferPutF32 (&payload, y) &&
              FrameMake (frame, NET_CURSOR, &payload);
    BufferFree (&payload);
    return ok;
}






bool FrameHeartbeat (Buffer *frame) {
    Buffer payload = {0};
    bool ok = FrameMake (frame, NET_HEARTBEAT, &payload);
    BufferFree (&payload);
    return ok;
}









bool FrameObjectDragUpdate (Buffer *frame, u64 id,
                                        float dx, float dy) {
    Buffer payload = {0};
    bool ok = BufferPutU64 (&payload, id) &&
              BufferPutF32 (&payload, dx) &&
              BufferPutF32 (&payload, dy) &&
              FrameMake (frame, NET_DRAG_UPDATE, &payload);
    BufferFree (&payload);
    return ok;
}








bool FrameObjectLock (Buffer *frame, enum NetPacketType type, u64 id) {
    Buffer payload = {0};
    bool ok = BufferPutU64 (&payload, id) &&
              FrameMake (frame, type, &payload);
    BufferFree (&payload);
    return ok;
}
