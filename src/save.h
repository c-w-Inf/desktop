#ifndef SAVE_H
#define SAVE_H

#include <stdbool.h>

#include "script.h"

#define SAVE_INTERVAL_SECONDS 30

// 将服务端当前内容原子写入 ZIP 结构的 path。globalEnv 为服务端全局脚本环境。
bool SaveWrite (const char *path, ScriptEnv *globalEnv);

// 从 path 读取 ZIP 存档。成功时 nextEntityId 为下一个可用实体 ID。
// 用户环境和用户菜单不落盘。
bool SaveRead (const char *path, ScriptEnv *globalEnv, u64 *nextEntityId);

/* 增量导入：合并全局环境、素材、物件（含物件环境）和菜单。
   重名项不替换并写入 conflicts；allocId 提供新实体 ID。 */
typedef u64 (*SaveIdAllocator) (void *context);
bool SaveMerge (const char *path, ScriptEnv *globalEnv,
                SaveIdAllocator allocId, void *context, TextBuffer *conflicts);

#endif
