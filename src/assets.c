#define _POSIX_C_SOURCE 200809L

#include "assets.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "entity.h"
#include "strmap.h"

static StrMap* assetNames;

static unsigned char* LoadImageAlpha (Image image) {
    Color* colors = LoadImageColors (image);
    if (colors == NULL) return NULL;

    size_t count = (size_t)image.width * (size_t)image.height;
    unsigned char* alpha = malloc (count);
    if (alpha != NULL) {
        for (size_t i = 0; i < count; i++) alpha[i] = colors[i].a;
    }

    UnloadImageColors (colors);
    return alpha;
}

static void DestroyAsset (Asset* asset) {
    if (asset == NULL) return;
    if (asset->tex.id != 0) UnloadTexture (asset->tex);
    free (asset->alpha);
    free (asset->fileData);
    free (asset);
}

static Asset* CreateAssetFromImage (
    Image image, u64 id, const char* name, const char* ext, const unsigned char* fileData,
    int fileSize) {
    unsigned char* alpha = LoadImageAlpha (image);
    if (alpha == NULL) return NULL;

    Texture2D tex = LoadTextureFromImage (image);
    if (tex.id == 0) {
        free (alpha);
        return NULL;
    }

    Asset* asset = malloc (sizeof (Asset));
    if (asset == NULL) {
        UnloadTexture (tex);
        free (alpha);
        return NULL;
    }

    asset->tex = tex;
    asset->id = id;
    asset->alpha = alpha;
    snprintf (asset->name, sizeof (asset->name), "%s", name);
    snprintf (asset->ext, sizeof (asset->ext), "%s", (ext != NULL) ? ext : "");
    asset->fileSize = fileSize;
    asset->fileData = NULL;

    if (fileData != NULL && fileSize > 0) {
        asset->fileData = malloc ((size_t)fileSize);
        if (asset->fileData == NULL) {
            DestroyAsset (asset);
            return NULL;
        }
        memcpy (asset->fileData, fileData, (size_t)fileSize);
    }

    return asset;
}

bool AssetNameUsed (const char* name, const Asset* exclude) {
    void *existing = NULL;
    if (assetNames == NULL || !StrMapGet (assetNames, name, &existing)) return false;
    return exclude == NULL || existing != exclude;
}

static bool RegisterAsset (Asset* asset) {
    if (asset == NULL) return false;
    if (assetNames == NULL) assetNames = StrMapCreate ();
    if (assetNames == NULL) return false;

    if (!EntityRegister (asset->id, asset)) return false;
    if (!StrMapAdd (assetNames, asset->name, asset)) {
        EntityUnregister (asset->id);
        return false;
    }
    return true;
}

void AssetInit (void) {
    if (assetNames != NULL) StrMapDestroy (assetNames);
    assetNames = StrMapCreate ();
}

IdGroup AssetIds (void) {
    IdGroup group = IdGroupCreate ();
    int total = StrMapCount (assetNames);
    for (int i = 0; i < total; i++) {
        Asset *asset = NULL;
        if (StrMapAt (assetNames, i, NULL, (void **)&asset) && asset != NULL)
            IdGroupAdd (&group, asset->id);
    }
    return group;
}

Asset* AssetFindById (u64 id) {
    int total = StrMapCount (assetNames);
    for (int i = 0; i < total; i++) {
        Asset *asset = NULL;
        if (StrMapAt (assetNames, i, NULL, (void **)&asset) && asset != NULL &&
            asset->id == id)
            return asset;
    }
    return NULL;
}

Asset* AssetFindByName (const char *name) {
    void *asset = NULL;
    if (name == NULL || assetNames == NULL ||
        !StrMapGet (assetNames, name, &asset))
        return NULL;
    return asset;
}

Asset* AssetAdd (
    const char* name, const char* ext, const unsigned char* data, int size, u64 id) {
    if (name == NULL || name[0] == '\0' || data == NULL || size <= 0) return NULL;
    if (AssetNameUsed (name, NULL)) return NULL;
    if (id == 0) return NULL;
    if (EntityFind (id) != NULL) return NULL;

    Image img = LoadImageFromMemory (ext, data, size);
    if (!IsImageValid (img)) {
        UnloadImage (img);
        return NULL;
    }

    Asset* asset = CreateAssetFromImage (img, id, name, ext, data, size);
    UnloadImage (img);
    if (asset == NULL) return NULL;

    if (!RegisterAsset (asset)) {
        DestroyAsset (asset);
        return NULL;
    }
    return asset;
}

