#ifndef NET_COMMON_H
#define NET_COMMON_H

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "utils.h"
#include "menu.h"

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
#define NET_REQ_IMPORT 30
#define NET_DRAG_END 31
#define NET_LOCK_FAILED 32
#define NET_DRAG_UPDATE 33
#define NET_UNLOCK_NOTIFY 34
#define NET_REQ_ASSET_OBJECT 35
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


static inline bool BufferReserve (Buffer *buffer, size_t extra) {
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

static inline bool BufferAppend (Buffer *buffer, const void *data, size_t size) {
    if (!BufferReserve (buffer, size)) return false;
    if (size > 0) memcpy (buffer->data + buffer->len, data, size);
    buffer->len += size;
    return true;
}

static inline bool BufferPutU8 (Buffer *buffer, uint8_t value) {
    return BufferAppend (buffer, &value, 1);
}

static inline bool BufferPutU16 (Buffer *buffer, uint16_t value) {
    unsigned char bytes[2] = {(unsigned char)(value & 0xFF), (unsigned char)(value >> 8)};
    return BufferAppend (buffer, bytes, 2);
}

static inline bool BufferPutU32 (Buffer *buffer, uint32_t value) {
    unsigned char bytes[4];
    for (int i = 0; i < 4; i++) bytes[i] = (unsigned char)((value >> (8 * i)) & 0xFF);
    return BufferAppend (buffer, bytes, 4);
}

static inline bool BufferPutU64 (Buffer *buffer, u64 value) {
    unsigned char bytes[8];
    for (int i = 0; i < 8; i++) bytes[i] = (unsigned char)((value >> (8 * i)) & 0xFF);
    return BufferAppend (buffer, bytes, 8);
}

static inline bool BufferPutF32 (Buffer *buffer, float value) {
    uint32_t bits;
    memcpy (&bits, &value, sizeof (bits));
    return BufferPutU32 (buffer, bits);
}

static inline bool BufferPutString (Buffer *buffer, const char *text) {
    size_t length = strlen (text);
    if (length > 0xFFFFu) length = 0xFFFFu;
    return BufferPutU16 (buffer, (uint16_t)length) &&
           BufferAppend (buffer, text, length);
}

static inline void BufferFree (Buffer *buffer) {
    free (buffer->data);
    buffer->data = NULL;
    buffer->len = 0;
    buffer->cap = 0;
}

static inline bool FrameMake (Buffer *frame, uint8_t type, const Buffer *payload) {
    bool ok = BufferPutU32 (frame, (uint32_t)(payload->len + 1)) &&
              BufferPutU8 (frame, type) &&
              BufferAppend (frame, payload->data, payload->len);
    return ok;
}

static inline bool ReaderAvailable (const Reader *reader, size_t size) {
    return reader->pos <= reader->len && size <= reader->len - reader->pos;
}

static inline bool ReaderBytes (Reader *reader, void *out, size_t size) {
    if (!ReaderAvailable (reader, size)) return false;
    if (out != NULL && size > 0) memcpy (out, reader->data + reader->pos, size);
    reader->pos += size;
    return true;
}

static inline bool ReaderU16 (Reader *reader, uint16_t *value) {
    unsigned char bytes[2];
    if (!ReaderBytes (reader, bytes, 2)) return false;
    *value = (uint16_t)(bytes[0] | ((uint16_t)bytes[1] << 8));
    return true;
}

static inline bool ReaderU32 (Reader *reader, uint32_t *value) {
    unsigned char bytes[4];
    if (!ReaderBytes (reader, bytes, 4)) return false;
    *value = (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
             ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
    return true;
}

static inline bool ReaderU64 (Reader *reader, u64 *value) {
    unsigned char bytes[8];
    if (!ReaderBytes (reader, bytes, 8)) return false;
    *value = 0;
    for (int i = 0; i < 8; i++) *value |= (u64)bytes[i] << (8 * i);
    return true;
}

static inline bool ReaderF32 (Reader *reader, float *value) {
    uint32_t bits;
    if (!ReaderU32 (reader, &bits)) return false;
    memcpy (value, &bits, sizeof (bits));
    return true;
}

static inline bool ReaderString (Reader *reader, char *text, size_t size) {
    uint16_t length;
    if (!ReaderU16 (reader, &length)) return false;
    if (!ReaderAvailable (reader, length) || size == 0) return false;

    size_t copy = (length < size - 1) ? length : size - 1;
    memcpy (text, reader->data + reader->pos, copy);
    text[copy] = '\0';
    reader->pos += length;
    return true;
}


static inline bool FrameAssetAdd (Buffer *frame, u64 requestId, u64 requesterId,
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

static inline bool FrameAssetRename (Buffer *frame, u64 id, const char *name) {
    Buffer payload = {0};
    bool ok = BufferPutU64 (&payload, id) &&
              BufferPutString (&payload, name) &&
              FrameMake (frame, NET_ASSET_RENAME, &payload);
    BufferFree (&payload);
    return ok;
}

static inline bool FrameAssetReplace (Buffer *frame, u64 id, const char *ext,
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

static inline bool FrameAssetDelete (Buffer *frame, u64 id) {
    Buffer payload = {0};
    bool ok = BufferPutU64 (&payload, id) &&
              FrameMake (frame, NET_ASSET_DELETE, &payload);
    BufferFree (&payload);
    return ok;
}

static inline bool FrameAssetDeleteFailed (Buffer *frame, u64 id) {
    Buffer payload = {0};
    bool ok = BufferPutU64 (&payload, id) &&
              FrameMake (frame, NET_ASSET_DELETE_FAILED, &payload);
    BufferFree (&payload);
    return ok;
}

static inline bool FrameMessage (Buffer *frame, uint8_t type, const char *message) {
    Buffer payload = {0};
    bool ok = BufferPutString (&payload, message) &&
              FrameMake (frame, type, &payload);
    BufferFree (&payload);
    return ok;
}

static inline bool FrameObjectUpsert (Buffer *frame, u64 id, const char *name,
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

static inline bool FrameObjectCreated (Buffer *frame, u64 requestId,
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

static inline bool FrameMenuRequest (Buffer *frame, u64 id, uint32_t index,
                              const char *name) {
    Buffer payload = {0};
    bool ok = BufferPutU64 (&payload, id) &&
              BufferPutU32 (&payload, index) &&
              BufferPutString (&payload, name) &&
              FrameMake (frame, NET_REQ_MENU, &payload);
    BufferFree (&payload);
    return ok;
}

static inline bool FrameObjectDelete (Buffer *frame, u64 id) {
    Buffer payload = {0};
    bool ok = BufferPutU64 (&payload, id) &&
              FrameMake (frame, NET_OBJECT_DELETE, &payload);
    BufferFree (&payload);
    return ok;
}

static inline bool FrameMenu (Buffer *frame, u64 id) {
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

static inline bool FrameUserState (Buffer *frame, u64 id, const char *username,
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

static inline bool FrameUserLeave (Buffer *frame, u64 id) {
    Buffer payload = {0};
    bool ok = BufferPutU64 (&payload, id) &&
              FrameMake (frame, NET_USER_LEAVE, &payload);
    BufferFree (&payload);
    return ok;
}

static inline bool FrameCursorState (Buffer *frame, u64 id, float x, float y) {
    Buffer payload = {0};
    bool ok = BufferPutU64 (&payload, id) &&
              BufferPutF32 (&payload, x) &&
              BufferPutF32 (&payload, y) &&
              FrameMake (frame, NET_CURSOR_STATE, &payload);
    BufferFree (&payload);
    return ok;
}

static inline bool FrameLockState (Buffer *frame, u64 id, u64 ownerId) {
    Buffer payload = {0};
    bool ok = BufferPutU64 (&payload, id) &&
              BufferPutU64 (&payload, ownerId) &&
              FrameMake (frame, NET_LOCK_STATE, &payload);
    BufferFree (&payload);
    return ok;
}

static inline bool FrameCursor (Buffer *frame, float x, float y) {
    Buffer payload = {0};
    bool ok = BufferPutF32 (&payload, x) &&
              BufferPutF32 (&payload, y) &&
              FrameMake (frame, NET_CURSOR, &payload);
    BufferFree (&payload);
    return ok;
}

static inline bool FrameObjectDragUpdate (Buffer *frame, u64 id,
                                        float dx, float dy) {
    Buffer payload = {0};
    bool ok = BufferPutU64 (&payload, id) &&
              BufferPutF32 (&payload, dx) &&
              BufferPutF32 (&payload, dy) &&
              FrameMake (frame, NET_DRAG_UPDATE, &payload);
    BufferFree (&payload);
    return ok;
}

static inline bool FrameObjectLock (Buffer *frame, uint8_t type, u64 id) {
    Buffer payload = {0};
    bool ok = BufferPutU64 (&payload, id) &&
              FrameMake (frame, type, &payload);
    BufferFree (&payload);
    return ok;
}


#endif
