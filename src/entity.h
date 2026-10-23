#ifndef ENTITY_H
#define ENTITY_H

#include <stdbool.h>
#include "utils.h"

typedef void (*EntityDestroyFn) (void *data);

bool EntityRegister (u64 id, void* ptr);
void EntityUnregister (u64 id);
void* EntityFind (u64 id);
bool EntitySetData (u64 id, void *data, EntityDestroyFn destroy);
void* EntityGetData (u64 id);

void EntityUnload (void);

#endif
