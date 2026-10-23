#define _POSIX_C_SOURCE 200809L

#include "save.h"

#include <dirent.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include <json-c/json.h>
#include <minizip/unzip.h>
#include <minizip/zip.h>
#include <zlib.h>

#include "assets.h"
#include "entity.h"
#include "hjson.h"
#include "menu.h"
#include "object.h"

#define DSK_ZIP_VERSION 1u
#define DSK_ZIP_MAX_ENTRY ((uint64_t)512u * 1024u * 1024u)
#define DSK_ZIP_MAX_TOTAL ((uint64_t)1024u * 1024u * 1024u)
#define DSK_FOLDER_MAX_DEPTH 32
#define DSK_FOLDER_MAX_ENTRIES 100000
#define DSK_PATH_MAX 4096

typedef struct {
    char *name;
    unsigned char *data;
    size_t size;
} DskZipEntry;

typedef struct {
    DskZipEntry *entries;
    int count;
    int capacity;
} DskZip;

static void DskZipFree (DskZip *zip) {
    for (int i = 0; i < zip->count; i++) {
        free (zip->entries[i].name);
        free (zip->entries[i].data);
    }
    free (zip->entries);
    memset (zip, 0, sizeof (*zip));
}

static bool DskZipAdd (DskZip *zip, const char *name,
                       unsigned char *data, size_t size) {
    if (zip->count == zip->capacity) {
        int capacity = zip->capacity == 0 ? 16 : zip->capacity * 2;
        DskZipEntry *entries = realloc (
            zip->entries, (size_t)capacity * sizeof (DskZipEntry));
        if (entries == NULL) return false;
        zip->entries = entries;
        zip->capacity = capacity;
    }

    char *copy = StrDup (name);
    if (copy == NULL) return false;
    zip->entries[zip->count].name = copy;
    zip->entries[zip->count].data = data;
    zip->entries[zip->count].size = size;
    zip->count++;
    return true;
}

static bool DskReadZip (const char *path, DskZip *zip) {
    memset (zip, 0, sizeof (*zip));
    unzFile file = unzOpen (path);
    if (file == NULL) return false;

    unz_global_info64 global;
    if (unzGetGlobalInfo64 (file, &global) != UNZ_OK) {
        unzClose (file);
        return false;
    }

    bool ok = true;
    size_t total = 0;
    int status = unzGoToFirstFile (file);
    while (status == UNZ_OK) {
        unz_file_info64 info;
        if (unzGetCurrentFileInfo64 (file, &info, NULL, 0, NULL, 0, NULL, 0) !=
            UNZ_OK || info.size_filename >= 4096) {
            ok = false;
            break;
        }

        char name[4096];
        if (unzGetCurrentFileInfo64 (file, &info, name, sizeof (name),
                                     NULL, 0, NULL, 0) != UNZ_OK) {
            ok = false;
            break;
        }
        name[sizeof (name) - 1] = '\0';

        bool directory = name[0] != '\0' && name[strlen (name) - 1] == '/';
        if (!directory) {
            uint64_t size = info.uncompressed_size;
            if ((info.flag & 1) != 0 || size > DSK_ZIP_MAX_ENTRY ||
                size > SIZE_MAX || total > DSK_ZIP_MAX_TOTAL - size) {
                ok = false;
                break;
            }

            unsigned char *data = size > 0 ? malloc ((size_t)size) : NULL;
            if (size > 0 && data == NULL) {
                ok = false;
                break;
            }

            if (unzOpenCurrentFile (file) != UNZ_OK) {
                free (data);
                ok = false;
                break;
            }

            size_t offset = 0;
            while (offset < (size_t)size) {
                size_t left = (size_t)size - offset;
                unsigned int chunk = left > (1024u * 1024u)
                                         ? (unsigned int)(1024u * 1024u)
                                         : (unsigned int)left;
                int read = unzReadCurrentFile (file, data + offset, chunk);
                if (read <= 0) {
                    ok = false;
                    break;
                }
                offset += (size_t)read;
            }

            int closeStatus = unzCloseCurrentFile (file);
            if (!ok || closeStatus != UNZ_OK ||
                !DskZipAdd (zip, name, data, (size_t)size)) {
                free (data);
                ok = false;
                break;
            }
            total += (size_t)size;
        }

        status = unzGoToNextFile (file);
    }

    if (status != UNZ_END_OF_LIST_OF_FILE) ok = false;
    unzClose (file);
    if (!ok) {
        DskZipFree (zip);
        return false;
    }
    return true;
}


