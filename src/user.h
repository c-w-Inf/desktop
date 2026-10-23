#ifndef USER_H
#define USER_H

#include <stdbool.h>
#include "utils.h"
#include "raylib.h"

typedef struct User {
    u64 id;
    char name[128];
    bool hasCursor;
    float x;
    float y;
    uint32_t color; // RGBA
    bool canscript;
    char **menuNames;
    char **menuFuncs;
    int menuSize;
    int menuCapacity;
} User;

void UserInit (void);
void UserUnload (void);

User *UserFind (u64 id);
User *UserAdd (u64 id, const char *name);
bool UserRemove (u64 id);
void UserSetCursor (u64 id, bool hasCursor, float x, float y);
void UserSetColor (u64 id, uint32_t color);
void UserSetCanScript (u64 id, bool canscript);
Color UserColor (const User *user);

/* 返回所有在线用户 ID 组，调用方负责 IdGroupUnload。 */
IdGroup UserIds (void);

#endif
