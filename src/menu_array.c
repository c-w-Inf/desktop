#include "menu_array.h"

#include <stdlib.h>

#include "utils.h"

static bool MenuArrayReserve (char ***names, char ***funcs, int *capacity,
                              int needed) {
    if (needed <= *capacity) return true;

    int next = (*capacity == 0) ? 8 : *capacity * 2;
    while (next < needed) next *= 2;

    char **nameItems = realloc (*names, (size_t)next * sizeof (char *));
    if (nameItems == NULL) return false;
    *names = nameItems;

    char **funcItems = realloc (*funcs, (size_t)next * sizeof (char *));
    if (funcItems == NULL) return false;
    *funcs = funcItems;
    *capacity = next;
    return true;
}

bool MenuArrayAdd (char ***names, char ***funcs, int *size, int *capacity,
                   const char *name, const char *func) {
    if (!MenuArrayReserve (names, funcs, capacity, *size + 1)) return false;

    char *nameCopy = strdup (name);
    char *funcCopy = func != NULL ? strdup (func) : NULL;
    if (nameCopy == NULL || (func != NULL && funcCopy == NULL)) {
        free (nameCopy);
        free (funcCopy);
        return false;
    }
    (*names)[*size] = nameCopy;
    (*funcs)[*size] = funcCopy;
    (*size)++;
    return true;
}

void MenuArrayClear (char ***names, char ***funcs, int *size) {
    for (int i = 0; i < *size; i++) {
        free ((*names)[i]);
        if (*funcs != NULL) free ((*funcs)[i]);
    }
    *size = 0;
}

void MenuArrayFree (char ***names, char ***funcs, int *size, int *capacity) {
    MenuArrayClear (names, funcs, size);
    free (*names);
    free (*funcs);
    *names = NULL;
    *funcs = NULL;
    *capacity = 0;
}

bool MenuArraySetNames (char ***names, char ***funcs, int *size, int *capacity,
                        const char *const *values, int count) {
    if (count < 0 || (count > 0 && values == NULL)) return false;

    char **newNames = NULL;
    if (count > 0) {
        newNames = calloc ((size_t)count, sizeof (char *));
        if (newNames == NULL) return false;
        for (int i = 0; i < count; i++) {
            newNames[i] = strdup (values[i]);
            if (newNames[i] == NULL) {
                for (int j = 0; j < i; j++) free (newNames[j]);
                free (newNames);
                return false;
            }
        }
    }

    MenuArrayClear (names, funcs, size);
    free (*names);
    free (*funcs);
    *names = newNames;
    *funcs = NULL;
    *size = count;
    *capacity = count;
    return true;
}
