#ifndef NET_H
#define NET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "assets.h"
#include "net_common.h"
#include "object.h"

typedef struct {
    u64 requestId;
    u64 requesterId;
    u64 id;
    char *name;
    char *ext;
    unsigned char *data;
    size_t size;
} ClientAssetAddPacket;

typedef struct {
    u64 id;
    char *name;
} ClientAssetRenamePacket;

typedef struct {
    u64 id;
    char *ext;
    unsigned char *data;
    size_t size;
} ClientAssetReplacePacket;

typedef struct {
    u64 id;
} ClientIdPacket;

typedef struct {
    u64 id;
    uint32_t fields;
    char *name;
    float x;
    float y;
    uint32_t layer;
    u64 timestamp;
    bool hidden;
    uint32_t assetIndex;
    uint32_t assetCount;
    u64 *assetIds;
    u64 lockOwnerId;
} ClientObjectPacket;

typedef struct {
    u64 requestId;
    u64 requesterId;
    ClientObjectPacket object;
} ClientObjectCreatedPacket;

typedef struct {
    u64 id;
    char *username;
    uint32_t color;
    bool canscript;
    bool hasCursor;
    float x;
    float y;
} ClientUserPacket;

typedef struct {
    u64 id;
    float x;
    float y;
} ClientCursorPacket;

typedef struct {
    u64 id;
    uint32_t count;
    char **names;
} ClientMenuPacket;

typedef struct {
    char *text;
} ClientMessagePacket;

typedef struct {
    u64 selfId;
} ClientHelloPacket;

typedef struct ClientPacket {
    enum NetPacketType type;
    union {
        ClientHelloPacket hello;
        ClientAssetAddPacket assetAdd;
        ClientAssetRenamePacket assetRename;
        ClientAssetReplacePacket assetReplace;
        ClientIdPacket id;
        ClientObjectPacket object;
        ClientObjectCreatedPacket objectCreated;
        ClientUserPacket user;
        ClientCursorPacket cursor;
        ClientMenuPacket menu;
        ClientMessagePacket message;
    } u;
} ClientPacket;



typedef struct {
    char *username;
} ServerHelloPacket;

typedef struct {
    u64 requestId;
    char *name;
    char *ext;
    unsigned char *data;
    size_t size;
    bool createObject;
    char *objectName;
    float x;
    float y;
    uint32_t layer;
} ServerAssetAddPacket;

typedef struct {
    u64 requestId;
    char *name;
    float x;
    float y;
    uint32_t layer;
    uint32_t assetCount;
    u64 *assetIds;
} ServerObjectCreatePacket;

typedef struct {
    u64 id;
    uint32_t fields;
    char *name;
    float x;
    float y;
    uint32_t layer;
    u64 timestamp;
    bool hidden;
    uint32_t assetIndex;
    uint32_t assetCount;
    u64 *assetIds;
    u64 lockOwnerId;
} ServerObjectUpsertPacket;

typedef struct {
    char *source;
    char *selfExpr;
} ServerScriptPacket;

typedef struct {
    u64 id;
    uint32_t index;
    char *name;
} ServerMenuPacket;

typedef struct {
    unsigned char *data;
    size_t size;
} ServerImportPacket;

typedef struct {
    u64 id;
    float dx;
    float dy;
} ServerDragPacket;

typedef struct {
    float x;
    float y;
} ServerCursorPacket;

typedef struct {
    u64 id;
} ServerIdPacket;

typedef struct {
    u64 id;
    char *name;
} ServerAssetRenamePacket;

typedef struct {
    u64 id;
    char *ext;
    unsigned char *data;
    size_t size;
} ServerAssetReplacePacket;

typedef struct ServerPacket {
    enum NetPacketType type;
    union {
        ServerHelloPacket hello;
        ServerAssetAddPacket assetAdd;
        ServerObjectCreatePacket objectCreate;
        ServerObjectUpsertPacket objectUpsert;
        ServerScriptPacket script;
        ServerMenuPacket menu;
        ServerImportPacket import;
        ServerDragPacket drag;
        ServerCursorPacket cursor;
        ServerIdPacket id;
        ServerAssetRenamePacket assetRename;
        ServerAssetReplacePacket assetReplace;
    } u;
} ServerPacket;

/** @brief 按长度、类型、载荷格式封装一帧。 */
bool FrameMake (Buffer *frame, enum NetPacketType type, const Buffer *payload);
/** @brief 构造素材新增帧。 */
bool FrameAssetAdd (Buffer *frame, u64 requestId, u64 requesterId,
                           u64 id, const char *name, const char *ext,
                           const unsigned char *data, uint32_t size);
/** @brief 构造素材重命名帧。 */
bool FrameAssetRename (Buffer *frame, u64 id, const char *name);
/** @brief 构造素材替换帧。 */
bool FrameAssetReplace (Buffer *frame, u64 id, const char *ext,
                               const unsigned char *data, uint32_t size);
/** @brief 构造素材删除帧。 */
bool FrameAssetDelete (Buffer *frame, u64 id);
/** @brief 构造带单条文本的消息帧。 */
bool FrameMessage (Buffer *frame, enum NetPacketType type, const char *message);
/** @brief 按字段掩码构造物件状态帧。 */
bool FrameObjectUpsert (Buffer *frame, u64 id, uint32_t fields,
                               const char *name, float x, float y,
                               int32_t layer, u64 timestamp, bool hidden,
                               uint32_t assetIndex, const u64 *assetIds,
                               uint32_t assetCount, u64 lockOwnerId);
