#define _POSIX_C_SOURCE 200809L

#include "ui.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "raymath.h"
extern void rlDrawRenderBatchActive (void);
#include "assets.h"
#include "cutils.h"
#include "entity.h"
#include "menu.h"
#include "netclient.h"
#include "user.h"
#include "view.h"

#define UI_ITEM_HEIGHT 32
#define UI_MENU_WIDTH 150
#define UI_SUBMENU_WIDTH 120
#define UI_FONT_SIZE 24
#define UI_CODE_FONT_SIZE 22
#define UI_CODE_LINE_HEIGHT 26.0f
#define UI_CONTEXT_SEPARATOR_HEIGHT 9.0f

static void DrawSmallButton (Font font, Rectangle rect, const char* text, bool danger, bool enabled);
static void ScriptClose ();
static void OpenScript ();
#define UI_PICKER_HEADER 48
#define UI_PICKER_FOOTER 56
#define UI_ASSET_ROW_HEIGHT 64
#define UI_MANAGER_HEADER 48
#define UI_MANAGER_FOOTER 62
#define UI_MANAGER_ROW_HEIGHT 80
#define UI_BUTTON_WIDTH 96.0f
#define UI_BUTTON_HEIGHT 38.0f
#define UI_OBJECT_ASSET_ROW_HEIGHT 64

static const char* menuNames[UI_MENU_COUNT] = {"文件", "素材", "物件", "脚本", "联机"};
static const char* menuItems[UI_MENU_COUNT][3] = {
    {"导入", "导入文件夹", "导出"}, {NULL}, {NULL}, {NULL}, {"连接", "开放"},
};
static int menuItemCounts[UI_MENU_COUNT] = {3, 0, 0, 0, 2};

void UiSetLocalMode (bool localMode) {
#if defined(__EMSCRIPTEN__)
    (void)localMode;
    menuItems[0][1] = NULL;
    menuItems[0][2] = NULL;
    menuItemCounts[0] = 1;
#else
    menuItems[0][1] = "导入文件夹";
    menuItems[0][2] = localMode ? "导出" : NULL;
    menuItemCounts[0] = localMode ? 3 : 2;
#endif
    if (!localMode && uiMenuOpen == 0) {
        uiMenuOpen = -1;
        uiSubmenuOpen = -1;
    }
}

enum { VIEW_SUBMENU_NONE = -1, VIEW_SUBMENU_ZOOM = 1, VIEW_SUBMENU_BACKGROUND = 2 };

static const float viewZoomValues[] = {1.0f, 2.0f, 0.5f};
static const char* viewZoomLabels[] = {"100%", "200%", "50%"};
static const char* viewBackgroundLabels[] = {"纯白", "格点", "格纹", "棋盘"};

static Rectangle ViewContextSubmenuRect () {
    int count = (uiSubmenuOpen == VIEW_SUBMENU_ZOOM) ? 3 : 4;
    int menus = MenuCount (0);
    float actionsY = uiContextPos.y + menus * UI_ITEM_HEIGHT + (menus > 0 ? UI_CONTEXT_SEPARATOR_HEIGHT : 0.0f);
    float x = uiContextPos.x + UI_SUBMENU_WIDTH;
    if (x + UI_SUBMENU_WIDTH > GetScreenWidth ()) x = uiContextPos.x - UI_SUBMENU_WIDTH;
    float offset = uiSubmenuOpen == VIEW_SUBMENU_ZOOM ? UI_ITEM_HEIGHT : 2 * UI_ITEM_HEIGHT;
    float height = count * UI_ITEM_HEIGHT;
    float y = actionsY + offset;
    if (y + height > GetScreenHeight ()) y = GetScreenHeight () - height;
    if (y < 0.0f) y = 0.0f;
    return (Rectangle){x, y, UI_SUBMENU_WIDTH, height};
}

static Rectangle ObjectContextRect () {
    Object* object = uiContextObject;
    int menus = MenuCount (object != NULL ? object->id : 0);
    int actions = (object != NULL ? 3 : 4) * UI_ITEM_HEIGHT;
    float separator = menus > 0 ? UI_CONTEXT_SEPARATOR_HEIGHT : 0.0f;
    return (Rectangle){uiContextPos.x, uiContextPos.y, UI_SUBMENU_WIDTH, menus * UI_ITEM_HEIGHT + separator + actions};
}

static bool DialogSucceeded (CutilsDialogResult result, const char* failure) {
    if (result == CUTILS_DIALOG_OK) return true;
    if (result == CUTILS_DIALOG_CANCELLED || result == CUTILS_DIALOG_PENDING) return false;
    if (result == CUTILS_DIALOG_TOO_LONG)
        UiShowError ("路径过长");
    else
        UiShowError (failure);
    return false;
}

static bool OpenImageDialog (char* path, int pathSize) {
    path[0] = '\0';
    CutilsDialogResult result = CutilsOpenImage (path, (size_t)pathSize);
    return DialogSucceeded (result, "打开文件对话框失败");
}

static bool OpenImportDialog (char* path, int pathSize) {
    path[0] = '\0';
    CutilsDialogResult result = CutilsOpenImport (path, (size_t)pathSize);
    return DialogSucceeded (result, "打开文件对话框失败");
}

static bool OpenFolderDialog (char* path, int pathSize) {
    path[0] = '\0';
    CutilsDialogResult result = CutilsOpenFolder (path, (size_t)pathSize);
    return DialogSucceeded (result, "打开文件夹对话框失败");
}

static bool OpenExportDialog (char* path, int pathSize) {
    path[0] = '\0';
    CutilsDialogResult result = CutilsSaveDsk (path, (size_t)pathSize);
    return DialogSucceeded (result, "打开文件对话框失败");
}

static void LayoutMenus () {
    float x = 8.0f;
    for (int i = 0; i < UI_MENU_COUNT; i++) {
        if (i == 4 && !uiShowConnectMenu) {
            uiMenuW[i] = 0.0f;
            uiMenuX[i] = x;
            continue;
        }
        float measured = MeasureTextEx (CutilsFont, menuNames[i], UI_FONT_SIZE, 0).x;
        if (measured < 8.0f) measured = 72.0f;
        uiMenuW[i] = measured + 32.0f;
        uiMenuX[i] = x;
        x += uiMenuW[i];
    }
}

void UiSetConnectMenuVisible (bool visible) {
    if (uiShowConnectMenu == visible) return;
    uiShowConnectMenu = visible;
    if (!visible && uiMenuOpen == 4) {
        uiMenuOpen = -1;
        uiSubmenuOpen = -1;
    }
    LayoutMenus ();
}

void UiOpenAssetManager () {
    if (!uiManagerOpen) {
        uiManagerOpen = true;
        uiManagerScroll = 0.0f;
        uiSelectedAsset = NULL;
    }
}

bool uiShowConnectMenu;
int uiMenuOpen;
bool uiPickerOpen;
float uiPickerScroll;
bool uiManagerOpen;
float uiManagerScroll;
Asset* uiSelectedAsset;
bool uiObjectManagerOpen;
float uiObjectScroll;
Object* uiSelectedObject;
bool uiObjectContextOpen;
Object* uiContextObject;
Vector2 uiContextPos;
bool uiLayerOpen;
Object* uiLayerObject;
char uiLayerText[32];
bool uiObjectAssetsOpen;
Object* uiAssetsObject;
int uiAssetsObjectSelected;
float uiAssetsObjectScroll;
Object* uiPickerObject;
bool uiRenameOpen;
bool uiRenameIsObject;
Asset* uiRenameAsset;
Object* uiRenameObject;
char uiRenameText[256];
bool uiConnectOpen;
int uiConnectField;
bool uiScriptOpen;
TextBuffer uiScriptText;
char uiScriptSelf[768];
bool uiScriptSelfFocused;
size_t uiScriptCursor;
int uiScriptPreferredColumn;
float uiScriptScroll;
char uiConnectUser[128];
char uiConnectHost[256];
char uiConnectPort[16];
char uiMessages[UI_MAX_MESSAGES][320];
float uiMessageTimes[UI_MAX_MESSAGES];
UiMessageKind uiMessageKinds[UI_MAX_MESSAGES];
int uiMessageCount;
bool uiPress;
int uiSubmenuOpen;
float uiMenuX[UI_MENU_COUNT];
float uiMenuW[UI_MENU_COUNT];

void UiInit () {
    uiShowConnectMenu = true;
    uiMenuOpen = -1;
    uiPickerOpen = false;
    uiPickerScroll = 0.0f;
    uiManagerOpen = false;
    uiManagerScroll = 0.0f;
    uiSelectedAsset = NULL;
    uiObjectManagerOpen = false;
    uiObjectScroll = 0.0f;
    uiSelectedObject = NULL;
    uiObjectContextOpen = false;
    uiContextObject = NULL;
    uiContextPos = (Vector2){0};
    uiLayerOpen = false;
    uiLayerObject = NULL;
    uiLayerText[0] = '\0';
    uiObjectAssetsOpen = false;
    uiAssetsObject = NULL;
    uiAssetsObjectSelected = -1;
    uiAssetsObjectScroll = 0.0f;
    uiPickerObject = NULL;
    uiRenameOpen = false;
    uiRenameIsObject = false;
    uiRenameAsset = NULL;
    uiRenameObject = NULL;
    uiRenameText[0] = '\0';
    uiConnectOpen = false;
    uiConnectField = 0;
    uiScriptOpen = false;
    uiScriptText = (TextBuffer){0};
    uiScriptSelf[0] = '\0';
    uiScriptSelfFocused = false;
    uiScriptCursor = 0;
    uiScriptPreferredColumn = -1;
    uiScriptScroll = 0.0f;
    uiConnectUser[0] = '\0';
    uiConnectHost[0] = '\0';
    uiConnectPort[0] = '\0';
    uiMessageCount = 0;
    uiPress = false;
    uiSubmenuOpen = -1;

    CutilsLoadFonts ();
}

void UiUnload () {
    CutilsUnloadFonts ();
    TextBufferFree (&uiScriptText);
}

static void ShowMessageKind (const char* message, UiMessageKind kind) {
    if (uiMessageCount < 0 || uiMessageCount > UI_MAX_MESSAGES) {
        uiMessageCount = 0;
        uiMessageTimes[0] = 0.0f;
    }
    if (uiMessageCount == UI_MAX_MESSAGES) {
        memmove (uiMessages, uiMessages + 1, (UI_MAX_MESSAGES - 1) * sizeof (uiMessages[0]));
        memmove (uiMessageTimes, uiMessageTimes + 1, (UI_MAX_MESSAGES - 1) * sizeof (uiMessageTimes[0]));
        memmove (uiMessageKinds, uiMessageKinds + 1, (UI_MAX_MESSAGES - 1) * sizeof (uiMessageKinds[0]));
        uiMessageCount--;
    }

    int index = uiMessageCount++;
    snprintf (uiMessages[index], sizeof (uiMessages[index]), "%s", message);
    uiMessageTimes[index] = 3.0f;
    uiMessageKinds[index] = kind;
}

void UiShowMessage (const char* message) { ShowMessageKind (message, UI_MESSAGE_INFO); }

void UiShowError (const char* message) { ShowMessageKind (message, UI_MESSAGE_ERROR); }

void UiShowMessageTextKind (const char* message, UiMessageKind kind) { ShowMessageKind (message, kind); }

static bool AssetValid (const Asset* asset) { return asset != NULL && EntityFind (AssetId (asset)) == asset; }

static void ValidateAssets () {
    if (uiSelectedAsset != NULL && !AssetValid (uiSelectedAsset)) uiSelectedAsset = NULL;
    if (uiRenameOpen && !uiRenameIsObject && !AssetValid (uiRenameAsset)) {
        uiRenameOpen = false;
        uiRenameAsset = NULL;
        uiRenameText[0] = '\0';
    }
}

static bool ObjectValid (const Object* object) { return object != NULL && ObjectContains (object); }

static void ValidateObjects () {
    if (!ObjectValid (uiSelectedObject)) uiSelectedObject = NULL;
    if (uiContextObject != NULL && !ObjectValid (uiContextObject)) {
        uiObjectContextOpen = false;
        uiContextObject = NULL;
    }
    if (!ObjectValid (uiLayerObject)) {
        uiLayerOpen = false;
        uiLayerObject = NULL;
        uiLayerText[0] = '\0';
    }
    if (!ObjectValid (uiAssetsObject)) {
        uiObjectAssetsOpen = false;
        uiAssetsObject = NULL;
        uiAssetsObjectSelected = -1;
    }
    if (!ObjectValid (uiPickerObject)) {
        uiPickerOpen = false;
        uiPickerObject = NULL;
    }
    if (uiRenameOpen && uiRenameIsObject && !ObjectValid (uiRenameObject)) {
        uiRenameOpen = false;
        uiRenameObject = NULL;
        uiRenameText[0] = '\0';
    }
}

void UiObjectDeleted () {
    uiSelectedObject = NULL;
    uiObjectScroll = 0.0f;
    uiObjectContextOpen = false;
    uiContextObject = NULL;
    uiLayerOpen = false;
    uiLayerObject = NULL;
    uiLayerText[0] = '\0';
    uiObjectAssetsOpen = false;
    uiAssetsObject = NULL;
    uiAssetsObjectSelected = -1;
    uiAssetsObjectScroll = 0.0f;
    uiPickerOpen = false;
    uiPickerObject = NULL;
    if (uiRenameOpen && uiRenameIsObject) {
        uiRenameOpen = false;
        uiRenameObject = NULL;
        uiRenameText[0] = '\0';
    }
}

static Rectangle PickerPanel (void) {
    float w = fminf (GetScreenWidth () - 80.0f, 640.0f);
    float h = fminf (GetScreenHeight () - 100.0f, 460.0f);
    return (Rectangle){(GetScreenWidth () - w) / 2.0f, (GetScreenHeight () - h) / 2.0f, w, h};
}

static Rectangle PickerListRect (Rectangle panel) {
    return (Rectangle){
        panel.x + 1, panel.y + UI_PICKER_HEADER, panel.width - 2, panel.height - UI_PICKER_HEADER - UI_PICKER_FOOTER};
}

