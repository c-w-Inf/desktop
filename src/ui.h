#ifndef UI_H
#define UI_H

#include <stdbool.h>

#include "assets.h"
#include "object.h"
#include "raylib.h"

/**
 * @brief 画布背景样式。
 */
typedef enum BackgroundStyle {
    BACKGROUND_WHITE = 0,
    BACKGROUND_DOTS,
    BACKGROUND_GRID,
    BACKGROUND_CHECKER
} BackgroundStyle;

/**
 * @brief 顶部栏高度。
 */
#define UI_BAR_HEIGHT 36
/**
 * @brief 顶部菜单数量。
 */
#define UI_MENU_COUNT 5
/**
 * @brief 同时显示的消息数量。
 */
#define UI_MAX_MESSAGES 8

/**
 * @brief 界面消息类型。
 */
typedef enum UiMessageKind { UI_MESSAGE_INFO = 0, UI_MESSAGE_ERROR, UI_MESSAGE_SCRIPT } UiMessageKind;

/**
 * @brief UiUpdate 返回的动作类型。
 */
typedef enum UiAction {
    UI_ACTION_NONE = 0,
    UI_ACTION_CLOSE,
    UI_ACTION_UI,
    UI_ACTION_IMPORT,
    UI_ACTION_RENAME_ASSET,
    UI_ACTION_REPLACE_ASSET,
    UI_ACTION_DELETE_ASSET,
    UI_ACTION_RENAME_OBJECT,
    UI_ACTION_DELETE_OBJECT,
    UI_ACTION_CREATE_OBJECT,
    UI_ACTION_ADD_OBJECT_ASSET,
    UI_ACTION_USE_OBJECT_ASSET,
    UI_ACTION_REMOVE_OBJECT_ASSET,
    UI_ACTION_MOVE_OBJECT_ASSET,
    UI_ACTION_SET_OBJECT_LAYER,
    UI_ACTION_LOCK_OBJECT,
    UI_ACTION_UNLOCK_OBJECT,
    UI_ACTION_RUN_MENU,
    UI_ACTION_RUN_GLOBAL_MENU,
    UI_ACTION_RUN_USER_MENU,
    UI_ACTION_SET_BACKGROUND,
    UI_ACTION_VIEW_ORIGIN,
    UI_ACTION_SET_ZOOM,
    UI_ACTION_OPEN_SERVER,
    UI_ACTION_CONNECT,
    UI_ACTION_IMPORT_SAVE,
    UI_ACTION_EXPORT_SAVE,
    UI_ACTION_RUN_SCRIPT,
    UI_ACTION_CANVAS_MOVE,
    UI_ACTION_CANVAS_CLICK,
    UI_ACTION_CANVAS_DRAG,
    UI_ACTION_CANVAS_RELEASE,
    UI_ACTION_CANVAS_RIGHT_CLICK,
    UI_ACTION_CANVAS_ZOOM
} UiAction;

typedef struct UiObjectEvent {
    Object* object;
} UiObjectEvent;
typedef struct UiObjectNameEvent {
    Object* object;
    char name[256];
} UiObjectNameEvent;
typedef struct UiAssetEvent {
    Asset* asset;
} UiAssetEvent;
typedef struct UiAssetNameEvent {
    Asset* asset;
    char name[256];
} UiAssetNameEvent;
typedef struct UiAssetPathEvent {
    Asset* asset;
    char path[1024];
} UiAssetPathEvent;
typedef struct UiObjectAssetEvent {
    Object* object;
    Asset* asset;
} UiObjectAssetEvent;
typedef struct UiObjectAssetsEvent {
    Object* object;
    int index;
    int target;
} UiObjectAssetsEvent;
typedef struct UiObjectMenuEvent {
    Object* object;
    int index;
} UiObjectMenuEvent;
typedef struct UiMenuEvent {
    int index;
} UiMenuEvent;
typedef struct UiPathEvent {
    char path[1024];
} UiPathEvent;
typedef struct UiConnectEvent {
    char host[256];
    int port;
    char username[128];
} UiConnectEvent;
typedef struct UiBackgroundEvent {
    BackgroundStyle style;
} UiBackgroundEvent;
typedef struct UiZoomEvent {
    float value;
} UiZoomEvent;
typedef struct UiObjectLayerEvent {
    Object* object;
    int layer;
} UiObjectLayerEvent;
typedef struct UiCanvasZoomEvent {
    float wheel;
} UiCanvasZoomEvent;

/**
 * @brief 一帧 UI 输入产生的动作和参数。
 */
