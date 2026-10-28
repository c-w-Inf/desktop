#define _POSIX_C_SOURCE 200809L

#include "save.h"

#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <json-c/json.h>

#include "assets.h"
#include "cutils.h"

#include <uv.h>
#include <zip.h>

#include "entity.h"
#include "hjson.h"
#include "menu.h"
#include "object.h"

#ifndef S_ISLNK
#define S_ISLNK(mode) (((mode) & S_IFMT) == S_IFLNK)
#endif
#ifndef S_ISDIR
#define S_ISDIR(mode) (((mode) & S_IFMT) == S_IFDIR)
#endif
#ifndef S_ISREG
#define S_ISREG(mode) (((mode) & S_IFMT) == S_IFREG)
#endif

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
    int error = 0;
    zip_t *file = zip_open (path, ZIP_RDONLY, &error);
    if (file == NULL) return false;

    zip_int64_t count = zip_get_num_entries (file, 0);
    if (count < 0 || count > DSK_FOLDER_MAX_ENTRIES) {
        zip_close (file);
        return false;
    }

    bool ok = true;
    size_t total = 0;
    for (zip_uint64_t i = 0; ok && i < (zip_uint64_t)count; i++) {
        const char *name = zip_get_name (file, i, 0);
        if (name == NULL || strlen (name) >= 4096) {
            ok = false;
            break;
        }

        zip_stat_t stat;
        zip_stat_init (&stat);
        if (zip_stat_index (file, i, 0, &stat) != 0 ||
            !(stat.valid & ZIP_STAT_SIZE) ||
            ((stat.valid & ZIP_STAT_ENCRYPTION_METHOD) &&
             stat.encryption_method != ZIP_EM_NONE)) {
            ok = false;
            break;
        }

        size_t nameLength = strlen (name);
        if (nameLength > 0 && name[nameLength - 1] == '/') continue;

        uint64_t size = stat.size;
        if (size > DSK_ZIP_MAX_ENTRY || size > SIZE_MAX ||
            total > DSK_ZIP_MAX_TOTAL - size) {
            ok = false;
            break;
        }

        unsigned char *data = size > 0 ? malloc ((size_t)size) : NULL;
        if (size > 0 && data == NULL) {
            ok = false;
            break;
        }

        zip_file_t *entry = zip_fopen_index (file, i, 0);
        if (entry == NULL) {
            free (data);
            ok = false;
            break;
        }
        size_t offset = 0;
        while (offset < (size_t)size) {
            zip_int64_t read = zip_fread (entry, data + offset,
                                          (zip_uint64_t)((size_t)size - offset));
            if (read <= 0) {
                ok = false;
                break;
            }
            offset += (size_t)read;
        }
        if (zip_fclose (entry) != 0) ok = false;
        if (!ok || !DskZipAdd (zip, name, data, (size_t)size)) {
            free (data);
            ok = false;
            break;
        }
        total += (size_t)size;
    }

    zip_close (file);
    if (!ok) {
        DskZipFree (zip);
        return false;
    }
    return true;
}


