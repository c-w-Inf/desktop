#include "path.h"

#include <string.h>
#include <strings.h>

bool PathHasExtension (const char *path, const char *extension) {
    const char *dot = strrchr (path, '.');
    return dot != NULL && strcasecmp (dot, extension) == 0;
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
