#ifndef ENTITY_H
#define ENTITY_H

#include <stdbool.h>
#include "utils.h"

/**
 * @brief 实体附加数据的销毁回调。
 * @param data 要释放的数据指针。
 */
typedef void (*EntityDestroyFn) (void *data);

/**
 * @brief 实体类型，用于区分同 ID 空间中的素材、物件和用户。
 */
typedef enum EntityKind {
    ENTITY_KIND_NONE = 0,
    ENTITY_KIND_ASSET,
    ENTITY_KIND_OBJECT,
    ENTITY_KIND_USER
} EntityKind;

/**
 * @brief 注册实体 ID、类型与指针的映射。
 * @param id 非 0 且未注册的实体 ID。
 * @param ptr 实体指针，不能为 NULL。
 * @param kind 实体类型。
 * @return 注册成功时为 true。
 */
bool EntityRegister (u64 id, void* ptr, EntityKind kind);
/**
 * @brief 注销实体并触发附加数据销毁回调。
 * @param id 要注销的实体 ID。
 */
void EntityUnregister (u64 id);
/**
 * @brief 按 ID 查找实体。
 * @param id 实体 ID。
 * @return 实体指针，不存在时为 NULL。
 */
void* EntityFind (u64 id);
/**
 * @brief 查询实体类型。
 * @param id 实体 ID。
 * @return 实体类型，不存在时为 ENTITY_KIND_NONE。
 */
EntityKind EntityKindOf (u64 id);
/**
 * @brief 设置实体附加数据及销毁回调。
 * @param id 实体 ID。
 * @param data 附加数据，可为 NULL。
 * @param destroy 数据销毁回调，可为 NULL。
 * @return 实体存在并设置成功时为 true。
 */
bool EntitySetData (u64 id, void *data, EntityDestroyFn destroy);
/**
 * @brief 获取实体附加数据。
 * @param id 实体 ID。
 * @return 附加数据，实体或数据不存在时为 NULL。
 */
void* EntityGetData (u64 id);

/**
 * @brief 清空全部实体并触发附加数据销毁回调。
 */
void EntityUnload (void);

#endif
