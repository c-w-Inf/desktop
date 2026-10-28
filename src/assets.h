#ifndef ASSETS_H
#define ASSETS_H

#include <stdbool.h>

#include "raylib.h"
#include "utils.h"

typedef struct Asset {
    Texture2D tex;
    int width;
    int height;
    u64 id;
    char name[256];
    unsigned char* alpha;     // 每个像素的 a 通道
    unsigned char* fileData;  // 原始压缩文件数据（联机同步用）
    int fileSize;
    char ext[8];
} Asset;

void AssetInit (void);
void AssetUnload (void);

/* 返回按名称升序排列的素材 ID 组，调用方负责 IdGroupUnload。 */
IdGroup AssetIds (void);
Asset* AssetFindById (u64 id);
Asset* AssetFindByName (const char* name);
Asset* AssetAdd (const char* name, const char* ext, const unsigned char* data, int size, u64 id);
Asset* AssetAddRaw (const char* name, const char* ext, const unsigned char* data, int size, u64 id);
bool AssetRename (Asset* asset, const char* newName);
bool AssetReplace (Asset* asset, const char* ext, const unsigned char* data, int size);
bool AssetReplaceRaw (Asset* asset, const char* ext, const unsigned char* data, int size);
bool AssetNameUsed (const char* name, const Asset* exclude);
bool AssetDelete (Asset* asset);
bool AssetIsOpaqueAt (const Asset* asset, int x, int y);

#endif
