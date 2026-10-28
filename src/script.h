#ifndef SCRIPT_H
#define SCRIPT_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "utils.h"

/**
 * @brief 不透明的脚本环境。
 */
typedef struct Env ScriptEnv;

/**
 * @brief 创建全局脚本环境。
 * @return 新环境，失败时为 NULL；其他 self 环境以它为父环境。
 */
ScriptEnv *ScriptEnvCreate (void);
/**
 * @brief 销毁脚本环境及其本地变量。
 * @param env 要销毁的环境。
 */
void ScriptEnvDestroy (ScriptEnv *env);


/**
 * @brief 用 JSON 对象替换指定环境的本地变量。
 * @param globalEnv 全局环境。
 * @param id 实体 ID，0 表示全局环境。
 * @param json JSON 对象文本。
 * @return 解析和替换成功时为 true。
 */
bool ScriptEnvLoadJson (ScriptEnv *globalEnv, u64 id, const char *json);

/**
 * @brief 将 JSON 对象中尚不存在的变量合并进环境。
 * @param env 目标环境。
 * @param json JSON 对象文本。
 * @param conflicts 输出重名变量名，每行一个，可为 NULL。
 * @return 合并成功时为 true。
 */
bool ScriptEnvMergeJson (ScriptEnv *env, const char *json, TextBuffer *conflicts);

/**
 * @brief 将环境本地变量序列化为 JSON 对象文本。
 * @param globalEnv 全局环境。
 * @param id 实体 ID，0 表示全局环境。
 * @return JSON 文本，调用方负责 free；环境不存在或失败时为 NULL。
 */
char *ScriptEnvJson (ScriptEnv *globalEnv, u64 id);

/**
 * @brief 执行一段脚本。
 * @param self 执行脚本的实体 ID，0 表示全局。
 * @param source 脚本源码。
 * @param env 脚本环境。
 * @param resultJson 输出返回值 JSON，无返回值时为 NULL；调用方负责 free。
 * @return 错误文本；长度为 0 表示执行成功。
 */
TextBuffer ScriptRun (u64 self, const char *source, ScriptEnv *env, char **resultJson);
/**
 * @brief 以指定 self 和调用者执行函数文本。
 * @param self 执行函数的目标实体 ID。
 * @param functionText 函数文本。
 * @param env 脚本环境。
 * @param caller 调用者实体 ID。
 * @param resultJson 输出返回值 JSON，无返回值时为 NULL；调用方负责 free。
 * @return 错误文本；长度为 0 表示执行成功。
 */
TextBuffer ScriptRunFunction (u64 self, const char *functionText, ScriptEnv *env,
                              u64 caller, char **resultJson);

/**
 * @brief 调用物件的本地事件函数。
 * @param self 物件 ID。
 * @param event 事件名，如 ontouch、ondrag、dragged。
 * @param userId 作为唯一参数传入的用户 ID。
 * @param env 脚本环境。
 * @return 错误文本；事件不存在或执行成功时长度为 0。
 */
TextBuffer ScriptRunObjectEvent (u64 self, const char *event, u64 userId,
                                 ScriptEnv *env);

#endif