static bool DskReadDiskFile (const char *path, uint64_t expected,
                             unsigned char **data, size_t *size) {
    FILE *file = fopen (path, "rb");
    if (file == NULL) return false;

    unsigned char *bytes = NULL;
    if (expected > 0) {
        bytes = malloc ((size_t)expected);
        if (bytes == NULL) {
            fclose (file);
            return false;
        }
    }

    bool ok = expected == 0 ||
              fread (bytes, 1, (size_t)expected, file) == (size_t)expected;
    fclose (file);
    if (!ok) {
        free (bytes);
        return false;
    }
    *data = bytes;
    *size = (size_t)expected;
    return true;
}

static bool DskReadDirectoryTree (const char *root, const char *relative,
                                  DskZip *zip, size_t *total, int depth) {
    if (depth > DSK_FOLDER_MAX_DEPTH) return false;

    char directoryPath[DSK_PATH_MAX];
    int length = relative[0] == '\0'
                     ? snprintf (directoryPath, sizeof (directoryPath), "%s", root)
                     : snprintf (directoryPath, sizeof (directoryPath), "%s/%s",
                                 root, relative);
    if (length < 0 || (size_t)length >= sizeof (directoryPath)) return false;

    DIR *directory = opendir (directoryPath);
    if (directory == NULL) return false;

    bool ok = true;
    struct dirent *entry;
    while (ok && (entry = readdir (directory)) != NULL) {
        if (strcmp (entry->d_name, ".") == 0 ||
            strcmp (entry->d_name, "..") == 0)
            continue;

        char childRelative[DSK_PATH_MAX];
        length = relative[0] == '\0'
                     ? snprintf (childRelative, sizeof (childRelative), "%s",
                                 entry->d_name)
                     : snprintf (childRelative, sizeof (childRelative), "%s/%s",
                                 relative, entry->d_name);
        if (length < 0 || (size_t)length >= sizeof (childRelative)) {
            ok = false;
            break;
        }

        char childPath[DSK_PATH_MAX];
        length = snprintf (childPath, sizeof (childPath), "%s/%s", root,
                           childRelative);
        if (length < 0 || (size_t)length >= sizeof (childPath)) {
            ok = false;
            break;
        }

        struct stat info;
        if (lstat (childPath, &info) != 0) {
            ok = false;
            break;
        }
        if (S_ISLNK (info.st_mode)) continue;

        if (S_ISDIR (info.st_mode)) {
            if (!DskReadDirectoryTree (root, childRelative, zip, total,
                                       depth + 1)) {
                ok = false;
                break;
            }
            continue;
        }
        if (!S_ISREG (info.st_mode)) continue;
        if (info.st_size < 0 ||
            (uint64_t)info.st_size > DSK_ZIP_MAX_ENTRY ||
            (uint64_t)info.st_size > SIZE_MAX ||
            *total > DSK_ZIP_MAX_TOTAL - (uint64_t)info.st_size ||
            zip->count >= DSK_FOLDER_MAX_ENTRIES) {
            ok = false;
            break;
        }

        unsigned char *data = NULL;
        size_t size = 0;
        if (!DskReadDiskFile (childPath, (uint64_t)info.st_size, &data,
                              &size) ||
            !DskZipAdd (zip, childRelative, data, size)) {
            free (data);
            ok = false;
            break;
        }
        *total += size;
    }

    closedir (directory);
    return ok;
}

static bool DskReadSource (const char *path, DskZip *zip) {
    struct stat info;
    if (path == NULL || path[0] == '\0' || stat (path, &info) != 0)
        return false;

    if (!S_ISDIR (info.st_mode)) return DskReadZip (path, zip);

    memset (zip, 0, sizeof (*zip));
    size_t total = 0;
    bool ok = DskReadDirectoryTree (path, "", zip, &total, 0);
    if (!ok) {
        DskZipFree (zip);
        return false;
    }
    return true;
}

typedef enum {
    DSK_ZIP_OTHER = 0,
    DSK_ZIP_ASSET,
    DSK_ZIP_OBJECT,
    DSK_ZIP_SCENE
} DskZipKind;

