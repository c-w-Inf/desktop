#ifndef CUTILS_H
#define CUTILS_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    CUTILS_DIALOG_CANCELLED,
    CUTILS_DIALOG_OK,
    CUTILS_DIALOG_TOO_LONG,
    CUTILS_DIALOG_FAILED
} CutilsDialogResult;

CutilsDialogResult CutilsOpenImage (char *path, size_t size);
CutilsDialogResult CutilsOpenImport (char *path, size_t size);
CutilsDialogResult CutilsOpenFolder (char *path, size_t size);
CutilsDialogResult CutilsSaveDsk (char *path, size_t size);

bool PathIsDirectory (const char *path);
unsigned char *ReadFileBytes (const char *path, int *size);

#endif