static Rectangle PickerCancelRect (Rectangle panel) {
    return (Rectangle){
        panel.x + panel.width - 24.0f - UI_BUTTON_WIDTH, panel.y + panel.height - 24.0f - UI_BUTTON_HEIGHT,
        UI_BUTTON_WIDTH, UI_BUTTON_HEIGHT};
}

static Rectangle ManagerPanel (void) {
    float w = fminf (GetScreenWidth () - 80.0f, 780.0f);
    float h = fminf (GetScreenHeight () - 100.0f, 540.0f);
    return (Rectangle){(GetScreenWidth () - w) / 2.0f, (GetScreenHeight () - h) / 2.0f, w, h};
}

static Rectangle ManagerListRect (Rectangle panel) {
    return (Rectangle){
        panel.x + 1, panel.y + UI_MANAGER_HEADER, panel.width - 2,
        panel.height - UI_MANAGER_HEADER - UI_MANAGER_FOOTER};
}

static Rectangle ManagerExitRect (Rectangle panel) {
    return (Rectangle){
        panel.x + panel.width - 24.0f - UI_BUTTON_WIDTH, panel.y + panel.height - 24.0f - UI_BUTTON_HEIGHT,
        UI_BUTTON_WIDTH, UI_BUTTON_HEIGHT};
}

static Rectangle ManagerRenameRect (Rectangle panel) {
    return (Rectangle){
        panel.x + 24.0f, panel.y + panel.height - 24.0f - UI_BUTTON_HEIGHT, UI_BUTTON_WIDTH, UI_BUTTON_HEIGHT};
}

static Rectangle ManagerReplaceRect (Rectangle panel) {
    Rectangle renameBtn = ManagerRenameRect (panel);
    return (Rectangle){renameBtn.x + renameBtn.width + 8.0f, renameBtn.y, UI_BUTTON_WIDTH, UI_BUTTON_HEIGHT};
}

static Rectangle ManagerDeleteRect (Rectangle panel) {
    Rectangle replaceBtn = ManagerReplaceRect (panel);
    return (Rectangle){replaceBtn.x + replaceBtn.width + 8.0f, replaceBtn.y, UI_BUTTON_WIDTH, UI_BUTTON_HEIGHT};
}

static Rectangle ManagerImportRect (Rectangle panel) {
    Rectangle doneBtn = ManagerExitRect (panel);
    return (Rectangle){doneBtn.x - 8.0f - UI_BUTTON_WIDTH, doneBtn.y, UI_BUTTON_WIDTH, UI_BUTTON_HEIGHT};
}

static Rectangle ObjectRenameRect (Rectangle panel) {
    return (Rectangle){
        panel.x + 24.0f, panel.y + panel.height - 24.0f - UI_BUTTON_HEIGHT, UI_BUTTON_WIDTH, UI_BUTTON_HEIGHT};
}

static Rectangle ObjectManageRect (Rectangle panel) {
    Rectangle renameBtn = ObjectRenameRect (panel);
    return (Rectangle){renameBtn.x + renameBtn.width + 8.0f, renameBtn.y, UI_BUTTON_WIDTH, UI_BUTTON_HEIGHT};
}

static Rectangle ObjectLayerRect (Rectangle panel) {
    Rectangle manageBtn = ObjectManageRect (panel);
    return (Rectangle){manageBtn.x + manageBtn.width + 8.0f, manageBtn.y, UI_BUTTON_WIDTH, UI_BUTTON_HEIGHT};
}

static Rectangle ObjectDeleteRect (Rectangle panel) {
    Rectangle layerBtn = ObjectLayerRect (panel);
    return (Rectangle){layerBtn.x + layerBtn.width + 8.0f, layerBtn.y, UI_BUTTON_WIDTH, UI_BUTTON_HEIGHT};
}

static Rectangle ObjectDoneRect (Rectangle panel) {
    return (Rectangle){
        panel.x + panel.width - 24.0f - UI_BUTTON_WIDTH, panel.y + panel.height - 24.0f - UI_BUTTON_HEIGHT,
        UI_BUTTON_WIDTH, UI_BUTTON_HEIGHT};
}

static Rectangle ObjectCreateRect (Rectangle panel) {
    Rectangle doneBtn = ObjectDoneRect (panel);
    return (Rectangle){doneBtn.x - 8.0f - UI_BUTTON_WIDTH, doneBtn.y, UI_BUTTON_WIDTH, UI_BUTTON_HEIGHT};
}

static Rectangle ObjectAssetsPanel (void) {
    float w = fminf (GetScreenWidth () - 80.0f, 720.0f);
    float h = fminf (GetScreenHeight () - 100.0f, 500.0f);
    return (Rectangle){(GetScreenWidth () - w) / 2.0f, (GetScreenHeight () - h) / 2.0f, w, h};
}

static Rectangle ObjectAssetsListRect (Rectangle panel) {
    return (Rectangle){
        panel.x + 1, panel.y + UI_MANAGER_HEADER, panel.width - 2,
        panel.height - UI_MANAGER_HEADER - UI_MANAGER_FOOTER};
}

static Rectangle ObjectAssetUseRect (Rectangle panel) {
    return (Rectangle){
        panel.x + 24.0f, panel.y + panel.height - 24.0f - UI_BUTTON_HEIGHT, UI_BUTTON_WIDTH, UI_BUTTON_HEIGHT};
}

static Rectangle ObjectAssetUpRect (Rectangle panel) {
    Rectangle use = ObjectAssetUseRect (panel);
    return (Rectangle){use.x + use.width + 8.0f, use.y, UI_BUTTON_WIDTH, UI_BUTTON_HEIGHT};
}

static Rectangle ObjectAssetDownRect (Rectangle panel) {
    Rectangle upBtn = ObjectAssetUpRect (panel);
    return (Rectangle){upBtn.x + upBtn.width + 8.0f, upBtn.y, UI_BUTTON_WIDTH, UI_BUTTON_HEIGHT};
}

static Rectangle ObjectAssetRemoveRect (Rectangle panel) {
    Rectangle downBtn = ObjectAssetDownRect (panel);
    return (Rectangle){downBtn.x + downBtn.width + 8.0f, downBtn.y, UI_BUTTON_WIDTH, UI_BUTTON_HEIGHT};
}

static Rectangle ObjectAssetDoneRect (Rectangle panel) {
    return (Rectangle){
        panel.x + panel.width - 24.0f - UI_BUTTON_WIDTH, panel.y + panel.height - 24.0f - UI_BUTTON_HEIGHT,
        UI_BUTTON_WIDTH, UI_BUTTON_HEIGHT};
}

static Rectangle ObjectAssetAddRect (Rectangle panel) {
    Rectangle doneBtn = ObjectAssetDoneRect (panel);
    return (Rectangle){doneBtn.x - 8.0f - UI_BUTTON_WIDTH, doneBtn.y, UI_BUTTON_WIDTH, UI_BUTTON_HEIGHT};
}

static Rectangle RenamePanel (void) {
    float w = fminf (GetScreenWidth () - 80.0f, 480.0f);
    float h = fminf (GetScreenHeight () - 100.0f, 200.0f);
    return (Rectangle){(GetScreenWidth () - w) / 2.0f, (GetScreenHeight () - h) / 2.0f, w, h};
}

static Rectangle RenameInputRect (Rectangle panel) {
    return (Rectangle){panel.x + 24.0f, panel.y + 62.0f, panel.width - 48.0f, 34.0f};
}

static Rectangle RenameOkRect (Rectangle panel) {
    return (Rectangle){
        panel.x + panel.width / 2.0f - UI_BUTTON_WIDTH - 8.0f, panel.y + panel.height - 24.0f - UI_BUTTON_HEIGHT,
        UI_BUTTON_WIDTH, UI_BUTTON_HEIGHT};
}

static Rectangle RenameCancelRect (Rectangle panel) {
    return (Rectangle){
        panel.x + panel.width / 2.0f + 8.0f, panel.y + panel.height - 24.0f - UI_BUTTON_HEIGHT, UI_BUTTON_WIDTH,
        UI_BUTTON_HEIGHT};
}

static Rectangle ConnectPanel (void) {
    float w = fminf (GetScreenWidth () - 80.0f, 480.0f);
    float h = fminf (GetScreenHeight () - 100.0f, 330.0f);
    return (Rectangle){(GetScreenWidth () - w) / 2.0f, (GetScreenHeight () - h) / 2.0f, w, h};
}

static Rectangle ConnectUserRect (Rectangle panel) {
    return (Rectangle){panel.x + 24.0f, panel.y + 80.0f, panel.width - 48.0f, 34.0f};
}

static Rectangle ConnectHostRect (Rectangle panel) {
    return (Rectangle){panel.x + 24.0f, panel.y + 146.0f, panel.width - 48.0f, 34.0f};
}

static Rectangle ConnectPortRect (Rectangle panel) {
    return (Rectangle){panel.x + 24.0f, panel.y + 212.0f, 160.0f, 34.0f};
}

static Rectangle ScriptPanel (void) {
    float w = fminf (GetScreenWidth () - 80.0f, 720.0f);
    float h = fminf (GetScreenHeight () - 100.0f, 560.0f);
    return (Rectangle){(GetScreenWidth () - w) / 2.0f, (GetScreenHeight () - h) / 2.0f, w, h};
}

static Rectangle ScriptCodeRect (Rectangle panel) {
    return (Rectangle){panel.x + 24.0f, panel.y + 56.0f, panel.width - 48.0f, panel.height - 132.0f};
}

static Rectangle ScriptCancelRect (Rectangle panel) {
    return (Rectangle){
        panel.x + panel.width - 24.0f - UI_BUTTON_WIDTH, panel.y + panel.height - 24.0f - UI_BUTTON_HEIGHT,
        UI_BUTTON_WIDTH, UI_BUTTON_HEIGHT};
}

static Rectangle ScriptSubmitRect (Rectangle panel) {
    Rectangle cancel = ScriptCancelRect (panel);
    return (Rectangle){cancel.x - 8.0f - UI_BUTTON_WIDTH, cancel.y, UI_BUTTON_WIDTH, UI_BUTTON_HEIGHT};
}

static Rectangle ScriptSelfRect (Rectangle panel) {
    Rectangle submit = ScriptSubmitRect (panel);
    return (Rectangle){submit.x - 8.0f - 240.0f, submit.y, 240.0f, UI_BUTTON_HEIGHT};
}

static void RemoveLastCodepoint (char* text) {
    int length = (int)strlen (text);
    if (length == 0) return;

    int index = length - 1;
    while (index > 0 && ((unsigned char)text[index] & 0xC0) == 0x80) index--;
    text[index] = '\0';
}

static void AppendCodepoint (char* text, int size, int codepoint) {
    int utf8Size = 0;
    const char* utf8 = CodepointToUTF8 (codepoint, &utf8Size);
    int length = (int)strlen (text);

    if (length + utf8Size < size) {
        memcpy (text + length, utf8, (size_t)utf8Size);
        text[length + utf8Size] = '\0';
    }
}

typedef enum { TEXT_INPUT_NONE = 0, TEXT_INPUT_CONFIRM, TEXT_INPUT_CANCEL } TextInputResult;

static TextInputResult UpdateTextInput (char* text, int size, bool numericOnly) {
    Vector2 mouse = GetMousePosition ();
    Rectangle panel = RenamePanel ();
    Rectangle ok = RenameOkRect (panel);
    Rectangle cancel = RenameCancelRect (panel);

    int codepoint;
    while ((codepoint = CutilsCharPressed ()) > 0) {
        if (codepoint < 32) continue;

        bool accepted = true;
        if (numericOnly) {
            accepted = (codepoint >= '0' && codepoint <= '9') || (codepoint == '-' && text[0] == '\0');
        }
        if (!accepted) continue;

        int length = (int)strlen (text);
        if (length + 4 >= size) continue;

        AppendCodepoint (text, size, codepoint);
    }

    if (CutilsKeyPressed (KEY_BACKSPACE)) RemoveLastCodepoint (text);

    bool confirm = CutilsKeyPressed (KEY_ENTER) || CutilsKeyPressed (KEY_KP_ENTER);
    if (CutilsMousePressed (MOUSE_BUTTON_LEFT)) {
        if (CheckCollisionPointRec (mouse, ok)) {
            confirm = true;
        } else if (CheckCollisionPointRec (mouse, cancel) || !CheckCollisionPointRec (mouse, panel)) {
            return TEXT_INPUT_CANCEL;
        }
    }

    return confirm ? TEXT_INPUT_CONFIRM : TEXT_INPUT_NONE;
}

static void OpenConnect () {
    uiConnectOpen = true;
    uiConnectField = 0;
}

static void ConfirmConnect (UiEvent* event) {
    if (uiConnectUser[0] == '\0') {
        UiShowError ("请输入用户名");
        return;
    }

    const char* host = (uiConnectHost[0] != '\0') ? uiConnectHost : "127.0.0.1";
    if (host[0] == '-') {
        UiShowError ("服务器地址无效");
        return;
    }

    long port = 7777;
    if (uiConnectPort[0] != '\0') {
        errno = 0;
        char* end = NULL;
        port = strtol (uiConnectPort, &end, 10);
        if (errno == ERANGE || end == uiConnectPort || *end != '\0' || port < 1 || port > 65535) {
            UiShowError ("请输入有效端口");
            return;
        }
    }

    uiConnectOpen = false;
    event->action = UI_ACTION_CONNECT;
    snprintf (event->u.connect.host, sizeof (event->u.connect.host), "%s", host);
    event->u.connect.port = (int)port;
    snprintf (event->u.connect.username, sizeof (event->u.connect.username), "%s", uiConnectUser);
}

