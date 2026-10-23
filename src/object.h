#ifndef OBJECT_H
#define OBJECT_H

#include <stdbool.h>

#include "assets.h"
#include "raylib.h"
#include "utils.h"

typedef struct ObjectVisibility ObjectVisibility;

typedef struct Object {
    u64 id;
    char name[256];
    Vector2 pos;    // 物件中心
    IdGroup assets; // 素材 ID 组
    int assetIndex; // 当前显示的素材下标
    char **menuNames;    // 菜单名称，客户端只需要名称
    char **menuFuncs;    // 菜单函数，仅服务端使用
    int menuSize;
    int menuCapacity;
    int layer;  // 层数
    u64 timestamp;
    bool hidden; // 未启用按用户可见度时的全局隐藏状态
    ObjectVisibility *visibility; // 非 NULL 时启用按用户可见度
    u64 lockOwnerId;
} Object;

void ObjectInit (void);
IdGroup ObjectIds (void);
bool ObjectContains (const Object* object);
bool ObjectNameUsed (const char* name, const Object* exclude);
Object* ObjectFindByName (const char* name);
Object* ObjectFindById (u64 id);

Object* ObjectAdd (const char* name, Vector2 center, int layer, u64 timestamp, u64 id);
IdGroup* ObjectAssets (Object* object);
Asset* ObjectDisplayedAsset (const Object* object);

bool ObjectRename (Object* object, const char* newName);
bool ObjectDelete (Object* object);
void ObjectTouch (Object* object);
bool ObjectSetLayer (Object* object, int layer);
bool ObjectIsHiddenForUser (const Object* object, u64 userId);
bool ObjectSetHiddenForUser (Object* object, u64 userId, bool hidden);
void ObjectSetHidden (Object* object, bool hidden);

Object* ObjectPick (Vector2 world);
int ObjectCompareDrawOrder (const Object* a, const Object* b);
void ObjectMove (Object* object, Vector2 pos);
void ObjectUnload (void);

#endif
