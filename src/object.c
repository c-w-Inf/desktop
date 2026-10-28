#include "object.h"

#include "entity.h"
#include "menu_array.h"
#include "strmap.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static StrMap *objectNames;
static u64 timestampCounter = 0;

struct ObjectVisibility {
    u64 *userIds;
    bool *hidden;
    int count;
    int capacity;
};

static void VisibilityFree (ObjectVisibility *visibility) {
    if (visibility == NULL) return;
    free (visibility->userIds);
    free (visibility->hidden);
    free (visibility);
}

static bool VisibilityReserve (ObjectVisibility *visibility, int needed) {
    if (needed <= visibility->capacity) return true;
    int capacity = visibility->capacity == 0 ? 4 : visibility->capacity * 2;
    while (capacity < needed) capacity *= 2;

    u64 *userIds = malloc ((size_t)capacity * sizeof (u64));
    bool *hidden = malloc ((size_t)capacity * sizeof (bool));
    if (userIds == NULL || hidden == NULL) {
        free (userIds);
        free (hidden);
        return false;
    }
    if (visibility->count > 0) {
        memcpy (userIds, visibility->userIds,
                (size_t)visibility->count * sizeof (u64));
        memcpy (hidden, visibility->hidden,
                (size_t)visibility->count * sizeof (bool));
    }
    free (visibility->userIds);
    free (visibility->hidden);
    visibility->userIds = userIds;
    visibility->hidden = hidden;
    visibility->capacity = capacity;
    return true;
}

bool ObjectMenuAdd (Object* object, const char* name, const char* func) {
    if (object == NULL || name == NULL) return false;
    return MenuArrayAdd (&object->menuNames, &object->menuFuncs, &object->menuSize,
                         &object->menuCapacity, name, func);
}

void ObjectMenuClear (Object* object) {
    if (object == NULL) return;
    MenuArrayClear (&object->menuNames, &object->menuFuncs, &object->menuSize);
}

int ObjectMenuCount (const Object* object) {
    return object != NULL ? object->menuSize : 0;
}

const char* ObjectMenuName (const Object* object, int index) {
    if (object == NULL || index < 0 || index >= object->menuSize) return NULL;
    return object->menuNames[index];
}

const char* ObjectMenuFunc (const Object* object, int index) {
    if (object == NULL || object->menuFuncs == NULL ||
        index < 0 || index >= object->menuSize) return NULL;
    return object->menuFuncs[index];
}

bool ObjectMenuSetNames (Object* object, const char* const* names, int count) {
    if (object == NULL) return false;
    return MenuArraySetNames (&object->menuNames, &object->menuFuncs,
                              &object->menuSize, &object->menuCapacity,
                              names, count);
}

static u64 NextTimestamp (void) {
    return ++timestampCounter;
}

bool ObjectNameUsed (const char *name, const Object *exclude) {
    void *existing = NULL;
    if (objectNames == NULL || !StrMapGet (objectNames, name, &existing)) return false;
    return exclude == NULL || existing != exclude;
}

Object *ObjectFindByName (const char *name) {
    void *object = NULL;
    if (name == NULL || objectNames == NULL ||
        !StrMapGet (objectNames, name, &object))
        return NULL;
    return object;
}

Object *ObjectFindById (u64 id) {
    if (id == 0 || objectNames == NULL) return NULL;
    int total = StrMapCount (objectNames);
    for (int i = 0; i < total; i++) {
        Object *object = NULL;
        if (StrMapAt (objectNames, i, NULL, (void **)&object) &&
            object != NULL && object->id == id)
            return object;
    }
    return NULL;
}

static bool RegisterObject (Object *object) {
    if (object == NULL) return false;
    if (objectNames == NULL) objectNames = StrMapCreate ();
    if (objectNames == NULL) return false;
    if (!StrMapAdd (objectNames, object->name, object)) return false;
    if (!EntityRegister (object->id, object)) {
        StrMapRemove (objectNames, object->name);
        return false;
    }
    return true;
}

static bool RemoveObject (Object *object) {
    if (object == NULL) return false;
    StrMapRemove (objectNames, object->name);
    EntityUnregister (object->id);
    return true;
}

void ObjectInit (void) {
    if (objectNames != NULL) StrMapDestroy (objectNames);
    objectNames = StrMapCreate ();
    timestampCounter = 0;
}

IdGroup ObjectIds (void) {
    IdGroup group = IdGroupCreate ();
    int total = StrMapCount (objectNames);
    for (int i = 0; i < total; i++) {
        Object *object = NULL;
        if (StrMapAt (objectNames, i, NULL, (void **)&object) && object != NULL)
            IdGroupAdd (&group, object->id);
    }
    return group;
}

bool ObjectContains (const Object *object) {
    return object != NULL && EntityFind (object->id) == object;
}

Object *ObjectAdd (const char *name, Vector2 center, int layer,
                                        u64 timestamp, u64 id) {
    if (name == NULL || name[0] == '\0') return NULL;
    if (ObjectNameUsed (name, NULL)) return NULL;
    if (id == 0) return NULL;
    if (EntityFind (id) != NULL) return NULL;

    Object *object = malloc (sizeof (Object));
    if (object == NULL) return NULL;

    object->id = id;
    object->pos = center;
    object->lockOwnerId = 0;
    object->assets = IdGroupCreate ();
    object->assetIndex = 0;
    object->menuNames = NULL;
    object->menuFuncs = NULL;
    object->menuSize = 0;
    object->menuCapacity = 0;
    object->layer = layer;
    object->timestamp = timestamp;
    object->hidden = false;
    object->visibility = NULL;
    if (timestamp > timestampCounter) timestampCounter = timestamp;
    snprintf (object->name, sizeof (object->name), "%s", name);

    if (!RegisterObject (object)) {
        free (object);
        return NULL;
    }

    return object;
}

