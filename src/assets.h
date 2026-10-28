#ifndef ASSETS_H
#define ASSETS_H

#include <stdbool.h>

#include "raylib.h"
#include "utils.h"

/**
 * @brief 素材对象。
 */
typedef struct Asset Asset;

/**
 * @brief 获取素材 ID。
 * @param asset 素材。
 * @return 素材 ID。
 */
u64 AssetId (const Asset* asset);
/**
 * @brief 获取素材名称。
 * @param asset 素材。
 * @return 素材名称。
 */
const char* AssetName (const Asset* asset);
#ifdef DESKTOP_CLIENT
/**
 * @brief 获取客户端 GPU 纹理。
 * @param asset 素材。
 * @return 纹理副本，素材无效时为零值。
 */
Texture2D AssetTexture (const Asset* asset);
#endif
#ifdef DESKTOP_SERVER
/**
 * @brief 获取服务端素材的原始文件视图。
 * @param asset 素材。
 * @return data/size/ext；data 归 Asset 所有，不要 RawFileFree。
 */
RawFile AssetRawFile (const Asset* asset);
#endif

void AssetInit (void);
/**
 * @brief 销毁全部素材并释放素材管理器。
 */
void AssetUnload (void);

/**
 * @brief 按名称升序返回全部素材 ID。
 * @return ID 组，调用方负责 IdGroupUnload。
 */
IdGroup AssetIds (void);
/**
 * @brief 按名称查找素材。
 * @param name 素材名称。
 * @return 素材，不存在时为 NULL。
 */
Asset* AssetFind (const char* name);
/**
 * @brief 创建未初始化素材，之后用 AssetLoad 加载内容。
 * @param name 唯一素材名称。
 * @param id 非 0 且未占用的新 ID。
 * @return 新素材，失败时为 NULL。
 */
Asset* AssetCreate (const char* name, u64 id);
/**
 * @brief 将内存文件加载到已有素材。
 * @param asset 目标素材。
 * @param file 图片文件，AssetLoad 不接管其内存。
 * @return 成功时为 true，否则为 false。
 *
 * 客户端重新加载纹理和 alpha；服务端只替换原始文件字节。
 */
bool AssetLoad (Asset* asset, RawFile file);
/**
 * @brief 修改素材名称。
 * @param asset 目标素材。
 * @param newName 唯一新名称。
 * @return 成功或名称未变时为 true，否则为 false。
 */
bool AssetRename (Asset* asset, const char* newName);
/**
 * @brief 删除素材并从实体表注销。
 * @param asset 目标素材。
 * @return 成功时为 true，否则为 false。
 */
bool AssetDelete (Asset* asset);
/**
 * @brief 判断指定像素是否完全不透明。
 * @param asset 目标素材。
 * @param x 图片 x 坐标。
 * @param y 图片 y 坐标。
 * @return alpha 恰好为 255 时为 true，越界或无效时为 false。
 */
bool AssetIsOpaqueAt (const Asset* asset, int x, int y);

#endif
