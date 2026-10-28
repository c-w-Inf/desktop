#ifndef NET_COMMON_H
#define NET_COMMON_H

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "utils.h"

/** @brief 网络包类型；NET_NONE 表示没有包。 */
enum NetPacketType {
    NET_NONE = 0,
    NET_ASSET_ADD = 2,
    NET_ASSET_RENAME = 3,
    NET_ASSET_REPLACE = 4,
    NET_ASSET_DELETE = 5,
    NET_OBJECT_UPSERT = 6,
    NET_OBJECT_DELETE = 7,
    NET_HELLO = 8,
    NET_USER_STATE = 10,
    NET_USER_LEAVE = 11,
    NET_CURSOR_STATE = 12,
    NET_CURSOR = 14,
    NET_LOCK = 15,
    NET_UNLOCK = 16,
    NET_LOCK_FAILED = 17,
    NET_MESSAGE = 18,
    NET_HEARTBEAT = 19,
    NET_REQ_ASSET_ADD = 20,
    NET_REQ_OBJECT_CREATE = 21,
    NET_OBJECT_CREATED = 22,
    NET_REQ_SCRIPT = 24,
    NET_ERROR = 25,
    NET_INFO = 26,
    NET_MENU = 27,
    NET_REQ_MENU = 28,
    NET_REQ_IMPORT = 30,
    NET_DRAG_END = 31,
    NET_DRAG_UPDATE = 33,
    NET_UNLOCK_NOTIFY = 34,
    SERVER_EVENT_DISCONNECT = 255,
};

/** @brief 物件名称字段。 */
#define OBJECT_FIELD_NAME (1u << 0)
/** @brief 物件位置字段。 */
#define OBJECT_FIELD_POSITION (1u << 1)
/** @brief 物件层级字段。 */
#define OBJECT_FIELD_LAYER (1u << 2)
/** @brief 物件时间戳字段。 */
#define OBJECT_FIELD_TIMESTAMP (1u << 3)
/** @brief 物件隐藏状态字段。 */
#define OBJECT_FIELD_HIDDEN (1u << 4)
/** @brief 物件素材数组字段。 */
#define OBJECT_FIELD_ASSETS (1u << 5)
/** @brief 物件当前素材下标字段。 */
#define OBJECT_FIELD_ASSET_INDEX (1u << 6)
/** @brief 物件锁占用者字段。 */
#define OBJECT_FIELD_LOCK_OWNER (1u << 7)
/** @brief 物件完整状态字段。 */
#define OBJECT_FIELD_ALL (OBJECT_FIELD_NAME | OBJECT_FIELD_POSITION | \
                          OBJECT_FIELD_LAYER | OBJECT_FIELD_TIMESTAMP | \
                          OBJECT_FIELD_HIDDEN | OBJECT_FIELD_ASSETS | \
                          OBJECT_FIELD_ASSET_INDEX | OBJECT_FIELD_LOCK_OWNER)

/** @brief 单帧最大字节数。 */
#define NET_MAX_FRAME (256u * 1024u * 1024u)
/** @brief 最大客户端数量。 */
#define NET_MAX_CLIENTS 16
/** @brief 单个连接输出缓冲上限。 */
#define NET_MAX_OUTPUT (512u * 1024u * 1024u)
/** @brief 消息文本最大字节数。 */
#define NET_MAX_MESSAGE 256

/**
 * @brief 可增长字节缓冲区。
 */
typedef struct {
    unsigned char *data; /**< 数据指针。 */
    size_t len; /**< 已写入字节数。 */
    size_t cap; /**< 已分配容量。 */
} Buffer;

/**
 * @brief 只读字节流读取游标。
 */
typedef struct {
    const unsigned char *data; /**< 数据指针。 */
    size_t len; /**< 数据总字节数。 */
    size_t pos; /**< 当前读取位置。 */
} Reader;

/**
 * @brief 确保缓冲区可追加 extra 字节。
 * @param buffer 缓冲区。
 * @param extra 额外字节数。
 * @return 容量足够时为 true，溢出或分配失败为 false。
 */
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

/**
 * @brief 向缓冲区追加字节。
 * @param buffer 缓冲区。
 * @param data 源数据，size 为 0 时可为 NULL。
 * @param size 字节数。
 * @return 追加成功时为 true。
 */
static inline bool BufferAppend (Buffer *buffer, const void *data, size_t size) {
    if (!BufferReserve (buffer, size)) return false;
    if (size > 0) memcpy (buffer->data + buffer->len, data, size);
    buffer->len += size;
    return true;
}

/**
 * @brief 追加 8 位无符号整数。
 * @param buffer 缓冲区。
 * @param value 值。
 * @return 追加成功时为 true。
 */
static inline bool BufferPutU8 (Buffer *buffer, uint8_t value) {
    return BufferAppend (buffer, &value, 1);
}

/**
 * @brief 追加小端 16 位无符号整数。
 * @param buffer 缓冲区。
 * @param value 值。
 * @return 追加成功时为 true。
 */
