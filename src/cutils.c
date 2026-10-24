#include "cutils.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#endif

#ifndef S_ISDIR
#define S_ISDIR(mode) (((mode) & S_IFMT) == S_IFDIR)
#endif

static CutilsFileChosenFn fileHandler;

void CutilsSetFileHandler (CutilsFileChosenFn handler) {
    fileHandler = handler;
}

#if !defined(__EMSCRIPTEN__)
#include "tinyfiledialogs.h"

static const char *imagePatterns[] = {
    "*.png", "*.jpg", "*.jpeg", "*.bmp", "*.tga",
    "*.gif", "*.qoi", "*.psd"
};
static const char *importPatterns[] = {
    "*.dsk", "*.png", "*.jpg", "*.jpeg", "*.bmp",
    "*.tga", "*.gif", "*.qoi", "*.psd"
};

static CutilsDialogResult CopySelected (const char *selected, char *path,
                                        size_t size) {
    if (selected == NULL || selected[0] == '\0')
        return CUTILS_DIALOG_CANCELLED;
    size_t length = strlen (selected);
    if (length >= size) return CUTILS_DIALOG_TOO_LONG;
    memcpy (path, selected, length + 1);
    return CUTILS_DIALOG_OK;
}

static void PrepareDialogs (void) {
#ifdef _WIN32
    tinyfd_winUtf8 = 1;
#endif
}
#endif

#if defined(__EMSCRIPTEN__)
EM_JS(void, CutilsPickImportFile, (void), {
    const input = document.createElement("input");
    input.type = "file";
    input.accept = ".dsk,image/*";
    input.style.display = "none";
    document.body.appendChild(input);
    input.onchange = () => {
        const file = input.files[0];
        if (!file) {
            document.body.removeChild(input);
            return;
        }
        const reader = new FileReader();
        reader.onload = () => {
            const path = "/tmp/import-" + Date.now() + "-" + file.name;
            try {
                FS.writeFile(path, new Uint8Array(reader.result));
                ccall("CutilsWebFileSelected", null, ["string"], [path]);
            } catch (error) {
                console.error(error);
            }
            document.body.removeChild(input);
        };
        reader.readAsArrayBuffer(file);
    };
    input.click();
});

EMSCRIPTEN_KEEPALIVE
void CutilsWebFileSelected (const char *path) {
    if (fileHandler != NULL) fileHandler (path);
}
#endif

CutilsDialogResult CutilsOpenImage (char *path, size_t size) {
#if defined(__EMSCRIPTEN__)
    (void)path;
    (void)size;
    if (fileHandler == NULL) return CUTILS_DIALOG_FAILED;
    CutilsPickImportFile ();
    return CUTILS_DIALOG_PENDING;
#else
    PrepareDialogs ();
    const char *selected = tinyfd_openFileDialog (
        "选择图片", "", 8, imagePatterns, "图片", 0);
    return CopySelected (selected, path, size);
#endif
}

CutilsDialogResult CutilsOpenImport (char *path, size_t size) {
#if defined(__EMSCRIPTEN__)
    (void)path;
    (void)size;
    if (fileHandler == NULL) return CUTILS_DIALOG_FAILED;
    CutilsPickImportFile ();
    return CUTILS_DIALOG_PENDING;
#else
    PrepareDialogs ();
    const char *selected = tinyfd_openFileDialog (
        "导入", "", 9, importPatterns, "支持的文件", 0);
    return CopySelected (selected, path, size);
#endif
}

CutilsDialogResult CutilsOpenFolder (char *path, size_t size) {
#if defined(__EMSCRIPTEN__)
    (void)path;
    (void)size;
    return CUTILS_DIALOG_FAILED;
#else
    PrepareDialogs ();
    const char *selected = tinyfd_selectFolderDialog ("导入文件夹", "");
    return CopySelected (selected, path, size);
#endif
}

CutilsDialogResult CutilsSaveDsk (char *path, size_t size) {
#if defined(__EMSCRIPTEN__)
    (void)path;
    (void)size;
    return CUTILS_DIALOG_FAILED;
#else
    PrepareDialogs ();
    const char *selected = tinyfd_saveFileDialog (
        "导出存档", "image_canvas.dsk", 1,
        (const char *[]){ "*.dsk" }, "存档");
    if (selected == NULL || selected[0] == '\0')
        return CUTILS_DIALOG_CANCELLED;
    size_t length = strlen (selected);
    if (length + 4 >= size) return CUTILS_DIALOG_TOO_LONG;
    memcpy (path, selected, length);
    if (length < 4 || strcmp (path + length - 4, ".dsk") != 0) {
        memcpy (path + length, ".dsk", 4);
        length += 4;
    }
    path[length] = '\0';
    return CUTILS_DIALOG_OK;
#endif
}

bool PathIsDirectory (const char *path) {
    if (path == NULL || path[0] == '\0') return false;
    struct stat info;
    return stat (path, &info) == 0 && S_ISDIR (info.st_mode);
}

unsigned char *ReadFileBytes (const char *path, int *size) {
    if (path == NULL || path[0] == '\0' || size == NULL) return NULL;
    *size = 0;

    FILE *file = fopen (path, "rb");
    if (file == NULL) return NULL;
    if (fseek (file, 0, SEEK_END) != 0) {
        fclose (file);
        return NULL;
    }
    long length = ftell (file);
    if (length <= 0 || length > INT_MAX || fseek (file, 0, SEEK_SET) != 0) {
        fclose (file);
        return NULL;
    }

    unsigned char *data = malloc ((size_t)length);
    if (data == NULL) {
        fclose (file);
        return NULL;
    }
    size_t read = fread (data, 1, (size_t)length, file);
    fclose (file);
    if (read != (size_t)length) {
        free (data);
        return NULL;
    }
    *size = (int)length;
    return data;
}
