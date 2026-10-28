#ifndef USER_H
#define USER_H

#include <stdbool.h>
#include "utils.h"
#include "raylib.h"

/**
 * @brief 在线用户及其光标、颜色、脚本权限和菜单。
 */
typedef struct User {
    u64 id;
    char name[128];
    bool hasCursor;
    float x;
    float y;
    uint32_t color; /**< RGBA 颜色。 */
    bool canscript;
    char **menuNames;
#ifdef DESKTOP_SERVER
    char **menuFuncs; /**< 菜单函数数组。 */
#endif
    int menuSize;
    int menuCapacity;
} User;

/**
 * @brief 初始化用户管理器。
 */
void UserInit (void);
/**
 * @brief 销毁全部用户并释放菜单。
 */
void UserUnload (void);

/**
 * @brief 创建并注册用户。
 * @param id 非 0 且未占用的用户 ID。
 * @param name 非空用户名。
 * @return 新用户，失败时为 NULL。
 */
User *UserAdd (u64 id, const char *name);
/**
 * @brief 删除用户并释放其菜单。
 * @param id 用户 ID。
 * @return 删除成功时为 true。
 */
bool UserRemove (u64 id);
/**
 * @brief 更新用户光标状态和世界坐标。
 * @param id 用户 ID。
 * @param hasCursor 是否显示光标。
 * @param x 世界坐标 x。
 * @param y 世界坐标 y。
 */
void UserSetCursor (u64 id, bool hasCursor, float x, float y);
/**
 * @brief 设置用户 RGBA 颜色。
 * @param id 用户 ID。
 * @param color RGBA 颜色。
 */
void UserSetColor (u64 id, uint32_t color);
/**
 * @brief 设置用户是否允许提交脚本。
 * @param id 用户 ID。
 * @param canscript 是否允许。
 */
void UserSetCanScript (u64 id, bool canscript);
/**
 * @brief 将用户颜色转换为 raylib Color。
 * @param user 用户。
 * @return raylib 颜色；user 为 NULL 时为黑色。
 */
Color UserColor (const User *user);

/**
 * @brief 返回全部在线用户 ID。
 * @return ID 组，调用方负责 IdGroupUnload。
 */
IdGroup UserIds (void);

#endif