static void UpdateConnect (UiEvent* event) {
    Vector2 mouse = GetMousePosition ();
    Rectangle panel = ConnectPanel ();
    Rectangle userRect = ConnectUserRect (panel);
    Rectangle hostRect = ConnectHostRect (panel);
    Rectangle portRect = ConnectPortRect (panel);
    Rectangle ok = RenameOkRect (panel);
    Rectangle cancel = RenameCancelRect (panel);

    if (CutilsMousePressed (MOUSE_BUTTON_LEFT)) {
        if (CheckCollisionPointRec (mouse, ok)) {
            ConfirmConnect (event);
            return;
        }
        if (CheckCollisionPointRec (mouse, cancel) || !CheckCollisionPointRec (mouse, panel)) {
            uiConnectOpen = false;
            return;
        }
        if (CheckCollisionPointRec (mouse, userRect))
            uiConnectField = 0;
        else if (CheckCollisionPointRec (mouse, hostRect))
            uiConnectField = 1;
        else if (CheckCollisionPointRec (mouse, portRect))
            uiConnectField = 2;
    }

    if (CutilsKeyPressed (KEY_ESCAPE)) {
        uiConnectOpen = false;
        return;
    }
    if (CutilsKeyPressed (KEY_TAB)) uiConnectField = (uiConnectField + 1) % 3;

    bool portField = (uiConnectField == 2);
    char* text = portField ? uiConnectPort : (uiConnectField == 1 ? uiConnectHost : uiConnectUser);
    int size = (int)(portField ? sizeof (uiConnectPort)
                               : (uiConnectField == 1 ? sizeof (uiConnectHost) : sizeof (uiConnectUser)));

    int codepoint;
    while ((codepoint = CutilsCharPressed ()) > 0) {
        if (codepoint < 32) continue;
        if (portField && (codepoint < '0' || codepoint > '9')) continue;

        int length = (int)strlen (text);
        if (length + 4 >= size) continue;

        AppendCodepoint (text, size, codepoint);
    }

    if (CutilsKeyPressed (KEY_BACKSPACE)) RemoveLastCodepoint (text);

    if (CutilsKeyPressed (KEY_ENTER) || CutilsKeyPressed (KEY_KP_ENTER)) ConfirmConnect (event);
}

void UiSelectObject (Object* object) {
    uiSelectedObject = object;
    if (object == NULL || !uiObjectManagerOpen) return;

    IdGroup ids = ObjectIds ();
    int count = ids.count;
    int index = -1;
    for (int i = 0; i < ids.count; i++) {
        if (ids.ids[i] == object->id) {
            index = i;
            break;
        }
    }
    IdGroupUnload (&ids);
    if (index < 0) return;

    Rectangle list = ManagerListRect (ManagerPanel ());
    float top = index * UI_MANAGER_ROW_HEIGHT;
    float bottom = top + UI_MANAGER_ROW_HEIGHT;

    if (top < uiObjectScroll)
        uiObjectScroll = top;
    else if (bottom > uiObjectScroll + list.height)
        uiObjectScroll = bottom - list.height;

    float maxScroll = fmaxf (0.0f, count * UI_MANAGER_ROW_HEIGHT - list.height);
    uiObjectScroll = Clamp (uiObjectScroll, 0.0f, maxScroll);
}

void UiOpenObjectManager (Object* selectObject) {
    uiObjectManagerOpen = true;
    uiObjectScroll = 0.0f;
    uiSelectedObject = NULL;
    if (selectObject != NULL && selectObject->lockOwnerId == NetClientUserId ()) UiSelectObject (selectObject);
}

void UiOpenObjectAssets (Object* object) {
    if (object == NULL) return;

    uiObjectAssetsOpen = true;
    uiAssetsObject = object;
    uiAssetsObjectSelected = -1;
    uiAssetsObjectScroll = 0.0f;
}

static void OpenContext (Object* object, Vector2 screenPos) {
    int menus = MenuCount (object != NULL ? object->id : 0);
    float width = UI_SUBMENU_WIDTH;
    float height = menus * UI_ITEM_HEIGHT + (menus > 0 ? UI_CONTEXT_SEPARATOR_HEIGHT : 0.0f) +
                   (object != NULL ? 3 : 4) * UI_ITEM_HEIGHT;
    float x = screenPos.x;
    float y = screenPos.y;

    if (x + width > GetScreenWidth ()) x = GetScreenWidth () - width;
    if (y + height > GetScreenHeight ()) y = GetScreenHeight () - height;
    if (x < 0.0f) x = 0.0f;
    if (y < 0.0f) y = 0.0f;

    uiObjectContextOpen = true;
    uiContextObject = object;
    uiContextPos = (Vector2){x, y};
    uiSubmenuOpen = VIEW_SUBMENU_NONE;
}

void UiOpenObjectContext (Object* object, Vector2 screenPos) {
    if (object == NULL) return;
    OpenContext (object, screenPos);
}

void UiOpenGlobalContext (Vector2 screenPos) { OpenContext (NULL, screenPos); }

static bool ScriptAllowed () {
    User* self = (User*)EntityFind (NetClientUserId ());
    if (self != NULL && !self->canscript) {
        UiShowError ("当前用户不允许提交脚本");
        return false;
    }
    return true;
}

static void OpenScriptForObject (Object* object) {
    if (!ScriptAllowed ()) return;
    OpenScript ();

    const char* prefix = "object(\"";
    size_t pos = 0;
    for (size_t i = 0; prefix[i] != '\0' && pos < sizeof (uiScriptSelf) - 1; i++) uiScriptSelf[pos++] = prefix[i];

    for (size_t i = 0; object->name[i] != '\0' && pos < sizeof (uiScriptSelf) - 2; i++) {
        unsigned char ch = (unsigned char)object->name[i];
        if (ch == '"' || ch == '\\') {
            uiScriptSelf[pos++] = '\\';
            uiScriptSelf[pos++] = (char)ch;
        } else if (ch == '\n') {
            uiScriptSelf[pos++] = '\\';
            uiScriptSelf[pos++] = 'n';
        } else if (ch == '\r') {
            uiScriptSelf[pos++] = '\\';
            uiScriptSelf[pos++] = 'r';
        } else if (ch == '\t') {
            uiScriptSelf[pos++] = '\\';
            uiScriptSelf[pos++] = 't';
        } else {
            uiScriptSelf[pos++] = (char)ch;
        }
    }
    if (pos < sizeof (uiScriptSelf) - 1) uiScriptSelf[pos++] = '"';
    if (pos < sizeof (uiScriptSelf) - 1) uiScriptSelf[pos++] = ')';
    uiScriptSelf[pos] = '\0';
}

static int UserMenuPerRow (void) {
    int available = GetScreenWidth () - 48;
    int perRow = (available + 8) / ((int)UI_BUTTON_WIDTH + 8);
    return perRow > 0 ? perRow : 1;
}

static Rectangle UserMenuButtonRect (int index) {
    int perRow = UserMenuPerRow ();
    int row = index / perRow;
    int column = index % perRow;
    int rowFirst = row * perRow;
    int total = MenuCount (NetClientUserId ());
    int rowCount = total - rowFirst;
    if (rowCount > perRow) rowCount = perRow;
    if (rowCount < 0) rowCount = 0;
    float rowWidth = rowCount * UI_BUTTON_WIDTH + (rowCount - 1) * 8.0f;
    return (Rectangle){
        (GetScreenWidth () - rowWidth) / 2.0f + column * (UI_BUTTON_WIDTH + 8.0f),
        GetScreenHeight () - 24.0f - UI_BUTTON_HEIGHT - row * (UI_BUTTON_HEIGHT + 8.0f), UI_BUTTON_WIDTH,
        UI_BUTTON_HEIGHT};
}

static void DrawUiText (Font font, const char* text, Vector2 position,
                        float size, float spacing, Color color) {
    position.x = roundf (position.x);
    position.y = roundf (position.y);
    DrawTextEx (font, text, position, size, spacing, color);
}

static bool UpdateUserMenuButtons (UiEvent* event) {
    if (!CutilsMousePressed (MOUSE_BUTTON_LEFT)) return false;

    u64 userId = NetClientUserId ();
    if (userId == 0) return false;
    int count = MenuCount (userId);
    if (count == 0) return false;

    Vector2 mouse = GetMousePosition ();
    for (int i = 0; i < count; i++) {
        if (!CheckCollisionPointRec (mouse, UserMenuButtonRect (i))) continue;
        uiPress = true;
        event->action = UI_ACTION_RUN_USER_MENU;
        event->u.menu.index = i;
        return true;
    }
    return false;
}

static void UpdateObjectContext (UiEvent* event) {
    Vector2 mouse = GetMousePosition ();
    Rectangle menu = ObjectContextRect ();
    Object* object = uiContextObject;
    int menuCount = MenuCount (object != NULL ? object->id : 0);
    bool global = object == NULL;
    Rectangle submenu = {0};
    bool submenuVisible = global && uiSubmenuOpen >= 0;
    if (submenuVisible) submenu = ViewContextSubmenuRect ();

    if (!CutilsMousePressed (MOUSE_BUTTON_LEFT)) return;
    uiPress = true;

    bool inMenu = CheckCollisionPointRec (mouse, menu);
    bool inSubmenu = submenuVisible && CheckCollisionPointRec (mouse, submenu);
    if (!inMenu && !inSubmenu) {
        uiObjectContextOpen = false;
        uiContextObject = NULL;
        uiSubmenuOpen = VIEW_SUBMENU_NONE;
        return;
    }

    if (inSubmenu) {
        int item = (int)((mouse.y - submenu.y) / UI_ITEM_HEIGHT);
        int count = (int)(submenu.height / UI_ITEM_HEIGHT);
        if (item >= 0 && item < count) {
            if (uiSubmenuOpen == VIEW_SUBMENU_ZOOM) {
                event->action = UI_ACTION_SET_ZOOM;
                event->u.zoom.value = viewZoomValues[item];
            } else {
                event->action = UI_ACTION_SET_BACKGROUND;
                event->u.background.style = (BackgroundStyle)item;
            }
        }
        uiObjectContextOpen = false;
        uiContextObject = NULL;
        uiSubmenuOpen = VIEW_SUBMENU_NONE;
        return;
    }

    float menuEnd = menu.y + menuCount * UI_ITEM_HEIGHT;
    uiObjectContextOpen = false;
    uiContextObject = NULL;
    if (mouse.y < menuEnd) {
        int item = (int)((mouse.y - menu.y) / UI_ITEM_HEIGHT);
        if (item >= 0 && item < menuCount) {
            event->action = object != NULL ? UI_ACTION_RUN_MENU : UI_ACTION_RUN_GLOBAL_MENU;
            if (object != NULL) {
                event->u.objectMenu.object = object;
                event->u.objectMenu.index = item;
            } else {
                event->u.menu.index = item;
            }
        }
        uiSubmenuOpen = VIEW_SUBMENU_NONE;
        return;
    }

    float actionY = menuEnd + (menuCount > 0 ? UI_CONTEXT_SEPARATOR_HEIGHT : 0.0f);
    if (mouse.y < actionY) {
        uiSubmenuOpen = VIEW_SUBMENU_NONE;
        return;
    }

    int action = (int)((mouse.y - actionY) / UI_ITEM_HEIGHT);
    if (global) {
        if (action == 0) {
            event->action = UI_ACTION_VIEW_ORIGIN;
            uiSubmenuOpen = VIEW_SUBMENU_NONE;
        } else if (action == 1) {
            uiSubmenuOpen = uiSubmenuOpen == VIEW_SUBMENU_ZOOM ? VIEW_SUBMENU_NONE : VIEW_SUBMENU_ZOOM;
            uiObjectContextOpen = true;
            uiContextObject = NULL;
        } else if (action == 2) {
            uiSubmenuOpen = uiSubmenuOpen == VIEW_SUBMENU_BACKGROUND ? VIEW_SUBMENU_NONE : VIEW_SUBMENU_BACKGROUND;
            uiObjectContextOpen = true;
            uiContextObject = NULL;
        } else if (action == 3) {
            if (ScriptAllowed ()) {
                OpenScript ();
                snprintf (uiScriptSelf, sizeof (uiScriptSelf), "0");
            }
            uiSubmenuOpen = VIEW_SUBMENU_NONE;
        } else {
            uiSubmenuOpen = VIEW_SUBMENU_NONE;
        }
        return;
    }

    uiSubmenuOpen = VIEW_SUBMENU_NONE;
    if (action == 0) {
        UiOpenObjectManager (object);
    } else if (action == 1) {
        OpenScriptForObject (object);
    } else if (action == 2) {
        if (uiSelectedObject == object) uiSelectedObject = NULL;
        event->action = UI_ACTION_DELETE_OBJECT;
        event->u.object.object = object;
    }
}

static void UpdateManager (UiEvent* event, float wheel) {
    Vector2 mouse = GetMousePosition ();
    IdGroup assetIds = AssetIds ();
    int assetCount = assetIds.count;
    IdGroupUnload (&assetIds);
    Rectangle panel = ManagerPanel ();
    Rectangle list = ManagerListRect (panel);
    Rectangle exit = ManagerExitRect (panel);
    Rectangle renameBtn = ManagerRenameRect (panel);
    Rectangle replaceBtn = ManagerReplaceRect (panel);
    Rectangle deleteBtn = ManagerDeleteRect (panel);
    Rectangle importBtn = ManagerImportRect (panel);
    float maxScroll = fmaxf (0.0f, assetCount * UI_MANAGER_ROW_HEIGHT - list.height);
    uiManagerScroll = Clamp (uiManagerScroll - wheel * 40.0f, 0.0f, maxScroll);

    if (!CutilsMousePressed (MOUSE_BUTTON_LEFT)) return;
    uiPress = true;

    if (CheckCollisionPointRec (mouse, exit)) {
        uiManagerOpen = false;
        return;
    }
    if (CheckCollisionPointRec (mouse, importBtn)) {
        if (OpenImageDialog (event->u.path.path, (int)sizeof (event->u.path.path))) event->action = UI_ACTION_IMPORT;
        return;
    }

    if (uiSelectedAsset != NULL) {
        if (CheckCollisionPointRec (mouse, renameBtn)) {
            uiRenameOpen = true;
            uiRenameIsObject = false;
            uiRenameAsset = uiSelectedAsset;
            snprintf (uiRenameText, sizeof (uiRenameText), "%s", AssetName (uiSelectedAsset));
            return;
        }
        if (CheckCollisionPointRec (mouse, replaceBtn)) {
            if (OpenImageDialog (event->u.assetPath.path, (int)sizeof (event->u.assetPath.path))) {
                event->action = UI_ACTION_REPLACE_ASSET;
                event->u.assetPath.asset = uiSelectedAsset;
            }
            return;
        }
        if (CheckCollisionPointRec (mouse, deleteBtn)) {
            event->action = UI_ACTION_DELETE_ASSET;
            event->u.asset.asset = uiSelectedAsset;
            uiSelectedAsset = NULL;
            return;
        }
    }

    if (CheckCollisionPointRec (mouse, list)) {
        int index = (int)((mouse.y - list.y + uiManagerScroll) / UI_MANAGER_ROW_HEIGHT);
        assetIds = AssetIds ();
        if (index >= 0 && index < assetIds.count) {
            Asset* asset = EntityFind (assetIds.ids[index]);
            if (asset != NULL) {
                uiSelectedAsset = asset;
                IdGroupUnload (&assetIds);
                return;
            }
        }
        IdGroupUnload (&assetIds);
    }

    uiSelectedAsset = NULL;
}