static bool DskUvStat (const char *path, uv_stat_t *info, bool link) {
    uv_fs_t request;
    int result = link ? uv_fs_lstat (NULL, &request, path, NULL)
                      : uv_fs_stat (NULL, &request, path, NULL);
    if (result < 0) {
        uv_fs_req_cleanup (&request);
        return false;
    }
    *info = request.statbuf;
    uv_fs_req_cleanup (&request);
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

    uv_fs_t request;
    int scanResult = uv_fs_scandir (NULL, &request, directoryPath, 0, NULL);
    if (scanResult < 0) {
        uv_fs_req_cleanup (&request);
        return false;
    }

    bool ok = true;
    uv_dirent_t entry;
    int next;
    while (ok && (next = uv_fs_scandir_next (&request, &entry)) == 0) {
        if (strcmp (entry.name, ".") == 0 || strcmp (entry.name, "..") == 0)
            continue;

        char childRelative[DSK_PATH_MAX];
        length = relative[0] == '\0'
                     ? snprintf (childRelative, sizeof (childRelative), "%s",
                                 entry.name)
                     : snprintf (childRelative, sizeof (childRelative), "%s/%s",
                                 relative, entry.name);
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

        uv_stat_t info;
        if (!DskUvStat (childPath, &info, true)) {
            ok = false;
            break;
        }
        if (S_ISLNK (info.st_mode)) continue;
        if (S_ISDIR (info.st_mode)) {
            if (!DskReadDirectoryTree (root, childRelative, zip, total,
                                       depth + 1))
                ok = false;
            continue;
        }
        if (!S_ISREG (info.st_mode)) continue;
        if ((uint64_t)info.st_size > DSK_ZIP_MAX_ENTRY ||
            (uint64_t)info.st_size > SIZE_MAX ||
            *total > DSK_ZIP_MAX_TOTAL - (uint64_t)info.st_size ||
            zip->count >= DSK_FOLDER_MAX_ENTRIES) {
            ok = false;
            break;
        }

        RawFile file = RawFileRead (childPath);
        if (file.data == NULL || (uint64_t)file.size != (uint64_t)info.st_size ||
            !DskZipAdd (zip, childRelative, file.data, file.size)) {
            RawFileFree (&file);
            ok = false;
            break;
        }
        *total += file.size;
    }
    if (next != UV_EOF) ok = false;
    uv_fs_req_cleanup (&request);
    return ok;
}

