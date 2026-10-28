#include "idgroup.h"

#include <stdlib.h>
#include <string.h>

IdGroup IdGroupCreate (void) {
    return (IdGroup){0};
}

void IdGroupUnload (IdGroup *group) {
    free (group->ids);
    group->ids = NULL;
    group->count = 0;
    group->capacity = 0;
}

void IdGroupClear (IdGroup *group) {
    group->count = 0;
}

static bool IdGroupReserve (IdGroup *group, int needed) {
    if (needed <= group->capacity) return true;

    int capacity = (group->capacity == 0) ? 4 : group->capacity;
    while (capacity < needed) capacity *= 2;
    u64 *ids = realloc (group->ids, (size_t)capacity * sizeof (u64));
    if (ids == NULL) return false;
    group->ids = ids;
    group->capacity = capacity;
    return true;
}

bool IdGroupAdd (IdGroup *group, u64 id) {
    if (!IdGroupReserve (group, group->count + 1)) return false;
    group->ids[group->count++] = id;
    return true;
}

bool IdGroupRemoveAt (IdGroup *group, int index) {
    if (index < 0 || index >= group->count) return false;
    memmove (&group->ids[index], &group->ids[index + 1],
             (size_t)(group->count - index - 1) * sizeof (u64));
    group->count--;
    return true;
}

bool IdGroupMove (IdGroup *group, int from, int to) {
    if (from < 0 || from >= group->count ||
        to < 0 || to >= group->count) return false;
    if (from == to) return true;

    u64 id = group->ids[from];
    if (from < to) {
        memmove (&group->ids[from], &group->ids[from + 1],
                 (size_t)(to - from) * sizeof (u64));
    } else {
        memmove (&group->ids[to + 1], &group->ids[to],
                 (size_t)(from - to) * sizeof (u64));
    }
    group->ids[to] = id;
    return true;
}

bool IdGroupCopy (IdGroup *group, const u64 *ids, int count) {
    if (count < 0 || (count > 0 && ids == NULL)) return false;
    if (count > 0 && !IdGroupReserve (group, count)) return false;
    if (count > 0) memcpy (group->ids, ids, (size_t)count * sizeof (u64));
    group->count = count;
    return true;
}
