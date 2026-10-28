#define _POSIX_C_SOURCE 200809L

#include "entity.h"

#include "menu_array.h"

#include <stdint.h>
#include <stdlib.h>

#define ENTITY_TOMBSTONE UINT64_MAX

typedef struct {
    u64 id;
    void* ptr;
    void* data;
    EntityDestroyFn destroy;
    EntityKind kind;
} Entity;

static Entity* entities;
static char **globalMenuNames;
static char **globalMenuFuncs;
static int globalMenuSize;
static int globalMenuCapacity;
static size_t entityCount;
static size_t entityCapacity;
static size_t tombstoneCount;

static size_t HashId (u64 id) {
    id ^= id >> 30;
    id *= 0xBF58476D1CE4E5B9ULL;
    id ^= id >> 27;
    id *= 0x94D049BB133111EBULL;
    id ^= id >> 31;
    return (size_t)id;
}

bool EntityGlobalMenuAdd (const char* name, const char* func) {
    if (name == NULL) return false;
    return MenuArrayAdd (&globalMenuNames, &globalMenuFuncs, &globalMenuSize,
                         &globalMenuCapacity, name, func);
}

void EntityGlobalMenuClear (void) {
    MenuArrayClear (&globalMenuNames, &globalMenuFuncs, &globalMenuSize);
}

int EntityGlobalMenuCount (void) {
    return globalMenuSize;
}

void EntityGlobalMenuSetNames (const char* const* names, int count) {
    MenuArraySetNames (&globalMenuNames, &globalMenuFuncs, &globalMenuSize,
                       &globalMenuCapacity, names, count);
}

const char* EntityGlobalMenuName (int index) {
    if (index < 0 || index >= globalMenuSize) return NULL;
    return globalMenuNames[index];
}

const char* EntityGlobalMenuFunc (int index) {
    if (globalMenuFuncs == NULL || index < 0 || index >= globalMenuSize)
        return NULL;
    return globalMenuFuncs[index];
}

static bool EntityReserve (void) {
    if (entityCapacity > 0 &&
        (entityCount + tombstoneCount + 1) * 4 < entityCapacity * 3)
        return true;

    size_t capacity = (entityCapacity == 0) ? 32 : entityCapacity * 2;
    Entity* entries = calloc (capacity, sizeof (Entity));
    if (entries == NULL) return false;

    size_t mask = capacity - 1;
    for (size_t i = 0; i < entityCapacity; i++) {
        if (entities[i].id == 0 || entities[i].id == ENTITY_TOMBSTONE) continue;
        size_t index = HashId (entities[i].id) & mask;
        while (entries[index].id != 0) index = (index + 1) & mask;
        entries[index] = entities[i];
    }

    free (entities);
    entities = entries;
    entityCapacity = capacity;
    tombstoneCount = 0;
    return true;
}

bool EntityRegister (u64 id, void* ptr, EntityKind kind) {
    if (id == 0 || id == ENTITY_TOMBSTONE || ptr == NULL ||
        kind == ENTITY_KIND_NONE) return false;
    if (!EntityReserve ()) return false;

    size_t mask = entityCapacity - 1;
    size_t index = HashId (id) & mask;
    size_t firstTombstone = SIZE_MAX;

    while (entities[index].id != 0) {
        if (entities[index].id == id) return false;
        if (entities[index].id == ENTITY_TOMBSTONE && firstTombstone == SIZE_MAX)
            firstTombstone = index;
        index = (index + 1) & mask;
    }

    if (firstTombstone != SIZE_MAX) {
        index = firstTombstone;
        tombstoneCount--;
    }
    entities[index].id = id;
    entities[index].ptr = ptr;
    entities[index].data = NULL;
    entities[index].destroy = NULL;
    entities[index].kind = kind;
    entityCount++;
    return true;
}

void EntityUnregister (u64 id) {
    if (id == 0 || id == ENTITY_TOMBSTONE || entityCapacity == 0) return;

    size_t mask = entityCapacity - 1;
    size_t index = HashId (id) & mask;
    while (entities[index].id != 0) {
        if (entities[index].id == id) {
            if (entities[index].destroy != NULL)
                entities[index].destroy (entities[index].data);
            entities[index].id = ENTITY_TOMBSTONE;
            entities[index].ptr = NULL;
            entities[index].data = NULL;
            entities[index].destroy = NULL;
            entities[index].kind = ENTITY_KIND_NONE;
            entityCount--;
            tombstoneCount++;
            return;
        }
        index = (index + 1) & mask;
    }
}

void* EntityFind (u64 id) {
    if (id == 0 || entityCapacity == 0) return NULL;

    size_t mask = entityCapacity - 1;
    size_t index = HashId (id) & mask;
    while (entities[index].id != 0) {
        if (entities[index].id == id) return entities[index].ptr;
        index = (index + 1) & mask;
    }
    return NULL;
}

EntityKind EntityKindOf (u64 id) {
    if (id == 0 || entityCapacity == 0) return ENTITY_KIND_NONE;

    size_t mask = entityCapacity - 1;
    size_t index = HashId (id) & mask;
    while (entities[index].id != 0) {
        if (entities[index].id == id) return entities[index].kind;
        index = (index + 1) & mask;
    }
    return ENTITY_KIND_NONE;
}

bool EntitySetData (u64 id, void *data, EntityDestroyFn destroy) {
    if (id == 0 || entityCapacity == 0) return false;
    size_t mask = entityCapacity - 1;
    size_t index = HashId (id) & mask;
    while (entities[index].id != 0) {
        if (entities[index].id == id) {
            if (entities[index].destroy != NULL && entities[index].data != data)
                entities[index].destroy (entities[index].data);
            entities[index].data = data;
            entities[index].destroy = destroy;
            return true;
        }
        index = (index + 1) & mask;
    }
    return false;
}

void* EntityGetData (u64 id) {
    if (id == 0 || entityCapacity == 0) return NULL;
    size_t mask = entityCapacity - 1;
    size_t index = HashId (id) & mask;
    while (entities[index].id != 0) {
        if (entities[index].id == id) return entities[index].data;
        index = (index + 1) & mask;
    }
    return NULL;
}

void EntityUnload (void) {
    for (size_t i = 0; i < entityCapacity; i++) {
        if (entities[i].id == 0 || entities[i].id == ENTITY_TOMBSTONE) continue;
        if (entities[i].destroy != NULL) entities[i].destroy (entities[i].data);
    }
    MenuArrayFree (&globalMenuNames, &globalMenuFuncs, &globalMenuSize,
                   &globalMenuCapacity);
    free (entities);
    entities = NULL;
    entityCount = 0;
    entityCapacity = 0;
    tombstoneCount = 0;
}