static DskZipKind DskZipClassify (const char *name, const char **relative) {
    const char *part = name;
    while (*part != '\0') {
        const char *slash = strchr (part, '/');
        size_t length = slash != NULL ? (size_t)(slash - part) : strlen (part);
        if (slash != NULL && length == 6 &&
            strncmp (part, "assets", 6) == 0) {
            *relative = part;
            return DSK_ZIP_ASSET;
        }
        if (slash != NULL && length == 7 &&
            strncmp (part, "objects", 7) == 0) {
            *relative = part;
            return DSK_ZIP_OBJECT;
        }
        if (slash == NULL &&
            ((length == 10 && strncasecmp (part, "scene.json", 10) == 0) ||
             (length == 11 && strncasecmp (part, "scene.hjson", 11) == 0))) {
            *relative = part;
            return DSK_ZIP_SCENE;
        }
        if (slash == NULL) break;
        part = slash + 1;
    }
    return DSK_ZIP_OTHER;
}

static void AppendImportConflict (TextBuffer *conflicts, const char *kind,
                                  const char *name) {
    if (conflicts == NULL) return;
    TextBufferAppend (conflicts, kind);
    TextBufferAppendChar (conflicts, ':');
    TextBufferAppendChar (conflicts, ' ');
    TextBufferAppend (conflicts, name);
    TextBufferAppendChar (conflicts, '\n');
}

static void AppendEnvConflictLines (TextBuffer *conflicts, const char *names,
                                    const char *prefix) {
    if (conflicts == NULL || names == NULL) return;
    const char *line = names;
    while (*line != '\0') {
        const char *end = strchr (line, '\n');
        size_t length = end != NULL ? (size_t)(end - line) : strlen (line);
        if (length > 0) {
            TextBufferAppend (conflicts, prefix);
            TextBufferAppendChar (conflicts, ':');
            TextBufferAppendChar (conflicts, ' ');
            TextBufferAppendN (conflicts, line, length);
            TextBufferAppendChar (conflicts, '\n');
        }
        if (end == NULL) break;
        line = end + 1;
    }
}

static bool MenuHasName (u64 id, const char *name) {
    int count = MenuCount (id);
    for (int i = 0; i < count; i++) {
        const char *menuName = MenuName (id, i);
        if (menuName != NULL && strcmp (menuName, name) == 0) return true;
    }
    return false;
}

static struct json_object *DskJsonParse (const unsigned char *data, size_t size) {
    if (data == NULL || size == 0 || size > INT_MAX) return NULL;

    struct json_object *object = HjsonParse ((const char *)data, size);
    if (object == NULL || !json_object_is_type (object, json_type_object)) {
        json_object_put (object);
        return NULL;
    }
    return object;
}

static bool DskAssetParts (const char *relative, char **name, char **ext) {
    const char *rest = relative + strlen ("assets/");
    size_t restLength = strlen (rest);
    if (restLength == 0 || rest[restLength - 1] == '/') return false;

    const char *slash = strrchr (rest, '/');
    const char *dot = strrchr (rest, '.');
    if (dot != NULL && slash != NULL && dot < slash) dot = NULL;

    size_t nameLength = dot != NULL ? (size_t)(dot - rest) : restLength;
    size_t extLength = dot != NULL ? strlen (dot) : 0;
    if (nameLength == 0 || nameLength >= 256 || extLength >= 8) return false;

    char *resultName = malloc (nameLength + 1);
    char *resultExt = malloc (extLength + 1);
    if (resultName == NULL || resultExt == NULL) {
        free (resultName);
        free (resultExt);
        return false;
    }
    memcpy (resultName, rest, nameLength);
    resultName[nameLength] = '\0';
    if (extLength > 0) memcpy (resultExt, dot, extLength);
    resultExt[extLength] = '\0';
    *name = resultName;
    *ext = resultExt;
    return true;
}

static char *DskObjectName (const char *relative) {
    const char *rest = relative + strlen ("objects/");
    size_t length = strlen (rest);
    size_t suffixLength = 0;
    if (length > 5 && strcasecmp (rest + length - 5, ".json") == 0)
        suffixLength = 5;
    else if (length > 6 && strcasecmp (rest + length - 6, ".hjson") == 0)
        suffixLength = 6;
    else
        return NULL;

    size_t nameLength = length - suffixLength;
    if (nameLength == 0 || nameLength >= 256) return NULL;

    char *name = malloc (nameLength + 1);
    if (name == NULL) return NULL;
    memcpy (name, rest, nameLength);
    name[nameLength] = '\0';
    return name;
}

