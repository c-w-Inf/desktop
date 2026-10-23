#include "strmap.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define MAP_TOMBSTONE ((char *)(uintptr_t)1)

typedef struct {
    char *key;
    void *value;
} StrMapEntry;

struct StrMap {
    StrMapEntry *entries;
    int count;
    int capacity;
    int tombstones;
    StrMapEntry **order;
    int orderCapacity;
    bool orderDirty;
};

static size_t HashKey (const char *key) {
    size_t hash = 1469598103934665603ULL;
    while (*key != '\0') {
        hash ^= (unsigned char)*key++;
        hash *= 1099511628211ULL;
    }
    return hash;
}

static char *CopyKey (const char *key) {
    size_t size = strlen (key) + 1;
    char *copy = malloc (size);
    if (copy != NULL) memcpy (copy, key, size);
    return copy;
}

static bool Rehash (StrMap *map, int capacity) {
    StrMapEntry *entries = calloc ((size_t)capacity, sizeof (StrMapEntry));
    if (entries == NULL) return false;

    size_t mask = (size_t)capacity - 1;
    for (int i = 0; i < map->capacity; i++) {
        StrMapEntry *entry = &map->entries[i];
        if (entry->key == NULL || entry->key == MAP_TOMBSTONE) continue;
        size_t slot = HashKey (entry->key) & mask;
        while (entries[slot].key != NULL) slot = (slot + 1) & mask;
        entries[slot] = *entry;
    }

    free (map->entries);
    map->entries = entries;
    map->capacity = capacity;
    map->tombstones = 0;
    map->orderDirty = true;
    return true;
}

static bool EnsureCapacity (StrMap *map) {
    if (map->capacity == 0) return Rehash (map, 16);
    if ((map->count + map->tombstones + 1) * 4 < map->capacity * 3) return true;
    return Rehash (map, map->capacity * 2);
}

/* Return false if the table has no capacity. found reports an exact match. */
static bool FindSlot (const StrMap *map, const char *key, size_t *slot, bool *found) {
    *found = false;
    if (map->capacity == 0) return false;

    size_t mask = (size_t)map->capacity - 1;
    size_t index = HashKey (key) & mask;
    size_t firstTombstone = SIZE_MAX;

    while (map->entries[index].key != NULL) {
        if (map->entries[index].key == MAP_TOMBSTONE) {
            if (firstTombstone == SIZE_MAX) firstTombstone = index;
        } else if (strcmp (map->entries[index].key, key) == 0) {
            *slot = index;
            *found = true;
            return true;
        }
        index = (index + 1) & mask;
    }

    *slot = (firstTombstone != SIZE_MAX) ? firstTombstone : index;
    return true;
}

static int CompareEntries (const void *a, const void *b) {
    const StrMapEntry *ea = *(const StrMapEntry *const *)a;
    const StrMapEntry *eb = *(const StrMapEntry *const *)b;
    return strcmp (ea->key, eb->key);
}

static bool EnsureOrder (StrMap *map) {
    if (!map->orderDirty) return true;
    if (map->count == 0) {
        map->orderDirty = false;
        return true;
    }

    if (map->orderCapacity < map->count) {
        int capacity = (map->orderCapacity == 0) ? 16 : map->orderCapacity;
        while (capacity < map->count) capacity *= 2;
        StrMapEntry **order = realloc (map->order, (size_t)capacity * sizeof (StrMapEntry *));
        if (order == NULL) return false;
        map->order = order;
        map->orderCapacity = capacity;
    }

    int count = 0;
    for (int i = 0; i < map->capacity; i++) {
        StrMapEntry *entry = &map->entries[i];
        if (entry->key == NULL || entry->key == MAP_TOMBSTONE) continue;
        map->order[count++] = entry;
    }
    qsort (map->order, (size_t)count, sizeof (StrMapEntry *), CompareEntries);
    map->orderDirty = false;
    return true;
}

StrMap *StrMapCreate (void) {
    return calloc (1, sizeof (StrMap));
}

void StrMapDestroy (StrMap *map) {
    for (int i = 0; i < map->capacity; i++) {
        if (map->entries[i].key != NULL && map->entries[i].key != MAP_TOMBSTONE)
            free (map->entries[i].key);
    }
    free (map->entries);
    free (map->order);
    free (map);
}

bool StrMapAdd (StrMap *map, const char *key, void *value) {
    if (!EnsureCapacity (map)) return false;

    size_t slot = 0;
    bool found = false;
    if (!FindSlot (map, key, &slot, &found)) return false;
    if (found) return false;

    char *copy = CopyKey (key);
    if (copy == NULL) return false;
    if (map->entries[slot].key == MAP_TOMBSTONE) map->tombstones--;
    map->entries[slot].key = copy;
    map->entries[slot].value = value;
    map->count++;
    map->orderDirty = true;
    return true;
}

bool StrMapSet (StrMap *map, const char *key, void *value) {
    if (!EnsureCapacity (map)) return false;

    size_t slot = 0;
    bool found = false;
    if (!FindSlot (map, key, &slot, &found)) return false;
    if (found) {
        map->entries[slot].value = value;
        return true;
    }

    char *copy = CopyKey (key);
    if (copy == NULL) return false;
    if (map->entries[slot].key == MAP_TOMBSTONE) map->tombstones--;
    map->entries[slot].key = copy;
    map->entries[slot].value = value;
    map->count++;
    map->orderDirty = true;
    return true;
}

bool StrMapGet (const StrMap *map, const char *key, void **value) {
    size_t slot = 0;
    bool found = false;
    if (!FindSlot (map, key, &slot, &found) || !found) return false;
    *value = map->entries[slot].value;
    return true;
}

bool StrMapRemove (StrMap *map, const char *key) {
    size_t slot = 0;
    bool found = false;
    if (!FindSlot (map, key, &slot, &found) || !found) return false;

    free (map->entries[slot].key);
    map->entries[slot].key = MAP_TOMBSTONE;
    map->count--;
    map->tombstones++;
    map->orderDirty = true;
    return true;
}

void **StrMapSlot (StrMap *map, const char *key) {
    size_t slot = 0;
    bool found = false;
    if (!FindSlot (map, key, &slot, &found) || !found) return NULL;
    return &map->entries[slot].value;
}

int StrMapCount (const StrMap *map) {
    return map->count;
}

bool StrMapAt (StrMap *map, int index, const char **key, void **value) {
    if (index < 0 || index >= map->count) return false;
    if (!EnsureOrder (map)) return false;
    StrMapEntry *entry = map->order[index];
    if (key != NULL) *key = entry->key;
    if (value != NULL) *value = entry->value;
    return true;
}
