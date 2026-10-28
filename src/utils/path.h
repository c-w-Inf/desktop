#ifndef UTILS_PATH_H
#define UTILS_PATH_H

#include <stdbool.h>

/**
 * @brief 判断路径扩展名是否匹配。
 * @param path 文件路径。
 * @param extension 带点的扩展名，大小写不敏感。
 * @return 匹配时为 true。
 */
bool PathHasExtension (const char *path, const char *extension);
/**
 * @brief 判断路径是否为支持的图片文件。
 * @param path 文件路径。
 * @return 是支持的图片时为 true。
 */
bool PathIsImage (const char *path);

#endif