static bool DskJsonNumber (struct json_object *object, const char *key,
                           double *value, bool *present) {
    struct json_object *field = NULL;
    *present = json_object_object_get_ex (object, key, &field);
    if (!*present) return true;
    if (!json_object_is_type (field, json_type_int) &&
        !json_object_is_type (field, json_type_double))
        return false;
    *value = json_object_get_double (field);
    return isfinite (*value);
}

static bool DskJsonInt (struct json_object *object, const char *key,
                        int64_t *value, bool *present) {
    struct json_object *field = NULL;
    *present = json_object_object_get_ex (object, key, &field);
    if (!*present) return true;
    if (!json_object_is_type (field, json_type_int)) return false;
    *value = json_object_get_int64 (field);
    return true;
}

static bool DskJsonBool (struct json_object *object, const char *key,
                         bool *value, bool *present) {
    struct json_object *field = NULL;
    *present = json_object_object_get_ex (object, key, &field);
    if (!*present) return true;
    if (!json_object_is_type (field, json_type_boolean)) return false;
    *value = json_object_get_boolean (field) != 0;
    return true;
}

static bool DskLoadAssetsZip (const DskZip *zip, u64 *nextId,
                             SaveIdAllocator allocId, void *context,
                             TextBuffer *conflicts) {
    bool merge = allocId != NULL;

    for (int i = 0; i < zip->count; i++) {
        const DskZipEntry *entry = &zip->entries[i];
        const char *relative = NULL;
        if (DskZipClassify (entry->name, &relative) != DSK_ZIP_ASSET)
            continue;
        size_t relativeLength = strlen (relative);
        if (relativeLength == 0 || relative[relativeLength - 1] == '/')
            continue;

        char *name = NULL;
        char *ext = NULL;
        if (!DskAssetParts (relative, &name, &ext) ||
            entry->size == 0 || entry->size > INT_MAX) {
            free (name);
            free (ext);
            return false;
        }

        if (AssetFindByName (name) != NULL) {
            if (!merge) {
                free (name);
                free (ext);
                return false;
            }
            AppendImportConflict (conflicts, "素材", name);
            free (name);
            free (ext);
            continue;
        }

        u64 id = merge ? allocId (context) : (*nextId)++;
        Asset *asset = AssetAddRaw (name, ext, entry->data,
                                    (int)entry->size, id);
        free (name);
        free (ext);
        if (asset == NULL) return false;
    }
    return true;
}