typedef struct UiEvent {
    UiAction action;
    Vector2 mousePosition;
    Vector2 mouseDelta;
    FilePathList files;
    union {
        UiObjectEvent object;
        UiObjectNameEvent objectName;
        UiAssetEvent asset;
        UiAssetNameEvent assetName;
        UiAssetPathEvent assetPath;
        UiObjectAssetEvent objectAsset;
        UiObjectAssetsEvent objectAssets;
        UiObjectMenuEvent objectMenu;
        UiMenuEvent menu;
        UiPathEvent path;
        UiConnectEvent connect;
        UiBackgroundEvent background;
        UiZoomEvent zoom;
        UiObjectLayerEvent objectLayer;
        UiCanvasZoomEvent canvasZoom;
    } u;
} UiEvent;

/**
 * @brief 全局界面状态。
 */
extern bool uiShowConnectMenu;
extern int uiMenuOpen;
extern bool uiPickerOpen;
extern float uiPickerScroll;
extern bool uiManagerOpen;
extern float uiManagerScroll;
extern Asset* uiSelectedAsset;
extern bool uiObjectManagerOpen;
extern float uiObjectScroll;
extern Object* uiSelectedObject;
extern bool uiObjectContextOpen;
extern Object* uiContextObject;
extern Vector2 uiContextPos;
extern bool uiLayerOpen;
extern Object* uiLayerObject;
extern char uiLayerText[32];
extern bool uiObjectAssetsOpen;
extern Object* uiAssetsObject;
extern int uiAssetsObjectSelected;
extern float uiAssetsObjectScroll;
extern Object* uiPickerObject;
extern bool uiRenameOpen;
extern bool uiRenameIsObject;
extern Asset* uiRenameAsset;
extern Object* uiRenameObject;
extern char uiRenameText[256];
extern bool uiConnectOpen;
extern int uiConnectField;
extern bool uiScriptOpen;
extern TextBuffer uiScriptText;
extern char uiScriptSelf[768];
extern bool uiScriptSelfFocused;
extern size_t uiScriptCursor;
extern int uiScriptPreferredColumn;
extern float uiScriptScroll;
extern char uiConnectUser[128];
extern char uiConnectHost[256];
extern char uiConnectPort[16];
extern char uiMessages[UI_MAX_MESSAGES][320];
extern float uiMessageTimes[UI_MAX_MESSAGES];
extern UiMessageKind uiMessageKinds[UI_MAX_MESSAGES];
extern int uiMessageCount;
extern bool uiPress;
extern int uiSubmenuOpen;
extern float uiMenuX[UI_MENU_COUNT];
extern float uiMenuW[UI_MENU_COUNT];

/**
 * @brief 初始化界面状态和字体。
 */
void UiInit ();
/**
 * @brief 释放界面持有的资源。
 */
void UiUnload ();
/**
 * @brief 显示一条普通消息。
 * @param message 消息文本。
 */
void UiShowMessage (const char* message);
/**
 * @brief 显示一条错误消息。
 * @param message 错误文本。
 */
void UiShowError (const char* message);
/**
 * @brief 在屏幕提示区显示指定类型消息。
 * @param message 消息文本。
 * @param kind 消息类型。
 */
void UiShowMessageTextKind (const char* message, UiMessageKind kind);
/**
 * @brief 打开素材管理窗口。
 */
void UiOpenAssetManager ();
/**
 * @brief 设置是否显示连接菜单。
 * @param visible 是否显示。
 */
void UiSetConnectMenuVisible (bool visible);
/**
 * @brief 根据本地或远程模式重建文件菜单。
 * @param localMode 是否为本地服务端模式。
 */
void UiSetLocalMode (bool localMode);
/**
 * @brief 打开物件管理窗口并可预选一个已锁定的物件。
 * @param selectObject 要预选的物件，可为 NULL。
 */
void UiOpenObjectManager (Object* selectObject);
/**
 * @brief 打开物件的素材管理窗口。
 * @param object 目标物件。
 */
void UiOpenObjectAssets (Object* object);
/**
 * @brief 选择物件并刷新相关弹窗。
 * @param object 要选择的物件，可为 NULL。
 */
void UiSelectObject (Object* object);
/**
 * @brief 打开物件右键菜单。
 * @param object 目标物件。
 * @param screenPos 菜单屏幕坐标。
 */
void UiOpenObjectContext (Object* object, Vector2 screenPos);
/**
 * @brief 打开画布空白处右键菜单。
 * @param screenPos 菜单屏幕坐标。
 */
void UiOpenGlobalContext (Vector2 screenPos);
/**
 * @brief 清除已删除物件的选择、右键菜单和层级编辑状态。
 */
void UiObjectDeleted ();
/**
 * @brief 处理本帧输入并生成 UI 或画布事件。
 * @return 本帧事件；无动作时 action 为 NONE。
 */
UiEvent UiUpdate ();
/**
 * @brief 绘制界面、菜单和弹窗。
 */
void UiDraw ();

#endif
