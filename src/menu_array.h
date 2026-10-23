#ifndef MENU_ARRAY_H
#define MENU_ARRAY_H

#include <stdbool.h>

/* 菜单名称/函数数组的内部公共操作；仅服务端保留函数字符串。 */
bool MenuArrayAdd (char ***names, char ***funcs, int *size, int *capacity,
                   const char *name, const char *func);
void MenuArrayClear (char ***names, char ***funcs, int *size);
void MenuArrayFree (char ***names, char ***funcs, int *size, int *capacity);
bool MenuArraySetNames (char ***names, char ***funcs, int *size, int *capacity,
                        const char *const *values, int count);

#endif