static bool DskReadSource (const char *path, DskZip *zip) {
    uv_stat_t info;
    if (path == NULL || path[0] == '\0' || !DskUvStat (path, &info, false))
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


#ifdef DESKTOP_SERVER
static bool DskEqualsN (const char *left, const char *right, size_t length) {
    for (size_t i = 0; i < length; i++) {
        unsigned char a = (unsigned char)left[i];
        unsigned char b = (unsigned char)right[i];
        if (tolower (a) != tolower (b)) return false;
    }
    return true;
}

static bool DskEquals (const char *left, const char *right) {
    size_t length = strlen (left);
    return strlen (right) == length && DskEqualsN (left, right, length);
}

static bool DskEndsWith (const char *text, const char *suffix) {
    size_t length = strlen (text);
    size_t suffixLength = strlen (suffix);
    return length >= suffixLength &&
           DskEquals (text + length - suffixLength, suffix);
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
            ((length == 10 && DskEqualsN (part, "scene.json", 10)) ||
             (length == 11 && DskEqualsN (part, "scene.hjson", 11)))) {
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
    if (length > 5 && DskEndsWith (rest, ".json"))
        suffixLength = 5;
    else if (length > 6 && DskEndsWith (rest, ".hjson"))
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

        if (AssetFind (name) != NULL) {
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
        Asset *asset = AssetCreate (name, id);
        RawFile file = {(unsigned char *)entry->data, entry->size, ""};
        snprintf (file.ext, sizeof (file.ext), "%s", ext);
        if (asset == NULL || !AssetLoad (asset, file)) {
            if (asset != NULL) AssetDelete (asset);
            free (name);
            free (ext);
            return false;
        }
        free (name);
        free (ext);
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

        if (ObjectFind (name) != NULL) {
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
                Asset *asset = AssetFind (json_object_get_string (item));
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
            if (!IdGroupAdd (ObjectAssets (object), AssetId(refs[a]))) {
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
            (DskEquals (relative, "scene.json") ||
             DskEquals (relative, "scene.hjson"))) {
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

typedef struct {
    zip_t *zip;
    unsigned char **owned;
    int ownedCount;
    int ownedCapacity;
} DskWriter;

static bool DskWriterKeep (DskWriter *writer, unsigned char *data) {
    if (writer->ownedCount == writer->ownedCapacity) {
        int capacity = writer->ownedCapacity == 0 ? 16 : writer->ownedCapacity * 2;
        unsigned char **owned = realloc (
            writer->owned, (size_t)capacity * sizeof (unsigned char *));
        if (owned == NULL) return false;
        writer->owned = owned;
        writer->ownedCapacity = capacity;
    }
    writer->owned[writer->ownedCount++] = data;
    return true;
}

static void DskWriterFree (DskWriter *writer) {
    for (int i = 0; i < writer->ownedCount; i++) free (writer->owned[i]);
    free (writer->owned);
    memset (writer, 0, sizeof (*writer));
}

static bool DskZipWriteData (DskWriter *writer, const char *name,
                             const unsigned char *data, size_t size,
                             bool copy) {
    if (copy) {
        unsigned char *owned = malloc (size);
        if (owned == NULL) return false;
        memcpy (owned, data, size);
        if (!DskWriterKeep (writer, owned)) {
            free (owned);
            return false;
        }
        data = owned;
    }

    zip_source_t *source = zip_source_buffer (writer->zip, data,
                                              (zip_uint64_t)size, 0);
    if (source == NULL) return false;
    zip_int64_t index = zip_file_add (writer->zip, name, source,
                                      ZIP_FL_ENC_UTF_8);
    if (index < 0) {
        zip_source_free (source);
        return false;
    }
    return zip_set_file_compression (writer->zip, (zip_uint64_t)index,
                                     ZIP_CM_DEFLATE, 9) == 0;
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

static bool DskWriteAssetZip (DskWriter *writer, const Asset *asset) {
    RawFile file = AssetRawFile(asset);
    if (file.size > 0 && file.data == NULL) return false;

    const char *ext = file.ext;
    bool addDot = ext[0] != '\0' && ext[0] != '.';
    size_t nameLength = strlen (AssetName(asset));
    size_t extLength = strlen (ext);
    size_t prefixLength = strlen ("dsk/assets/");
    size_t pathLength = prefixLength + nameLength + (addDot ? 1 : 0) +
                        extLength;
    char *path = malloc (pathLength + 1);
    if (path == NULL) return false;

    char *write = path;
    memcpy (write, "dsk/assets/", prefixLength);
    write += prefixLength;
    memcpy (write, AssetName(asset), nameLength);
    write += nameLength;
    if (addDot) *write++ = '.';
    memcpy (write, ext, extLength);
    write += extLength;
    *write = '\0';

    bool ok = DskZipWriteData (writer, path, file.data, file.size, false);
    free (path);
    return ok;
}

static bool DskWriteObjectZip (DskWriter *writer, ScriptEnv *globalEnv,
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
        Asset *asset = (Asset *)EntityFind (object->assets.ids[i]);
        if (asset == NULL) {
            json_object_put (root);
            return false;
        }
        if (json_object_array_add (assets,
                                   json_object_new_string (AssetName(asset))) != 0) {
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
            ok = DskZipWriteData (writer, path, (const unsigned char *)text,
                                  strlen (text), true);
            free (path);
        }
    }
    json_object_put (root);
    return ok;
}

static bool DskWriteSceneZip (DskWriter *writer, ScriptEnv *globalEnv) {
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
              DskZipWriteData (writer, "dsk/scene.json",
                               (const unsigned char *)text, strlen (text), true);
    json_object_put (root);
    return ok;
}

static char *DskTempPath (const char *path) {
    size_t size = strlen (path) + 64;
    char *temp = malloc (size);
    if (temp == NULL) return NULL;
    snprintf (temp, size, "%s.tmp-%ld", path, (long)uv_os_getpid ());
    return temp;
}

static bool DskSyncFile (const char *path) {
    uv_fs_t request;
    int openResult = uv_fs_open (NULL, &request, path, UV_FS_O_RDWR, 0, NULL);
    uv_fs_req_cleanup (&request);
    if (openResult < 0) return false;

    int fd = openResult;
    bool ok = uv_fs_fsync (NULL, &request, fd, NULL) == 0;
    uv_fs_req_cleanup (&request);
    if (uv_fs_close (NULL, &request, fd, NULL) < 0) ok = false;
    uv_fs_req_cleanup (&request);
    return ok;
}

static bool DskReplaceFile (const char *temp, const char *path) {
    uv_fs_t request;
    int result = uv_fs_rename (NULL, &request, temp, path, NULL);
    uv_fs_req_cleanup (&request);
    return result == 0;
}

static void DskRemoveFile (const char *path) {
    uv_fs_t request;
    uv_fs_unlink (NULL, &request, path, NULL);
    uv_fs_req_cleanup (&request);
}

static bool DskWriteZip (const char *path, ScriptEnv *globalEnv) {
    char *tempPath = DskTempPath (path);
    if (tempPath == NULL) return false;

    int error = 0;
    zip_t *archive = zip_open (tempPath, ZIP_CREATE | ZIP_TRUNCATE, &error);
    if (archive == NULL) {
        free (tempPath);
        return false;
    }

    DskWriter writer = {0};
    writer.zip = archive;
    bool ok = true;

    IdGroup assetIds = AssetIds ();
    for (int i = 0; ok && i < assetIds.count; i++) {
        Asset *asset = EntityFind (assetIds.ids[i]);
        if (asset == NULL) ok = false;
        else ok = DskWriteAssetZip (&writer, asset);
    }
    IdGroupUnload (&assetIds);

    IdGroup objectIds = ObjectIds ();
    for (int i = 0; ok && i < objectIds.count; i++) {
        Object *object = EntityFind (objectIds.ids[i]);
        if (object == NULL) ok = false;
        else ok = DskWriteObjectZip (&writer, globalEnv, object);
    }
    IdGroupUnload (&objectIds);

    if (ok) ok = DskWriteSceneZip (&writer, globalEnv);
    if (zip_close (archive) != 0) {
        if (ok) zip_discard (archive);
        ok = false;
    }
    DskWriterFree (&writer);

    if (!ok || !DskSyncFile (tempPath) || !DskReplaceFile (tempPath, path)) {
        DskRemoveFile (tempPath);
        free (tempPath);
        return false;
    }
    free (tempPath);
    return true;
}

#endif

RawFile SavePackFolderData (const char *path) {
    RawFile result = {0};
    if (path == NULL || path[0] == '\0') return result;

    DskZip source;
    if (!DskReadSource (path, &source)) return result;

    char directory[1024];
    size_t directorySize = sizeof (directory);
    if (uv_os_tmpdir (directory, &directorySize) != 0) {
        DskZipFree (&source);
        return result;
    }
    char tempPath[1024];
    int needed = snprintf (tempPath, sizeof (tempPath),
                           "%s/desktop-folder-XXXXXX", directory);
    if (needed < 0 || (size_t)needed >= sizeof (tempPath)) {
        DskZipFree (&source);
        return result;
    }

    uv_fs_t request;
    int fd = uv_fs_mkstemp (NULL, &request, tempPath, NULL);
    uv_fs_req_cleanup (&request);
    if (fd < 0) {
        DskZipFree (&source);
        return result;
    }
    uv_fs_close (NULL, &request, fd, NULL);
    uv_fs_req_cleanup (&request);

    int error = 0;
    zip_t *archive = zip_open (tempPath, ZIP_CREATE | ZIP_TRUNCATE, &error);
    bool ok = archive != NULL;
    for (int i = 0; ok && i < source.count; i++) {
        zip_source_t *entry = zip_source_buffer (
            archive, source.entries[i].data, source.entries[i].size, 0);
        if (entry == NULL ||
            zip_file_add (archive, source.entries[i].name, entry,
                          ZIP_FL_ENC_UTF_8) < 0) {
            if (entry != NULL) zip_source_free (entry);
            ok = false;
        }
    }
    if (archive != NULL) {
        if (zip_close (archive) != 0) {
            zip_discard (archive);
            ok = false;
        }
    }

    if (ok) {
        result = RawFileRead (tempPath);
        if (result.data != NULL) {
            snprintf (result.ext, sizeof (result.ext), "%s", ".dsk");
        }
    }
    uv_fs_unlink (NULL, &request, tempPath, NULL);
    uv_fs_req_cleanup (&request);
    DskZipFree (&source);
    return result;
}

#ifdef DESKTOP_SERVER
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
#endif
