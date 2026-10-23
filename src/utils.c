#include "utils.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

char *StrDup (const char *text) {
    size_t length = strlen (text) + 1;
    char *copy = malloc (length);
    if (copy != NULL) memcpy (copy, text, length);
    return copy;
}

static u64 randomState = 0;
static bool randomSeeded = false;

u64 RandomNext (void) {
    if (!randomSeeded) {
        u64 seed = (u64)time (NULL) ^ ((u64)(uintptr_t)&randomState << 17) ^ (u64)clock ();
        if (seed == 0) seed = 0x9E3779B97F4A7C15ULL;
        randomState = seed;
        randomSeeded = true;
    }
    u64 x = randomState;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    randomState = x;
    return x * 2685821657736338717ULL;
}

u64 RandomBounded (u64 limit) {
    u64 bound = UINT64_MAX - (UINT64_MAX % limit);
    u64 value;
    do {
        value = RandomNext ();
    } while (value >= bound);
    return value % limit;
}

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

bool TextBufferReserve (TextBuffer *buffer, size_t extra) {
    if (buffer->length + extra + 1 <= buffer->capacity) return true;

    size_t capacity = (buffer->capacity == 0) ? 128 : buffer->capacity;
    while (capacity < buffer->length + extra + 1) capacity *= 2;
    char *data = realloc (buffer->data, capacity);
    if (data == NULL) return false;
    buffer->data = data;
    buffer->capacity = capacity;
    buffer->data[buffer->length] = '\0';
    return true;
}

bool TextBufferAppendN (TextBuffer *buffer, const char *text, size_t length) {
    if (!TextBufferReserve (buffer, length)) return false;
    if (length > 0) memcpy (buffer->data + buffer->length, text, length);
    buffer->length += length;
    buffer->data[buffer->length] = '\0';
    return true;
}

bool TextBufferAppend (TextBuffer *buffer, const char *text) {
    return TextBufferAppendN (buffer, text, strlen (text));
}

bool TextBufferAppendChar (TextBuffer *buffer, char ch) {
    return TextBufferAppendN (buffer, &ch, 1);
}

char *TextBufferTake (TextBuffer *buffer) {
    if (!TextBufferReserve (buffer, 0)) return NULL;
    char *data = buffer->data;
    buffer->data = NULL;
    buffer->length = 0;
    buffer->capacity = 0;
    return data;
}

void TextBufferFree (TextBuffer *buffer) {
    free (buffer->data);
    buffer->data = NULL;
    buffer->length = 0;
    buffer->capacity = 0;
}

bool PathHasExtension (const char *path, const char *extension) {
    const char *dot = strrchr (path, '.');
    return dot != NULL && strcasecmp (dot, extension) == 0;
}

bool PathIsDirectory (const char *path) {
    if (path == NULL || path[0] == '\0') return false;
    struct stat info;
    return stat (path, &info) == 0 && S_ISDIR (info.st_mode);
}

bool PathIsImage (const char *path) {
    static const char *extensions[] = {
        ".png", ".jpg", ".jpeg", ".bmp", ".tga", ".gif", ".qoi", ".psd", NULL
    };
    for (int i = 0; extensions[i] != NULL; i++) {
        if (PathHasExtension (path, extensions[i])) return true;
    }
    return false;
}
