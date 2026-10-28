#define _POSIX_C_SOURCE 200809L

#include "assets.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "entity.h"
#include "strmap.h"

static StrMap* assetNames;
#define MAX_PENDING_ASSETS 1024
static Asset* pendingAssets[MAX_PENDING_ASSETS];
static int pendingAssetCount;

struct Asset {
    u64 id;
    char name[256];
    char ext[8];
#ifdef DESKTOP_CLIENT
    Texture2D tex;
    unsigned char* alpha;
#endif
#ifdef DESKTOP_SERVER
    unsigned char* fileData;
    int fileSize;
#endif
};

u64 AssetId (const Asset* asset) {
    return asset != NULL ? asset->id : 0;
}

const char* AssetName (const Asset* asset) {
    return asset != NULL ? asset->name : "";
}

#ifdef DESKTOP_CLIENT
Texture2D AssetTexture (const Asset* asset) {
    return asset != NULL ? asset->tex : (Texture2D){0};
}
#endif

#ifdef DESKTOP_SERVER
RawFile AssetRawFile (const Asset* asset) {
    RawFile file = {0};
    if (asset == NULL) return file;
    file.data = asset->fileData;
    file.size = (size_t)asset->fileSize;
    snprintf (file.ext, sizeof (file.ext), "%s", asset->ext);
    return file;
}
#endif

#ifdef DESKTOP_CLIENT
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
#endif

static void DestroyAsset (Asset* asset) {
    if (asset == NULL) return;
#ifdef DESKTOP_CLIENT
    if (asset->tex.id != 0) UnloadTexture (asset->tex);
    free (asset->alpha);
#endif
#ifdef DESKTOP_SERVER
    free (asset->fileData);
#endif
    free (asset);
}

#ifdef DESKTOP_CLIENT
static bool AssetLoadClient (Asset* asset, const char* ext,
                             const unsigned char* data, int size) {
    Image image = LoadImageFromMemory (ext, data, size);
    if (!IsImageValid (image)) {
        UnloadImage (image);
        return false;
    }

    unsigned char* alpha = LoadImageAlpha (image);
    Texture2D tex = LoadTextureFromImage (image);
    UnloadImage (image);

    if (alpha == NULL || tex.id == 0) {
        free (alpha);
        if (tex.id != 0) UnloadTexture (tex);
        return false;
    }

    if (asset->tex.id != 0) UnloadTexture (asset->tex);
    free (asset->alpha);

    asset->tex = tex;
    asset->alpha = alpha;
    snprintf (asset->ext, sizeof (asset->ext), "%s", ext);
    return true;
}
#endif

static bool RegisterAsset (Asset* asset) {
    if (asset == NULL) return false;
    if (assetNames == NULL) assetNames = StrMapCreate ();
    if (assetNames == NULL) return false;

    if (!EntityRegister (asset->id, asset, ENTITY_KIND_ASSET)) return false;
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

Asset* AssetFind (const char *name) {
    void *asset = NULL;
    if (name == NULL || assetNames == NULL ||
        !StrMapGet (assetNames, name, &asset))
        return NULL;
    return asset;
}

bool AssetLoad (Asset* asset, RawFile file) {
    if (asset == NULL || file.data == NULL || file.size == 0 ||
        file.size > INT_MAX) return false;
#ifdef DESKTOP_CLIENT
    return AssetLoadClient (asset, file.ext, file.data, (int)file.size);
#else
    unsigned char* fileData = malloc (file.size);
    if (fileData == NULL) return false;
    memcpy (fileData, file.data, file.size);
    free (asset->fileData);
    asset->fileData = fileData;
    asset->fileSize = (int)file.size;
    snprintf (asset->ext, sizeof (asset->ext), "%s", file.ext);
    return true;
#endif
}

Asset* AssetCreate (const char* name, u64 id) {
    if (name == NULL || name[0] == '\0') return NULL;
    if (AssetFind (name) != NULL) return NULL;
    if (id == 0 || EntityFind (id) != NULL) return NULL;

    Asset* asset = calloc (1, sizeof (Asset));
    if (asset == NULL) return NULL;
    asset->id = id;
    snprintf (asset->name, sizeof (asset->name), "%s", name);

    if (!RegisterAsset (asset)) {
        DestroyAsset (asset);
        return NULL;
    }
    fflush (stdout);
    return asset;
}

bool AssetRename (Asset* asset, const char* newName) {
    if (asset == NULL || newName == NULL || newName[0] == '\0') return false;

    char name[sizeof (asset->name)];
    snprintf (name, sizeof (name), "%s", newName);
    if (strcmp (asset->name, name) == 0) return true;
    Asset *existing = AssetFind (name);
    if (existing != NULL && existing != asset) return false;
    if (!StrMapAdd (assetNames, name, asset)) return false;
    if (!StrMapRemove (assetNames, asset->name)) {
        StrMapRemove (assetNames, name);
        return false;
    }

    snprintf (asset->name, sizeof (asset->name), "%s", name);
    return true;
}

bool AssetDelete (Asset* asset) {
    if (asset == NULL || EntityFind (asset->id) != asset) return false;

    fflush (stdout);
    StrMapRemove (assetNames, asset->name);
    EntityUnregister (asset->id);
    if (pendingAssetCount < MAX_PENDING_ASSETS)
        pendingAssets[pendingAssetCount++] = asset;
    else
        DestroyAsset (asset);
    return true;
}

#ifdef DESKTOP_CLIENT
bool AssetIsOpaqueAt (const Asset* asset, int x, int y) {
    if (asset == NULL || asset->alpha == NULL) return false;
    if (x < 0 || y < 0 || x >= asset->tex.width || y >= asset->tex.height)
        return false;

    return (asset->alpha[(size_t)y * (size_t)asset->tex.width + (size_t)x] == 255);
}
#endif

void AssetUnload (void) {
    for (int i = 0; i < pendingAssetCount; i++)
        DestroyAsset (pendingAssets[i]);
    pendingAssetCount = 0;
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