static void UpdateObjectManager (UiEvent* event, float wheel) {
    Vector2 mouse = GetMousePosition ();
    IdGroup objectIds = ObjectIds ();
    int objectCount = objectIds.count;
    IdGroupUnload (&objectIds);
    Rectangle panel = ManagerPanel ();
    Rectangle list = ManagerListRect (panel);
    Rectangle renameBtn = ObjectRenameRect (panel);
    Rectangle manageBtn = ObjectManageRect (panel);
    Rectangle layerBtn = ObjectLayerRect (panel);
    Rectangle deleteBtn = ObjectDeleteRect (panel);
    Rectangle createBtn = ObjectCreateRect (panel);
    Rectangle doneBtn = ObjectDoneRect (panel);
    float maxScroll = fmaxf (0.0f, objectCount * UI_MANAGER_ROW_HEIGHT - list.height);
    uiObjectScroll = Clamp (uiObjectScroll - wheel * 40.0f, 0.0f, maxScroll);

    if (!CutilsMousePressed (MOUSE_BUTTON_LEFT)) return;
    uiPress = true;

    if (CheckCollisionPointRec (mouse, doneBtn)) {
        if (uiSelectedObject != NULL) {
            event->action = UI_ACTION_UNLOCK_OBJECT;
            event->u.object.object = uiSelectedObject;
            uiSelectedObject = NULL;
        }
        uiObjectManagerOpen = false;
        return;
    }
    if (CheckCollisionPointRec (mouse, createBtn)) {
        event->action = UI_ACTION_CREATE_OBJECT;
        return;
    }

    if (uiSelectedObject != NULL) {
        if (CheckCollisionPointRec (mouse, renameBtn)) {
            uiRenameOpen = true;
            uiRenameIsObject = true;
            uiRenameObject = uiSelectedObject;
            snprintf (uiRenameText, sizeof (uiRenameText), "%s", uiSelectedObject->name);
            return;
        }
        if (CheckCollisionPointRec (mouse, manageBtn)) {
            UiOpenObjectAssets (uiSelectedObject);
            event->action = UI_ACTION_LOCK_OBJECT;
            event->u.object.object = uiSelectedObject;
            return;
        }
        if (CheckCollisionPointRec (mouse, layerBtn)) {
            uiLayerOpen = true;
            uiLayerObject = uiSelectedObject;
            snprintf (uiLayerText, sizeof (uiLayerText), "%d", uiSelectedObject->layer);
            return;
        }
        if (CheckCollisionPointRec (mouse, deleteBtn)) {
            event->action = UI_ACTION_DELETE_OBJECT;
            event->u.object.object = uiSelectedObject;
            uiSelectedObject = NULL;
            return;
        }
    }

    if (CheckCollisionPointRec (mouse, list)) {
        int index = (int)((mouse.y - list.y + uiObjectScroll) / UI_MANAGER_ROW_HEIGHT);
        objectIds = ObjectIds ();
        Object* object = (index >= 0 && index < objectIds.count) ? EntityFind (objectIds.ids[index]) : NULL;
        IdGroupUnload (&objectIds);
        if (object != NULL) {
            if (object->lockOwnerId != 0 && object->lockOwnerId != NetClientUserId ()) return;

            if (object != uiSelectedObject || object->lockOwnerId != NetClientUserId ()) {
                if (object->lockOwnerId == NetClientUserId ()) {
                    uiSelectedObject = object;
                } else {

                    uiSelectedObject = NULL;
                    event->action = UI_ACTION_LOCK_OBJECT;
                    event->u.object.object = object;
                }
            }

            return;
        }
    }

    if (uiSelectedObject != NULL) {
        event->action = UI_ACTION_UNLOCK_OBJECT;
        event->u.object.object = uiSelectedObject;
    }
    uiSelectedObject = NULL;
}

static void UpdateObjectAssets (UiEvent* event, float wheel) {
    if (uiAssetsObject == NULL) {
        uiObjectAssetsOpen = false;
        return;
    }

    Vector2 mouse = GetMousePosition ();
    int count = uiAssetsObject->assets.count;
    Rectangle panel = ObjectAssetsPanel ();
    Rectangle list = ObjectAssetsListRect (panel);
    Rectangle useBtn = ObjectAssetUseRect (panel);
    Rectangle upBtn = ObjectAssetUpRect (panel);
    Rectangle downBtn = ObjectAssetDownRect (panel);
    Rectangle removeBtn = ObjectAssetRemoveRect (panel);
    Rectangle addBtn = ObjectAssetAddRect (panel);
    Rectangle doneBtn = ObjectAssetDoneRect (panel);
    float maxScroll = fmaxf (0.0f, count * UI_OBJECT_ASSET_ROW_HEIGHT - list.height);
    uiAssetsObjectScroll = Clamp (uiAssetsObjectScroll - wheel * 40.0f, 0.0f, maxScroll);

    if (!CutilsMousePressed (MOUSE_BUTTON_LEFT)) return;
    uiPress = true;

    if (CheckCollisionPointRec (mouse, doneBtn)) {
        uiObjectAssetsOpen = false;
        uiAssetsObject = NULL;
        uiAssetsObjectSelected = -1;
        uiAssetsObjectScroll = 0.0f;
        return;
    }
    if (CheckCollisionPointRec (mouse, addBtn)) {
        uiPickerOpen = true;
        uiPickerScroll = 0.0f;
        uiPickerObject = uiAssetsObject;
        return;
    }

    bool hasSelection = (uiAssetsObjectSelected >= 0 && uiAssetsObjectSelected < count);
    if (hasSelection) {
        if (CheckCollisionPointRec (mouse, useBtn)) {
            event->action = UI_ACTION_USE_OBJECT_ASSET;
            event->u.objectAssets.object = uiAssetsObject;
            event->u.objectAssets.index = uiAssetsObjectSelected;
            return;
        }
        if (CheckCollisionPointRec (mouse, upBtn)) {
            if (uiAssetsObjectSelected > 0) {
                int from = uiAssetsObjectSelected;
                event->action = UI_ACTION_MOVE_OBJECT_ASSET;
                event->u.objectAssets.object = uiAssetsObject;
                event->u.objectAssets.index = from;
                event->u.objectAssets.target = from - 1;
                uiAssetsObjectSelected = from - 1;
            }
            return;
        }
        if (CheckCollisionPointRec (mouse, downBtn)) {
            if (uiAssetsObjectSelected < count - 1) {
                int from = uiAssetsObjectSelected;
                event->action = UI_ACTION_MOVE_OBJECT_ASSET;
                event->u.objectAssets.object = uiAssetsObject;
                event->u.objectAssets.index = from;
                event->u.objectAssets.target = from + 1;
                uiAssetsObjectSelected = from + 1;
            }
            return;
        }
        if (CheckCollisionPointRec (mouse, removeBtn)) {
            event->action = UI_ACTION_REMOVE_OBJECT_ASSET;
            event->u.objectAssets.object = uiAssetsObject;
            event->u.objectAssets.index = uiAssetsObjectSelected;
            uiAssetsObjectSelected = -1;
            return;
        }
    }

    if (CheckCollisionPointRec (mouse, list)) {
        int index = (int)((mouse.y - list.y + uiAssetsObjectScroll) / UI_OBJECT_ASSET_ROW_HEIGHT);
        if (index >= 0 && index < count) {
            uiAssetsObjectSelected = index;
            return;
        }
    }

    uiAssetsObjectSelected = -1;
}

static void UpdateRename (UiEvent* event) {
    TextInputResult result = UpdateTextInput (uiRenameText, (int)sizeof (uiRenameText), false);
    if (result == TEXT_INPUT_CANCEL) {
        uiRenameOpen = false;
        return;
    }
    if (result != TEXT_INPUT_CONFIRM || uiRenameText[0] == '\0') return;

    if (uiRenameIsObject) {
        event->action = UI_ACTION_RENAME_OBJECT;
        event->u.objectName.object = uiRenameObject;
        snprintf (event->u.objectName.name, sizeof (event->u.objectName.name), "%s", uiRenameText);
    } else {
        event->action = UI_ACTION_RENAME_ASSET;
        event->u.assetName.asset = uiRenameAsset;
        snprintf (event->u.assetName.name, sizeof (event->u.assetName.name), "%s", uiRenameText);
    }
    uiRenameOpen = false;
}

static void UpdateLayer (UiEvent* event) {
    TextInputResult result = UpdateTextInput (uiLayerText, (int)sizeof (uiLayerText), true);
    if (result == TEXT_INPUT_CANCEL) {
        uiLayerOpen = false;
        return;
    }
    if (result != TEXT_INPUT_CONFIRM) return;

    if (uiLayerText[0] == '\0' || strcmp (uiLayerText, "-") == 0) {
        UiShowError ("请输入有效层数");
        return;
    }

    errno = 0;
    char* end = NULL;
    long value = strtol (uiLayerText, &end, 10);
    if (errno == ERANGE || end == uiLayerText || *end != '\0' || value < INT_MIN || value > INT_MAX) {
        UiShowError ("层数超出 int 范围");
        return;
    }

    uiLayerOpen = false;
    event->action = UI_ACTION_SET_OBJECT_LAYER;
    event->u.objectLayer.object = uiLayerObject;
    event->u.objectLayer.layer = (int)value;
}

static int ScriptLineCount (const TextBuffer* text) {
    int count = 1;
    for (size_t i = 0; i < text->length; i++) {
        if (text->data[i] == '\n') count++;
    }
    return count;
}

static int ScriptVisibleLines (void) {
    Rectangle code = ScriptCodeRect (ScriptPanel ());
    int visible = (int)(code.height / UI_CODE_LINE_HEIGHT);
    return visible > 0 ? visible : 1;
}

static float ScriptMaxScroll (const TextBuffer* text) {
    int maxScroll = ScriptLineCount (text) - ScriptVisibleLines ();
    return maxScroll > 0 ? (float)maxScroll : 0.0f;
}

static size_t ScriptPrevCodepoint (const TextBuffer* text, size_t pos) {
    if (pos == 0 || pos > text->length) return 0;
    pos--;
    while (pos > 0 && ((unsigned char)text->data[pos] & 0xC0) == 0x80) pos--;
    return pos;
}

static size_t ScriptNextCodepoint (const TextBuffer* text, size_t pos) {
    if (pos >= text->length) return text->length;
    pos++;
    while (pos < text->length && ((unsigned char)text->data[pos] & 0xC0) == 0x80) pos++;
    return pos;
}

static size_t ScriptLineStart (const TextBuffer* text, int line) {
    size_t pos = 0;
    for (int i = 0; i < line && pos < text->length; i++) {
        while (pos < text->length && text->data[pos] != '\n') pos++;
        if (pos < text->length) pos++;
    }
    return pos;
}

static size_t ScriptLineEnd (const TextBuffer* text, size_t start) {
    size_t pos = start;
    while (pos < text->length && text->data[pos] != '\n') pos++;
    return pos;
}

static int ScriptLineOfOffset (const TextBuffer* text, size_t offset) {
    int line = 0;
    for (size_t i = 0; i < offset && i < text->length; i++) {
        if (text->data[i] == '\n') line++;
    }
    return line;
}

static int ScriptDisplayColumn (const TextBuffer* text, size_t start, size_t pos) {
    int column = 0;
    for (size_t i = start; i < pos && i < text->length;) {
        column += text->data[i] == '\t' ? 4 : 1;
        i = ScriptNextCodepoint (text, i);
    }
    return column;
}

static size_t ScriptOffsetAtColumn (const TextBuffer* text, size_t start, size_t end, int column) {
    int current = 0;
    size_t pos = start;
    while (pos < end) {
        int width = text->data[pos] == '\t' ? 4 : 1;
        if (current + width > column) return pos;
        current += width;
        pos = ScriptNextCodepoint (text, pos);
    }
    return end;
}

static void ScriptScrollToCursor () {
    int line = ScriptLineOfOffset (&uiScriptText, uiScriptCursor);
    int visible = ScriptVisibleLines ();
    int start = (int)uiScriptScroll;
    if (line < start)
        uiScriptScroll = (float)line;
    else if (line >= start + visible)
        uiScriptScroll = (float)(line - visible + 1);
    uiScriptScroll = Clamp (uiScriptScroll, 0.0f, ScriptMaxScroll (&uiScriptText));
}

static bool InsertScriptText (const char* text, size_t length) {
    if (length == 0) return true;
    TextBuffer* buffer = &uiScriptText;
    if (!TextBufferReserve (buffer, length)) return false;

    memmove (buffer->data + uiScriptCursor + length, buffer->data + uiScriptCursor, buffer->length - uiScriptCursor);
    memcpy (buffer->data + uiScriptCursor, text, length);
    buffer->length += length;
    buffer->data[buffer->length] = '\0';
    uiScriptCursor += length;
    uiScriptPreferredColumn = -1;
    return true;
}

