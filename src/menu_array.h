#ifndef MENU_ARRAY_H
#define MENU_ARRAY_H

#include <stdbool.h>

/**
 * @brief 向菜单名称/函数数组追加一项。
 * @param names 名称数组。
 * @param funcs 函数数组。
 * @param size 当前元素数。
 * @param capacity 当前容量。
 * @param name 菜单名称，不能为 NULL。
 * @param func 菜单函数文本，可为 NULL。
 * @return 追加成功时为 true。
 */
bool MenuArrayAdd (char ***names, char ***funcs, int *size, int *capacity,
                   const char *name, const char *func);
/**
 * @brief 释放全部名称和函数字符串，保留数组存储。
 * @param names 名称数组。
 * @param funcs 函数数组。
 * @param size 当前元素数，清零。
 */
void MenuArrayClear (char ***names, char ***funcs, int *size);
/**
 * @brief 释放全部字符串和数组存储。
 * @param names 名称数组。
 * @param funcs 函数数组。
 * @param size 当前元素数，清零。
 * @param capacity 当前容量，清零。
 */
void MenuArrayFree (char ***names, char ***funcs, int *size, int *capacity);
/**
 * @brief 用名称列表替换菜单内容，并清空函数数组。
 * @param names 名称数组。
 * @param funcs 函数数组。
 * @param size 当前元素数。
 * @param capacity 当前容量。
 * @param values 新名称列表。
 * @param count 新名称数量。
 * @return 替换成功时为 true。
 */
bool MenuArraySetNames (char ***names, char ***funcs, int *size, int *capacity,
                        const char *const *values, int count);

#endif
