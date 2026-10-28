#ifndef STRMAP_H
#define STRMAP_H

#include <stdbool.h>

/**
 * @brief 不透明的字符串键哈希表；键会被复制，值由调用方管理。
 */
typedef struct StrMap StrMap;

/**
 * @brief 创建空哈希表。
 * @return 新哈希表，失败时为 NULL。
 */
StrMap *StrMapCreate (void);
/**
 * @brief 销毁哈希表及其复制的键，不释放值。
 * @param map 哈希表。
 */
void StrMapDestroy (StrMap *map);

/**
 * @brief 插入新键值；键已存在时不覆盖。
 * @param map 哈希表。
 * @param key 非空键，内部复制。
 * @param value 值指针。
 * @return 插入成功时为 true。
 */
bool StrMapAdd (StrMap *map, const char *key, void *value);
/**
 * @brief 按键取值。
 * @param map 哈希表。
 * @param key 键。
 * @param value 输出值指针。
 * @return 键存在时为 true。
 */
bool StrMapGet (const StrMap *map, const char *key, void **value);
/**
 * @brief 获取键对应值槽的地址。
 * @param map 哈希表。
 * @param key 键。
 * @return 值槽地址，键不存在时为 NULL。
 */
void **StrMapSlot (StrMap *map, const char *key);
/**
 * @brief 删除键及其复制的键字符串。
 * @param map 哈希表。
 * @param key 键。
 * @return 键存在并删除成功时为 true。
 */
bool StrMapRemove (StrMap *map, const char *key);

/**
 * @brief 获取键值对数量。
 * @param map 哈希表。
 * @return 元素数量。
 */
int StrMapCount (const StrMap *map);

/**
 * @brief 按升序键顺序访问第 index 个元素。
 * @param map 哈希表。
 * @param index 下标。
 * @param key 输出键，可为 NULL。
 * @param value 输出值，可为 NULL。
 * @return 下标有效时为 true。
 */
bool StrMapAt (StrMap *map, int index, const char **key, void **value);

#endif