static bool InsertScriptCodepoint (int codepoint) {
    int size = 0;
    const char* utf8 = CodepointToUTF8 (codepoint, &size);
    if (utf8 == NULL || size <= 0) return false;
    return InsertScriptText (utf8, (size_t)size);
}

static void RemoveScriptRange (size_t start, size_t end) {
    TextBuffer* buffer = &uiScriptText;
    if (start >= end || end > buffer->length) return;

    memmove (buffer->data + start, buffer->data + end, buffer->length - end);
    buffer->length -= end - start;
    buffer->data[buffer->length] = '\0';
    uiScriptCursor = start;
    uiScriptPreferredColumn = -1;
}

static void ScriptMoveVertical (int direction) {
    TextBuffer* text = &uiScriptText;
    int line = ScriptLineOfOffset (text, uiScriptCursor);
    size_t start = ScriptLineStart (text, line);
    if (uiScriptPreferredColumn < 0) uiScriptPreferredColumn = ScriptDisplayColumn (text, start, uiScriptCursor);

    int target = line + direction;
    int total = ScriptLineCount (text);
    if (target < 0 || target >= total) return;

    size_t targetStart = ScriptLineStart (text, target);
    size_t targetEnd = ScriptLineEnd (text, targetStart);
    uiScriptCursor = ScriptOffsetAtColumn (text, targetStart, targetEnd, uiScriptPreferredColumn);
}

static void ScriptSetCursorFromMouse (Rectangle code, Vector2 mouse) {
    TextBuffer* text = &uiScriptText;
    int total = ScriptLineCount (text);
    int start = (int)uiScriptScroll;
    int line = start + (int)((mouse.y - (code.y + 6.0f)) / UI_CODE_LINE_HEIGHT);
    if (line < 0) line = 0;
    if (line >= total) line = total - 1;

    size_t lineStart = ScriptLineStart (text, line);
    size_t lineEnd = ScriptLineEnd (text, lineStart);
    float target = mouse.x - (code.x + 8.0f);

    size_t best = lineStart;
    float bestDistance = fabsf (target);
    TextBuffer display = {0};
    for (size_t pos = lineStart; pos < lineEnd;) {
        size_t next = ScriptNextCodepoint (text, pos);
        for (size_t i = pos; i < next; i++) {
            if (text->data[i] == '\t')
                TextBufferAppend (&display, "    ");
            else
                TextBufferAppendChar (&display, text->data[i]);
        }
        pos = next;
        float width =
            MeasureTextEx (CutilsMonoFont, display.data != NULL ? display.data : "", UI_CODE_FONT_SIZE, 0.0f).x;
        float distance = fabsf (width - target);
        if (distance < bestDistance) {
            best = pos;
            bestDistance = distance;
        }
    }
    TextBufferFree (&display);

    uiScriptCursor = best;
    uiScriptPreferredColumn = -1;
}

static void ScriptClose () {
    uiScriptOpen = false;
    uiScriptSelf[0] = '\0';
    uiScriptSelfFocused = false;
    uiScriptCursor = 0;
    uiScriptPreferredColumn = -1;
    uiScriptScroll = 0.0f;
    TextBufferFree (&uiScriptText);
}

static void OpenScript () {
    ScriptClose ();
    uiScriptOpen = true;
}

static void UpdateScript (UiEvent* event) {
    Vector2 mouse = GetMousePosition ();
    Rectangle panel = ScriptPanel ();
    Rectangle code = ScriptCodeRect (panel);
    Rectangle self = ScriptSelfRect (panel);
    Rectangle submit = ScriptSubmitRect (panel);
    Rectangle cancel = ScriptCancelRect (panel);
    float maxScroll = ScriptMaxScroll (&uiScriptText);
    uiScriptScroll = Clamp (uiScriptScroll - CutilsWheelMove () * 3.0f, 0.0f, maxScroll);

    if (CutilsMousePressed (MOUSE_BUTTON_LEFT)) {
        if (CheckCollisionPointRec (mouse, submit)) {
            event->action = UI_ACTION_RUN_SCRIPT;
            return;
        }
        if (CheckCollisionPointRec (mouse, cancel)) {
            ScriptClose ();
            return;
        }
        if (CheckCollisionPointRec (mouse, self)) {
            uiScriptSelfFocused = true;
            return;
        }
        if (CheckCollisionPointRec (mouse, code)) {
            uiScriptSelfFocused = false;
            ScriptSetCursorFromMouse (code, mouse);
            ScriptScrollToCursor ();
            return;
        }
    }
    if (CutilsKeyPressed (KEY_ESCAPE)) {
        ScriptClose ();
        return;
    }

    bool control = CutilsKeyDown (KEY_LEFT_CONTROL) || CutilsKeyDown (KEY_RIGHT_CONTROL);
    if (control && (CutilsKeyPressed (KEY_ENTER) || CutilsKeyPressed (KEY_KP_ENTER))) {
        event->action = UI_ACTION_RUN_SCRIPT;
        return;
    }

    if (uiScriptSelfFocused) {
        int codepoint;
        while ((codepoint = CutilsCharPressed ()) > 0) {
            if (codepoint < 32 || codepoint == 127) continue;
            AppendCodepoint (uiScriptSelf, (int)sizeof (uiScriptSelf), codepoint);
        }
        if (CutilsKeyPressed (KEY_BACKSPACE)) RemoveLastCodepoint (uiScriptSelf);
        if (CutilsKeyPressed (KEY_ENTER) || CutilsKeyPressed (KEY_KP_ENTER)) {
            event->action = UI_ACTION_RUN_SCRIPT;
        }
        return;
    }

    bool cursorChanged = false;
    TextBuffer* text = &uiScriptText;
    int line = ScriptLineOfOffset (text, uiScriptCursor);
    size_t lineStart = ScriptLineStart (text, line);
    size_t lineEnd = ScriptLineEnd (text, lineStart);

    if (CutilsKeyPressed (KEY_LEFT)) {
        uiScriptCursor = ScriptPrevCodepoint (text, uiScriptCursor);
        uiScriptPreferredColumn = -1;
        cursorChanged = true;
    } else if (CutilsKeyPressed (KEY_RIGHT)) {
        uiScriptCursor = ScriptNextCodepoint (text, uiScriptCursor);
        uiScriptPreferredColumn = -1;
        cursorChanged = true;
    } else if (CutilsKeyPressed (KEY_UP)) {
        ScriptMoveVertical (-1);
        cursorChanged = true;
    } else if (CutilsKeyPressed (KEY_DOWN)) {
        ScriptMoveVertical (1);
        cursorChanged = true;
    } else if (CutilsKeyPressed (KEY_HOME)) {
        uiScriptCursor = control ? 0 : lineStart;
        uiScriptPreferredColumn = -1;
        cursorChanged = true;
    } else if (CutilsKeyPressed (KEY_END)) {
        uiScriptCursor = control ? text->length : lineEnd;
        uiScriptPreferredColumn = -1;
        cursorChanged = true;
    } else if (CutilsKeyPressed (KEY_BACKSPACE)) {
        if (uiScriptCursor > 0) {
            size_t previous = ScriptPrevCodepoint (text, uiScriptCursor);
            RemoveScriptRange (previous, uiScriptCursor);
            cursorChanged = true;
        }
    } else if (CutilsKeyPressed (KEY_DELETE)) {
        if (uiScriptCursor < text->length) {
            size_t next = ScriptNextCodepoint (text, uiScriptCursor);
            RemoveScriptRange (uiScriptCursor, next);
            cursorChanged = true;
        }
    }

    int codepoint;
    while ((codepoint = CutilsCharPressed ()) > 0) {
        if (codepoint < 32 || codepoint == 127) continue;
        if (InsertScriptCodepoint (codepoint)) cursorChanged = true;
    }
    if (CutilsKeyPressed (KEY_TAB)) {
        if (InsertScriptText ("\t", 1)) cursorChanged = true;
    }
    if (CutilsKeyPressed (KEY_ENTER) || CutilsKeyPressed (KEY_KP_ENTER)) {
        if (InsertScriptText ("\n", 1)) cursorChanged = true;
    }

    if (uiScriptCursor > text->length) uiScriptCursor = text->length;
    if (cursorChanged) ScriptScrollToCursor ();
}

UiEvent UiUpdate () {
    UiEvent event = {0};
    bool captured = false;
    ValidateAssets ();
    ValidateObjects ();
    if (uiObjectManagerOpen) {
        Object* locked = EntityFind (NetClientLockedObjectId ());
        if (uiSelectedObject != NULL && uiSelectedObject != locked) {
            UiObjectDeleted ();
            uiObjectManagerOpen = false;
        } else if (uiSelectedObject != locked) {
            UiSelectObject (locked);
        }
    }
    LayoutMenus ();
    Vector2 mouse = GetMousePosition ();
    float wheel = CutilsWheelMove ();

    if (!IsMouseButtonDown (MOUSE_BUTTON_LEFT)) uiPress = false;

    float dt = fminf (GetFrameTime (), 0.1f);
    int activeMessages = 0;
    for (int i = 0; i < uiMessageCount; i++) {
        uiMessageTimes[i] -= dt;
        if (uiMessageTimes[i] <= 0.0f) continue;
        if (activeMessages != i) {
            memmove (uiMessages[activeMessages], uiMessages[i], sizeof (uiMessages[0]));
            uiMessageTimes[activeMessages] = uiMessageTimes[i];
            uiMessageKinds[activeMessages] = uiMessageKinds[i];
        }
        activeMessages++;
    }
    uiMessageCount = activeMessages;

    if (uiScriptOpen) {
        UpdateScript (&event);
        captured = true;
        goto ui_done;
    }

    if (uiConnectOpen) {
        UpdateConnect (&event);
        captured = true;
        goto ui_done;
    }

    if (uiLayerOpen) {
        UpdateLayer (&event);
        captured = true;
        goto ui_done;
    }

    if (uiRenameOpen) {
        UpdateRename (&event);
        captured = true;
        goto ui_done;
    }

    if (uiObjectContextOpen) {
        UpdateObjectContext (&event);
        captured = true;
        goto ui_done;
    }

    if (uiManagerOpen) {
        UpdateManager (&event, wheel);
        captured = true;
        goto ui_done;
    }

    if (uiPickerOpen) {
        IdGroup assetIds = AssetIds ();
        int assetCount = assetIds.count;
        Rectangle panel = PickerPanel ();
        Rectangle list = PickerListRect (panel);
        Rectangle cancel = PickerCancelRect (panel);
        float maxScroll = fmaxf (0.0f, assetCount * UI_ASSET_ROW_HEIGHT - list.height);
        uiPickerScroll = Clamp (uiPickerScroll - wheel * 40.0f, 0.0f, maxScroll);

        if (CutilsMousePressed (MOUSE_BUTTON_LEFT)) {
            if (CheckCollisionPointRec (mouse, cancel) || !CheckCollisionPointRec (mouse, panel)) {
                uiPickerOpen = false;
            } else if (CheckCollisionPointRec (mouse, list)) {
                int index = (int)((mouse.y - list.y + uiPickerScroll) / UI_ASSET_ROW_HEIGHT);
                if (index >= 0 && index < assetIds.count) {
                    Asset* asset = EntityFind (assetIds.ids[index]);
                    if (asset != NULL && uiPickerObject != NULL) {
                        uiAssetsObjectSelected = uiPickerObject->assets.count;
                        event.action = UI_ACTION_ADD_OBJECT_ASSET;
                        event.u.objectAsset.object = uiPickerObject;
                        event.u.objectAsset.asset = asset;
                        uiPickerOpen = false;
                    }
                }
            }
            uiPress = true;
        }

        IdGroupUnload (&assetIds);
        captured = true;
        goto ui_done;
    }

    if (uiObjectAssetsOpen) {
        UpdateObjectAssets (&event, wheel);
        captured = true;
        goto ui_done;
    }

    if (uiObjectManagerOpen) {
        UpdateObjectManager (&event, wheel);
        captured = true;
        goto ui_done;
    }

    if (uiMenuOpen < 0 && UpdateUserMenuButtons (&event)) {
        captured = true;
        goto ui_done;
    }

    if (CutilsMousePressed (MOUSE_BUTTON_LEFT)) {
        if (mouse.y <= UI_BAR_HEIGHT) {
            int hit = -1;
            for (int i = 0; i < UI_MENU_COUNT; i++) {
                if (i == 4 && !uiShowConnectMenu) continue;
                Rectangle title = {uiMenuX[i], 0.0f, uiMenuW[i], UI_BAR_HEIGHT};
                if (CheckCollisionPointRec (mouse, title)) {
                    hit = i;
                    break;
                }
            }

            if (hit == 0) {
                bool wasOpen = (uiMenuOpen == 0);
                uiMenuOpen = wasOpen ? -1 : 0;
                uiSubmenuOpen = -1;
            } else if (hit == 1) {
                uiMenuOpen = -1;
                uiSubmenuOpen = -1;
                UiOpenAssetManager ();
            } else if (hit == 2) {
                uiMenuOpen = -1;
                uiSubmenuOpen = -1;
                UiOpenObjectManager (NULL);
            } else if (hit == 3) {
                uiMenuOpen = -1;
                uiSubmenuOpen = -1;
                if (ScriptAllowed ()) {
                    OpenScript ();
                    snprintf (uiScriptSelf, sizeof (uiScriptSelf), "self");
                }
            } else if (hit == 4) {
                bool wasOpen = (uiMenuOpen == 4);
                uiMenuOpen = wasOpen ? -1 : 4;
                uiSubmenuOpen = -1;
            } else {
                uiMenuOpen = -1;
                uiSubmenuOpen = -1;
            }

            uiPress = true;
        } else if (uiMenuOpen >= 0) {
            int menu = uiMenuOpen;
            Rectangle drop = {uiMenuX[menu], UI_BAR_HEIGHT, UI_MENU_WIDTH, menuItemCounts[menu] * UI_ITEM_HEIGHT};
            if (CheckCollisionPointRec (mouse, drop)) {
                int item = (int)((mouse.y - UI_BAR_HEIGHT) / UI_ITEM_HEIGHT);
                if (menu == 0 && item == 0) {
                    if (OpenImportDialog (event.u.path.path, (int)sizeof (event.u.path.path))) {
                        if (PathHasExtension (event.u.path.path, ".dsk"))
                            event.action = UI_ACTION_IMPORT_SAVE;
                        else if (PathIsImage (event.u.path.path))
                            event.action = UI_ACTION_IMPORT;
                        else
                            UiShowError ("不支持的文件类型");
                    }
                } else if (menu == 0 && item == 1) {
                    if (OpenFolderDialog (event.u.path.path, (int)sizeof (event.u.path.path)))
                        event.action = UI_ACTION_IMPORT_SAVE;
                } else if (menu == 0 && item == 2) {
                    if (OpenExportDialog (event.u.path.path, (int)sizeof (event.u.path.path)))
                        event.action = UI_ACTION_EXPORT_SAVE;
                } else if (menu == 4 && item == 0) {
                    OpenConnect ();
                } else if (menu == 4 && item == 1) {
                    event.action = UI_ACTION_OPEN_SERVER;
                }
            }
            uiMenuOpen = -1;
            uiSubmenuOpen = -1;
            uiPress = true;
        }
    }

    captured = uiMenuOpen >= 0 || mouse.y <= UI_BAR_HEIGHT;

ui_done:
    event.mousePosition = GetMousePosition ();
    if (WindowShouldClose ()) {
        event.action = UI_ACTION_CLOSE;
        return event;
    }
    if (IsFileDropped ()) event.files = LoadDroppedFiles ();
    event.mouseDelta = GetMouseDelta ();

    if (event.action == UI_ACTION_NONE) {
        float wheel = CutilsWheelMove ();
        bool mouseActive =
            CutilsMousePressed (MOUSE_BUTTON_LEFT) ||
            IsMouseButtonDown (MOUSE_BUTTON_LEFT) ||
            CutilsMouseReleased (MOUSE_BUTTON_LEFT) ||
            CutilsMousePressed (MOUSE_BUTTON_RIGHT) ||
            IsMouseButtonDown (MOUSE_BUTTON_RIGHT) ||
            CutilsMouseReleased (MOUSE_BUTTON_RIGHT);
        if (captured) {
            if (mouseActive) event.action = UI_ACTION_UI;
        } else if (wheel != 0.0f) {
            event.action = UI_ACTION_CANVAS_ZOOM;
            event.u.canvasZoom.wheel = wheel;
        } else if (CutilsMousePressed (MOUSE_BUTTON_LEFT)) {
            event.action = UI_ACTION_CANVAS_CLICK;
        } else if (CutilsMousePressed (MOUSE_BUTTON_RIGHT)) {
            event.action = UI_ACTION_CANVAS_RIGHT_CLICK;
        } else if (IsMouseButtonDown (MOUSE_BUTTON_LEFT)) {
            Vector2 delta = GetMouseDelta ();
            if (delta.x != 0.0f || delta.y != 0.0f) event.action = UI_ACTION_CANVAS_DRAG;
        } else if (CutilsMouseReleased (MOUSE_BUTTON_LEFT)) {
            event.action = UI_ACTION_CANVAS_RELEASE;
        } else {
            Vector2 delta = GetMouseDelta ();
            if (delta.x != 0.0f || delta.y != 0.0f) event.action = UI_ACTION_CANVAS_MOVE;
        }
    }
    return event;
}

