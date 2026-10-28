#ifndef OBJECT_H
#define OBJECT_H

#include <stdbool.h>

#include "assets.h"
#include "raylib.h"
#include "utils.h"

/**
 * @brief 不透明的按用户可见度数据。
 */
typedef struct ObjectVisibility ObjectVisibility;

/**
 * @brief 画布物件，包含位置、素材、菜单、层级和锁状态。
 */
typedef struct Object {
    u64 id; /**< 物件 ID。 */
    char name[256]; /**< 显示名称。 */
    Vector2 pos; /**< 物件中心的世界坐标。 */
    IdGroup assets; /**< 素材 ID 组。 */
    int assetIndex; /**< 当前显示素材下标。 */
    char **menuNames; /**< 菜单名称数组。 */
#ifdef DESKTOP_SERVER
    char **menuFuncs; /**< 菜单函数数组。 */
#endif
    int menuSize; /**< 菜单项数量。 */
    int menuCapacity; /**< 菜单数组容量。 */
    int layer; /**< 绘制层级。 */
    u64 timestamp; /**< 同一层内的绘制顺序时间戳。 */
    bool hidden; /**< 全局隐藏状态。 */
#ifdef DESKTOP_SERVER
    ObjectVisibility *visibility; /**< 非 NULL 时启用按用户可见度覆盖。 */
#endif
    u64 lockOwnerId; /**< 占用者用户 ID，0 表示未占用。 */
} Object;

/**
 * @brief 初始化物件管理器。
 */
void ObjectInit (void);
/**
 * @brief 返回全部物件 ID。
 * @return ID 组，调用方负责 IdGroupUnload。
 */
IdGroup ObjectIds (void);
/**
 * @brief 检查物件指针是否仍注册在管理器中。
 * @param object 物件指针。
 * @return 仍存在时为 true。
 */
bool ObjectContains (const Object* object);
/**
 * @brief 检查物件名称是否已被占用。
 * @param name 待检查名称。
 * @param exclude 可忽略的物件，可为 NULL。
 * @return 被其他物件占用时为 true。
 */
bool ObjectNameUsed (const char* name, const Object* exclude);
/**
 * @brief 按名称查找物件。
 * @param name 物件名称。
 * @return 物件，不存在时为 NULL。
 */
Object* ObjectFind (const char* name);

/**
 * @brief 创建并注册物件。
 * @param name 唯一物件名称。
 * @param center 物件中心世界坐标。
 * @param layer 初始层级。
 * @param timestamp 初始时间戳。
 * @param id 非 0 且未占用的物件 ID。
 * @return 新物件，失败时为 NULL。
 */
Object* ObjectAdd (const char* name, Vector2 center, int layer, u64 timestamp, u64 id);
/**
 * @brief 获取物件的素材 ID 组。
 * @param object 物件。
 * @return 素材 ID 组指针，object 为 NULL 时为 NULL。
 */
IdGroup* ObjectAssets (Object* object);
/**
 * @brief 获取物件当前显示的素材。
 * @param object 物件。
 * @return 当前素材，没有素材或素材不存在时为 NULL。
 */
Asset* ObjectDisplayedAsset (const Object* object);

/**
 * @brief 修改物件名称并更新时间戳。
 * @param object 物件。
 * @param newName 唯一新名称。
 * @return 成功或名称未变时为 true。
 */
bool ObjectRename (Object* object, const char* newName);
/**
 * @brief 从管理器删除并释放物件。
 * @param object 物件。
 * @return 删除成功时为 true。
 */
bool ObjectDelete (Object* object);
/**
 * @brief 将物件时间戳更新为当前最新值，用于置顶绘制。
 * @param object 物件。
 */
void ObjectTouch (Object* object);
/**
 * @brief 设置物件层级并更新时间戳。
 * @param object 物件。
 * @param layer 新层级。
 * @return 成功时为 true。
 */
bool ObjectSetLayer (Object* object, int layer);
/**
 * @brief 判断物件对指定用户是否隐藏。
 * @param object 物件。
 * @param userId 用户 ID。
 * @return 对该用户隐藏时为 true。
 */
bool ObjectIsHiddenForUser (const Object* object, u64 userId);
/**
 * @brief 设置物件对指定用户的可见度覆盖。
 * @param object 物件。
 * @param userId 用户 ID。
 * @param hidden 是否隐藏。
 * @return 设置成功时为 true。
 */
bool ObjectSetHiddenForUser (Object* object, u64 userId, bool hidden);
/**
 * @brief 设置全局隐藏状态并清除按用户覆盖。
 * @param object 物件。
 * @param hidden 是否隐藏。
 */
void ObjectSetHidden (Object* object, bool hidden);

/**
 * @brief 拾取世界坐标下最上层的可见物件。
 * @param world 世界坐标。
 * @return 命中的物件，未命中时为 NULL。
 */
Object* ObjectPick (Vector2 world);
/**
 * @brief 比较两个物件的绘制顺序。
 * @param a 第一个物件。
 * @param b 第二个物件。
 * @return 排序比较结果，负数表示 a 排在 b 前。
 */
int ObjectCompareDrawOrder (const Object* a, const Object* b);
/**
 * @brief 直接设置物件中心位置。
 * @param object 物件。
 * @param pos 新世界坐标。
 */
void ObjectMove (Object* object, Vector2 pos);
/**
 * @brief 删除并释放全部物件。
 */
void ObjectUnload (void);

#endif