static bool DskLoadObjectsZip (const DskZip *zip, ScriptEnv *globalEnv,
                              u64 *nextId, SaveIdAllocator allocId,
                              void *context, TextBuffer *conflicts) {
    bool merge = allocId != NULL;

    for (int i = 0; i < zip->count; i++) {
        const DskZipEntry *entry = &zip->entries[i];
        const char *relative = NULL;
        if (DskZipClassify (entry->name, &relative) != DSK_ZIP_OBJECT)
            continue;
        size_t relativeLength = strlen (relative);
        if (relativeLength == 0 || relative[relativeLength - 1] == '/')
            continue;

        char *name = DskObjectName (relative);
        if (name == NULL) return false;

        if (ObjectFindByName (name) != NULL) {
            if (!merge) {
                free (name);
                return false;
            }
            AppendImportConflict (conflicts, "物件", name);
            free (name);
            continue;
        }

        struct json_object *root = DskJsonParse (entry->data, entry->size);
        if (root == NULL) {
            free (name);
            return false;
        }

        struct json_object *assetsField = NULL;
        Asset **refs = NULL;
        int refCount = 0;
        bool refsOk = true;
        if (json_object_object_get_ex (root, "assets", &assetsField)) {
            if (!json_object_is_type (assetsField, json_type_array) ||
                json_object_array_length (assetsField) > 100000) {
                json_object_put (root);
                free (name);
                return false;
            }
            size_t count = json_object_array_length (assetsField);
            if (count > 0) {
                refs = calloc (count, sizeof (Asset *));
                if (refs == NULL) {
                    json_object_put (root);
                    free (name);
                    return false;
                }
            }
            for (size_t a = 0; a < count; a++) {
                struct json_object *item = json_object_array_get_idx (assetsField, a);
                if (!json_object_is_type (item, json_type_string)) {
                    refsOk = false;
                    continue;
                }
                Asset *asset = AssetFindByName (json_object_get_string (item));
                if (asset == NULL) refsOk = false;
                else refs[refCount++] = asset;
            }
        }

        if (!refsOk) {
            if (merge) {
                AppendImportConflict (conflicts, "物件", name);
                free (refs);
                free (name);
                json_object_put (root);
                continue;
            }
            free (refs);
            free (name);
            json_object_put (root);
            return false;
        }

        struct json_object *envField = NULL;
        char *envText = NULL;
        if (json_object_object_get_ex (root, "env", &envField)) {
            if (!json_object_is_type (envField, json_type_object)) {
                free (refs);
                free (name);
                json_object_put (root);
                return false;
            }
            const char *text = json_object_to_json_string_ext (
                envField, JSON_C_TO_STRING_PLAIN);
            envText = text != NULL ? StrDup (text) : NULL;
            if (envText == NULL) {
                free (refs);
                free (name);
                json_object_put (root);
                return false;
            }
        }

        double x = 0.0, y = 0.0;
        int64_t layer = 0, assetIndex = 0;
        bool hidden = false;
        bool present = false;
        if (!DskJsonNumber (root, "x", &x, &present) ||
            !DskJsonNumber (root, "y", &y, &present) ||
            !DskJsonInt (root, "layer", &layer, &present) ||
            !DskJsonBool (root, "hidden", &hidden, &present) ||
            !DskJsonInt (root, "assetIndex", &assetIndex, &present) ||
            layer < INT_MIN || layer > INT_MAX ||
            assetIndex < 0 || assetIndex > INT_MAX) {
            free (envText);
            free (refs);
            free (name);
            json_object_put (root);
            return false;
        }

        u64 id = merge ? allocId (context) : (*nextId)++;
        Object *object = ObjectAdd (name, (Vector2){(float)x, (float)y},
                                    (int)layer, 0, id);
        if (object == NULL) {
            free (envText);
            free (refs);
            free (name);
            json_object_put (root);
            return false;
        }
        ObjectTouch (object);

        for (int a = 0; a < refCount; a++) {
            if (!IdGroupAdd (ObjectAssets (object), refs[a]->id)) {
                ObjectDelete (object);
                free (envText);
                free (refs);
                free (name);
                json_object_put (root);
                return false;
            }
        }
        object->assetIndex = (int)assetIndex < refCount ? (int)assetIndex : 0;
        object->hidden = hidden;

        if (envText != NULL &&
            !ScriptEnvLoadJson (globalEnv, id, envText)) {
            ObjectDelete (object);
            free (envText);
            free (refs);
            free (name);
            json_object_put (root);
            return false;
        }
        free (envText);
        envText = NULL;

        struct json_object *menuField = NULL;
        if (json_object_object_get_ex (root, "menu", &menuField)) {
            if (!json_object_is_type (menuField, json_type_array)) {
                ObjectDelete (object);
                free (refs);
                free (name);
                json_object_put (root);
                return false;
            }
            size_t menuCount = json_object_array_length (menuField);
            if (menuCount > 100000) {
                ObjectDelete (object);
                free (refs);
                free (name);
                json_object_put (root);
                return false;
            }
            for (size_t m = 0; m < menuCount; m++) {
                struct json_object *item =
                    json_object_array_get_idx (menuField, m);
                struct json_object *menuName = NULL;
                struct json_object *menuFunc = NULL;
                if (!json_object_is_type (item, json_type_object) ||
                    !json_object_object_get_ex (item, "name", &menuName) ||
                    !json_object_is_type (menuName, json_type_string)) {
                    ObjectDelete (object);
                    free (refs);
                    free (name);
                    json_object_put (root);
                    return false;
                }
                if (json_object_object_get_ex (item, "func", &menuFunc) &&
                    menuFunc != NULL &&
                    !json_object_is_type (menuFunc, json_type_string)) {
                    ObjectDelete (object);
                    free (refs);
                    free (name);
                    json_object_put (root);
                    return false;
                }
                if (!MenuAdd (id, json_object_get_string (menuName),
                              menuFunc != NULL ? json_object_get_string (menuFunc)
                                               : NULL)) {
                    ObjectDelete (object);
                    free (refs);
                    free (name);
                    json_object_put (root);
                    return false;
                }
            }
        }

        free (refs);
        free (name);
        json_object_put (root);
    }
    return true;
}