static float MenuTextY (Font font, const char* text, Rectangle item) {
    float height = MeasureTextEx (font, text, UI_FONT_SIZE, 0).y;
    return item.y + (item.height - height) / 2.0f;
}

static void DrawMenuBar () {
    LayoutMenus ();
    Vector2 mouse = GetMousePosition ();
    DrawRectangle (0, 0, GetScreenWidth (), UI_BAR_HEIGHT, (Color){16, 18, 23, 255});
    DrawRectangle (0, UI_BAR_HEIGHT - 1, GetScreenWidth (), 1, (Color){55, 60, 72, 255});

    for (int i = 0; i < UI_MENU_COUNT; i++) {
        if (i == 4 && !uiShowConnectMenu) continue;
        Rectangle title = {uiMenuX[i], 0.0f, uiMenuW[i], UI_BAR_HEIGHT};
        if (uiMenuOpen == i)
            DrawRectangleRec (title, (Color){45, 49, 61, 255});
        else if (CheckCollisionPointRec (mouse, title))
            DrawRectangleRec (title, (Color){32, 35, 44, 255});
        DrawUiText (
            CutilsFont, menuNames[i], (Vector2){title.x + 12.0f, MenuTextY (CutilsFont, menuNames[i], title)},
            UI_FONT_SIZE, 0, (Color){230, 232, 240, 255});
    }

    if (uiMenuOpen >= 0) {
        int menu = uiMenuOpen;
        Rectangle drop = {uiMenuX[menu], UI_BAR_HEIGHT, UI_MENU_WIDTH, menuItemCounts[menu] * UI_ITEM_HEIGHT};
        DrawRectangleRec (drop, (Color){28, 31, 39, 255});

        for (int i = 0; i < menuItemCounts[menu]; i++) {
            Rectangle item = {drop.x, drop.y + i * UI_ITEM_HEIGHT, drop.width, UI_ITEM_HEIGHT};
            if (CheckCollisionPointRec (mouse, item)) DrawRectangleRec (item, (Color){45, 49, 61, 255});
            DrawUiText (
                CutilsFont, menuItems[menu][i],
                (Vector2){item.x + 12.0f, MenuTextY (CutilsFont, menuItems[menu][i], item)}, UI_FONT_SIZE, 0,
                (Color){225, 228, 236, 255});
        }

        DrawRectangleLinesEx (drop, 1, (Color){60, 65, 80, 255});
    }
}

static void DrawAssetFit (const Asset* asset, Rectangle box) {
    if (asset == NULL || AssetTexture (asset).width <= 0 || AssetTexture (asset).height <= 0) return;
    Rectangle source = {0, 0, (float)AssetTexture (asset).width, (float)AssetTexture (asset).height};
    float scale = fminf (box.width / AssetTexture (asset).width, box.height / AssetTexture (asset).height);
    Rectangle dest = {
        box.x + (box.width - AssetTexture (asset).width * scale) / 2.0f,
        box.y + (box.height - AssetTexture (asset).height * scale) / 2.0f, AssetTexture (asset).width * scale,
        AssetTexture (asset).height * scale};
    DrawTexturePro (AssetTexture (asset), source, dest, (Vector2){0}, 0.0f, WHITE);
    rlDrawRenderBatchActive ();
}

static void DrawDialogBase (Rectangle panel, const char* title) {
    DrawRectangle (0, 0, GetScreenWidth (), GetScreenHeight (), Fade (BLACK, 0.55f));
    DrawRectangleRec (panel, (Color){26, 29, 36, 255});
    DrawRectangleLinesEx (panel, 1, (Color){70, 76, 92, 255});
    DrawUiText (
        CutilsFont, title, (Vector2){panel.x + 24.0f, panel.y + 16.0f}, UI_FONT_SIZE, 0, (Color){235, 238, 245, 255});
    DrawRectangle (
        (int)(panel.x + 24.0f), (int)(panel.y + 43.0f), (int)(panel.width - 48.0f), 1, (Color){58, 63, 76, 255});
}

static void DrawListScrollbar (Rectangle list, int itemCount, float rowHeight, float scroll) {
    float contentH = itemCount * rowHeight;
    if (contentH <= list.height) return;

    float barH = list.height * list.height / contentH;
    float maxScroll = contentH - list.height;
    float t = (maxScroll > 0.0f) ? scroll / maxScroll : 0.0f;

    DrawRectangle (
        (int)(list.x + list.width - 7.0f), (int)(list.y + t * (list.height - barH)), 3, (int)barH,
        (Color){90, 96, 112, 255});
}

static void DrawPicker () {
    if (!uiPickerOpen) return;

    IdGroup assetIds = AssetIds ();
    int assetCount = assetIds.count;
    Vector2 mouse = GetMousePosition ();
    Rectangle panel = PickerPanel ();
    Rectangle list = PickerListRect (panel);
    Rectangle cancel = PickerCancelRect (panel);

    DrawDialogBase (panel, "选择素材");

    if (assetCount == 0) {
        DrawUiText (
            CutilsFont, "没有素材", (Vector2){panel.x + 24.0f, list.y + 20.0f}, UI_FONT_SIZE, 0,
            (Color){225, 228, 236, 255});
        DrawUiText (
            CutilsFont, "请先导入或拖入图片", (Vector2){panel.x + 24.0f, list.y + 44.0f}, UI_FONT_SIZE, 0,
            (Color){150, 158, 172, 255});
    } else {
        CutilsBeginScissor ((int)list.x, (int)list.y, (int)list.width, (int)list.height);

        for (int i = 0; i < assetCount; i++) {
            float rowY = list.y + i * UI_ASSET_ROW_HEIGHT - uiPickerScroll;
            if (rowY + UI_ASSET_ROW_HEIGHT < list.y || rowY > list.y + list.height) continue;

            Rectangle row = {list.x, rowY, list.width, UI_ASSET_ROW_HEIGHT};
            if (CheckCollisionPointRec (mouse, list) && CheckCollisionPointRec (mouse, row))
                DrawRectangleRec (row, (Color){45, 49, 61, 255});

            Asset* asset = EntityFind (assetIds.ids[i]);
            if (asset == NULL) continue;

            DrawAssetFit (
                asset, (Rectangle){list.x + 14.0f, rowY + (UI_ASSET_ROW_HEIGHT - 48.0f) / 2.0f, 56.0f, 48.0f});

            DrawUiText (
                CutilsFont, TextFormat ("素材 %d", i + 1), (Vector2){list.x + 86.0f, rowY + 24.0f}, UI_FONT_SIZE, 0,
                (Color){225, 228, 236, 255});
        }

        CutilsEndScissor ();

        DrawListScrollbar (list, assetCount, UI_ASSET_ROW_HEIGHT, uiPickerScroll);
    }

    IdGroupUnload (&assetIds);
    DrawSmallButton (CutilsFont, cancel, "取消", false, true);
}

static void DrawSmallButton (Font font, Rectangle rect, const char* text, bool danger, bool enabled) {
    Vector2 mouse = GetMousePosition ();
    bool hot = enabled && CheckCollisionPointRec (mouse, rect);

    Color background = !enabled ? (Color){30, 33, 41, 255} : hot ? (Color){55, 61, 76, 255} : (Color){38, 42, 52, 255};
    Color foreground = !enabled ? (Color){90, 96, 112, 255}
                       : danger ? (Color){240, 120, 120, 255}
                                : (Color){225, 228, 236, 255};

    DrawRectangleRec (rect, background);
    DrawRectangleLinesEx (rect, 1, (Color){70, 76, 92, 255});

    float width = MeasureTextEx (font, text, UI_FONT_SIZE, 0).x;
    DrawUiText (
        font, text, (Vector2){rect.x + (rect.width - width) / 2.0f, rect.y + (rect.height - UI_FONT_SIZE) / 2.0f},
        UI_FONT_SIZE, 0, foreground);
}

static void DrawManager () {
    if (!uiManagerOpen) return;

    Vector2 mouse = GetMousePosition ();
    IdGroup assetIds = AssetIds ();
    int assetCount = assetIds.count;
    Rectangle panel = ManagerPanel ();
    Rectangle list = ManagerListRect (panel);
    Rectangle exit = ManagerExitRect (panel);

    DrawDialogBase (panel, "素材管理");

    if (assetCount == 0) {
        DrawUiText (
            CutilsFont, "没有素材", (Vector2){panel.x + 24.0f, list.y + 24.0f}, UI_FONT_SIZE, 0,
            (Color){225, 228, 236, 255});
    } else {
        CutilsBeginScissor ((int)list.x, (int)list.y, (int)list.width, (int)list.height);

        for (int i = 0; i < assetCount; i++) {
            float rowY = list.y + i * UI_MANAGER_ROW_HEIGHT - uiManagerScroll;
            if (rowY + UI_MANAGER_ROW_HEIGHT < list.y || rowY > list.y + list.height) continue;

            Rectangle row = {list.x, rowY, list.width, UI_MANAGER_ROW_HEIGHT};
            Asset* asset = EntityFind (assetIds.ids[i]);
            if (asset == NULL) continue;

            if (asset == uiSelectedAsset) {
                DrawRectangleRec (row, (Color){45, 60, 88, 255});
                DrawRectangle ((int)list.x, (int)rowY, 3, UI_MANAGER_ROW_HEIGHT, (Color){84, 158, 255, 255});
            } else if (CheckCollisionPointRec (mouse, list) && CheckCollisionPointRec (mouse, row)) {
                DrawRectangleRec (row, (Color){38, 42, 52, 255});
            }

            DrawAssetFit (asset, (Rectangle){list.x + 16.0f, rowY + 10.0f, 80.0f, 60.0f});

            DrawUiText (
                CutilsFont, AssetName (asset), (Vector2){list.x + 112.0f, rowY + 32.0f}, UI_FONT_SIZE, 0,
                (Color){225, 228, 236, 255});

            DrawRectangle (
                (int)list.x, (int)(rowY + UI_MANAGER_ROW_HEIGHT - 1.0f), (int)list.width, 1, (Color){45, 49, 60, 255});
        }

        CutilsEndScissor ();

        DrawListScrollbar (list, assetCount, UI_MANAGER_ROW_HEIGHT, uiManagerScroll);
    }

    IdGroupUnload (&assetIds);
    Rectangle renameBtn = ManagerRenameRect (panel);
    Rectangle replaceBtn = ManagerReplaceRect (panel);
    Rectangle deleteBtn = ManagerDeleteRect (panel);
    Rectangle importBtn = ManagerImportRect (panel);
    bool hasSelection = (uiSelectedAsset != NULL);

    DrawRectangle (
        (int)(panel.x + 24.0f), (int)(panel.y + panel.height - UI_MANAGER_FOOTER), (int)(panel.width - 48.0f), 1,
        (Color){58, 63, 76, 255});
    DrawSmallButton (CutilsFont, renameBtn, "重命名", false, hasSelection);
    DrawSmallButton (CutilsFont, replaceBtn, "更换", false, hasSelection);
    DrawSmallButton (CutilsFont, deleteBtn, "删除", true, hasSelection);
    DrawSmallButton (CutilsFont, importBtn, "导入", false, true);
    DrawSmallButton (CutilsFont, exit, "确定", false, true);
}

