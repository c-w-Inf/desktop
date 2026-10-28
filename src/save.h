#ifndef SAVE_H
#define SAVE_H

#include <stdbool.h>

#include "script.h"

/**
 * @brief 自动保存间隔秒数。
 */
#define SAVE_INTERVAL_SECONDS 30

/**
 * @brief 将当前场景原子写入 ZIP 格式的 .dsk。
 * @param path 目标文件路径。
 * @param globalEnv 服务端全局脚本环境。
 * @return 写入成功时为 true。
 */
bool SaveWrite (const char *path, ScriptEnv *globalEnv);

/**
 * @brief 读取 .dsk 存档并替换当前场景。
 * @param path 存档路径。
 * @param globalEnv 服务端全局脚本环境。
 * @param nextEntityId 输出下一个可用实体 ID。
 * @return 读取成功时为 true；用户环境和用户菜单不落盘。
 */
bool SaveRead (const char *path, ScriptEnv *globalEnv, u64 *nextEntityId);

/**
 * @brief 增量导入时分配新实体 ID 的回调。
 * @param context 调用方上下文。
 * @return 新实体 ID。
 */
typedef u64 (*SaveIdAllocator) (void *context);
/**
 * @brief 增量导入并合并场景内容。
 * @param path .dsk 存档路径。
 * @param globalEnv 服务端全局脚本环境。
 * @param allocId 新实体 ID 分配回调。
 * @param context 传给回调的上下文。
 * @param conflicts 输出重名项，逐行追加。
 * @return 合并流程完成时为 true，存在重名项也可能为 true。
 */
bool SaveMerge (const char *path, ScriptEnv *globalEnv,
                SaveIdAllocator allocId, void *context, TextBuffer *conflicts);

/**
 * @brief 将文件夹压缩为 .dsk 内存文件。
 * @param path 文件夹路径。
 * @return 压缩文件；失败时 data 为 NULL，调用方用 RawFileFree 释放。
 */
RawFile SavePackFolderData (const char *path);

#endif