static bool DskLoadSceneZip (const DskZip *zip, ScriptEnv *globalEnv,
                            bool merge, TextBuffer *conflicts) {
    const DskZipEntry *scene = NULL;
    for (int i = 0; i < zip->count; i++) {
        const char *relative = NULL;
        if (DskZipClassify (zip->entries[i].name, &relative) ==
                DSK_ZIP_SCENE &&
            (strcasecmp (relative, "scene.json") == 0 ||
             strcasecmp (relative, "scene.hjson") == 0)) {
            scene = &zip->entries[i];
            break;
        }
    }
    if (scene == NULL) return true;

    struct json_object *root = DskJsonParse (scene->data, scene->size);
    if (root == NULL) return false;

    struct json_object *versionField = NULL;
    if (json_object_object_get_ex (root, "version", &versionField)) {
        if (!json_object_is_type (versionField, json_type_int) ||
            json_object_get_int64 (versionField) != (int64_t)DSK_ZIP_VERSION) {
            json_object_put (root);
            return false;
        }
    }

    struct json_object *envField = NULL;
    if (json_object_object_get_ex (root, "env", &envField)) {
        if (!json_object_is_type (envField, json_type_object)) {
            json_object_put (root);
            return false;
        }
        const char *text = json_object_to_json_string_ext (
            envField, JSON_C_TO_STRING_PLAIN);
        if (text == NULL) {
            json_object_put (root);
            return false;
        }
        if (merge) {
            TextBuffer envConflicts = {0};
            bool ok = ScriptEnvMergeJson (globalEnv, text, &envConflicts);
            if (ok && envConflicts.length > 0)
                AppendEnvConflictLines (conflicts, envConflicts.data,
                                        "全局变量");
            TextBufferFree (&envConflicts);
            if (!ok) {
                json_object_put (root);
                return false;
            }
        } else if (!ScriptEnvLoadJson (globalEnv, 0, text)) {
            json_object_put (root);
            return false;
        }
    }

    struct json_object *menuField = NULL;
    if (json_object_object_get_ex (root, "menu", &menuField)) {
        if (!json_object_is_type (menuField, json_type_array) ||
            json_object_array_length (menuField) > 100000) {
            json_object_put (root);
            return false;
        }
        size_t count = json_object_array_length (menuField);
        for (size_t m = 0; m < count; m++) {
            struct json_object *item = json_object_array_get_idx (menuField, m);
            struct json_object *menuName = NULL;
            struct json_object *menuFunc = NULL;
            if (!json_object_is_type (item, json_type_object) ||
                !json_object_object_get_ex (item, "name", &menuName) ||
                !json_object_is_type (menuName, json_type_string)) {
                json_object_put (root);
                return false;
            }
            if (json_object_object_get_ex (item, "func", &menuFunc) &&
                menuFunc != NULL &&
                !json_object_is_type (menuFunc, json_type_string)) {
                json_object_put (root);
                return false;
            }
            const char *name = json_object_get_string (menuName);
            const char *func = menuFunc != NULL
                                   ? json_object_get_string (menuFunc) : NULL;
            if (merge && MenuHasName (0, name)) {
                AppendImportConflict (conflicts, "菜单", name);
                continue;
            }
            if (!MenuAdd (0, name, func)) {
                json_object_put (root);
                return false;
            }
        }
    }

    json_object_put (root);
    return true;
}

static bool DskZipWriteData (zipFile file, const char *name,
                             const unsigned char *data, size_t size) {
    zip_fileinfo info;
    memset (&info, 0, sizeof (info));
    if (zipOpenNewFileInZip (file, name, &info, NULL, 0, NULL, 0, NULL,
                             Z_DEFLATED, Z_DEFAULT_COMPRESSION) != ZIP_OK)
        return false;

    size_t offset = 0;
    while (offset < size) {
        size_t left = size - offset;
        unsigned int chunk = left > (1024u * 1024u)
                                 ? (unsigned int)(1024u * 1024u)
                                 : (unsigned int)left;
        if (zipWriteInFileInZip (file, data + offset, chunk) != ZIP_OK)
            return false;
        offset += (size_t)chunk;
    }
    return zipCloseFileInZip (file) == ZIP_OK;
}

