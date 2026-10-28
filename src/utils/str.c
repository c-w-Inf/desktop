#include "str.h"

#include <stdlib.h>
#include <string.h>

char *StrDup (const char *text) {
    size_t length = strlen (text) + 1;
    char *copy = malloc (length);
    if (copy != NULL) memcpy (copy, text, length);
    return copy;
}
