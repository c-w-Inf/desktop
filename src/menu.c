#include "menu.h"

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

static Object* FindObject (u64 id) {
    return ObjectFindById (id);
}

bool MenuAdd (u64 id, const char* name, const char* func) {
    if (id == 0) return EntityGlobalMenuAdd (name, func);
    User *user = UserFind (id);
    if (user != NULL) return UserMenuAdd (user, name, func);
    Object *object = FindObject (id);
    if (object != NULL) return ObjectMenuAdd (object, name, func);
    return false;
}

void MenuClear (u64 id) {
    if (id == 0) {
        EntityGlobalMenuClear ();
        return;
    }
    User *user = UserFind (id);
    if (user != NULL) {
        UserMenuClear (user);
        return;
    }
    Object *object = FindObject (id);
    if (object != NULL) ObjectMenuClear (object);
}

bool MenuSetNames (u64 id, const char* const* names, int count) {
    if (id == 0) {
        EntityGlobalMenuSetNames (names, count);
        return true;
    }
    User *user = UserFind (id);
    if (user != NULL) return UserMenuSetNames (user, names, count);
    Object *object = FindObject (id);
    if (object != NULL) return ObjectMenuSetNames (object, names, count);
    return false;
}

int MenuCount (u64 id) {
    if (id == 0) return EntityGlobalMenuCount ();
    User *user = UserFind (id);
    if (user != NULL) return UserMenuCount (user);
    Object *object = FindObject (id);
    if (object != NULL) return ObjectMenuCount (object);
    return 0;
}

const char* MenuName (u64 id, int index) {
    if (id == 0) return EntityGlobalMenuName (index);
    User *user = UserFind (id);
    if (user != NULL) return UserMenuName (user, index);
    Object *object = FindObject (id);
    if (object != NULL) return ObjectMenuName (object, index);
    return NULL;
}

const char* MenuFunc (u64 id, int index) {
    if (id == 0) return EntityGlobalMenuFunc (index);
    User *user = UserFind (id);
    if (user != NULL) return UserMenuFunc (user, index);
    Object *object = FindObject (id);
    if (object != NULL) return ObjectMenuFunc (object, index);
    return NULL;
}
