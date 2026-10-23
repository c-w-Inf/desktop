#include "user.h"

#include <stdio.h>
#include <stdlib.h>

#include "entity.h"
#include "menu_array.h"

static User **users;
static int userCount;
static int userCapacity;

static User *CreateUser (u64 id, const char *name) {
    User *user = malloc (sizeof (User));
    if (user == NULL) return NULL;

    user->id = id;
    snprintf (user->name, sizeof (user->name), "%s", name);
    user->hasCursor = false;
    user->x = 0.0f;
    user->y = 0.0f;
    user->color = 0x000000FFu; // RGBA black
    user->canscript = true;
    user->menuNames = NULL;
    user->menuFuncs = NULL;
    user->menuSize = 0;
    user->menuCapacity = 0;
    return user;
}

static bool AppendUser (User *user) {
    if (!EntityRegister (user->id, user)) return false;
    if (userCount == userCapacity) {
        int capacity = (userCapacity == 0) ? 8 : userCapacity * 2;
        User **list = realloc (users, (size_t)capacity * sizeof (User *));
        if (list == NULL) {
            EntityUnregister (user->id);
            return false;
        }
        users = list;
        userCapacity = capacity;
    }
    users[userCount++] = user;
    return true;
}

static void RemoveUserAt (int index) {
    User *user = users[index];
    EntityUnregister (user->id);
    MenuArrayFree (&user->menuNames, &user->menuFuncs, &user->menuSize,
                   &user->menuCapacity);
    free (user);
    for (int i = index; i < userCount - 1; i++) users[i] = users[i + 1];
    userCount--;
    users[userCount] = NULL;
}

void UserInit (void) {
    users = NULL;
    userCount = 0;
    userCapacity = 0;
}

void UserUnload (void) {
    for (int i = 0; i < userCount; i++) {
        EntityUnregister (users[i]->id);
        MenuArrayFree (&users[i]->menuNames, &users[i]->menuFuncs,
                       &users[i]->menuSize, &users[i]->menuCapacity);
        free (users[i]);
    }
    free (users);
    users = NULL;
    userCount = 0;
    userCapacity = 0;
}

User *UserFind (u64 id) {
    for (int i = 0; i < userCount; i++) {
        if (users[i]->id == id) return users[i];
    }
    return NULL;
}

User *UserAdd (u64 id, const char *name) {
    if (id == 0 || name == NULL || name[0] == '\0') return NULL;
    if (UserFind (id) != NULL) return NULL;

    User *user = CreateUser (id, name);
    if (user == NULL) return NULL;
    if (!AppendUser (user)) {
        free (user);
        return NULL;
    }
    return user;
}

bool UserRemove (u64 id) {
    for (int i = 0; i < userCount; i++) {
        if (users[i]->id != id) continue;
        RemoveUserAt (i);
        return true;
    }
    return false;
}

bool UserMenuAdd (User* user, const char* name, const char* func) {
    if (user == NULL || name == NULL) return false;
    return MenuArrayAdd (&user->menuNames, &user->menuFuncs, &user->menuSize,
                         &user->menuCapacity, name, func);
}

void UserMenuClear (User* user) {
    if (user == NULL) return;
    MenuArrayClear (&user->menuNames, &user->menuFuncs, &user->menuSize);
}

bool UserMenuSetNames (User* user, const char* const* names, int count) {
    if (user == NULL) return false;
    return MenuArraySetNames (&user->menuNames, &user->menuFuncs,
                              &user->menuSize, &user->menuCapacity,
                              names, count);
}

int UserMenuCount (const User* user) {
    return user != NULL ? user->menuSize : 0;
}

const char* UserMenuName (const User* user, int index) {
    if (user == NULL || index < 0 || index >= user->menuSize) return NULL;
    return user->menuNames[index];
}

const char* UserMenuFunc (const User* user, int index) {
    if (user == NULL || user->menuFuncs == NULL ||
        index < 0 || index >= user->menuSize) return NULL;
    return user->menuFuncs[index];
}

void UserSetCursor (u64 id, bool hasCursor, float x, float y) {
    User *user = UserFind (id);
    if (user == NULL) return;
    user->hasCursor = hasCursor;
    user->x = x;
    user->y = y;
}

void UserSetColor (u64 id, uint32_t color) {
    User *user = UserFind (id);
    if (user == NULL) return;
    user->color = color;
}

void UserSetCanScript (u64 id, bool canscript) {
    User *user = UserFind (id);
    if (user == NULL) return;
    user->canscript = canscript;
}

Color UserColor (const User *user) {
    if (user == NULL) return BLACK;
    return (Color){(unsigned char)(user->color >> 24),
                   (unsigned char)(user->color >> 16),
                   (unsigned char)(user->color >> 8),
                   (unsigned char)user->color};
}

IdGroup UserIds (void) {
    IdGroup group = IdGroupCreate ();
    for (int i = 0; i < userCount; i++) IdGroupAdd (&group, users[i]->id);
    return group;
}