IdGroup* ObjectAssets (Object *object) {
    return (object != NULL) ? &object->assets : NULL;
}

Asset* ObjectDisplayedAsset (const Object *object) {
    if (object == NULL || object->assets.count == 0) return NULL;
    int index = object->assetIndex;
    if (index < 0 || index >= object->assets.count) index = 0;
    return EntityFind (object->assets.ids[index]);
}

int ObjectCompareDrawOrder (const Object *a, const Object *b) {
    if (a->layer != b->layer) return (a->layer > b->layer) ? -1 : 1;
    if (a->timestamp != b->timestamp) return (a->timestamp < b->timestamp) ? -1 : 1;
    return 0;
}

Object *ObjectPick (Vector2 world) {
    Object *top = NULL;

    IdGroup ids = ObjectIds ();
    for (int i = 0; i < ids.count; i++) {
        Object *object = EntityFind (ids.ids[i]);
        if (object == NULL || object->hidden || object->assets.count == 0) continue;

        Asset *asset = ObjectDisplayedAsset (object);
        if (asset == NULL) continue;

        float left = object->pos.x - asset->width / 2.0f;
        float topEdge = object->pos.y - asset->height / 2.0f;
        int x = (int)floorf (world.x - left);
        int y = (int)floorf (world.y - topEdge);

        // 只有 a 通道恰好为 255 的像素才算命中，不使用阈值
        bool hit = AssetIsOpaqueAt (asset, x, y);

        if (hit && (top == NULL || ObjectCompareDrawOrder (object, top) > 0)) top = object;
    }
    IdGroupUnload (&ids);

    return top;
}

bool ObjectRename (Object *object, const char *newName) {
    if (object == NULL || newName == NULL || newName[0] == '\0') return false;

    char name[sizeof (object->name)];
    snprintf (name, sizeof (name), "%s", newName);
    if (strcmp (object->name, name) == 0) return true;
    if (ObjectNameUsed (name, object)) return false;
    if (!StrMapAdd (objectNames, name, object)) return false;
    if (!StrMapRemove (objectNames, object->name)) {
        StrMapRemove (objectNames, name);
        return false;
    }

    snprintf (object->name, sizeof (object->name), "%s", name);
    ObjectTouch (object);
    return true;
}

bool ObjectDelete (Object *object) {
    if (object == NULL) return false;
    if (!RemoveObject (object)) return false;

    MenuArrayFree (&object->menuNames, &object->menuFuncs, &object->menuSize,
                   &object->menuCapacity);
    IdGroupUnload (&object->assets);
    VisibilityFree (object->visibility);
    free (object);
    return true;
}

bool ObjectIsHiddenForUser (const Object *object, u64 userId) {
    if (object == NULL) return false;
    if (object->visibility == NULL) return object->hidden;
    for (int i = 0; i < object->visibility->count; i++) {
        if (object->visibility->userIds[i] == userId)
            return object->visibility->hidden[i];
    }
    return object->hidden;
}

bool ObjectSetHiddenForUser (Object *object, u64 userId, bool hidden) {
    if (object == NULL) return false;
    if (object->visibility == NULL) {
        object->visibility = calloc (1, sizeof (ObjectVisibility));
        if (object->visibility == NULL) return false;
    }

    ObjectVisibility *visibility = object->visibility;
    for (int i = 0; i < visibility->count; i++) {
        if (visibility->userIds[i] == userId) {
            visibility->hidden[i] = hidden;
            return true;
        }
    }
    if (!VisibilityReserve (visibility, visibility->count + 1)) return false;
    visibility->userIds[visibility->count] = userId;
    visibility->hidden[visibility->count] = hidden;
    visibility->count++;
    return true;
}

void ObjectSetHidden (Object *object, bool hidden) {
    if (object == NULL) return;
    VisibilityFree (object->visibility);
    object->visibility = NULL;
    object->hidden = hidden;
}

void ObjectTouch (Object *object) {
    if (object == NULL) return;

    // 外部同步可能写入更大的时间戳，先让计数器追上当前最大时间戳。
    int total = StrMapCount (objectNames);
    for (int i = 0; i < total; i++) {
        Object *other = NULL;
        if (StrMapAt (objectNames, i, NULL, (void **)&other) && other != NULL &&
            other->timestamp > timestampCounter)
            timestampCounter = other->timestamp;
    }
    object->timestamp = NextTimestamp ();
}

bool ObjectSetLayer (Object *object, int layer) {
    if (object == NULL) return false;
    object->layer = layer;
    ObjectTouch (object);
    return true;
}

void ObjectMove (Object *object, Vector2 pos) {
    if (object != NULL) object->pos = pos;
}

void ObjectUnload (void) {
    int count = StrMapCount (objectNames);
    for (int i = 0; i < count; i++) {
        Object *object = NULL;
        if (!StrMapAt (objectNames, i, NULL, (void **)&object) || object == NULL)
            continue;
        EntityUnregister (object->id);
        MenuArrayFree (&object->menuNames, &object->menuFuncs, &object->menuSize,
                       &object->menuCapacity);
        IdGroupUnload (&object->assets);
        VisibilityFree (object->visibility);
        free (object);
    }
    StrMapDestroy (objectNames);
    objectNames = NULL;
    timestampCounter = 0;
}