static bool DskPutEnv (struct json_object *root, const char *key,
                       ScriptEnv *globalEnv, u64 id) {
    bool hasEnv = id == 0 || EntityGetData (id) != NULL;
    char *text = ScriptEnvJson (globalEnv, id);
    if (hasEnv && text == NULL) return false;

    struct json_object *env = NULL;
    if (text != NULL)
        env = DskJsonParse ((const unsigned char *)text, strlen (text));
    free (text);
    if (env == NULL) {
        if (hasEnv) return false;
        env = json_object_new_object ();
        if (env == NULL) return false;
    }

    if (json_object_object_add (root, key, env) != 0) {
        json_object_put (env);
        return false;
    }
    return true;
}

static bool DskPutMenu (struct json_object *root, const char *key, u64 id) {
    int count = MenuCount (id);
    if (count == 0) return true;

    struct json_object *array = json_object_new_array ();
    if (array == NULL) return false;

    for (int i = 0; i < count; i++) {
        const char *name = MenuName (id, i);
        const char *func = MenuFunc (id, i);
        if (name == NULL) {
            json_object_put (array);
            return false;
        }

        struct json_object *item = json_object_new_object ();
        if (item == NULL ||
            json_object_object_add (item, "name",
                                    json_object_new_string (name)) != 0 ||
            (func != NULL &&
             json_object_object_add (item, "func",
                                     json_object_new_string (func)) != 0) ||
            json_object_array_add (array, item) != 0) {
            json_object_put (item);
            json_object_put (array);
            return false;
        }
    }

    if (json_object_object_add (root, key, array) != 0) {
        json_object_put (array);
        return false;
    }
    return true;
}

static bool DskWriteAssetZip (zipFile file, const Asset *asset) {
    if (asset->fileSize < 0 ||
        (asset->fileSize > 0 && asset->fileData == NULL)) return false;

    const char *ext = asset->ext;
    bool addDot = ext[0] != '\0' && ext[0] != '.';
    size_t nameLength = strlen (asset->name);
    size_t extLength = strlen (ext);
    size_t prefixLength = strlen ("dsk/assets/");
    size_t pathLength = prefixLength + nameLength + (addDot ? 1 : 0) +
                        extLength;
    char *path = malloc (pathLength + 1);
    if (path == NULL) return false;

    char *write = path;
    memcpy (write, "dsk/assets/", prefixLength);
    write += prefixLength;
    memcpy (write, asset->name, nameLength);
    write += nameLength;
    if (addDot) *write++ = '.';
    memcpy (write, ext, extLength);
    write += extLength;
    *write = '\0';

    bool ok = DskZipWriteData (file, path, asset->fileData,
                               (size_t)asset->fileSize);
    free (path);
    return ok;
}

static bool DskWriteObjectZip (zipFile file, ScriptEnv *globalEnv,
                               const Object *object) {
    struct json_object *root = json_object_new_object ();
    struct json_object *assets = json_object_new_array ();
    if (root == NULL || assets == NULL) {
        json_object_put (root);
        json_object_put (assets);
        return false;
    }
    json_object_object_add (root, "assets", assets);

    for (int i = 0; i < object->assets.count; i++) {
        Asset *asset = AssetFindById (object->assets.ids[i]);
        if (asset == NULL) {
            json_object_put (root);
            return false;
        }
        if (json_object_array_add (assets,
                                   json_object_new_string (asset->name)) != 0) {
            json_object_put (root);
            return false;
        }
    }

    if (!DskPutEnv (root, "env", globalEnv, object->id) ||
        json_object_object_add (root, "x",
                                json_object_new_double (object->pos.x)) != 0 ||
        json_object_object_add (root, "y",
                                json_object_new_double (object->pos.y)) != 0 ||
        json_object_object_add (root, "layer",
                                json_object_new_int (object->layer)) != 0 ||
        json_object_object_add (root, "hidden",
                                json_object_new_boolean (object->hidden)) != 0 ||
        json_object_object_add (root, "assetIndex",
                                json_object_new_int (object->assetIndex)) != 0 ||
        !DskPutMenu (root, "menu", object->id)) {
        json_object_put (root);
        return false;
    }

    const char *text = json_object_to_json_string_ext (
        root, JSON_C_TO_STRING_PLAIN);
    bool ok = false;
    if (text != NULL) {
        size_t nameLength = strlen (object->name);
        size_t pathLength = strlen ("dsk/objects/") + nameLength +
                            strlen (".json");
        char *path = malloc (pathLength + 1);
        if (path != NULL) {
            snprintf (path, pathLength + 1, "dsk/objects/%s.json",
                      object->name);
            ok = DskZipWriteData (file, path, (const unsigned char *)text,
                                  strlen (text));
            free (path);
        }
    }
    json_object_put (root);
    return ok;
}