static Asset* CreateAssetRaw (u64 id, const char* name, const char* ext,
                             const unsigned char* fileData, int fileSize) {
    Asset* asset = calloc (1, sizeof (Asset));
    if (asset == NULL) return NULL;
    asset->id = id;
    snprintf (asset->name, sizeof (asset->name), "%s", name);
    snprintf (asset->ext, sizeof (asset->ext), "%s", (ext != NULL) ? ext : "");
    asset->fileSize = fileSize;
    if (fileData != NULL && fileSize > 0) {
        asset->fileData = malloc ((size_t)fileSize);
        if (asset->fileData == NULL) {
            free (asset);
            return NULL;
        }
        memcpy (asset->fileData, fileData, (size_t)fileSize);
    }
    return asset;
}

Asset* AssetAddRaw (const char* name, const char* ext, const unsigned char* data,
                    int size, u64 id) {
    if (name == NULL || name[0] == '\0' || data == NULL || size <= 0) return NULL;
    if (AssetNameUsed (name, NULL)) return NULL;
    if (id == 0 || EntityFind (id) != NULL) return NULL;

    Asset* asset = CreateAssetRaw (id, name, ext, data, size);
    if (asset == NULL) return NULL;
    if (!RegisterAsset (asset)) {
        DestroyAsset (asset);
        return NULL;
    }
    return asset;
}

bool AssetReplaceRaw (Asset* asset, const char* ext, const unsigned char* data, int size) {
    if (asset == NULL || ext == NULL || data == NULL || size <= 0) return false;
    unsigned char* fileData = malloc ((size_t)size);
    if (fileData == NULL) return false;
    memcpy (fileData, data, (size_t)size);
    free (asset->fileData);
    asset->fileData = fileData;
    asset->fileSize = size;
    snprintf (asset->ext, sizeof (asset->ext), "%s", ext);
    return true;
}

bool AssetRename (Asset* asset, const char* newName) {
    if (asset == NULL || newName == NULL || newName[0] == '\0') return false;

    char name[sizeof (asset->name)];
    snprintf (name, sizeof (name), "%s", newName);
    if (strcmp (asset->name, name) == 0) return true;
    if (AssetNameUsed (name, asset)) return false;
    if (!StrMapAdd (assetNames, name, asset)) return false;
    if (!StrMapRemove (assetNames, asset->name)) {
        StrMapRemove (assetNames, name);
        return false;
    }

    snprintf (asset->name, sizeof (asset->name), "%s", name);
    return true;
}

bool AssetReplace (Asset* asset, const char* ext, const unsigned char* data, int size) {
    if (asset == NULL || ext == NULL || data == NULL || size <= 0) return false;

    Image img = LoadImageFromMemory (ext, data, size);
    if (!IsImageValid (img)) {
        UnloadImage (img);
        return false;
    }

    unsigned char* alpha = LoadImageAlpha (img);
    Texture2D tex = LoadTextureFromImage (img);
    UnloadImage (img);

    unsigned char* fileData = malloc ((size_t)size);
    if (alpha == NULL || tex.id == 0 || fileData == NULL) {
        free (alpha);
        free (fileData);
        if (tex.id != 0) UnloadTexture (tex);
        return false;
    }
    memcpy (fileData, data, (size_t)size);

    if (asset->tex.id != 0) UnloadTexture (asset->tex);
    free (asset->alpha);
    free (asset->fileData);

    asset->tex = tex;
    asset->alpha = alpha;
    asset->fileData = fileData;
    asset->fileSize = size;
    snprintf (asset->ext, sizeof (asset->ext), "%s", ext);
    return true;
}

bool AssetDelete (Asset* asset) {
    if (asset == NULL || EntityFind (asset->id) != asset) return false;

    StrMapRemove (assetNames, asset->name);
    EntityUnregister (asset->id);
    DestroyAsset (asset);
    return true;
}

bool AssetIsOpaqueAt (const Asset* asset, int x, int y) {
    if (asset == NULL || asset->alpha == NULL) return false;
    if (x < 0 || y < 0 || x >= asset->tex.width || y >= asset->tex.height) return false;

    return (asset->alpha[(size_t)y * (size_t)asset->tex.width + (size_t)x] == 255);
}

void AssetUnload (void) {
    int count = StrMapCount (assetNames);
    for (int i = 0; i < count; i++) {
        Asset *asset = NULL;
        if (!StrMapAt (assetNames, i, NULL, (void **)&asset) || asset == NULL)
            continue;
        EntityUnregister (asset->id);
        DestroyAsset (asset);
    }

    StrMapDestroy (assetNames);
    assetNames = NULL;
}
