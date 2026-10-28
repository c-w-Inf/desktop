#ifndef UTILS_RANDOM_H
#define UTILS_RANDOM_H

#include "types.h"

/**
 * @brief 生成下一个伪随机数。
 * @return 64 位随机数。
 */
u64 RandomNext (void);
/**
 * @brief 生成小于 limit 的随机数。
 * @param limit 上界，必须大于 0。
 * @return [0, limit) 内的随机数。
 */
u64 RandomBounded (u64 limit);

#endif