static bool DskWriteSceneZip (zipFile file, ScriptEnv *globalEnv) {
    struct json_object *root = json_object_new_object ();
    if (root == NULL) return false;

    if (json_object_object_add (root, "version",
                                json_object_new_int ((int)DSK_ZIP_VERSION)) != 0 ||
        !DskPutEnv (root, "env", globalEnv, 0) ||
        !DskPutMenu (root, "menu", 0)) {
        json_object_put (root);
        return false;
    }

    const char *text = json_object_to_json_string_ext (
        root, JSON_C_TO_STRING_PLAIN);
    bool ok = text != NULL &&
              DskZipWriteData (file, "dsk/scene.json",
                               (const unsigned char *)text, strlen (text));
    json_object_put (root);
    return ok;
}

static bool DskWriteZip (const char *path, ScriptEnv *globalEnv) {
    size_t pathLength = strlen (path);
    char *tempPath = malloc (pathLength + sizeof (".tmp"));
    if (tempPath == NULL) return false;
    snprintf (tempPath, pathLength + sizeof (".tmp"), "%s.tmp", path);

    zipFile file = zipOpen (tempPath, APPEND_STATUS_CREATE);
    if (file == NULL) {
        free (tempPath);
        return false;
    }

    bool ok = true;
    IdGroup assetIds = AssetIds ();
    for (int i = 0; ok && i < assetIds.count; i++) {
        Asset *asset = EntityFind (assetIds.ids[i]);
        if (asset == NULL) ok = false;
        else ok = DskWriteAssetZip (file, asset);
    }
    IdGroupUnload (&assetIds);

    IdGroup objectIds = ObjectIds ();
    for (int i = 0; ok && i < objectIds.count; i++) {
        Object *object = EntityFind (objectIds.ids[i]);
        if (object == NULL) ok = false;
        else ok = DskWriteObjectZip (file, globalEnv, object);
    }
    IdGroupUnload (&objectIds);

    if (ok) ok = DskWriteSceneZip (file, globalEnv);
    if (zipClose (file, NULL) != ZIP_OK) ok = false;

    if (!ok || rename (tempPath, path) != 0) {
        remove (tempPath);
        free (tempPath);
        return false;
    }
    free (tempPath);
    return true;
}

bool SaveMerge (const char *path, ScriptEnv *globalEnv,
                SaveIdAllocator allocId, void *context,
                TextBuffer *conflicts) {
    if (path == NULL || path[0] == '\0' || globalEnv == NULL ||
        allocId == NULL)
        return false;

    DskZip zip;
    if (!DskReadSource (path, &zip)) return false;

    u64 nextId = 1;
    bool ok = DskLoadAssetsZip (&zip, &nextId, allocId, context, conflicts);
    if (ok)
        ok = DskLoadObjectsZip (&zip, globalEnv, &nextId, allocId, context,
                                conflicts);
    if (ok) ok = DskLoadSceneZip (&zip, globalEnv, true, conflicts);
    DskZipFree (&zip);
    return ok;
}

bool SaveRead (const char *path, ScriptEnv *globalEnv, u64 *nextEntityId) {
    if (path == NULL || path[0] == '\0' || globalEnv == NULL ||
        nextEntityId == NULL)
        return false;

    DskZip zip;
    if (!DskReadSource (path, &zip)) return false;

    u64 nextId = 1;
    bool ok = DskLoadAssetsZip (&zip, &nextId, NULL, NULL, NULL);
    if (ok)
        ok = DskLoadObjectsZip (&zip, globalEnv, &nextId, NULL, NULL, NULL);
    if (ok) ok = DskLoadSceneZip (&zip, globalEnv, false, NULL);
    DskZipFree (&zip);
    if (!ok) return false;
    *nextEntityId = nextId;
    return true;
}

bool SaveWrite (const char *path, ScriptEnv *globalEnv) {
    if (path == NULL || path[0] == '\0' || globalEnv == NULL)
        return false;
    return DskWriteZip (path, globalEnv);
}
