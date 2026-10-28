#include "menu.h"

#include "entity.h"
#include "object.h"
#include "user.h"

bool ObjectMenuAdd (Object* object, const char* name, const char* func);
void ObjectMenuClear (Object* object);
bool ObjectMenuSetNames (Object* object, const char* const* names, int count);
int ObjectMenuCount (const Object* object);
const char* ObjectMenuName (const Object* object, int index);
const char* ObjectMenuFunc (const Object* object, int index);

bool UserMenuAdd (User* user, const char* name, const char* func);
void UserMenuClear (User* user);
bool UserMenuSetNames (User* user, const char* const* names, int count);
int UserMenuCount (const User* user);
const char* UserMenuName (const User* user, int index);
const char* UserMenuFunc (const User* user, int index);

bool EntityGlobalMenuAdd (const char* name, const char* func);
void EntityGlobalMenuClear (void);
void EntityGlobalMenuSetNames (const char* const* names, int count);
int EntityGlobalMenuCount (void);
const char* EntityGlobalMenuName (int index);
const char* EntityGlobalMenuFunc (int index);

bool MenuAdd (u64 id, const char* name, const char* func) {
    if (id == 0) return EntityGlobalMenuAdd (name, func);
    EntityKind kind = EntityKindOf (id);
    if (kind == ENTITY_KIND_USER)
        return UserMenuAdd ((User *)EntityFind (id), name, func);
    if (kind == ENTITY_KIND_OBJECT)
        return ObjectMenuAdd ((Object *)EntityFind (id), name, func);
    return false;
}

void MenuClear (u64 id) {
    if (id == 0) {
        EntityGlobalMenuClear ();
        return;
    }
    EntityKind kind = EntityKindOf (id);
    if (kind == ENTITY_KIND_USER) {
        UserMenuClear ((User *)EntityFind (id));
        return;
    }
    if (kind == ENTITY_KIND_OBJECT)
        ObjectMenuClear ((Object *)EntityFind (id));
}

bool MenuSetNames (u64 id, const char* const* names, int count) {
    if (id == 0) {
        EntityGlobalMenuSetNames (names, count);
        return true;
    }
    EntityKind kind = EntityKindOf (id);
    if (kind == ENTITY_KIND_USER)
        return UserMenuSetNames ((User *)EntityFind (id), names, count);
    if (kind == ENTITY_KIND_OBJECT)
        return ObjectMenuSetNames ((Object *)EntityFind (id), names, count);
    return false;
}

int MenuCount (u64 id) {
    if (id == 0) return EntityGlobalMenuCount ();
    EntityKind kind = EntityKindOf (id);
    if (kind == ENTITY_KIND_USER) return UserMenuCount ((User *)EntityFind (id));
    if (kind == ENTITY_KIND_OBJECT) return ObjectMenuCount ((Object *)EntityFind (id));
    return 0;
}

const char* MenuName (u64 id, int index) {
    if (id == 0) return EntityGlobalMenuName (index);
    EntityKind kind = EntityKindOf (id);
    if (kind == ENTITY_KIND_USER) return UserMenuName ((User *)EntityFind (id), index);
    if (kind == ENTITY_KIND_OBJECT) return ObjectMenuName ((Object *)EntityFind (id), index);
    return NULL;
}

const char* MenuFunc (u64 id, int index) {
    if (id == 0) return EntityGlobalMenuFunc (index);
    EntityKind kind = EntityKindOf (id);
    if (kind == ENTITY_KIND_USER) return UserMenuFunc ((User *)EntityFind (id), index);
    if (kind == ENTITY_KIND_OBJECT) return ObjectMenuFunc ((Object *)EntityFind (id), index);
    return NULL;
}