static void DrawObjectManager () {
    if (!uiObjectManagerOpen) return;

    Vector2 mouse = GetMousePosition ();
    IdGroup objectIds = ObjectIds ();
    int objectCount = objectIds.count;
    Rectangle panel = ManagerPanel ();
    Rectangle list = ManagerListRect (panel);

    DrawDialogBase (panel, "物件管理");

    if (objectCount == 0) {
        DrawUiText (
            CutilsFont, "没有物件", (Vector2){panel.x + 24.0f, list.y + 24.0f}, UI_FONT_SIZE, 0,
            (Color){225, 228, 236, 255});
    } else {
        CutilsBeginScissor ((int)list.x, (int)list.y, (int)list.width, (int)list.height);

        for (int i = 0; i < objectCount; i++) {
            float rowY = list.y + i * UI_MANAGER_ROW_HEIGHT - uiObjectScroll;
            if (rowY + UI_MANAGER_ROW_HEIGHT < list.y || rowY > list.y + list.height) continue;

            Rectangle row = {list.x, rowY, list.width, UI_MANAGER_ROW_HEIGHT};
            Object* object = EntityFind (objectIds.ids[i]);
            if (object == NULL) continue;

            bool unavailable =
                object->lockOwnerId != 0 && object->lockOwnerId != NetClientUserId () && object != uiSelectedObject;
            if (object == uiSelectedObject) {
                DrawRectangleRec (row, (Color){45, 60, 88, 255});
                DrawRectangle ((int)list.x, (int)rowY, 3, UI_MANAGER_ROW_HEIGHT, (Color){84, 158, 255, 255});
            } else if (unavailable) {
                DrawRectangleRec (row, (Color){58, 61, 68, 255});
                DrawRectangle ((int)list.x, (int)rowY, 4, UI_MANAGER_ROW_HEIGHT, (Color){120, 124, 134, 255});
            } else if (CheckCollisionPointRec (mouse, list) && CheckCollisionPointRec (mouse, row)) {
                DrawRectangleRec (row, (Color){38, 42, 52, 255});
            }

            Rectangle thumb = {list.x + 16.0f, rowY + 10.0f, 80.0f, 60.0f};
            Asset* firstAsset = ObjectDisplayedAsset (object);
            if (firstAsset != NULL) {
                DrawAssetFit (firstAsset, thumb);
            } else {
                DrawRectangleRec (thumb, (Color){20, 22, 28, 255});
                DrawRectangleLinesEx (thumb, 1, (Color){60, 65, 80, 255});
                DrawUiText (
                    CutilsFont, "空", (Vector2){thumb.x + 32.0f, thumb.y + 22.0f}, UI_FONT_SIZE, 0,
                    (Color){110, 116, 132, 255});
            }

            Color nameColor = unavailable ? (Color){215, 218, 225, 255} : (Color){225, 228, 236, 255};
            Color layerColor = unavailable ? (Color){160, 165, 175, 255} : (Color){150, 158, 172, 255};
            DrawUiText (CutilsFont, object->name, (Vector2){list.x + 112.0f, rowY + 18.0f}, UI_FONT_SIZE, 0, nameColor);
            DrawUiText (
                CutilsFont, TextFormat ("层数 %d", object->layer), (Vector2){list.x + 112.0f, rowY + 44.0f},
                UI_FONT_SIZE, 0, layerColor);

            if (unavailable) {
                User* owner = EntityFind (object->lockOwnerId);
                const char* ownerText = owner ? owner->name : "";
                float ownerWidth = MeasureTextEx (CutilsFont, ownerText, UI_FONT_SIZE, 0).x;
                Color ownerColor = owner != NULL ? UserColor (owner) : (Color){205, 209, 218, 255};
                DrawUiText (
                    CutilsFont, ownerText, (Vector2){list.x + list.width - ownerWidth - 16.0f, rowY + 32.0f},
                    UI_FONT_SIZE, 0, ownerColor);
            }

            DrawRectangle (
                (int)list.x, (int)(rowY + UI_MANAGER_ROW_HEIGHT - 1.0f), (int)list.width, 1, (Color){45, 49, 60, 255});
        }

        CutilsEndScissor ();

        DrawListScrollbar (list, objectCount, UI_MANAGER_ROW_HEIGHT, uiObjectScroll);
    }

    IdGroupUnload (&objectIds);
    bool hasSelection = (uiSelectedObject != NULL);
    DrawRectangle (
        (int)(panel.x + 24.0f), (int)(panel.y + panel.height - UI_MANAGER_FOOTER), (int)(panel.width - 48.0f), 1,
        (Color){58, 63, 76, 255});
    DrawSmallButton (CutilsFont, ObjectRenameRect (panel), "重命名", false, hasSelection);
    DrawSmallButton (CutilsFont, ObjectManageRect (panel), "管理", false, hasSelection);
    DrawSmallButton (CutilsFont, ObjectLayerRect (panel), "浮动", false, hasSelection);
    DrawSmallButton (CutilsFont, ObjectDeleteRect (panel), "删除", true, hasSelection);
    DrawSmallButton (CutilsFont, ObjectCreateRect (panel), "新建", false, true);
    DrawSmallButton (CutilsFont, ObjectDoneRect (panel), "确定", false, true);
}

static void DrawObjectAssets () {
    if (!uiObjectAssetsOpen || uiAssetsObject == NULL) return;

    Object* object = uiAssetsObject;
    int count = object->assets.count;
    Vector2 mouse = GetMousePosition ();
    Rectangle panel = ObjectAssetsPanel ();
    Rectangle list = ObjectAssetsListRect (panel);

    DrawDialogBase (panel, TextFormat ("物件素材: %s", object->name));

    if (count == 0) {
        DrawUiText (
            CutilsFont, "没有素材", (Vector2){panel.x + 24.0f, list.y + 24.0f}, UI_FONT_SIZE, 0,
            (Color){225, 228, 236, 255});
    } else {
        CutilsBeginScissor ((int)list.x, (int)list.y, (int)list.width, (int)list.height);

        for (int i = 0; i < count; i++) {
            float rowY = list.y + i * UI_OBJECT_ASSET_ROW_HEIGHT - uiAssetsObjectScroll;
            if (rowY + UI_OBJECT_ASSET_ROW_HEIGHT < list.y || rowY > list.y + list.height) continue;

            Rectangle row = {list.x, rowY, list.width, UI_OBJECT_ASSET_ROW_HEIGHT};
            Asset* asset = EntityFind (object->assets.ids[i]);
            if (asset == NULL) continue;

            int usedIndex = object->assetIndex;
            if (usedIndex < 0 || usedIndex >= count) usedIndex = 0;
            bool used = (i == usedIndex);

            if (i == uiAssetsObjectSelected) {
                DrawRectangleRec (row, (Color){45, 60, 88, 255});
                DrawRectangle ((int)list.x, (int)rowY, 3, UI_OBJECT_ASSET_ROW_HEIGHT, (Color){84, 158, 255, 255});
            } else if (CheckCollisionPointRec (mouse, list) && CheckCollisionPointRec (mouse, row)) {
                DrawRectangleRec (row, (Color){38, 42, 52, 255});
            }

            Color indexColor = used ? (Color){110, 220, 135, 255} : (Color){150, 158, 172, 255};
            DrawUiText (
                CutilsFont, TextFormat ("#%d", i), (Vector2){list.x + 16.0f, rowY + 24.0f}, UI_FONT_SIZE, 0,
                indexColor);

            DrawAssetFit (asset, (Rectangle){list.x + 64.0f, rowY + 8.0f, 60.0f, 48.0f});

            Color nameColor = used ? (Color){110, 220, 135, 255} : (Color){225, 228, 236, 255};
            DrawUiText (
                CutilsFont, AssetName (asset), (Vector2){list.x + 136.0f, rowY + 24.0f}, UI_FONT_SIZE, 0, nameColor);

            DrawRectangle (
                (int)list.x, (int)(rowY + UI_OBJECT_ASSET_ROW_HEIGHT - 1.0f), (int)list.width, 1,
                (Color){45, 49, 60, 255});
        }

        CutilsEndScissor ();

        DrawListScrollbar (list, count, UI_OBJECT_ASSET_ROW_HEIGHT, uiAssetsObjectScroll);
    }

    bool hasSelection = (uiAssetsObjectSelected >= 0 && uiAssetsObjectSelected < count);
    bool canMoveUp = hasSelection && uiAssetsObjectSelected > 0;
    bool canMoveDown = hasSelection && uiAssetsObjectSelected < count - 1;

    DrawRectangle (
        (int)(panel.x + 24.0f), (int)(panel.y + panel.height - 24.0f - UI_MANAGER_FOOTER), (int)(panel.width - 48.0f),
        1, (Color){58, 63, 76, 255});
    DrawSmallButton (CutilsFont, ObjectAssetUseRect (panel), "使用", false, hasSelection);
    DrawSmallButton (CutilsFont, ObjectAssetUpRect (panel), "上移", false, canMoveUp);
    DrawSmallButton (CutilsFont, ObjectAssetDownRect (panel), "下移", false, canMoveDown);
    DrawSmallButton (CutilsFont, ObjectAssetRemoveRect (panel), "移除", true, hasSelection);
    DrawSmallButton (CutilsFont, ObjectAssetAddRect (panel), "添加", false, true);
    DrawSmallButton (CutilsFont, ObjectAssetDoneRect (panel), "确定", false, true);
}

static void DrawConnectField (Rectangle input, const char* text, const char* placeholder, bool active) {
    DrawRectangleRec (input, (Color){20, 22, 28, 255});
    DrawRectangleLinesEx (input, 1, active ? (Color){70, 130, 200, 255} : (Color){70, 76, 92, 255});

    CutilsBeginScissor ((int)input.x + 1, (int)input.y + 1, (int)input.width - 2, (int)input.height - 2);
    if (text[0] != '\0') {
        DrawUiText (
            CutilsFont, text, (Vector2){input.x + 8.0f, input.y + 5.0f}, UI_FONT_SIZE, 0, (Color){235, 238, 245, 255});
    } else {
        DrawUiText (
            CutilsFont, placeholder, (Vector2){input.x + 8.0f, input.y + 5.0f}, UI_FONT_SIZE, 0,
            (Color){112, 120, 136, 255});
    }

    if (active) {
        float textWidth = MeasureTextEx (CutilsFont, text, UI_FONT_SIZE, 0).x;
        float caretX = input.x + 8.0f + textWidth;
        if (caretX < input.x + input.width - 2.0f)
            DrawRectangle ((int)caretX, (int)(input.y + 5.0f), 1, 24, (Color){235, 238, 245, 255});
    }
    CutilsEndScissor ();
}

static void DrawTextInputDialog (const char* title, const char* text) {
    Rectangle panel = RenamePanel ();
    Rectangle input = RenameInputRect (panel);
    Rectangle ok = RenameOkRect (panel);
    Rectangle cancel = RenameCancelRect (panel);

    DrawRectangle (0, 0, GetScreenWidth (), GetScreenHeight (), Fade (BLACK, 0.45f));
    DrawRectangleRec (panel, (Color){30, 33, 41, 255});
    DrawRectangleLinesEx (panel, 1, (Color){80, 86, 104, 255});
    DrawUiText (
        CutilsFont, title, (Vector2){panel.x + 24.0f, panel.y + 18.0f}, UI_FONT_SIZE, 0, (Color){240, 242, 248, 255});

    DrawRectangleRec (input, (Color){20, 22, 28, 255});
    DrawRectangleLinesEx (input, 1, (Color){70, 76, 92, 255});

    CutilsBeginScissor ((int)input.x + 1, (int)input.y + 1, (int)input.width - 2, (int)input.height - 2);
    DrawUiText (
        CutilsFont, text, (Vector2){input.x + 8.0f, input.y + 5.0f}, UI_FONT_SIZE, 0, (Color){235, 238, 245, 255});

    float textWidth = MeasureTextEx (CutilsFont, text, UI_FONT_SIZE, 0).x;
    float caretX = input.x + 8.0f + textWidth;
    if (caretX < input.x + input.width - 2.0f)
        DrawRectangle ((int)caretX, (int)(input.y + 5.0f), 1, 24, (Color){235, 238, 245, 255});
    CutilsEndScissor ();

    DrawSmallButton (CutilsFont, ok, "确定", false, true);
    DrawSmallButton (CutilsFont, cancel, "取消", false, true);
}

static void DrawRename () {
    if (!uiRenameOpen) return;
    DrawTextInputDialog (uiRenameIsObject ? "重命名物件" : "重命名素材", uiRenameText);
}

static void DrawLayer () {
    if (!uiLayerOpen) return;
    DrawTextInputDialog ("修改层数", uiLayerText);
}

