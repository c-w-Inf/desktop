#ifndef UI_H
#define UI_H

#include <stdbool.h>
#include "raylib.h"
#include "assets.h"
#include "object.h"

typedef enum BackgroundStyle {
    BACKGROUND_WHITE = 0,
    BACKGROUND_DOTS,
    BACKGROUND_GRID,
    BACKGROUND_CHECKER
} BackgroundStyle;

#define UI_BAR_HEIGHT 36
#define UI_MENU_COUNT 5
#define UI_MAX_MESSAGES 8

typedef enum UiMessageKind {
    UI_MESSAGE_INFO = 0,
    UI_MESSAGE_ERROR,
    UI_MESSAGE_SCRIPT
} UiMessageKind;

typedef enum UiAction {
    UI_ACTION_NONE = 0,
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
    UI_ACTION_RUN_SCRIPT
} UiAction;

typedef struct UiEvent {
    UiAction action;
    bool mouseCaptured;
    bool deselectObject;
    Asset *asset;
    Object *object;
    char assetName[256];
    char objectName[256];
    char path[1024];
    int assetIndex;
    int assetTargetIndex;
    int menuIndex;
    BackgroundStyle background;
    float zoom;
    int layer;
    char host[256];
    int port;
    char username[128];
} UiEvent;

typedef struct Ui {
    Font font;
    Font monoFont;
    bool customFont;
    bool customMonoFont;
    bool showConnectMenu;
    bool showFileMenu;
    int menuOpen;
    bool pickerOpen;
    float pickerScroll;
    bool managerOpen;
    float managerScroll;
    Asset *selectedAsset;
    bool objectManagerOpen;
    float objectScroll;
    Object *selectedObject;
    Object *lastObjectClick;
    double lastObjectClickTime;
    bool objectContextOpen;
    Object *contextObject;
    Vector2 contextPos;
    bool layerOpen;
    Object *layerObject;
    char layerText[32];
    bool objectAssetsOpen;
    Object *assetsObject;
    int assetsObjectSelected;
    int assetsLastClickIndex;
    double assetsLastClickTime;
    float assetsObjectScroll;
    Object *pickerObject;
    bool renameOpen;
    bool renameIsObject;
    Asset *renameAsset;
    Object *renameObject;
    char renameText[256];
    bool connectOpen;
    int connectField;
    bool scriptOpen;
    TextBuffer scriptText;
    char scriptSelf[768];
    bool scriptSelfFocused;
    size_t scriptCursor;
    int scriptPreferredColumn;
    float scriptScroll;
    char connectUser[128];
    char connectHost[256];
    char connectPort[16];
    char messages[UI_MAX_MESSAGES][320];
    float messageTimes[UI_MAX_MESSAGES];
    UiMessageKind messageKinds[UI_MAX_MESSAGES];
    int messageCount;
    bool uiPress;
    int submenuOpen;
    float menuX[UI_MENU_COUNT];
    float menuW[UI_MENU_COUNT];
} Ui;

void UiInit (Ui *ui);
void UiUnload (Ui *ui);
void UiShowMessage (Ui *ui, const char *message);
void UiShowError (Ui *ui, const char *message);
void UiShowMessageText (const char *message);
void UiShowMessageTextKind (const char *message, UiMessageKind kind);
void UiOpenAssetManager (Ui *ui);
void UiSetConnectMenuVisible (Ui *ui, bool visible);
void UiSetFileMenuVisible (Ui *ui, bool visible);
void UiOpenObjectManager (Ui *ui, Object *selectObject);
void UiOpenObjectAssets (Ui *ui, Object *object);
void UiSelectAsset (Ui *ui, Asset *asset);
void UiSelectObject (Ui *ui, Object *object);
void UiOpenObjectContext (Ui *ui, Object *object, Vector2 screenPos);
void UiOpenGlobalContext (Ui *ui, Vector2 screenPos);
void UiAssetDeleted (Ui *ui);
void UiObjectDeleted (Ui *ui);
void UiResetSceneState (Ui *ui);
bool UiIsModal (const Ui *ui);
UiEvent UiUpdate (Ui *ui);
void UiDraw (const Ui *ui);

#endif
