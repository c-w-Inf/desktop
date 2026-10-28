#include "rawfile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

RawFile RawFileRead (const char *path) {
    RawFile file = {0};
    if (path == NULL || path[0] == '\0') return file;

    FILE *handle = fopen (path, "rb");
    if (handle == NULL) return file;
    if (fseek (handle, 0, SEEK_END) != 0) {
        fclose (handle);
        return file;
    }
    long length = ftell (handle);
    if (length <= 0 || fseek (handle, 0, SEEK_SET) != 0) {
        fclose (handle);
        return file;
    }

    unsigned char *data = malloc ((size_t)length);
    if (data == NULL) {
        fclose (handle);
        return file;
    }
    size_t read = fread (data, 1, (size_t)length, handle);
    fclose (handle);
    if (read != (size_t)length) {
        free (data);
        return file;
    }

    file.data = data;
    file.size = (size_t)length;
    const char *slash = strrchr (path, '/');
#ifdef _WIN32
    const char *backslash = strrchr (path, '\\');
    if (backslash != NULL && (slash == NULL || backslash > slash))
        slash = backslash;
#endif
    const char *dot = strrchr (slash != NULL ? slash + 1 : path, '.');
    if (dot != NULL && dot[1] != '\0')
        snprintf (file.ext, sizeof (file.ext), "%s", dot);
    return file;
}

void RawFileFree (RawFile *file) {
    if (file == NULL) return;
    free (file->data);
    memset (file, 0, sizeof (*file));
}
