#ifndef SCRIPT_H
#define SCRIPT_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "utils.h"

typedef struct Env ScriptEnv;

// 创建全局环境。self 0 直接使用它，其他 self 环境以它为父环境。
ScriptEnv *ScriptEnvCreate (void);
void ScriptEnvDestroy (ScriptEnv *env);

// 将 id 对应环境写入 file。id == 0 使用 globalEnv；没有环境时不写入且 written=false。
// 写入内容为 u8 ownerKind + u32 小端长度前缀的 ownerName + u32 小端长度前缀的环境 JSON。
// 返回 false 表示序列化或写入失败。
bool ScriptEnvWrite (FILE *file, ScriptEnv *globalEnv, u64 id, uint8_t ownerKind,
                     const char *ownerName, bool *written);

// 用 JSON 对象文本替换 id 对应环境（id == 0 为 globalEnv）的本地变量。
bool ScriptEnvLoadJson (ScriptEnv *globalEnv, u64 id, const char *json);

// 将 JSON 对象中的变量合并进 env 的本地变量；已存在的名称不替换，并把名称逐行写入 conflicts。
bool ScriptEnvMergeJson (ScriptEnv *env, const char *json, TextBuffer *conflicts);

// 将 id 对应环境的本地变量序列化为 JSON 对象文本；没有环境或序列化失败时返回 NULL。
char *ScriptEnvJson (ScriptEnv *globalEnv, u64 id);

// 执行脚本；有非 null 返回值时 resultJson 为 JSON 字符串，否则为 NULL。返回空 TextBuffer 表示成功。
TextBuffer ScriptRun (u64 self, const char *source, ScriptEnv *env, char **resultJson);
TextBuffer ScriptRunFunction (u64 self, const char *functionText, ScriptEnv *env,
                              u64 caller, char **resultJson);

// 调用物件本地事件函数（ontouch/ondrag/dragged），userId 作为唯一参数。
// 事件不存在或为 null 时返回空 TextBuffer。
TextBuffer ScriptRunObjectEvent (u64 self, const char *event, u64 userId,
                                 ScriptEnv *env);

#endif
