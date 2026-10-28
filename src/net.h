#ifndef NET_H
#define NET_H

#include <stdbool.h>

#include "assets.h"
#include "object.h"

/* ---------- 客户端状态标志 ---------- */
#define NET_UPDATE_NONE 0
#define NET_UPDATE_SCENE 1
#define NET_UPDATE_DELETED 2
#define NET_UPDATE_REJECTED 4
#define NET_UPDATE_ASSET_DELETED 8
#define NET_UPDATE_ASSET_DELETE_FAILED 16
#define NET_UPDATE_LOCK_LOST 32

typedef void (*NetLockDoneFn)(bool acquired);

/* ---------- 初始化 / 销毁 ---------- */
bool NetClientConnect (const char *host, int port, const char *username);
bool NetClientConnected (void);
void NetClientDisconnect (void);

/* ---------- 服务端动作 ---------- */
/* 启动无头服务器并阻塞运行，返回即表示服务端已停止。
   address: IPv4 bind address; NULL or empty means 0.0.0.0.
   loadPath: optional .dsk to import at startup.
   savePath: optional save file; when NULL or empty, saving is disabled. */
int NetRunServer (int port, const char *address, const char *loadPath,
                  const char *savePath);
void NetServerBroadcastMessage (const char *message);
void NetServerSendMessage (u64 target, const char *message);
void NetServerSendError (u64 target, const char *message);
/* 发送一般提示（蓝色）给指定客户端。 */
void NetServerSendInfo (u64 target, const char *message);
void NetServerBroadcastObjectState (const Object *object);
void NetServerBroadcastMenu (u64 id);
/* 设置用户颜色并广播更新后的用户状态。 */
void NetServerSetUserColor (u64 id, uint32_t color);
/* 设置用户是否允许提交脚本，并广播更新后的用户状态。 */
void NetServerSetUserCanScript (u64 id, bool canscript);
/* 释放物件当前占用锁并广播（供脚本 untouch() 使用）。 */
void NetServerReleaseObjectLock (u64 id);
/* 服务端删除物件：先释放锁，再删除并广播删除。 */
bool NetServerDeleteObject (u64 id);
/* 为脚本等服务器内部操作分配一个新实体 ID。 */
u64 NetServerAllocateEntityId (void);

/* ---------- 客户端动作 ---------- */
int NetClientUpdate (void);

bool NetRequestAssetRename (u64 id, const char *name);
bool NetRequestAssetReplace (u64 id, const char *ext,
                             const unsigned char *data, int size);
bool NetRequestAssetDelete (u64 id);
bool NetPublishObjectUpsert (const Object *object);
/* 通知服务端当前物件正在拖动；服务端据此持续触发 ondrag。 */
bool NetRequestDragUpdate (u64 id, Vector2 delta);
/* 通知服务端当前物件的拖动已经结束。 */
bool NetRequestDragEnd (u64 id);
/* 调用前物件必须已由当前用户锁定；时间戳由服务端分配并统一广播。 */
bool NetRequestObjectFront (u64 id);
bool NetRequestObjectDelete (u64 id);
/* 提交脚本；selfExpr 以提交者身份求值并选中执行 self，空串表示 self=0。 */
bool NetRequestScript (const char *source, const char *selfExpr);
bool NetRequestMenu (u64 id, int index, const char *name);
/* 请求服务端把当前完整场景导出到 path；导出是服务端本地文件操作。 */
/* 读取 path（文件或文件夹，文件夹会先打包）并把内容发送给服务端合并。 */
bool NetRequestImport (const char *path);
bool NetRequestAssetAdd (const char *path);
bool NetRequestAssetObject (const char *path, Vector2 position,
                            NetLockDoneFn done);
bool NetRequestObjectCreate (Vector2 center, NetLockDoneFn done);
bool NetRequestLock (u64 id, NetLockDoneFn done);
bool NetClientLockPending (void);
bool NetPublishCursor (Vector2 worldPos);
bool NetLockObject (u64 id);
/* 用户主动释放锁；服务端会先执行物件的 touched 回调。 */
bool NetUnlockObject (u64 id);
/* 内部静默释放锁；不触发 touched。 */
bool NetUnlockObjectSilent (u64 id);

u64 NetClientUserId (void);
/* 当前客户端持有的唯一物件锁；没有锁时为 0。 */
u64 NetClientLockedObjectId (void);
/* 返回最近一次普通脚本 untouch 导致掉锁的物件 ID。 */
u64 NetClientLockLostId (void);
/* 每次客户端应用全量快照时递增，用于发现阻塞请求期间发生的场景重建。 */
u64 NetClientSceneVersion (void);

#endif
