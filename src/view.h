#ifndef VIEW_H
#define VIEW_H

#include "raylib.h"
#include "ui.h"
#include "object.h"

/**
 * @brief 全局画布视图状态。
 */
extern Camera2D viewCam;
extern BackgroundStyle viewBackground;
extern Shader viewShadowShader;
extern Object **viewDrawOrder;
extern int viewDrawOrderCapacity;
extern const Object *viewDragPreview;
extern Vector2 viewDragPreviewPos;

/**
 * @brief 初始化相机、背景和阴影着色器。
 */
void ViewInit ();
/**
 * @brief 释放视图持有的绘制数组和着色器。
 */
void ViewUnload ();
/**
 * @brief 窗口尺寸变化后更新相机偏移。
 */
void ViewUpdateViewport ();
/**
 * @brief 设置背景样式。
 * @param background 背景样式。
 */
void ViewSetBackground (BackgroundStyle background);
/**
 * @brief 将相机目标重置到世界原点。
 */
void ViewSetOrigin ();
/**
 * @brief 设置缩放并保持屏幕中心对应的世界坐标不变。
 * @param zoom 目标缩放，内部限制在允许范围内。
 */
void ViewSetZoom (float zoom);
/**
 * @brief 以指定屏幕点为中心缩放。
 * @param screenPoint 屏幕坐标。
 * @param wheel 滚轮增量。
 */
void ViewZoomAt (Vector2 screenPoint, float wheel);
/**
 * @brief 按屏幕像素位移平移画布。
 * @param mouseDelta 屏幕像素位移。
 */
void ViewPan (Vector2 mouseDelta);
/**
 * @brief 将屏幕坐标转换为世界坐标。
 * @param screenPoint 屏幕坐标。
 * @return 世界坐标。
 */
Vector2 ViewScreenToWorld (Vector2 screenPoint);
/**
 * @brief 设置拖动中的物件预览位置。
 * @param object 正在拖动的物件。
 * @param position 预览世界坐标。
 */
void ViewSetDragPreview (const Object *object, Vector2 position);
/**
 * @brief 清除拖动预览。
 */
void ViewClearDragPreview ();
/**
 * @brief 绘制背景、物件和拖动预览。
 * @param chosen 当前选中的物件，可为 NULL。
 */
void ViewDraw (const Object *chosen);

#endif
