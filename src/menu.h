#ifndef MENU_H
#define MENU_H

#include <stdbool.h>
#include "utils.h"

/**
 * @brief 向全局、用户或物件菜单追加一项。
 * @param id 0 表示全局，否则为用户或物件 ID。
 * @param name 菜单显示名称。
 * @param func 菜单函数文本；服务端保存，客户端可为 NULL。
 * @return 追加成功时为 true。
 */
bool MenuAdd (u64 id, const char* name, const char* func);
/**
 * @brief 清空指定全局、用户或物件菜单。
 * @param id 0 表示全局，否则为用户或物件 ID。
 */
void MenuClear (u64 id);
/**
 * @brief 用名称列表替换菜单，并清空函数文本。
 * @param id 0 表示全局，否则为用户或物件 ID。
 * @param names 新名称列表。
 * @param count 名称数量。
 * @return 替换成功时为 true。
 */
bool MenuSetNames (u64 id, const char* const* names, int count);
/**
 * @brief 获取菜单项数量。
 * @param id 0 表示全局，否则为用户或物件 ID。
 * @return 菜单项数量，目标不存在时为 0。
 */
int MenuCount (u64 id);
/**
 * @brief 获取菜单项名称。
 * @param id 0 表示全局，否则为用户或物件 ID。
 * @param index 菜单项下标。
 * @return 名称字符串，越界或目标不存在时为 NULL。
 */
const char* MenuName (u64 id, int index);
/**
 * @brief 获取菜单项函数文本。
 * @param id 0 表示全局，否则为用户或物件 ID。
 * @param index 菜单项下标。
 * @return 函数文本，仅服务端可用；越界或不存在时为 NULL。
 */
const char* MenuFunc (u64 id, int index);

#endif
