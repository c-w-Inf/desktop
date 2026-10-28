#ifndef NETCLIENT_H
#define NETCLIENT_H

#include <stdbool.h>

#include "assets.h"
#include "object.h"
#include "utils.h"

/**
 * @brief 异步开始连接服务器。
 * @param host 主机名或 IP。
 * @param port 端口。
 * @param username 非空用户名。
 * @return 成功提交连接时为 true。
 */
bool NetClientConnect (const char *host, int port, const char *username);
/**
 * @brief 检查连接握手是否完成。
 * @return 已连接时为 true。
 */
bool NetClientConnected (void);
/**
 * @brief 立即断线并清理客户端网络状态。
 */
void NetClientDisconnect (void);

/**
 * @brief 请求重命名素材。
 * @param id 素材 ID。
 * @param name 新名称。
 * @return 请求成功提交时为 true。
 */
bool NetRequestAssetRename (u64 id, const char *name);
/**
 * @brief 请求替换素材图片。
 * @param id 素材 ID。
 * @param file 新图片文件。
 * @return 请求成功提交时为 true。
 */
bool NetRequestAssetReplace (u64 id, RawFile file);
/**
 * @brief 请求删除素材。
 * @param id 素材 ID。
 * @return 请求成功提交时为 true。
 */
bool NetRequestAssetDelete (u64 id);
/**
 * @brief 向服务端提交物件完整状态。
 * @param object 要同步的物件。
 * @return 请求成功提交时为 true。
 */
bool NetPublishObjectUpsert (const Object *object);

/**
 * @brief 提交拖动增量并触发服务端 ondrag。
 * @param id 物件 ID。
 * @param delta 世界坐标位移。
 * @return 请求成功提交时为 true。
 */
bool NetRequestDragUpdate (u64 id, Vector2 delta);

/**
 * @brief 通知服务端拖动已结束。
 * @param id 物件 ID。
 * @return 请求成功提交时为 true。
 */
bool NetRequestDragEnd (u64 id);

/**
 * @brief 请求删除物件。
 * @param id 物件 ID。
 * @return 请求成功提交时为 true。
 */
bool NetRequestObjectDelete (u64 id);

/**
 * @brief 提交脚本。
 * @param source 脚本源码。
 * @param selfExpr self 表达式，空串表示 self=0。
 * @return 请求成功提交时为 true。
 */
bool NetRequestScript (const char *source, const char *selfExpr);
/**
 * @brief 请求执行菜单项。
 * @param id 0 为全局，否则为用户或物件 ID。
 * @param index 菜单项下标。
 * @param name 菜单显示名称。
 * @return 请求成功提交时为 true。
 */
bool NetRequestMenu (u64 id, int index, const char *name);

/**
 * @brief 读取文件或文件夹并发送给服务端导入。
 * @param path 本地路径；文件夹会先打包为 .dsk。
 * @return 请求成功提交时为 true。
 */
bool NetRequestImport (const char *path);
/**
 * @brief 读取图片并请求服务端添加素材。
 * @param path 图片文件路径。
 * @return 请求成功提交时为 true。
 */
bool NetRequestAssetAdd (const char *path);
/**
 * @brief 读取图片并请求创建使用该图片的物件。
 * @param path 图片文件路径。
 * @param position 物件中心世界坐标。
 * @return 请求成功提交时为 true。
 */
bool NetRequestAssetObject (const char *path, Vector2 position);
/**
 * @brief 请求创建空物件并锁定给当前用户。
 * @param center 新物件中心世界坐标。
 * @return 请求成功提交时为 true。
 */
bool NetRequestObjectCreate (Vector2 center);
/**
 * @brief 异步申请物件锁。
 * @param id 物件 ID。
 * @return 请求成功提交时为 true。
 */
bool NetRequestLock (u64 id);
/**
 * @brief 广播当前用户光标位置。
 * @param worldPos 世界坐标。
 * @return 请求成功提交时为 true。
 */
bool NetPublishCursor (Vector2 worldPos);
/**
 * @brief 主动释放锁，服务端先执行 touched 回调。
 * @param id 物件 ID。
 * @return 请求成功提交时为 true。
 */
bool NetUnlockObject (u64 id);

/**
 * @brief 获取当前客户端用户 ID。
 * @return 用户 ID，未连接时为 0。
 */
u64 NetClientUserId (void);

/**
 * @brief 获取当前持有的物件锁。
 * @return 锁定的物件 ID，没有锁时为 0。
 */
u64 NetClientLockedObjectId (void);



#endif
