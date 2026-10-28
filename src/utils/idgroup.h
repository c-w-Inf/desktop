#ifndef UTILS_ID_GROUP_H
#define UTILS_ID_GROUP_H

#include <stdbool.h>

#include "types.h"

/**
 * @brief 动态 u64 ID 数组。
 */
typedef struct IdGroup {
    u64 *ids;
    int count;
    int capacity;
} IdGroup;

/**
 * @brief 创建空 ID 组。
 * @return 空 ID 组。
 */
IdGroup IdGroupCreate (void);
/**
 * @brief 释放 ID 组存储并清零。
 * @param group ID 组。
 */
void IdGroupUnload (IdGroup *group);
/**
 * @brief 清空元素但保留存储。
 * @param group ID 组。
 */
void IdGroupClear (IdGroup *group);
/**
 * @brief 向 ID 组追加一个 ID。
 * @param group ID 组。
 * @param id 要追加的 ID。
 * @return 成功时为 true。
 */
bool IdGroupAdd (IdGroup *group, u64 id);
/**
 * @brief 删除指定下标的 ID。
 * @param group ID 组。
 * @param index 下标。
 * @return 下标有效时为 true。
 */
bool IdGroupRemoveAt (IdGroup *group, int index);
/**
 * @brief 移动 ID 到新下标。
 * @param group ID 组。
 * @param from 原下标。
 * @param to 新下标。
 * @return 下标有效时为 true。
 */
bool IdGroupMove (IdGroup *group, int from, int to);
/**
 * @brief 用 ID 数组替换 ID 组内容。
 * @param group ID 组。
 * @param ids 源 ID 数组。
 * @param count 元素数量。
 * @return 复制成功时为 true。
 */
bool IdGroupCopy (IdGroup *group, const u64 *ids, int count);

#endif