static void DrawConnect () {
    if (!uiConnectOpen) return;

    Rectangle panel = ConnectPanel ();
    Rectangle user = ConnectUserRect (panel);
    Rectangle host = ConnectHostRect (panel);
    Rectangle port = ConnectPortRect (panel);
    Rectangle ok = RenameOkRect (panel);
    Rectangle cancel = RenameCancelRect (panel);

    DrawRectangle (0, 0, GetScreenWidth (), GetScreenHeight (), Fade (BLACK, 0.55f));
    DrawRectangleRec (panel, (Color){26, 29, 36, 255});
    DrawRectangleLinesEx (panel, 1, (Color){70, 76, 92, 255});
    DrawUiText (
        CutilsFont, "连接服务器", (Vector2){panel.x + 24.0f, panel.y + 16.0f}, UI_FONT_SIZE, 0,
        (Color){235, 238, 245, 255});
    DrawRectangle (
        (int)(panel.x + 24.0f), (int)(panel.y + 43.0f), (int)(panel.width - 48.0f), 1, (Color){58, 63, 76, 255});

    DrawUiText (
        CutilsFont, "用户名", (Vector2){panel.x + 24.0f, panel.y + 54.0f}, UI_FONT_SIZE, 0,
        (Color){190, 196, 208, 255});
    DrawConnectField (user, uiConnectUser, "user", uiConnectField == 0);

    DrawUiText (
        CutilsFont, "IP", (Vector2){panel.x + 24.0f, panel.y + 120.0f}, UI_FONT_SIZE, 0, (Color){190, 196, 208, 255});
    DrawConnectField (host, uiConnectHost, "127.0.0.1", uiConnectField == 1);

    DrawUiText (
        CutilsFont, "端口", (Vector2){panel.x + 24.0f, panel.y + 186.0f}, UI_FONT_SIZE, 0, (Color){190, 196, 208, 255});
    DrawConnectField (port, uiConnectPort, "7777", uiConnectField == 2);

    DrawSmallButton (CutilsFont, ok, "连接", false, true);
    DrawSmallButton (CutilsFont, cancel, "取消", false, true);
}

static void DrawObjectContext () {
    if (!uiObjectContextOpen) return;

    Vector2 mouse = GetMousePosition ();
    Rectangle menu = ObjectContextRect ();
    Object* object = uiContextObject;
    int menuCount = MenuCount (object != NULL ? object->id : 0);
    static const char* labels[] = {"查看", "脚本", "删除"};
    static const char* viewLabels[] = {"回到原点", "缩放", "背景", "脚本"};

    DrawRectangle ((int)menu.x + 3, (int)menu.y + 4, (int)menu.width, (int)menu.height, Fade (BLACK, 0.28f));
    DrawRectangleRec (menu, (Color){28, 31, 39, 255});

    for (int i = 0; i < menuCount; i++) {
        Rectangle item = {menu.x, menu.y + i * UI_ITEM_HEIGHT, menu.width, UI_ITEM_HEIGHT};
        if (CheckCollisionPointRec (mouse, item)) DrawRectangleRec (item, (Color){45, 49, 61, 255});
        const char* name = MenuName (object != NULL ? object->id : 0, i);
        const char* nameText = name != NULL ? name : "";
        DrawUiText (
            CutilsFont, nameText, (Vector2){item.x + 12.0f, MenuTextY (CutilsFont, nameText, item)}, UI_FONT_SIZE, 0,
            (Color){225, 228, 236, 255});
    }

    float separatorY = menu.y + menuCount * UI_ITEM_HEIGHT;
    if (menuCount > 0)
        DrawRectangle ((int)menu.x, (int)(separatorY + 3.0f), (int)menu.width, 1, (Color){60, 65, 80, 255});
    float actionY = separatorY + (menuCount > 0 ? UI_CONTEXT_SEPARATOR_HEIGHT : 0.0f);

    if (object != NULL) {
        for (int i = 0; i < 3; i++) {
            Rectangle item = {menu.x, actionY + i * UI_ITEM_HEIGHT, menu.width, UI_ITEM_HEIGHT};
            if (CheckCollisionPointRec (mouse, item)) DrawRectangleRec (item, (Color){45, 49, 61, 255});
            Color color = (i == 2) ? (Color){240, 120, 120, 255} : (Color){225, 228, 236, 255};
            DrawUiText (
                CutilsFont, labels[i], (Vector2){item.x + 12.0f, MenuTextY (CutilsFont, labels[i], item)}, UI_FONT_SIZE,
                0, color);
        }
    } else {
        for (int i = 0; i < 4; i++) {
            Rectangle item = {menu.x, actionY + i * UI_ITEM_HEIGHT, menu.width, UI_ITEM_HEIGHT};
            if (CheckCollisionPointRec (mouse, item)) DrawRectangleRec (item, (Color){45, 49, 61, 255});
            DrawUiText (
                CutilsFont, viewLabels[i], (Vector2){item.x + 12.0f, MenuTextY (CutilsFont, viewLabels[i], item)},
                UI_FONT_SIZE, 0, (Color){225, 228, 236, 255});
            if (i == 1 || i == 2)
                DrawUiText (
                    CutilsFont, ">", (Vector2){item.x + item.width - 20.0f, MenuTextY (CutilsFont, ">", item)},
                    UI_FONT_SIZE, 0, (Color){150, 158, 172, 255});
        }

        if (uiSubmenuOpen >= 0) {
            Rectangle submenu = ViewContextSubmenuRect ();
            const char** items = (uiSubmenuOpen == VIEW_SUBMENU_ZOOM) ? viewZoomLabels : viewBackgroundLabels;
            int count = (int)(submenu.height / UI_ITEM_HEIGHT);
            DrawRectangleRec (submenu, (Color){28, 31, 39, 255});
            for (int i = 0; i < count; i++) {
                Rectangle item = {submenu.x, submenu.y + i * UI_ITEM_HEIGHT, submenu.width, UI_ITEM_HEIGHT};
                if (CheckCollisionPointRec (mouse, item)) DrawRectangleRec (item, (Color){45, 49, 61, 255});
                DrawUiText (
                    CutilsFont, items[i], (Vector2){item.x + 12.0f, MenuTextY (CutilsFont, items[i], item)},
                    UI_FONT_SIZE, 0, (Color){225, 228, 236, 255});
            }
            DrawRectangleLinesEx (submenu, 1, (Color){60, 65, 80, 255});
        }
    }

    DrawRectangleLinesEx (menu, 1, (Color){60, 65, 80, 255});
}

static void DrawMessage () {
    float height = 44.0f;
    float gap = 6.0f;
    int slot = 0;

    for (int i = uiMessageCount - 1; i >= 0; i--) {
        float time = uiMessageTimes[i];
        if (time <= 0.0f) continue;

        float textWidth = MeasureTextEx (CutilsFont, uiMessages[i], UI_FONT_SIZE, 0).x;
        float width = textWidth + 48.0f;
        float elapsed = 3.0f - time;
        float in = Clamp (elapsed / 0.25f, 0.0f, 1.0f);
        float out = Clamp (time / 0.35f, 0.0f, 1.0f);
        float k = fminf (in, out);
        k = 1.0f - (1.0f - k) * (1.0f - k) * (1.0f - k);

        float visibleY = UI_BAR_HEIGHT + 8.0f + slot * (height + gap);
        float hiddenY = -height - 4.0f + slot * (height + gap);
        float y = hiddenY + (visibleY - hiddenY) * k;
        float x = (GetScreenWidth () - width) / 2.0f;

        Color background = {30, 96, 170, 255};
        if (uiMessageKinds[i] == UI_MESSAGE_ERROR)
            background = (Color){180, 52, 58, 255};
        else if (uiMessageKinds[i] == UI_MESSAGE_SCRIPT)
            background = (Color){86, 91, 99, 255};

        DrawRectangleRounded ((Rectangle){x + 2.0f, y + 4.0f, width, height}, 0.5f, 8, Fade (BLACK, 0.25f * k));
        DrawRectangleRounded ((Rectangle){x, y, width, height}, 0.5f, 8, Fade (background, 0.95f * k));
        DrawUiText (CutilsFont, uiMessages[i], (Vector2){x + 24.0f, y + 10.0f}, UI_FONT_SIZE, 0.0f, Fade (WHITE, k));
        slot++;
    }
}

static void BuildScriptLine (const TextBuffer* text, size_t start, size_t end, TextBuffer* display) {
    for (size_t i = start; i < end; i++) {
        if (text->data[i] == '\t')
            TextBufferAppend (display, "    ");
        else
            TextBufferAppendChar (display, text->data[i]);
    }
}

static void DrawScript () {
    if (!uiScriptOpen) return;

    Rectangle panel = ScriptPanel ();
    Rectangle code = ScriptCodeRect (panel);
    DrawDialogBase (panel, "脚本");
    DrawRectangleRec (code, (Color){20, 22, 28, 255});
    DrawRectangleLinesEx (code, 1, (Color){70, 76, 92, 255});
    CutilsBeginScissor ((int)code.x + 1, (int)code.y + 1, (int)code.width - 2, (int)code.height - 2);

    int total = ScriptLineCount (&uiScriptText);
    int visible = ScriptVisibleLines ();
    int start = (int)uiScriptScroll;
    if (start < 0) start = 0;
    if (start > total - 1) start = total - 1;

    size_t pos = 0;
    int line = 0;
    while (line < start && pos < uiScriptText.length) {
        if (uiScriptText.data[pos] == '\n') line++;
        pos++;
    }

    for (int i = start; i < total && i < start + visible; i++) {
        size_t end = pos;
        while (end < uiScriptText.length && uiScriptText.data[end] != '\n') end++;

        TextBuffer display = {0};
        BuildScriptLine (&uiScriptText, pos, end, &display);
        DrawUiText (
            CutilsMonoFont, display.data != NULL ? display.data : "",
            (Vector2){code.x + 8.0f, code.y + 6.0f + (i - start) * UI_CODE_LINE_HEIGHT}, UI_CODE_FONT_SIZE, 0.0f,
            (Color){225, 228, 236, 255});
        TextBufferFree (&display);

        if (end < uiScriptText.length)
            pos = end + 1;
        else
            pos = uiScriptText.length;
    }

    int cursorLine = ScriptLineOfOffset (&uiScriptText, uiScriptCursor);
    if (cursorLine >= start && cursorLine < start + visible) {
        size_t cursorStart = ScriptLineStart (&uiScriptText, cursorLine);
        size_t cursorEnd = ScriptLineEnd (&uiScriptText, cursorStart);
        size_t prefixEnd = uiScriptCursor;
        if (prefixEnd < cursorStart) prefixEnd = cursorStart;
        if (prefixEnd > cursorEnd) prefixEnd = cursorEnd;

        TextBuffer display = {0};
        BuildScriptLine (&uiScriptText, cursorStart, prefixEnd, &display);
        if (fmod (GetTime (), 1.0) < 0.5) {
            float x =
                code.x + 8.0f +
                MeasureTextEx (CutilsMonoFont, display.data != NULL ? display.data : "", UI_CODE_FONT_SIZE, 0.0f).x;
            float y = code.y + 6.0f + (cursorLine - start) * UI_CODE_LINE_HEIGHT;
            DrawRectangle ((int)x, (int)y, 1, UI_CODE_FONT_SIZE, (Color){225, 228, 236, 255});
        }
        TextBufferFree (&display);
    }

    if (uiScriptText.length == 0) {
        DrawUiText (
            CutilsMonoFont, "在此输入脚本", (Vector2){code.x + 8.0f, code.y + 6.0f}, UI_CODE_FONT_SIZE, 0.0f,
            (Color){110, 116, 132, 255});
    }

    CutilsEndScissor ();
    DrawListScrollbar (code, ScriptLineCount (&uiScriptText), UI_CODE_LINE_HEIGHT, uiScriptScroll);

    Rectangle self = ScriptSelfRect (panel);
    DrawRectangleRec (self, (Color){20, 22, 28, 255});
    DrawRectangleLinesEx (self, 1, uiScriptSelfFocused ? (Color){70, 130, 200, 255} : (Color){70, 76, 92, 255});
    CutilsBeginScissor ((int)self.x + 1, (int)self.y + 1, (int)self.width - 2, (int)self.height - 2);
    if (uiScriptSelf[0] != '\0') {
        DrawUiText (
            CutilsMonoFont, uiScriptSelf, (Vector2){self.x + 8.0f, self.y + 9.0f}, UI_CODE_FONT_SIZE, 0.0f,
            (Color){235, 238, 245, 255});
    } else {
        DrawUiText (
            CutilsMonoFont, "self 表达式", (Vector2){self.x + 8.0f, self.y + 9.0f}, UI_CODE_FONT_SIZE, 0.0f,
            (Color){112, 120, 136, 255});
    }
    if (uiScriptSelfFocused && fmod (GetTime (), 1.0) < 0.5) {
        float width = MeasureTextEx (CutilsMonoFont, uiScriptSelf, UI_CODE_FONT_SIZE, 0.0f).x;
        DrawRectangle ((int)(self.x + 8.0f + width), (int)(self.y + 8.0f), 1, 18, (Color){235, 238, 245, 255});
    }
    CutilsEndScissor ();

    DrawSmallButton (CutilsFont, ScriptSubmitRect (panel), "提交", false, true);
    DrawSmallButton (CutilsFont, ScriptCancelRect (panel), "取消", false, true);
    DrawUiText (
        CutilsFont, "Ctrl+Enter 提交", (Vector2){panel.x + 24.0f, panel.y + panel.height - 48.0f}, 20.0f, 0.0f,
        (Color){150, 158, 172, 255});
}

static void DrawUserMenuButtons () {
    u64 userId = NetClientUserId ();
    if (userId == 0) return;
    int count = MenuCount (userId);
    for (int i = 0; i < count; i++) {
        const char* name = MenuName (userId, i);
        DrawSmallButton (CutilsFont, UserMenuButtonRect (i), name != NULL ? name : "", false, true);
    }
}

static void DrawNetCursors () {
    u64 selfId = NetClientUserId ();
    IdGroup ids = UserIds ();
    for (int i = 0; i < ids.count; i++) {
        User *user = EntityFind (ids.ids[i]);
        if (user == NULL || !user->hasCursor || user->id == selfId) continue;

        Vector2 screen = GetWorldToScreen2D ((Vector2){user->x, user->y}, viewCam);
        Color color = UserColor (user);
        DrawCircleV (screen, 4.0f, color);
        DrawUiText (CutilsFont, user->name,
                    (Vector2){screen.x + 7.0f, screen.y + 4.0f},
                    20.0f, 0.0f, color);
    }
    IdGroupUnload (&ids);
}

void UiDraw () {
    DrawNetCursors ();

    DrawMenuBar ();
    DrawUserMenuButtons ();
    DrawManager ();
    DrawObjectManager ();
    DrawObjectAssets ();
    DrawPicker ();
    DrawRename ();
    DrawLayer ();
    DrawConnect ();
    DrawObjectContext ();
    DrawScript ();
    DrawMessage ();
}