/** @brief 构造创建物件结果帧。 */
bool FrameObjectCreated (Buffer *frame, u64 requestId,
                                u64 requesterId, u64 id,
                                const char *name, float x, float y, int32_t layer,
                                u64 timestamp, bool hidden, uint32_t assetIndex,
                                const u64 *assetIds, uint32_t assetCount);
/** @brief 构造菜单执行请求帧。 */
bool FrameMenuRequest (Buffer *frame, u64 id, uint32_t index,
                              const char *name);
/** @brief 构造物件删除帧。 */
bool FrameObjectDelete (Buffer *frame, u64 id);
/** @brief 构造菜单广播帧。 */
bool FrameMenu (Buffer *frame, u64 id);
/** @brief 构造用户状态帧。 */
bool FrameUserState (Buffer *frame, u64 id, const char *username,
                            uint32_t color, bool canscript,
                            bool hasCursor, float x, float y);
/** @brief 构造用户离开帧。 */
bool FrameUserLeave (Buffer *frame, u64 id);
/** @brief 构造用户光标状态帧。 */
bool FrameCursorState (Buffer *frame, u64 id, float x, float y);
/** @brief 构造客户端光标位置帧。 */
bool FrameCursor (Buffer *frame, float x, float y);
/** @brief 构造无载荷心跳帧。 */
bool FrameHeartbeat (Buffer *frame);
/** @brief 构造拖动增量帧。 */
bool FrameObjectDragUpdate (Buffer *frame, u64 id,
                                        float dx, float dy);
/** @brief 构造带类型的锁相关物件帧。 */
bool FrameObjectLock (Buffer *frame, enum NetPacketType type, u64 id);

/**
 * @brief 解析一个客户端网络包。
 * @param type 包类型。
 * @param data 载荷。
 * @param length 载荷字节数。
 * @return 解析后的包；失败时返回 type 为 0 的空包。
 */
ClientPacket ClientPacketParse (enum NetPacketType type,
                                const unsigned char *data, size_t length);
/**
 * @brief 从客户端输入缓冲读取并解析一帧。
 * @param in 输入缓冲，成功读取后会移除该帧。
 * @param packet 输出包；没有完整帧时 type 为 NET_NONE。
 * @return 帧格式错误时为 false；数据不完整或读取成功时为 true。
 */
bool ClientPacketRead (Buffer *in, ClientPacket *packet);
/**
 * @brief 释放一个客户端网络包及其字符串、字节和数组。
 * @param packet 要释放的包。
 */
void ClientPacketFree (ClientPacket *packet);
/**
 * @brief 解析一个服务端网络包。
 * @param type 包类型。
 * @param data 载荷。
 * @param length 载荷字节数。
 * @return 解析后的包；失败时返回 type 为 0 的空包。
 */
ServerPacket ServerPacketParse (enum NetPacketType type,
                                const unsigned char *data, size_t length);
/**
 * @brief 释放一个服务端网络包及其字符串、字节和数组。
 * @param packet 要释放的包。
 */
void ServerPacketFree (ServerPacket *packet);

/**
 * @brief 启动无头服务器并阻塞运行。
 * @param port 监听端口。
 * @param address IPv4 地址，NULL 或空表示 0.0.0.0。
 * @param loadPath 启动时导入的 .dsk，可为 NULL。
 * @param savePath 自动保存路径，NULL 或空表示禁用。
 * @return 服务端停止后的退出码。
 */
int NetRunServer (int port, const char *address, const char *loadPath,
                  const char *savePath);
/**
 * @brief 向所有客户端广播提示消息。
 * @param message 消息文本。
 */
void NetServerBroadcastMessage (const char *message);
/**
 * @brief 向指定用户发送提示消息。
 * @param target 用户 ID。
 * @param message 消息文本。
 */
void NetServerSendMessage (u64 target, const char *message);
/**
 * @brief 向指定用户发送错误消息。
 * @param target 用户 ID。
 * @param message 错误文本。
 */
void NetServerSendError (u64 target, const char *message);

/**
 * @brief 向指定用户发送一般提示消息。
 * @param target 用户 ID。
 * @param message 提示文本。
 */
void NetServerSendInfo (u64 target, const char *message);
/**
 * @brief 广播物件的完整状态。
 * @param object 要广播的物件。
 */
void NetServerBroadcastObjectState (const Object *object);
/**
 * @brief 广播指定菜单。
 * @param id 0 为全局，否则为用户或物件 ID。
 */
void NetServerBroadcastMenu (u64 id);

/**
 * @brief 设置用户颜色并广播用户状态。
 * @param id 用户 ID。
 * @param color RGBA 颜色。
 */
void NetServerSetUserColor (u64 id, uint32_t color);

/**
 * @brief 设置用户脚本权限并广播用户状态。
 * @param id 用户 ID。
 * @param canscript 是否允许提交脚本。
 */
void NetServerSetUserCanScript (u64 id, bool canscript);

/**
 * @brief 释放物件锁并广播锁状态。
 * @param id 物件 ID。
 */
void NetServerReleaseObjectLock (u64 id);

/**
 * @brief 服务端删除物件并广播。
 * @param id 物件 ID。
 * @return 删除成功时为 true。
 */
bool NetServerDeleteObject (u64 id);

/**
 * @brief 分配新的实体 ID。
 * @return 未占用的实体 ID。
 */
u64 NetServerAllocateEntityId (void);


#endif