static inline bool BufferPutU16 (Buffer *buffer, uint16_t value) {
    unsigned char bytes[2] = {(unsigned char)(value & 0xFF), (unsigned char)(value >> 8)};
    return BufferAppend (buffer, bytes, 2);
}

/**
 * @brief 追加小端 32 位无符号整数。
 * @param buffer 缓冲区。
 * @param value 值。
 * @return 追加成功时为 true。
 */
static inline bool BufferPutU32 (Buffer *buffer, uint32_t value) {
    unsigned char bytes[4];
    for (int i = 0; i < 4; i++) bytes[i] = (unsigned char)((value >> (8 * i)) & 0xFF);
    return BufferAppend (buffer, bytes, 4);
}

/**
 * @brief 追加小端 64 位无符号整数。
 * @param buffer 缓冲区。
 * @param value 值。
 * @return 追加成功时为 true。
 */
static inline bool BufferPutU64 (Buffer *buffer, u64 value) {
    unsigned char bytes[8];
    for (int i = 0; i < 8; i++) bytes[i] = (unsigned char)((value >> (8 * i)) & 0xFF);
    return BufferAppend (buffer, bytes, 8);
}

/**
 * @brief 追加小端 IEEE 754 单精度浮点数。
 * @param buffer 缓冲区。
 * @param value 值。
 * @return 追加成功时为 true。
 */
static inline bool BufferPutF32 (Buffer *buffer, float value) {
    uint32_t bits;
    memcpy (&bits, &value, sizeof (bits));
    return BufferPutU32 (buffer, bits);
}

/**
 * @brief 追加 u16 长度前缀的字符串，超长部分截断。
 * @param buffer 缓冲区。
 * @param text NUL 结尾文本。
 * @return 追加成功时为 true。
 */
static inline bool BufferPutString (Buffer *buffer, const char *text) {
    size_t length = strlen (text);
    if (length > 0xFFFFu) length = 0xFFFFu;
    return BufferPutU16 (buffer, (uint16_t)length) &&
           BufferAppend (buffer, text, length);
}

/**
 * @brief 释放缓冲区存储并清零。
 * @param buffer 缓冲区。
 */
static inline void BufferFree (Buffer *buffer) {
    free (buffer->data);
    buffer->data = NULL;
    buffer->len = 0;
    buffer->cap = 0;
}

/**
 * @brief 检查读取游标后是否还有足够字节。
 * @param reader 读取器。
 * @param size 需要的字节数。
 * @return 可读取时为 true。
 */
static inline bool ReaderAvailable (const Reader *reader, size_t size) {
    return reader->pos <= reader->len && size <= reader->len - reader->pos;
}

/**
 * @brief 读取指定字节数并前进游标。
 * @param reader 读取器。
 * @param out 输出缓冲区，可为 NULL 表示跳过。
 * @param size 字节数。
 * @return 读取成功时为 true。
 */
static inline bool ReaderBytes (Reader *reader, void *out, size_t size) {
    if (!ReaderAvailable (reader, size)) return false;
    if (out != NULL && size > 0) memcpy (out, reader->data + reader->pos, size);
    reader->pos += size;
    return true;
}

/**
 * @brief 读取小端 16 位无符号整数。
 * @param reader 读取器。
 * @param value 输出值。
 * @return 读取成功时为 true。
 */
static inline bool ReaderU16 (Reader *reader, uint16_t *value) {
    unsigned char bytes[2];
    if (!ReaderBytes (reader, bytes, 2)) return false;
    *value = (uint16_t)(bytes[0] | ((uint16_t)bytes[1] << 8));
    return true;
}

/**
 * @brief 读取小端 32 位无符号整数。
 * @param reader 读取器。
 * @param value 输出值。
 * @return 读取成功时为 true。
 */
static inline bool ReaderU32 (Reader *reader, uint32_t *value) {
    unsigned char bytes[4];
    if (!ReaderBytes (reader, bytes, 4)) return false;
    *value = (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
             ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
    return true;
}

/**
 * @brief 读取小端 64 位无符号整数。
 * @param reader 读取器。
 * @param value 输出值。
 * @return 读取成功时为 true。
 */
static inline bool ReaderU64 (Reader *reader, u64 *value) {
    unsigned char bytes[8];
    if (!ReaderBytes (reader, bytes, 8)) return false;
    *value = 0;
    for (int i = 0; i < 8; i++) *value |= (u64)bytes[i] << (8 * i);
    return true;
}

/**
 * @brief 读取小端 IEEE 754 单精度浮点数。
 * @param reader 读取器。
 * @param value 输出值。
 * @return 读取成功时为 true。
 */
static inline bool ReaderF32 (Reader *reader, float *value) {
    uint32_t bits;
    if (!ReaderU32 (reader, &bits)) return false;
    memcpy (value, &bits, sizeof (bits));
    return true;
}

/**
 * @brief 读取 u16 长度前缀字符串并写入缓冲区。
 * @param reader 读取器。
 * @param text 输出缓冲区。
 * @param size 输出缓冲区字节数。
 * @return 读取成功时为 true；字符串过长时截断但游标前进完整长度。
 */
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

#endif
