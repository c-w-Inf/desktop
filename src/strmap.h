#ifndef STRMAP_H
#define STRMAP_H

#include <stdbool.h>

typedef struct StrMap StrMap;

StrMap *StrMapCreate (void);
void StrMapDestroy (StrMap *map);

bool StrMapAdd (StrMap *map, const char *key, void *value);
bool StrMapSet (StrMap *map, const char *key, void *value);
bool StrMapGet (const StrMap *map, const char *key, void **value);
void **StrMapSlot (StrMap *map, const char *key);
bool StrMapRemove (StrMap *map, const char *key);

int StrMapCount (const StrMap *map);

/* Traverse in ascending key order. Builds a sorted view lazily. */
bool StrMapAt (StrMap *map, int index, const char **key, void **value);

#endif
