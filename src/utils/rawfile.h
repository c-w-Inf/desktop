#ifndef UTILS_RAW_FILE_H
#define UTILS_RAW_FILE_H

#include <stddef.h>

/**
 * @brief 内存中的原始文件。
 */
typedef struct RawFile {
    unsigned char *data; /**< 文件内容，调用方通过 RawFileFree 释放。 */
    size_t size; /**< 文件字节数。 */
    char ext[8]; /**< 文件扩展名，包含点。 */
} RawFile;

/**
 * @brief 读取整个文件到内存。
 * @param path 文件路径。
 * @return 文件内容；失败时 data 为 NULL。
 */
RawFile RawFileRead (const char *path);
/**
 * @brief 释放 RawFile 的内容并清零。
 * @param file 要释放的文件。
 */
void RawFileFree (RawFile *file);

#endif
