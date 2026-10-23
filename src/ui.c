#define _POSIX_C_SOURCE 200809L

#include "ui.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "raymath.h"
#include "cutils.h"

#include "assets.h"
#include "menu.h"
#include "net.h"
#include "user.h"
#include "entity.h"

#define UI_ITEM_HEIGHT 32
#define UI_MENU_WIDTH 150
#define UI_SUBMENU_WIDTH 120
#define UI_FONT_SIZE 24
#define UI_CODE_FONT_SIZE 22
#define UI_CODE_LINE_HEIGHT 26.0f
#define UI_CONTEXT_SEPARATOR_HEIGHT 9.0f

static void DrawSmallButton (Font font, Rectangle rect, const char *text, bool danger, bool enabled);
static void ScriptClose (Ui *ui);
static void OpenScript (Ui *ui);
#define UI_PICKER_HEADER 48
#define UI_PICKER_FOOTER 56
#define UI_ASSET_ROW_HEIGHT 64
#define UI_MANAGER_HEADER 48
#define UI_MANAGER_FOOTER 62
#define UI_MANAGER_ROW_HEIGHT 80
#define UI_BUTTON_WIDTH 96.0f
#define UI_BUTTON_HEIGHT 38.0f
#define UI_OBJECT_ASSET_ROW_HEIGHT 64

static const char *menuNames[UI_MENU_COUNT] = {"文件", "素材", "物件", "脚本", "联机"};
static const char *menuItems[UI_MENU_COUNT][3] = {
    {"导入", "导入文件夹", "导出"},
    {NULL},
    {NULL},
    {NULL},
    {"连接", "开放"},
};
static const int menuItemCounts[UI_MENU_COUNT] = {3, 0, 0, 0, 2};

enum {
    VIEW_SUBMENU_NONE = -1,
    VIEW_SUBMENU_ZOOM = 1,
    VIEW_SUBMENU_BACKGROUND = 2
};

static const float viewZoomValues[] = {1.0f, 2.0f, 0.5f};
static const char *viewZoomLabels[] = {"100%", "200%", "50%"};
static const char *viewBackgroundLabels[] = {"纯白", "格点", "格纹", "棋盘"};

static Rectangle ViewContextSubmenuRect (const Ui *ui) {
    int count = (ui->submenuOpen == VIEW_SUBMENU_ZOOM) ? 3 : 4;
    int menus = MenuCount (0);
    float actionsY = ui->contextPos.y + menus * UI_ITEM_HEIGHT +
                     (menus > 0 ? UI_CONTEXT_SEPARATOR_HEIGHT : 0.0f);
    float x = ui->contextPos.x + UI_SUBMENU_WIDTH;
    if (x + UI_SUBMENU_WIDTH > GetScreenWidth ())
        x = ui->contextPos.x - UI_SUBMENU_WIDTH;
    float offset = ui->submenuOpen == VIEW_SUBMENU_ZOOM ? UI_ITEM_HEIGHT
                                                        : 2 * UI_ITEM_HEIGHT;
    float height = count * UI_ITEM_HEIGHT;
    float y = actionsY + offset;
    if (y + height > GetScreenHeight ()) y = GetScreenHeight () - height;
    if (y < 0.0f) y = 0.0f;
    return (Rectangle){x, y, UI_SUBMENU_WIDTH, height};
}

static Rectangle ObjectContextRect (const Ui *ui) {
    Object *object = ui->contextObject;
    int menus = MenuCount (object != NULL ? object->id : 0);
    int actions = (object != NULL ? 3 : 4) * UI_ITEM_HEIGHT;
    float separator = menus > 0 ? UI_CONTEXT_SEPARATOR_HEIGHT : 0.0f;
    return (Rectangle){ui->contextPos.x, ui->contextPos.y, UI_SUBMENU_WIDTH,
                       menus * UI_ITEM_HEIGHT + separator + actions};
}

// 丢弃等待处理的拖入文件
static bool DiscardDroppedFiles (void) {
    if (!IsFileDropped ()) return false;

    FilePathList files = LoadDroppedFiles ();
    UnloadDroppedFiles (files);
    return true;
}

static bool DialogSucceeded (Ui *ui, CutilsDialogResult result,
                             const char *failure) {
    if (result == CUTILS_DIALOG_OK) return true;
    if (result == CUTILS_DIALOG_CANCELLED ||
        result == CUTILS_DIALOG_PENDING) return false;
    if (result == CUTILS_DIALOG_TOO_LONG)
        UiShowError (ui, "路径过长");
    else
        UiShowError (ui, failure);
    return false;
}

static bool OpenImageDialog (Ui *ui, char *path, int pathSize) {
    path[0] = '\0';
    CutilsDialogResult result = CutilsOpenImage (path, (size_t)pathSize);
    bool ok = DialogSucceeded (ui, result, "打开文件对话框失败");
    if (DiscardDroppedFiles ()) UiShowError (ui, "弹窗打开时不能拖入文件");
    return ok;
}

static bool OpenImportDialog (Ui *ui, char *path, int pathSize) {
    path[0] = '\0';
    CutilsDialogResult result = CutilsOpenImport (path, (size_t)pathSize);
    return DialogSucceeded (ui, result, "打开文件对话框失败");
}

static bool OpenFolderDialog (Ui *ui, char *path, int pathSize) {
    path[0] = '\0';
    CutilsDialogResult result = CutilsOpenFolder (path, (size_t)pathSize);
    return DialogSucceeded (ui, result, "打开文件夹对话框失败");
}

static bool OpenExportDialog (Ui *ui, char *path, int pathSize) {
    path[0] = '\0';
    CutilsDialogResult result = CutilsSaveDsk (path, (size_t)pathSize);
    return DialogSucceeded (ui, result, "打开文件对话框失败");
}

static void LayoutMenus (Ui *ui) {
    float x = 8.0f;
    for (int i = 0; i < UI_MENU_COUNT; i++) {
        if ((i == 0 && !ui->showFileMenu) ||
            (i == 4 && !ui->showConnectMenu)) {
            ui->menuW[i] = 0.0f;
            ui->menuX[i] = x;
            continue;
        }
        ui->menuW[i] = MeasureTextEx (ui->font, menuNames[i], UI_FONT_SIZE, 0).x + 32.0f;
        ui->menuX[i] = x;
        x += ui->menuW[i];
    }
}

static int *BuildFontCodepoints (int *count) {
    static const int ranges[][2] = {
        {0x0020, 0x007E},
        {0x00A0, 0x024F},
        {0x2000, 0x206F},
        {0x3000, 0x303F},
        {0x4E00, 0x9FFF},
        {0xFF00, 0xFFEF}
    };
    int capacity = 0;
    for (size_t i = 0; i < sizeof (ranges) / sizeof (ranges[0]); i++)
        capacity += ranges[i][1] - ranges[i][0] + 1;

    int *codepoints = malloc ((size_t)capacity * sizeof (int));
    if (codepoints == NULL) {
        *count = 0;
        return NULL;
    }

    int total = 0;
    for (size_t i = 0; i < sizeof (ranges) / sizeof (ranges[0]); i++) {
        for (int cp = ranges[i][0]; cp <= ranges[i][1]; cp++)
            codepoints[total++] = cp;
    }
    *count = total;
    return codepoints;
}

static Font LoadAppFont (const char *name, int size) {
    char path[1024];
#if defined(__EMSCRIPTEN__)
    snprintf (path, sizeof (path), "/assets/%s", name);
#else
    snprintf (path, sizeof (path), "%sassets/%s", GetApplicationDirectory (), name);
#endif

    int count = 0;
    int *codepoints = BuildFontCodepoints (&count);
    Font font = LoadFontEx (path, size, codepoints, count);
    free (codepoints);
    return font;
}

static bool IsLoadedFont (Font font) {
    return font.texture.id != 0 && font.texture.id != GetFontDefault ().texture.id;
}

static void LoadFonts (Ui *ui) {
    Font sans = LoadAppFont ("WenQuanYiMicroHei.ttf", UI_FONT_SIZE);
    if (!IsLoadedFont (sans))
        sans = LoadAppFont ("unifont.otf", UI_FONT_SIZE);

    if (IsLoadedFont (sans)) {
        if (ui->customFont && ui->font.texture.id != sans.texture.id) UnloadFont (ui->font);
        ui->font = sans;
        ui->customFont = true;
    } else if (!ui->customFont) {
        ui->font = GetFontDefault ();
    }

    Font mono = LoadAppFont ("WenQuanYiMicroHeiMono.ttf", UI_CODE_FONT_SIZE);
    if (IsLoadedFont (mono)) {
        if (ui->customMonoFont && ui->monoFont.texture.id != mono.texture.id)
            UnloadFont (ui->monoFont);
        ui->monoFont = mono;
        ui->customMonoFont = true;
    } else {
        if (ui->customMonoFont && ui->monoFont.texture.id != ui->font.texture.id)
            UnloadFont (ui->monoFont);
        ui->monoFont = ui->font;
        ui->customMonoFont = false;
    }

    SetTextureFilter (ui->font.texture, TEXTURE_FILTER_POINT);
    SetTextureFilter (ui->monoFont.texture, TEXTURE_FILTER_POINT);
    LayoutMenus (ui);
}

void UiSetConnectMenuVisible (Ui *ui, bool visible) {
    if (ui->showConnectMenu == visible) return;
    ui->showConnectMenu = visible;
    if (!visible && ui->menuOpen == 4) {
        ui->menuOpen = -1;
        ui->submenuOpen = -1;
    }
    LayoutMenus (ui);
}

void UiSetFileMenuVisible (Ui *ui, bool visible) {
    if (ui->showFileMenu == visible) return;
    ui->showFileMenu = visible;
    if (!visible && ui->menuOpen == 0) {
        ui->menuOpen = -1;
        ui->submenuOpen = -1;
    }
    LayoutMenus (ui);
}

void UiOpenAssetManager (Ui *ui) {
    if (!ui->managerOpen) {
        ui->managerOpen = true;
        ui->managerScroll = 0.0f;
        ui->selectedAsset = NULL;
    }
}

static Ui *activeUi;

void UiInit (Ui *ui) {
    activeUi = ui;
    ui->showConnectMenu = true;
    ui->showFileMenu = true;
    ui->menuOpen = -1;
    ui->pickerOpen = false;
    ui->pickerScroll = 0.0f;
    ui->managerOpen = false;
    ui->managerScroll = 0.0f;
    ui->selectedAsset = NULL;
    ui->objectManagerOpen = false;
    ui->objectScroll = 0.0f;
    ui->selectedObject = NULL;
    ui->lastObjectClick = NULL;
    ui->lastObjectClickTime = 0.0;
    ui->objectContextOpen = false;
    ui->contextObject = NULL;
    ui->contextPos = (Vector2){0};
    ui->layerOpen = false;
    ui->layerObject = NULL;
    ui->layerText[0] = '\0';
    ui->objectAssetsOpen = false;
    ui->assetsObject = NULL;
    ui->assetsObjectSelected = -1;
    ui->assetsLastClickIndex = -1;
    ui->assetsLastClickTime = 0.0;
    ui->assetsObjectScroll = 0.0f;
    ui->pickerObject = NULL;
    ui->renameOpen = false;
    ui->renameIsObject = false;
    ui->renameAsset = NULL;
    ui->renameObject = NULL;
    ui->renameText[0] = '\0';
    ui->connectOpen = false;
    ui->connectField = 0;
    ui->scriptOpen = false;
    ui->scriptText = (TextBuffer){0};
    ui->scriptSelf[0] = '\0';
    ui->scriptSelfFocused = false;
    ui->scriptCursor = 0;
    ui->scriptPreferredColumn = -1;
    ui->scriptScroll = 0.0f;
    ui->connectUser[0] = '\0';
    ui->connectHost[0] = '\0';
    ui->connectPort[0] = '\0';
    ui->messageCount = 0;
    ui->uiPress = false;
    ui->submenuOpen = -1;

    ui->font = GetFontDefault ();
    ui->monoFont = GetFontDefault ();
    ui->customFont = false;
    ui->customMonoFont = false;
    LoadFonts (ui);
}

void UiUnload (Ui *ui) {
    if (ui->customMonoFont && ui->monoFont.texture.id != ui->font.texture.id)
        UnloadFont (ui->monoFont);
    if (ui->customFont) UnloadFont (ui->font);
    TextBufferFree (&ui->scriptText);
    if (activeUi == ui) activeUi = NULL;
}

static void ShowMessageKind (Ui *ui, const char *message, UiMessageKind kind) {
    if (ui->messageCount == UI_MAX_MESSAGES) {
        memmove (ui->messages, ui->messages + 1,
                 (UI_MAX_MESSAGES - 1) * sizeof (ui->messages[0]));
        memmove (ui->messageTimes, ui->messageTimes + 1,
                 (UI_MAX_MESSAGES - 1) * sizeof (ui->messageTimes[0]));
        memmove (ui->messageKinds, ui->messageKinds + 1,
                 (UI_MAX_MESSAGES - 1) * sizeof (ui->messageKinds[0]));
        ui->messageCount--;
    }

    int index = ui->messageCount++;
    snprintf (ui->messages[index], sizeof (ui->messages[index]), "%s", message);
    ui->messageTimes[index] = 3.0f;
    ui->messageKinds[index] = kind;
}

void UiShowMessage (Ui *ui, const char *message) {
    ShowMessageKind (ui, message, UI_MESSAGE_INFO);
}

void UiShowError (Ui *ui, const char *message) {
    ShowMessageKind (ui, message, UI_MESSAGE_ERROR);
}

void UiShowMessageText (const char *message) {
    if (activeUi != NULL) ShowMessageKind (activeUi, message, UI_MESSAGE_INFO);
}

void UiShowMessageTextKind (const char *message, UiMessageKind kind) {
    if (activeUi != NULL) ShowMessageKind (activeUi, message, kind);
}

bool UiIsModal (const Ui *ui) {
    return ui->pickerOpen || ui->managerOpen || ui->renameOpen || ui->objectManagerOpen ||
           ui->objectAssetsOpen || ui->objectContextOpen || ui->layerOpen || ui->connectOpen ||
           ui->scriptOpen;
}

void UiAssetDeleted (Ui *ui) {
    ui->selectedAsset = NULL;
    if (ui->renameOpen && !ui->renameIsObject) {
        ui->renameOpen = false;
        ui->renameAsset = NULL;
        ui->renameText[0] = '\0';
    }
}

void UiObjectDeleted (Ui *ui) {
    ui->selectedObject = NULL;
    ui->objectScroll = 0.0f;
    ui->lastObjectClick = NULL;
    ui->lastObjectClickTime = 0.0;
    ui->objectContextOpen = false;
    ui->contextObject = NULL;
    ui->layerOpen = false;
    ui->layerObject = NULL;
    ui->layerText[0] = '\0';
    ui->objectAssetsOpen = false;
    ui->assetsObject = NULL;
    ui->assetsObjectSelected = -1;
    ui->assetsLastClickIndex = -1;
    ui->assetsLastClickTime = 0.0;
    ui->assetsObjectScroll = 0.0f;
    ui->pickerOpen = false;
    ui->pickerObject = NULL;
    if (ui->renameOpen && ui->renameIsObject) {
        ui->renameOpen = false;
        ui->renameObject = NULL;
        ui->renameText[0] = '\0';
    }
}

void UiResetSceneState (Ui *ui) {
    ScriptClose (ui);
    ui->menuOpen = -1;
    ui->submenuOpen = VIEW_SUBMENU_NONE;
    ui->uiPress = false;

    ui->pickerOpen = false;
    ui->pickerScroll = 0.0f;
    ui->pickerObject = NULL;

    ui->managerOpen = false;
    ui->managerScroll = 0.0f;
    ui->selectedAsset = NULL;

    ui->objectManagerOpen = false;
    ui->objectScroll = 0.0f;
    ui->selectedObject = NULL;
    ui->lastObjectClick = NULL;
    ui->lastObjectClickTime = 0.0;

    ui->objectContextOpen = false;
    ui->contextObject = NULL;

    ui->layerOpen = false;
    ui->layerObject = NULL;
    ui->layerText[0] = '\0';

    ui->objectAssetsOpen = false;
    ui->assetsObject = NULL;
    ui->assetsObjectSelected = -1;
    ui->assetsLastClickIndex = -1;
    ui->assetsLastClickTime = 0.0;
    ui->assetsObjectScroll = 0.0f;

    ui->renameOpen = false;
    ui->renameAsset = NULL;
    ui->renameObject = NULL;
    ui->renameText[0] = '\0';

    ui->connectOpen = false;
    ui->connectField = 0;
    ui->scriptOpen = false;
    ui->scriptText = (TextBuffer){0};
    ui->scriptSelf[0] = '\0';
    ui->scriptSelfFocused = false;
    ui->scriptCursor = 0;
    ui->scriptPreferredColumn = -1;
    ui->scriptScroll = 0.0f;
}

static Rectangle PickerPanel (void) {
    float w = fminf (GetScreenWidth () - 80.0f, 640.0f);
    float h = fminf (GetScreenHeight () - 100.0f, 460.0f);
    return (Rectangle){(GetScreenWidth () - w) / 2.0f, (GetScreenHeight () - h) / 2.0f, w, h};
}

static Rectangle PickerListRect (Rectangle panel) {
    return (Rectangle){panel.x + 1, panel.y + UI_PICKER_HEADER, panel.width - 2,
                       panel.height - UI_PICKER_HEADER - UI_PICKER_FOOTER};
}

static Rectangle PickerCancelRect (Rectangle panel) {
    return (Rectangle){panel.x + panel.width - 24.0f - UI_BUTTON_WIDTH,
                       panel.y + panel.height - 24.0f - UI_BUTTON_HEIGHT,
                       UI_BUTTON_WIDTH, UI_BUTTON_HEIGHT};
}

static Rectangle ManagerPanel (void) {
    float w = fminf (GetScreenWidth () - 80.0f, 780.0f);
    float h = fminf (GetScreenHeight () - 100.0f, 540.0f);
    return (Rectangle){(GetScreenWidth () - w) / 2.0f, (GetScreenHeight () - h) / 2.0f, w, h};
}

static Rectangle ManagerListRect (Rectangle panel) {
    return (Rectangle){panel.x + 1, panel.y + UI_MANAGER_HEADER, panel.width - 2,
                       panel.height - UI_MANAGER_HEADER - UI_MANAGER_FOOTER};
}

static Rectangle ManagerExitRect (Rectangle panel) {
    return (Rectangle){panel.x + panel.width - 24.0f - UI_BUTTON_WIDTH,
                       panel.y + panel.height - 24.0f - UI_BUTTON_HEIGHT,
                       UI_BUTTON_WIDTH, UI_BUTTON_HEIGHT};
}

static Rectangle ManagerRenameRect (Rectangle panel) {
    return (Rectangle){panel.x + 24.0f, panel.y + panel.height - 24.0f - UI_BUTTON_HEIGHT,
                       UI_BUTTON_WIDTH, UI_BUTTON_HEIGHT};
}

static Rectangle ManagerReplaceRect (Rectangle panel) {
    Rectangle renameBtn = ManagerRenameRect (panel);
    return (Rectangle){renameBtn.x + renameBtn.width + 8.0f, renameBtn.y,
                       UI_BUTTON_WIDTH, UI_BUTTON_HEIGHT};
}

static Rectangle ManagerDeleteRect (Rectangle panel) {
    Rectangle replaceBtn = ManagerReplaceRect (panel);
    return (Rectangle){replaceBtn.x + replaceBtn.width + 8.0f, replaceBtn.y,
                       UI_BUTTON_WIDTH, UI_BUTTON_HEIGHT};
}

static Rectangle ManagerImportRect (Rectangle panel) {
    Rectangle doneBtn = ManagerExitRect (panel);
    return (Rectangle){doneBtn.x - 8.0f - UI_BUTTON_WIDTH, doneBtn.y,
                       UI_BUTTON_WIDTH, UI_BUTTON_HEIGHT};
}

static Rectangle ObjectRenameRect (Rectangle panel) {
    return (Rectangle){panel.x + 24.0f, panel.y + panel.height - 24.0f - UI_BUTTON_HEIGHT,
                       UI_BUTTON_WIDTH, UI_BUTTON_HEIGHT};
}

static Rectangle ObjectManageRect (Rectangle panel) {
    Rectangle renameBtn = ObjectRenameRect (panel);
    return (Rectangle){renameBtn.x + renameBtn.width + 8.0f, renameBtn.y,
                       UI_BUTTON_WIDTH, UI_BUTTON_HEIGHT};
}

static Rectangle ObjectLayerRect (Rectangle panel) {
    Rectangle manageBtn = ObjectManageRect (panel);
    return (Rectangle){manageBtn.x + manageBtn.width + 8.0f, manageBtn.y,
                       UI_BUTTON_WIDTH, UI_BUTTON_HEIGHT};
}

static Rectangle ObjectDeleteRect (Rectangle panel) {
    Rectangle layerBtn = ObjectLayerRect (panel);
    return (Rectangle){layerBtn.x + layerBtn.width + 8.0f, layerBtn.y,
                       UI_BUTTON_WIDTH, UI_BUTTON_HEIGHT};
}

static Rectangle ObjectDoneRect (Rectangle panel) {
    return (Rectangle){panel.x + panel.width - 24.0f - UI_BUTTON_WIDTH,
                       panel.y + panel.height - 24.0f - UI_BUTTON_HEIGHT,
                       UI_BUTTON_WIDTH, UI_BUTTON_HEIGHT};
}

static Rectangle ObjectCreateRect (Rectangle panel) {
    Rectangle doneBtn = ObjectDoneRect (panel);
    return (Rectangle){doneBtn.x - 8.0f - UI_BUTTON_WIDTH, doneBtn.y,
                       UI_BUTTON_WIDTH, UI_BUTTON_HEIGHT};
}

static Rectangle ObjectAssetsPanel (void) {
    float w = fminf (GetScreenWidth () - 80.0f, 720.0f);
    float h = fminf (GetScreenHeight () - 100.0f, 500.0f);
    return (Rectangle){(GetScreenWidth () - w) / 2.0f, (GetScreenHeight () - h) / 2.0f, w, h};
}

static Rectangle ObjectAssetsListRect (Rectangle panel) {
    return (Rectangle){panel.x + 1, panel.y + UI_MANAGER_HEADER, panel.width - 2,
                       panel.height - UI_MANAGER_HEADER - UI_MANAGER_FOOTER};
}

static Rectangle ObjectAssetUseRect (Rectangle panel) {
    return (Rectangle){panel.x + 24.0f, panel.y + panel.height - 24.0f - UI_BUTTON_HEIGHT,
                       UI_BUTTON_WIDTH, UI_BUTTON_HEIGHT};
}

static Rectangle ObjectAssetUpRect (Rectangle panel) {
    Rectangle use = ObjectAssetUseRect (panel);
    return (Rectangle){use.x + use.width + 8.0f, use.y,
                       UI_BUTTON_WIDTH, UI_BUTTON_HEIGHT};
}

static Rectangle ObjectAssetDownRect (Rectangle panel) {
    Rectangle upBtn = ObjectAssetUpRect (panel);
    return (Rectangle){upBtn.x + upBtn.width + 8.0f, upBtn.y,
                       UI_BUTTON_WIDTH, UI_BUTTON_HEIGHT};
}

static Rectangle ObjectAssetRemoveRect (Rectangle panel) {
    Rectangle downBtn = ObjectAssetDownRect (panel);
    return (Rectangle){downBtn.x + downBtn.width + 8.0f, downBtn.y,
                       UI_BUTTON_WIDTH, UI_BUTTON_HEIGHT};
}

static Rectangle ObjectAssetDoneRect (Rectangle panel) {
    return (Rectangle){panel.x + panel.width - 24.0f - UI_BUTTON_WIDTH,
                       panel.y + panel.height - 24.0f - UI_BUTTON_HEIGHT,
                       UI_BUTTON_WIDTH, UI_BUTTON_HEIGHT};
}

static Rectangle ObjectAssetAddRect (Rectangle panel) {
    Rectangle doneBtn = ObjectAssetDoneRect (panel);
    return (Rectangle){doneBtn.x - 8.0f - UI_BUTTON_WIDTH, doneBtn.y,
                       UI_BUTTON_WIDTH, UI_BUTTON_HEIGHT};
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
    return (Rectangle){panel.x + panel.width / 2.0f - UI_BUTTON_WIDTH - 8.0f,
                       panel.y + panel.height - 24.0f - UI_BUTTON_HEIGHT,
                       UI_BUTTON_WIDTH, UI_BUTTON_HEIGHT};
}

static Rectangle RenameCancelRect (Rectangle panel) {
    return (Rectangle){panel.x + panel.width / 2.0f + 8.0f,
                       panel.y + panel.height - 24.0f - UI_BUTTON_HEIGHT,
                       UI_BUTTON_WIDTH, UI_BUTTON_HEIGHT};
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
    return (Rectangle){panel.x + 24.0f, panel.y + 56.0f, panel.width - 48.0f,
                       panel.height - 132.0f};
}

static Rectangle ScriptCancelRect (Rectangle panel) {
    return (Rectangle){panel.x + panel.width - 24.0f - UI_BUTTON_WIDTH,
                       panel.y + panel.height - 24.0f - UI_BUTTON_HEIGHT,
                       UI_BUTTON_WIDTH, UI_BUTTON_HEIGHT};
}

static Rectangle ScriptSubmitRect (Rectangle panel) {
    Rectangle cancel = ScriptCancelRect (panel);
    return (Rectangle){cancel.x - 8.0f - UI_BUTTON_WIDTH, cancel.y,
                       UI_BUTTON_WIDTH, UI_BUTTON_HEIGHT};
}

static Rectangle ScriptSelfRect (Rectangle panel) {
    Rectangle submit = ScriptSubmitRect (panel);
    return (Rectangle){submit.x - 8.0f - 240.0f, submit.y, 240.0f, UI_BUTTON_HEIGHT};
}

static void RemoveLastCodepoint (char *text) {
    int length = (int)strlen (text);
    if (length == 0) return;

    int index = length - 1;
    while (index > 0 && ((unsigned char)text[index] & 0xC0) == 0x80) index--;
    text[index] = '\0';
}

static void AppendCodepoint (char *text, int size, int codepoint) {
    int utf8Size = 0;
    const char *utf8 = CodepointToUTF8 (codepoint, &utf8Size);
    int length = (int)strlen (text);

    if (length + utf8Size < size) {
        memcpy (text + length, utf8, (size_t)utf8Size);
        text[length + utf8Size] = '\0';
    }
}

typedef enum {
    TEXT_INPUT_NONE = 0,
    TEXT_INPUT_CONFIRM,
    TEXT_INPUT_CANCEL
} TextInputResult;

static TextInputResult UpdateTextInput (char *text, int size, bool numericOnly) {
    Vector2 mouse = GetMousePosition ();
    Rectangle panel = RenamePanel ();
    Rectangle ok = RenameOkRect (panel);
    Rectangle cancel = RenameCancelRect (panel);

    int codepoint;
    while ((codepoint = GetCharPressed ()) > 0) {
        if (codepoint < 32) continue;

        bool accepted = true;
        if (numericOnly) {
            accepted = (codepoint >= '0' && codepoint <= '9') ||
                       (codepoint == '-' && text[0] == '\0');
        }
        if (!accepted) continue;

        int length = (int)strlen (text);
        if (length + 4 >= size) continue;

        AppendCodepoint (text, size, codepoint);
    }

    if (IsKeyPressed (KEY_BACKSPACE)) RemoveLastCodepoint (text);

    bool confirm = IsKeyPressed (KEY_ENTER) || IsKeyPressed (KEY_KP_ENTER);
    if (IsMouseButtonPressed (MOUSE_BUTTON_LEFT)) {
        if (CheckCollisionPointRec (mouse, ok)) {
            confirm = true;
        } else if (CheckCollisionPointRec (mouse, cancel) || !CheckCollisionPointRec (mouse, panel)) {
            return TEXT_INPUT_CANCEL;
        }
    }

    return confirm ? TEXT_INPUT_CONFIRM : TEXT_INPUT_NONE;
}

static void OpenConnect (Ui *ui) {
    ui->connectOpen = true;
    ui->connectField = 0;
}

static void ConfirmConnect (Ui *ui, UiEvent *event) {
    if (ui->connectUser[0] == '\0') {
        UiShowError (ui, "请输入用户名");
        return;
    }

    const char *host = (ui->connectHost[0] != '\0') ? ui->connectHost : "127.0.0.1";
    if (host[0] == '-') {
        UiShowError (ui, "服务器地址无效");
        return;
    }

    long port = 7777;
    if (ui->connectPort[0] != '\0') {
        errno = 0;
        char *end = NULL;
        port = strtol (ui->connectPort, &end, 10);
        if (errno == ERANGE || end == ui->connectPort || *end != '\0' ||
            port < 1 || port > 65535) {
            UiShowError (ui, "请输入有效端口");
            return;
        }
    }

    ui->connectOpen = false;
    event->action = UI_ACTION_CONNECT;
    snprintf (event->host, sizeof (event->host), "%s", host);
    event->port = (int)port;
    snprintf (event->username, sizeof (event->username), "%s", ui->connectUser);
}

static void UpdateConnect (Ui *ui, UiEvent *event) {
    Vector2 mouse = GetMousePosition ();
    Rectangle panel = ConnectPanel ();
    Rectangle userRect = ConnectUserRect (panel);
    Rectangle hostRect = ConnectHostRect (panel);
    Rectangle portRect = ConnectPortRect (panel);
    Rectangle ok = RenameOkRect (panel);
    Rectangle cancel = RenameCancelRect (panel);

    if (IsMouseButtonPressed (MOUSE_BUTTON_LEFT)) {
        if (CheckCollisionPointRec (mouse, ok)) {
            ConfirmConnect (ui, event);
            return;
        }
        if (CheckCollisionPointRec (mouse, cancel) || !CheckCollisionPointRec (mouse, panel)) {
            ui->connectOpen = false;
            return;
        }
        if (CheckCollisionPointRec (mouse, userRect))
            ui->connectField = 0;
        else if (CheckCollisionPointRec (mouse, hostRect))
            ui->connectField = 1;
        else if (CheckCollisionPointRec (mouse, portRect))
            ui->connectField = 2;
    }

    if (IsKeyPressed (KEY_ESCAPE)) {
        ui->connectOpen = false;
        return;
    }
    if (IsKeyPressed (KEY_TAB)) ui->connectField = (ui->connectField + 1) % 3;

    bool portField = (ui->connectField == 2);
    char *text = portField ? ui->connectPort
                           : (ui->connectField == 1 ? ui->connectHost : ui->connectUser);
    int size = (int)(portField ? sizeof (ui->connectPort)
                               : (ui->connectField == 1 ? sizeof (ui->connectHost)
                                                        : sizeof (ui->connectUser)));

    int codepoint;
    while ((codepoint = GetCharPressed ()) > 0) {
        if (codepoint < 32) continue;
        if (portField && (codepoint < '0' || codepoint > '9')) continue;

        int length = (int)strlen (text);
        if (length + 4 >= size) continue;

        AppendCodepoint (text, size, codepoint);
    }

    if (IsKeyPressed (KEY_BACKSPACE)) RemoveLastCodepoint (text);

    if (IsKeyPressed (KEY_ENTER) || IsKeyPressed (KEY_KP_ENTER))
        ConfirmConnect (ui, event);
}

void UiSelectAsset (Ui *ui, Asset *asset) {
    ui->selectedAsset = asset;
    if (asset == NULL || !ui->managerOpen) return;

    IdGroup assetIds = AssetIds ();
    int assetCount = assetIds.count;
    int index = -1;
    for (int i = 0; i < assetIds.count; i++) {
        if (assetIds.ids[i] == asset->id) {
            index = i;
            break;
        }
    }
    IdGroupUnload (&assetIds);
    if (index < 0) return;

    Rectangle list = ManagerListRect (ManagerPanel ());
    float top = index * UI_MANAGER_ROW_HEIGHT;
    float bottom = top + UI_MANAGER_ROW_HEIGHT;

    if (top < ui->managerScroll)
        ui->managerScroll = top;
    else if (bottom > ui->managerScroll + list.height)
        ui->managerScroll = bottom - list.height;

    float maxScroll = fmaxf (0.0f, assetCount * UI_MANAGER_ROW_HEIGHT - list.height);
    ui->managerScroll = Clamp (ui->managerScroll, 0.0f, maxScroll);
}

void UiSelectObject (Ui *ui, Object *object) {
    ui->selectedObject = object;
    if (object == NULL || !ui->objectManagerOpen) return;

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

    if (top < ui->objectScroll)
        ui->objectScroll = top;
    else if (bottom > ui->objectScroll + list.height)
        ui->objectScroll = bottom - list.height;

    float maxScroll = fmaxf (0.0f, count * UI_MANAGER_ROW_HEIGHT - list.height);
    ui->objectScroll = Clamp (ui->objectScroll, 0.0f, maxScroll);
}

void UiOpenObjectManager (Ui *ui, Object *selectObject) {
    ui->objectManagerOpen = true;
    ui->objectScroll = 0.0f;
    ui->selectedObject = NULL;
    ui->lastObjectClick = NULL;
    ui->lastObjectClickTime = 0.0;
    if (selectObject != NULL) UiSelectObject (ui, selectObject);
}

void UiOpenObjectAssets (Ui *ui, Object *object) {
    if (object == NULL) return;

    ui->objectAssetsOpen = true;
    ui->assetsObject = object;
    ui->assetsObjectSelected = -1;
    ui->assetsLastClickIndex = -1;
    ui->assetsLastClickTime = 0.0;
    ui->assetsObjectScroll = 0.0f;
}

static void OpenContext (Ui *ui, Object *object, Vector2 screenPos) {
    int menus = MenuCount (object != NULL ? object->id : 0);
    float width = UI_SUBMENU_WIDTH;
    float height = menus * UI_ITEM_HEIGHT +
                   (menus > 0 ? UI_CONTEXT_SEPARATOR_HEIGHT : 0.0f) +
                   (object != NULL ? 3 : 4) * UI_ITEM_HEIGHT;
    float x = screenPos.x;
    float y = screenPos.y;

    if (x + width > GetScreenWidth ()) x = GetScreenWidth () - width;
    if (y + height > GetScreenHeight ()) y = GetScreenHeight () - height;
    if (x < 0.0f) x = 0.0f;
    if (y < 0.0f) y = 0.0f;

    ui->objectContextOpen = true;
    ui->contextObject = object;
    ui->contextPos = (Vector2){x, y};
    ui->submenuOpen = VIEW_SUBMENU_NONE;
}

void UiOpenObjectContext (Ui *ui, Object *object, Vector2 screenPos) {
    if (object == NULL) return;
    OpenContext (ui, object, screenPos);
}

void UiOpenGlobalContext (Ui *ui, Vector2 screenPos) {
    OpenContext (ui, NULL, screenPos);
}

static bool ScriptAllowed (Ui *ui) {
    User *self = UserFind (NetClientUserId ());
    if (self != NULL && !self->canscript) {
        UiShowError (ui, "当前用户不允许提交脚本");
        return false;
    }
    return true;
}

static void OpenScriptForObject (Ui *ui, Object *object) {
    if (!ScriptAllowed (ui)) return;
    OpenScript (ui);

    const char *prefix = "object(\"";
    size_t pos = 0;
    for (size_t i = 0; prefix[i] != '\0' && pos < sizeof (ui->scriptSelf) - 1; i++)
        ui->scriptSelf[pos++] = prefix[i];

    for (size_t i = 0; object->name[i] != '\0' && pos < sizeof (ui->scriptSelf) - 2; i++) {
        unsigned char ch = (unsigned char)object->name[i];
        if (ch == '"' || ch == '\\') {
            ui->scriptSelf[pos++] = '\\';
            ui->scriptSelf[pos++] = (char)ch;
        } else if (ch == '\n') {
            ui->scriptSelf[pos++] = '\\';
            ui->scriptSelf[pos++] = 'n';
        } else if (ch == '\r') {
            ui->scriptSelf[pos++] = '\\';
            ui->scriptSelf[pos++] = 'r';
        } else if (ch == '\t') {
            ui->scriptSelf[pos++] = '\\';
            ui->scriptSelf[pos++] = 't';
        } else {
            ui->scriptSelf[pos++] = (char)ch;
        }
    }
    if (pos < sizeof (ui->scriptSelf) - 1) ui->scriptSelf[pos++] = '"';
    if (pos < sizeof (ui->scriptSelf) - 1) ui->scriptSelf[pos++] = ')';
    ui->scriptSelf[pos] = '\0';
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
        GetScreenHeight () - 24.0f - UI_BUTTON_HEIGHT -
            row * (UI_BUTTON_HEIGHT + 8.0f),
        UI_BUTTON_WIDTH, UI_BUTTON_HEIGHT};
}

static bool UpdateUserMenuButtons (Ui *ui, UiEvent *event) {
    if (!IsMouseButtonPressed (MOUSE_BUTTON_LEFT)) return false;

    u64 userId = NetClientUserId ();
    if (userId == 0) return false;
    int count = MenuCount (userId);
    if (count == 0) return false;

    Vector2 mouse = GetMousePosition ();
    for (int i = 0; i < count; i++) {
        if (!CheckCollisionPointRec (mouse, UserMenuButtonRect (i))) continue;
        ui->uiPress = true;
        event->action = UI_ACTION_RUN_USER_MENU;
        event->menuIndex = i;
        return true;
    }
    return false;
}


static void UpdateObjectContext (Ui *ui, UiEvent *event) {
    Vector2 mouse = GetMousePosition ();
    Rectangle menu = ObjectContextRect (ui);
    Object *object = ui->contextObject;
    int menuCount = MenuCount (object != NULL ? object->id : 0);
    bool global = object == NULL;
    Rectangle submenu = {0};
    bool submenuVisible = global && ui->submenuOpen >= 0;
    if (submenuVisible) submenu = ViewContextSubmenuRect (ui);

    if (IsMouseButtonPressed (MOUSE_BUTTON_RIGHT)) {
        ui->objectContextOpen = false;
        ui->contextObject = NULL;
        ui->submenuOpen = VIEW_SUBMENU_NONE;
        return;
    }
    if (!IsMouseButtonPressed (MOUSE_BUTTON_LEFT)) return;
    ui->uiPress = true;

    bool inMenu = CheckCollisionPointRec (mouse, menu);
    bool inSubmenu = submenuVisible && CheckCollisionPointRec (mouse, submenu);
    if (!inMenu && !inSubmenu) {
        ui->objectContextOpen = false;
        ui->contextObject = NULL;
        ui->submenuOpen = VIEW_SUBMENU_NONE;
        return;
    }

    if (inSubmenu) {
        int item = (int)((mouse.y - submenu.y) / UI_ITEM_HEIGHT);
        int count = (int)(submenu.height / UI_ITEM_HEIGHT);
        if (item >= 0 && item < count) {
            if (ui->submenuOpen == VIEW_SUBMENU_ZOOM) {
                event->action = UI_ACTION_SET_ZOOM;
                event->zoom = viewZoomValues[item];
            } else {
                event->action = UI_ACTION_SET_BACKGROUND;
                event->background = (BackgroundStyle)item;
            }
        }
        ui->objectContextOpen = false;
        ui->contextObject = NULL;
        ui->submenuOpen = VIEW_SUBMENU_NONE;
        return;
    }

    float menuEnd = menu.y + menuCount * UI_ITEM_HEIGHT;
    ui->objectContextOpen = false;
    ui->contextObject = NULL;
    if (mouse.y < menuEnd) {
        int item = (int)((mouse.y - menu.y) / UI_ITEM_HEIGHT);
        if (item >= 0 && item < menuCount) {
            event->action = object != NULL ? UI_ACTION_RUN_MENU
                                           : UI_ACTION_RUN_GLOBAL_MENU;
            event->object = object;
            event->menuIndex = item;
        }
        ui->submenuOpen = VIEW_SUBMENU_NONE;
        return;
    }

    float actionY = menuEnd + (menuCount > 0 ? UI_CONTEXT_SEPARATOR_HEIGHT : 0.0f);
    if (mouse.y < actionY) {
        ui->submenuOpen = VIEW_SUBMENU_NONE;
        return;
    }

    int action = (int)((mouse.y - actionY) / UI_ITEM_HEIGHT);
    if (global) {
        if (action == 0) {
            event->action = UI_ACTION_VIEW_ORIGIN;
            ui->submenuOpen = VIEW_SUBMENU_NONE;
        } else if (action == 1) {
            ui->submenuOpen = ui->submenuOpen == VIEW_SUBMENU_ZOOM
                                  ? VIEW_SUBMENU_NONE : VIEW_SUBMENU_ZOOM;
            ui->objectContextOpen = true;
            ui->contextObject = NULL;
        } else if (action == 2) {
            ui->submenuOpen = ui->submenuOpen == VIEW_SUBMENU_BACKGROUND
                                  ? VIEW_SUBMENU_NONE : VIEW_SUBMENU_BACKGROUND;
            ui->objectContextOpen = true;
            ui->contextObject = NULL;
        } else if (action == 3) {
            if (ScriptAllowed (ui)) {
                OpenScript (ui);
                snprintf (ui->scriptSelf, sizeof (ui->scriptSelf), "0");
            }
            ui->submenuOpen = VIEW_SUBMENU_NONE;
        } else {
            ui->submenuOpen = VIEW_SUBMENU_NONE;
        }
        return;
    }

    ui->submenuOpen = VIEW_SUBMENU_NONE;
    if (action == 0) {
        UiOpenObjectManager (ui, object);
    } else if (action == 1) {
        OpenScriptForObject (ui, object);
    } else if (action == 2) {
        if (ui->selectedObject == object) ui->selectedObject = NULL;
        event->action = UI_ACTION_DELETE_OBJECT;
        event->object = object;
    }
}

static void UpdateManager (Ui *ui, UiEvent *event, float wheel) {
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
    ui->managerScroll = Clamp (ui->managerScroll - wheel * 40.0f, 0.0f, maxScroll);

    if (!IsMouseButtonPressed (MOUSE_BUTTON_LEFT)) return;
    ui->uiPress = true;

    if (CheckCollisionPointRec (mouse, exit)) {
        ui->managerOpen = false;
        return;
    }
    if (CheckCollisionPointRec (mouse, importBtn)) {
        if (OpenImageDialog (ui, event->path, (int)sizeof (event->path)))
            event->action = UI_ACTION_IMPORT;
        return;
    }

    if (ui->selectedAsset != NULL) {
        if (CheckCollisionPointRec (mouse, renameBtn)) {
            ui->renameOpen = true;
            ui->renameIsObject = false;
            ui->renameAsset = ui->selectedAsset;
            snprintf (ui->renameText, sizeof (ui->renameText), "%s", ui->selectedAsset->name);
            return;
        }
        if (CheckCollisionPointRec (mouse, replaceBtn)) {
            if (OpenImageDialog (ui, event->path, (int)sizeof (event->path))) {
                event->action = UI_ACTION_REPLACE_ASSET;
                event->asset = ui->selectedAsset;
            }
            return;
        }
        if (CheckCollisionPointRec (mouse, deleteBtn)) {
            event->action = UI_ACTION_DELETE_ASSET;
            event->asset = ui->selectedAsset;
            ui->selectedAsset = NULL;
            return;
        }
    }

    if (CheckCollisionPointRec (mouse, list)) {
        int index = (int)((mouse.y - list.y + ui->managerScroll) / UI_MANAGER_ROW_HEIGHT);
        assetIds = AssetIds ();
        if (index >= 0 && index < assetIds.count) {
            Asset *asset = EntityFind (assetIds.ids[index]);
            if (asset != NULL) {
                ui->selectedAsset = asset;
                IdGroupUnload (&assetIds);
                return;
            }
        }
        IdGroupUnload (&assetIds);
    }

    ui->selectedAsset = NULL;
}

static void UpdateObjectManager (Ui *ui, UiEvent *event, float wheel) {
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
    ui->objectScroll = Clamp (ui->objectScroll - wheel * 40.0f, 0.0f, maxScroll);

    if (!IsMouseButtonPressed (MOUSE_BUTTON_LEFT)) return;
    ui->uiPress = true;

    if (CheckCollisionPointRec (mouse, doneBtn)) {
        if (ui->selectedObject != NULL) {
            event->action = UI_ACTION_UNLOCK_OBJECT;
            event->object = ui->selectedObject;
            ui->selectedObject = NULL;
        }
        ui->objectManagerOpen = false;
        return;
    }
    if (CheckCollisionPointRec (mouse, createBtn)) {
        event->action = UI_ACTION_CREATE_OBJECT;
        return;
    }

    if (ui->selectedObject != NULL) {
        if (CheckCollisionPointRec (mouse, renameBtn)) {
            ui->renameOpen = true;
            ui->renameIsObject = true;
            ui->renameObject = ui->selectedObject;
            snprintf (ui->renameText, sizeof (ui->renameText), "%s", ui->selectedObject->name);
            return;
        }
        if (CheckCollisionPointRec (mouse, manageBtn)) {
            UiOpenObjectAssets (ui, ui->selectedObject);
            event->action = UI_ACTION_LOCK_OBJECT;
            event->object = ui->selectedObject;
            return;
        }
        if (CheckCollisionPointRec (mouse, layerBtn)) {
            ui->layerOpen = true;
            ui->layerObject = ui->selectedObject;
            snprintf (ui->layerText, sizeof (ui->layerText), "%d", ui->selectedObject->layer);
            return;
        }
        if (CheckCollisionPointRec (mouse, deleteBtn)) {
            event->action = UI_ACTION_DELETE_OBJECT;
            event->object = ui->selectedObject;
            ui->selectedObject = NULL;
            return;
        }
    }

    if (CheckCollisionPointRec (mouse, list)) {
        int index = (int)((mouse.y - list.y + ui->objectScroll) / UI_MANAGER_ROW_HEIGHT);
        objectIds = ObjectIds ();
        Object *object = (index >= 0 && index < objectIds.count)
                                   ? EntityFind (objectIds.ids[index]) : NULL;
        IdGroupUnload (&objectIds);
        if (object != NULL) {
                if (object->lockOwnerId != 0 &&
                    object->lockOwnerId != NetClientUserId())
                    return;

                if (object != ui->selectedObject ||
                    object->lockOwnerId != NetClientUserId ()) {
                    event->action = UI_ACTION_LOCK_OBJECT;
                    event->object = object;
                    ui->selectedObject = object;
                }

                double now = GetTime ();
                if (object == ui->lastObjectClick && (now - ui->lastObjectClickTime) <= 0.35) {
                    ui->lastObjectClick = NULL;
                    ui->lastObjectClickTime = 0.0;
                    UiOpenObjectAssets (ui, object);
                    event->action = UI_ACTION_LOCK_OBJECT;
                    event->object = object;
                } else {
                    ui->lastObjectClick = object;
                    ui->lastObjectClickTime = now;
                }
                return;
        }
    }

    if (ui->selectedObject != NULL) {
        event->action = UI_ACTION_UNLOCK_OBJECT;
        event->object = ui->selectedObject;
    }
    ui->selectedObject = NULL;
    ui->lastObjectClick = NULL;
    ui->lastObjectClickTime = 0.0;
}

static void UpdateObjectAssets (Ui *ui, UiEvent *event, float wheel) {
    if (ui->assetsObject == NULL) {
        ui->objectAssetsOpen = false;
        return;
    }

    Vector2 mouse = GetMousePosition ();
    int count = ui->assetsObject->assets.count;
    Rectangle panel = ObjectAssetsPanel ();
    Rectangle list = ObjectAssetsListRect (panel);
    Rectangle useBtn = ObjectAssetUseRect (panel);
    Rectangle upBtn = ObjectAssetUpRect (panel);
    Rectangle downBtn = ObjectAssetDownRect (panel);
    Rectangle removeBtn = ObjectAssetRemoveRect (panel);
    Rectangle addBtn = ObjectAssetAddRect (panel);
    Rectangle doneBtn = ObjectAssetDoneRect (panel);
    float maxScroll = fmaxf (0.0f, count * UI_OBJECT_ASSET_ROW_HEIGHT - list.height);
    ui->assetsObjectScroll = Clamp (ui->assetsObjectScroll - wheel * 40.0f, 0.0f, maxScroll);

    if (!IsMouseButtonPressed (MOUSE_BUTTON_LEFT)) return;
    ui->uiPress = true;

    if (CheckCollisionPointRec (mouse, doneBtn)) {
        ui->objectAssetsOpen = false;
        ui->assetsObject = NULL;
        ui->assetsObjectSelected = -1;
        ui->assetsLastClickIndex = -1;
        ui->assetsLastClickTime = 0.0;
        ui->assetsObjectScroll = 0.0f;
        return;
    }
    if (CheckCollisionPointRec (mouse, addBtn)) {
        ui->pickerOpen = true;
        ui->pickerScroll = 0.0f;
        ui->pickerObject = ui->assetsObject;
        return;
    }

    bool hasSelection = (ui->assetsObjectSelected >= 0 && ui->assetsObjectSelected < count);
    if (hasSelection) {
        if (CheckCollisionPointRec (mouse, useBtn)) {
            event->action = UI_ACTION_USE_OBJECT_ASSET;
            event->object = ui->assetsObject;
            event->assetIndex = ui->assetsObjectSelected;
            return;
        }
        if (CheckCollisionPointRec (mouse, upBtn)) {
            if (ui->assetsObjectSelected > 0) {
                int from = ui->assetsObjectSelected;
                event->action = UI_ACTION_MOVE_OBJECT_ASSET;
                event->object = ui->assetsObject;
                event->assetIndex = from;
                event->assetTargetIndex = from - 1;
                ui->assetsObjectSelected = from - 1;
            }
            return;
        }
        if (CheckCollisionPointRec (mouse, downBtn)) {
            if (ui->assetsObjectSelected < count - 1) {
                int from = ui->assetsObjectSelected;
                event->action = UI_ACTION_MOVE_OBJECT_ASSET;
                event->object = ui->assetsObject;
                event->assetIndex = from;
                event->assetTargetIndex = from + 1;
                ui->assetsObjectSelected = from + 1;
            }
            return;
        }
        if (CheckCollisionPointRec (mouse, removeBtn)) {
            event->action = UI_ACTION_REMOVE_OBJECT_ASSET;
            event->object = ui->assetsObject;
            event->assetIndex = ui->assetsObjectSelected;
            ui->assetsObjectSelected = -1;
            ui->assetsLastClickIndex = -1;
            ui->assetsLastClickTime = 0.0;
            return;
        }
    }

    if (CheckCollisionPointRec (mouse, list)) {
        int index = (int)((mouse.y - list.y + ui->assetsObjectScroll) / UI_OBJECT_ASSET_ROW_HEIGHT);
        if (index >= 0 && index < count) {
            double now = GetTime ();
            bool doubleClick = ui->assetsLastClickIndex == index &&
                               now - ui->assetsLastClickTime <= 0.35;
            ui->assetsObjectSelected = index;
            ui->assetsLastClickIndex = index;
            ui->assetsLastClickTime = now;
            if (doubleClick) {
                event->action = UI_ACTION_USE_OBJECT_ASSET;
                event->object = ui->assetsObject;
                event->assetIndex = index;
            }
            return;
        }
    }

    ui->assetsObjectSelected = -1;
    ui->assetsLastClickIndex = -1;
    ui->assetsLastClickTime = 0.0;
}

static void UpdateRename (Ui *ui, UiEvent *event) {
    TextInputResult result = UpdateTextInput (ui->renameText, (int)sizeof (ui->renameText), false);
    if (result == TEXT_INPUT_CANCEL) {
        ui->renameOpen = false;
        return;
    }
    if (result != TEXT_INPUT_CONFIRM || ui->renameText[0] == '\0') return;

    if (ui->renameIsObject) {
        event->action = UI_ACTION_RENAME_OBJECT;
        event->object = ui->renameObject;
        snprintf (event->objectName, sizeof (event->objectName), "%s", ui->renameText);
    } else {
        event->action = UI_ACTION_RENAME_ASSET;
        event->asset = ui->renameAsset;
        snprintf (event->assetName, sizeof (event->assetName), "%s", ui->renameText);
    }
    ui->renameOpen = false;
}

static void UpdateLayer (Ui *ui, UiEvent *event) {
    TextInputResult result = UpdateTextInput (ui->layerText, (int)sizeof (ui->layerText), true);
    if (result == TEXT_INPUT_CANCEL) {
        ui->layerOpen = false;
        return;
    }
    if (result != TEXT_INPUT_CONFIRM) return;

    if (ui->layerText[0] == '\0' || strcmp (ui->layerText, "-") == 0) {
        UiShowError (ui, "请输入有效层数");
        return;
    }

    errno = 0;
    char *end = NULL;
    long value = strtol (ui->layerText, &end, 10);
    if (errno == ERANGE || end == ui->layerText || *end != '\0' || value < INT_MIN || value > INT_MAX) {
        UiShowError (ui, "层数超出 int 范围");
        return;
    }

    ui->layerOpen = false;
    event->action = UI_ACTION_SET_OBJECT_LAYER;
    event->object = ui->layerObject;
    event->layer = (int)value;
}

static int ScriptLineCount (const TextBuffer *text) {
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

static float ScriptMaxScroll (const TextBuffer *text) {
    int maxScroll = ScriptLineCount (text) - ScriptVisibleLines ();
    return maxScroll > 0 ? (float)maxScroll : 0.0f;
}

static size_t ScriptPrevCodepoint (const TextBuffer *text, size_t pos) {
    if (pos == 0 || pos > text->length) return 0;
    pos--;
    while (pos > 0 && ((unsigned char)text->data[pos] & 0xC0) == 0x80) pos--;
    return pos;
}

static size_t ScriptNextCodepoint (const TextBuffer *text, size_t pos) {
    if (pos >= text->length) return text->length;
    pos++;
    while (pos < text->length && ((unsigned char)text->data[pos] & 0xC0) == 0x80) pos++;
    return pos;
}

static size_t ScriptLineStart (const TextBuffer *text, int line) {
    size_t pos = 0;
    for (int i = 0; i < line && pos < text->length; i++) {
        while (pos < text->length && text->data[pos] != '\n') pos++;
        if (pos < text->length) pos++;
    }
    return pos;
}

static size_t ScriptLineEnd (const TextBuffer *text, size_t start) {
    size_t pos = start;
    while (pos < text->length && text->data[pos] != '\n') pos++;
    return pos;
}

static int ScriptLineOfOffset (const TextBuffer *text, size_t offset) {
    int line = 0;
    for (size_t i = 0; i < offset && i < text->length; i++) {
        if (text->data[i] == '\n') line++;
    }
    return line;
}

static int ScriptDisplayColumn (const TextBuffer *text, size_t start, size_t pos) {
    int column = 0;
    for (size_t i = start; i < pos && i < text->length;) {
        column += text->data[i] == '\t' ? 4 : 1;
        i = ScriptNextCodepoint (text, i);
    }
    return column;
}

static size_t ScriptOffsetAtColumn (const TextBuffer *text, size_t start, size_t end,
                                    int column) {
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

static void ScriptScrollToCursor (Ui *ui) {
    int line = ScriptLineOfOffset (&ui->scriptText, ui->scriptCursor);
    int visible = ScriptVisibleLines ();
    int start = (int)ui->scriptScroll;
    if (line < start)
        ui->scriptScroll = (float)line;
    else if (line >= start + visible)
        ui->scriptScroll = (float)(line - visible + 1);
    ui->scriptScroll = Clamp (ui->scriptScroll, 0.0f,
                              ScriptMaxScroll (&ui->scriptText));
}

static bool InsertScriptText (Ui *ui, const char *text, size_t length) {
    if (length == 0) return true;
    TextBuffer *buffer = &ui->scriptText;
    if (!TextBufferReserve (buffer, length)) return false;

    memmove (buffer->data + ui->scriptCursor + length,
             buffer->data + ui->scriptCursor,
             buffer->length - ui->scriptCursor);
    memcpy (buffer->data + ui->scriptCursor, text, length);
    buffer->length += length;
    buffer->data[buffer->length] = '\0';
    ui->scriptCursor += length;
    ui->scriptPreferredColumn = -1;
    return true;
}

static bool InsertScriptCodepoint (Ui *ui, int codepoint) {
    int size = 0;
    const char *utf8 = CodepointToUTF8 (codepoint, &size);
    if (utf8 == NULL || size <= 0) return false;
    return InsertScriptText (ui, utf8, (size_t)size);
}

static void RemoveScriptRange (Ui *ui, size_t start, size_t end) {
    TextBuffer *buffer = &ui->scriptText;
    if (start >= end || end > buffer->length) return;

    memmove (buffer->data + start, buffer->data + end, buffer->length - end);
    buffer->length -= end - start;
    buffer->data[buffer->length] = '\0';
    ui->scriptCursor = start;
    ui->scriptPreferredColumn = -1;
}

static void ScriptMoveVertical (Ui *ui, int direction) {
    TextBuffer *text = &ui->scriptText;
    int line = ScriptLineOfOffset (text, ui->scriptCursor);
    size_t start = ScriptLineStart (text, line);
    if (ui->scriptPreferredColumn < 0)
        ui->scriptPreferredColumn = ScriptDisplayColumn (text, start, ui->scriptCursor);

    int target = line + direction;
    int total = ScriptLineCount (text);
    if (target < 0 || target >= total) return;

    size_t targetStart = ScriptLineStart (text, target);
    size_t targetEnd = ScriptLineEnd (text, targetStart);
    ui->scriptCursor = ScriptOffsetAtColumn (text, targetStart, targetEnd,
                                              ui->scriptPreferredColumn);
}

static void ScriptSetCursorFromMouse (Ui *ui, Rectangle code, Vector2 mouse) {
    TextBuffer *text = &ui->scriptText;
    int total = ScriptLineCount (text);
    int start = (int)ui->scriptScroll;
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
        float width = MeasureTextEx (ui->monoFont, display.data != NULL ? display.data : "",
                                     UI_CODE_FONT_SIZE, 0.0f).x;
        float distance = fabsf (width - target);
        if (distance < bestDistance) {
            best = pos;
            bestDistance = distance;
        }
    }
    TextBufferFree (&display);

    ui->scriptCursor = best;
    ui->scriptPreferredColumn = -1;
}

static void ScriptClose (Ui *ui) {
    ui->scriptOpen = false;
    ui->scriptSelf[0] = '\0';
    ui->scriptSelfFocused = false;
    ui->scriptCursor = 0;
    ui->scriptPreferredColumn = -1;
    ui->scriptScroll = 0.0f;
    TextBufferFree (&ui->scriptText);
}

static void OpenScript (Ui *ui) {
    ScriptClose (ui);
    ui->scriptOpen = true;
}

static void UpdateScript (Ui *ui, UiEvent *event) {
    Vector2 mouse = GetMousePosition ();
    Rectangle panel = ScriptPanel ();
    Rectangle code = ScriptCodeRect (panel);
    Rectangle self = ScriptSelfRect (panel);
    Rectangle submit = ScriptSubmitRect (panel);
    Rectangle cancel = ScriptCancelRect (panel);
    float maxScroll = ScriptMaxScroll (&ui->scriptText);
    ui->scriptScroll = Clamp (ui->scriptScroll - GetMouseWheelMove () * 3.0f, 0.0f, maxScroll);

    if (IsMouseButtonPressed (MOUSE_BUTTON_LEFT)) {
        if (CheckCollisionPointRec (mouse, submit)) {
            event->action = UI_ACTION_RUN_SCRIPT;
            return;
        }
        if (CheckCollisionPointRec (mouse, cancel)) {
            ScriptClose (ui);
            return;
        }
        if (CheckCollisionPointRec (mouse, self)) {
            ui->scriptSelfFocused = true;
            return;
        }
        if (CheckCollisionPointRec (mouse, code)) {
            ui->scriptSelfFocused = false;
            ScriptSetCursorFromMouse (ui, code, mouse);
            ScriptScrollToCursor (ui);
            return;
        }
    }
    if (IsKeyPressed (KEY_ESCAPE)) {
        ScriptClose (ui);
        return;
    }

    bool control = IsKeyDown (KEY_LEFT_CONTROL) || IsKeyDown (KEY_RIGHT_CONTROL);
    if (control && (IsKeyPressed (KEY_ENTER) || IsKeyPressed (KEY_KP_ENTER))) {
        event->action = UI_ACTION_RUN_SCRIPT;
        return;
    }

    if (ui->scriptSelfFocused) {
        int codepoint;
        while ((codepoint = GetCharPressed ()) > 0) {
            if (codepoint < 32 || codepoint == 127) continue;
            AppendCodepoint (ui->scriptSelf, (int)sizeof (ui->scriptSelf), codepoint);
        }
        if (IsKeyPressed (KEY_BACKSPACE)) RemoveLastCodepoint (ui->scriptSelf);
        if (IsKeyPressed (KEY_ENTER) || IsKeyPressed (KEY_KP_ENTER)) {
            event->action = UI_ACTION_RUN_SCRIPT;
        }
        return;
    }

    bool cursorChanged = false;
    TextBuffer *text = &ui->scriptText;
    int line = ScriptLineOfOffset (text, ui->scriptCursor);
    size_t lineStart = ScriptLineStart (text, line);
    size_t lineEnd = ScriptLineEnd (text, lineStart);

    if (IsKeyPressed (KEY_LEFT)) {
        ui->scriptCursor = ScriptPrevCodepoint (text, ui->scriptCursor);
        ui->scriptPreferredColumn = -1;
        cursorChanged = true;
    } else if (IsKeyPressed (KEY_RIGHT)) {
        ui->scriptCursor = ScriptNextCodepoint (text, ui->scriptCursor);
        ui->scriptPreferredColumn = -1;
        cursorChanged = true;
    } else if (IsKeyPressed (KEY_UP)) {
        ScriptMoveVertical (ui, -1);
        cursorChanged = true;
    } else if (IsKeyPressed (KEY_DOWN)) {
        ScriptMoveVertical (ui, 1);
        cursorChanged = true;
    } else if (IsKeyPressed (KEY_HOME)) {
        ui->scriptCursor = control ? 0 : lineStart;
        ui->scriptPreferredColumn = -1;
        cursorChanged = true;
    } else if (IsKeyPressed (KEY_END)) {
        ui->scriptCursor = control ? text->length : lineEnd;
        ui->scriptPreferredColumn = -1;
        cursorChanged = true;
    } else if (IsKeyPressed (KEY_BACKSPACE)) {
        if (ui->scriptCursor > 0) {
            size_t previous = ScriptPrevCodepoint (text, ui->scriptCursor);
            RemoveScriptRange (ui, previous, ui->scriptCursor);
            cursorChanged = true;
        }
    } else if (IsKeyPressed (KEY_DELETE)) {
        if (ui->scriptCursor < text->length) {
            size_t next = ScriptNextCodepoint (text, ui->scriptCursor);
            RemoveScriptRange (ui, ui->scriptCursor, next);
            cursorChanged = true;
        }
    }

    int codepoint;
    while ((codepoint = GetCharPressed ()) > 0) {
        if (codepoint < 32 || codepoint == 127) continue;
        if (InsertScriptCodepoint (ui, codepoint)) cursorChanged = true;
    }
    if (IsKeyPressed (KEY_TAB)) {
        if (InsertScriptText (ui, "\t", 1)) cursorChanged = true;
    }
    if (IsKeyPressed (KEY_ENTER) || IsKeyPressed (KEY_KP_ENTER)) {
        if (InsertScriptText (ui, "\n", 1)) cursorChanged = true;
    }

    if (ui->scriptCursor > text->length) ui->scriptCursor = text->length;
    if (cursorChanged) ScriptScrollToCursor (ui);
}

UiEvent UiUpdate (Ui *ui) {
    UiEvent event = {0};
    Vector2 mouse = GetMousePosition ();
    float wheel = GetMouseWheelMove ();

    if (!IsMouseButtonDown (MOUSE_BUTTON_LEFT)) ui->uiPress = false;

    float dt = fminf (GetFrameTime (), 0.1f);
    int activeMessages = 0;
    for (int i = 0; i < ui->messageCount; i++) {
        ui->messageTimes[i] -= dt;
        if (ui->messageTimes[i] <= 0.0f) continue;
        if (activeMessages != i) {
            memmove (ui->messages[activeMessages], ui->messages[i],
                     sizeof (ui->messages[0]));
            ui->messageTimes[activeMessages] = ui->messageTimes[i];
            ui->messageKinds[activeMessages] = ui->messageKinds[i];
        }
        activeMessages++;
    }
    ui->messageCount = activeMessages;

    if (ui->scriptOpen) {
        UpdateScript (ui, &event);
        event.mouseCaptured = true;
        return event;
    }

    if (ui->connectOpen) {
        UpdateConnect (ui, &event);
        event.mouseCaptured = true;
        return event;
    }

    if (ui->layerOpen) {
        UpdateLayer (ui, &event);
        event.mouseCaptured = true;
        return event;
    }

    if (ui->renameOpen) {
        UpdateRename (ui, &event);
        event.mouseCaptured = true;
        return event;
    }

    if (ui->objectContextOpen) {
        UpdateObjectContext (ui, &event);
        event.mouseCaptured = true;
        return event;
    }

    if (ui->managerOpen) {
        UpdateManager (ui, &event, wheel);
        event.mouseCaptured = true;
        return event;
    }

    if (ui->pickerOpen) {
        IdGroup assetIds = AssetIds ();
        int assetCount = assetIds.count;
        Rectangle panel = PickerPanel ();
        Rectangle list = PickerListRect (panel);
        Rectangle cancel = PickerCancelRect (panel);
        float maxScroll = fmaxf (0.0f, assetCount * UI_ASSET_ROW_HEIGHT - list.height);
        ui->pickerScroll = Clamp (ui->pickerScroll - wheel * 40.0f, 0.0f, maxScroll);

        if (IsMouseButtonPressed (MOUSE_BUTTON_LEFT)) {
            if (CheckCollisionPointRec (mouse, cancel) || !CheckCollisionPointRec (mouse, panel)) {
                ui->pickerOpen = false;
            } else if (CheckCollisionPointRec (mouse, list)) {
                int index = (int)((mouse.y - list.y + ui->pickerScroll) / UI_ASSET_ROW_HEIGHT);
                if (index >= 0 && index < assetIds.count) {
                    Asset *asset = EntityFind (assetIds.ids[index]);
                    if (asset != NULL && ui->pickerObject != NULL) {
                        ui->assetsObjectSelected = ui->pickerObject->assets.count;
                        event.action = UI_ACTION_ADD_OBJECT_ASSET;
                        event.object = ui->pickerObject;
                        event.asset = asset;
                        ui->pickerOpen = false;
                    }
                }
            }
            ui->uiPress = true;
        }

        IdGroupUnload (&assetIds);
        event.mouseCaptured = true;
        return event;
    }

    if (ui->objectAssetsOpen) {
        UpdateObjectAssets (ui, &event, wheel);
        event.mouseCaptured = true;
        return event;
    }

    if (ui->objectManagerOpen) {
        UpdateObjectManager (ui, &event, wheel);
        event.mouseCaptured = true;
        return event;
    }

    if (ui->menuOpen < 0 && UpdateUserMenuButtons (ui, &event)) {
        event.mouseCaptured = true;
        return event;
    }

    if (IsMouseButtonPressed (MOUSE_BUTTON_LEFT)) {
        if (mouse.y <= UI_BAR_HEIGHT) {
            int hit = -1;
            for (int i = 0; i < UI_MENU_COUNT; i++) {
                if ((i == 0 && !ui->showFileMenu) ||
                    (i == 4 && !ui->showConnectMenu))
                    continue;
                Rectangle title = {ui->menuX[i], 0.0f, ui->menuW[i], UI_BAR_HEIGHT};
                if (CheckCollisionPointRec (mouse, title)) {
                    hit = i;
                    break;
                }
            }

            if (hit >= 0) event.deselectObject = true;

            if (hit == 0) {
                bool wasOpen = (ui->menuOpen == 0);
                ui->menuOpen = wasOpen ? -1 : 0;
                ui->submenuOpen = -1;
            } else if (hit == 1) {
                ui->menuOpen = -1;
                ui->submenuOpen = -1;
                UiOpenAssetManager (ui);
            } else if (hit == 2) {
                ui->menuOpen = -1;
                ui->submenuOpen = -1;
                UiOpenObjectManager (ui, NULL);
            } else if (hit == 3) {
                ui->menuOpen = -1;
                ui->submenuOpen = -1;
                if (ScriptAllowed (ui)) {
                    OpenScript (ui);
                    snprintf (ui->scriptSelf, sizeof (ui->scriptSelf), "self");
                }
            } else if (hit == 4) {
                bool wasOpen = (ui->menuOpen == 4);
                ui->menuOpen = wasOpen ? -1 : 4;
                ui->submenuOpen = -1;
            } else {
                ui->menuOpen = -1;
                ui->submenuOpen = -1;
            }

            ui->uiPress = true;
        } else if (ui->menuOpen >= 0) {
            int menu = ui->menuOpen;
            Rectangle drop = {ui->menuX[menu], UI_BAR_HEIGHT, UI_MENU_WIDTH,
                              menuItemCounts[menu] * UI_ITEM_HEIGHT};
            if (CheckCollisionPointRec (mouse, drop)) {
                int item = (int)((mouse.y - UI_BAR_HEIGHT) / UI_ITEM_HEIGHT);
                if (menu == 0 && item == 0) {
                    if (OpenImportDialog (ui, event.path, (int)sizeof (event.path))) {
                        if (PathHasExtension (event.path, ".dsk"))
                            event.action = UI_ACTION_IMPORT_SAVE;
                        else if (PathIsImage (event.path))
                            event.action = UI_ACTION_IMPORT;
                        else
                            UiShowError (ui, "不支持的文件类型");
                    }
                } else if (menu == 0 && item == 1) {
                    if (OpenFolderDialog (ui, event.path,
                                          (int)sizeof (event.path)))
                        event.action = UI_ACTION_IMPORT_SAVE;
                } else if (menu == 0 && item == 2) {
                    if (OpenExportDialog (ui, event.path, (int)sizeof (event.path)))
                        event.action = UI_ACTION_EXPORT_SAVE;
                } else if (menu == 4 && item == 0) {
                    OpenConnect (ui);
                } else if (menu == 4 && item == 1) {
                    event.action = UI_ACTION_OPEN_SERVER;
                }
            }
            ui->menuOpen = -1;
            ui->submenuOpen = -1;
            ui->uiPress = true;
        }
    }

    event.mouseCaptured = ui->menuOpen >= 0 || ui->uiPress || mouse.y <= UI_BAR_HEIGHT;
    return event;
}

static float MenuTextY (Font font, const char *text, Rectangle item) {
    float height = MeasureTextEx (font, text, UI_FONT_SIZE, 0).y;
    return item.y + (item.height - height) / 2.0f;
}

static void DrawMenuBar (const Ui *ui) {
    Vector2 mouse = GetMousePosition ();
    DrawRectangle (0, 0, GetScreenWidth (), UI_BAR_HEIGHT, (Color){16, 18, 23, 255});
    DrawRectangle (0, UI_BAR_HEIGHT - 1, GetScreenWidth (), 1, (Color){55, 60, 72, 255});

    for (int i = 0; i < UI_MENU_COUNT; i++) {
        if ((i == 0 && !ui->showFileMenu) ||
            (i == 4 && !ui->showConnectMenu))
            continue;
        Rectangle title = {ui->menuX[i], 0.0f, ui->menuW[i], UI_BAR_HEIGHT};
        if (ui->menuOpen == i)
            DrawRectangleRec (title, (Color){45, 49, 61, 255});
        else if (CheckCollisionPointRec (mouse, title))
            DrawRectangleRec (title, (Color){32, 35, 44, 255});
        DrawTextEx (ui->font, menuNames[i],
                    (Vector2){title.x + 12.0f, MenuTextY (ui->font, menuNames[i], title)},
                    UI_FONT_SIZE, 0, (Color){230, 232, 240, 255});
    }

    if (ui->menuOpen >= 0) {
        int menu = ui->menuOpen;
        Rectangle drop = {ui->menuX[menu], UI_BAR_HEIGHT, UI_MENU_WIDTH,
                          menuItemCounts[menu] * UI_ITEM_HEIGHT};
        DrawRectangleRec (drop, (Color){28, 31, 39, 255});

        for (int i = 0; i < menuItemCounts[menu]; i++) {
            Rectangle item = {drop.x, drop.y + i * UI_ITEM_HEIGHT, drop.width, UI_ITEM_HEIGHT};
            if (CheckCollisionPointRec (mouse, item)) DrawRectangleRec (item, (Color){45, 49, 61, 255});
            DrawTextEx (ui->font, menuItems[menu][i],
                        (Vector2){item.x + 12.0f,
                                  MenuTextY (ui->font, menuItems[menu][i], item)},
                        UI_FONT_SIZE, 0, (Color){225, 228, 236, 255});
        }

        DrawRectangleLinesEx (drop, 1, (Color){60, 65, 80, 255});

    }
}

static void DrawTextureFit (Texture2D texture, Rectangle box) {
    float scale = fminf (box.width / texture.width, box.height / texture.height);
    Vector2 position = {
        box.x + (box.width - texture.width * scale) / 2.0f,
        box.y + (box.height - texture.height * scale) / 2.0f};
    DrawTextureEx (texture, position, 0.0f, scale, WHITE);
}

static void DrawDialogBase (const Ui *ui, Rectangle panel, const char *title) {
    DrawRectangle (0, 0, GetScreenWidth (), GetScreenHeight (), Fade (BLACK, 0.55f));
    DrawRectangleRec (panel, (Color){26, 29, 36, 255});
    DrawRectangleLinesEx (panel, 1, (Color){70, 76, 92, 255});
    DrawTextEx (ui->font, title, (Vector2){panel.x + 24.0f, panel.y + 16.0f}, UI_FONT_SIZE, 0,
                (Color){235, 238, 245, 255});
    DrawRectangle ((int)(panel.x + 24.0f), (int)(panel.y + 43.0f), (int)(panel.width - 48.0f), 1,
                   (Color){58, 63, 76, 255});
}

static void DrawListScrollbar (Rectangle list, int itemCount, float rowHeight, float scroll) {
    float contentH = itemCount * rowHeight;
    if (contentH <= list.height) return;

    float barH = list.height * list.height / contentH;
    float maxScroll = contentH - list.height;
    float t = (maxScroll > 0.0f) ? scroll / maxScroll : 0.0f;

    DrawRectangle ((int)(list.x + list.width - 7.0f),
                   (int)(list.y + t * (list.height - barH)), 3, (int)barH,
                   (Color){90, 96, 112, 255});
}

static void DrawPicker (const Ui *ui) {
    if (!ui->pickerOpen) return;

    IdGroup assetIds = AssetIds ();
    int assetCount = assetIds.count;
    Vector2 mouse = GetMousePosition ();
    Rectangle panel = PickerPanel ();
    Rectangle list = PickerListRect (panel);
    Rectangle cancel = PickerCancelRect (panel);

    DrawDialogBase (ui, panel, "选择素材");

    if (assetCount == 0) {
        DrawTextEx (ui->font, "没有素材", (Vector2){panel.x + 24.0f, list.y + 20.0f}, UI_FONT_SIZE, 0,
                    (Color){225, 228, 236, 255});
        DrawTextEx (ui->font, "请先导入或拖入图片", (Vector2){panel.x + 24.0f, list.y + 44.0f}, UI_FONT_SIZE, 0,
                    (Color){150, 158, 172, 255});
    } else {
        BeginScissorMode ((int)list.x, (int)list.y, (int)list.width, (int)list.height);

        for (int i = 0; i < assetCount; i++) {
            float rowY = list.y + i * UI_ASSET_ROW_HEIGHT - ui->pickerScroll;
            if (rowY + UI_ASSET_ROW_HEIGHT < list.y || rowY > list.y + list.height) continue;

            Rectangle row = {list.x, rowY, list.width, UI_ASSET_ROW_HEIGHT};
            if (CheckCollisionPointRec (mouse, list) && CheckCollisionPointRec (mouse, row))
                DrawRectangleRec (row, (Color){45, 49, 61, 255});

            Asset *asset = EntityFind (assetIds.ids[i]);
            if (asset == NULL) continue;

            DrawTextureFit (asset->tex,
                            (Rectangle){list.x + 14.0f,
                                        rowY + (UI_ASSET_ROW_HEIGHT - 48.0f) / 2.0f,
                                        56.0f, 48.0f});

            DrawTextEx (ui->font, TextFormat ("素材 %d", i + 1), (Vector2){list.x + 86.0f, rowY + 24.0f},
                        UI_FONT_SIZE, 0, (Color){225, 228, 236, 255});
        }

        EndScissorMode ();

        DrawListScrollbar (list, assetCount, UI_ASSET_ROW_HEIGHT, ui->pickerScroll);
    }

    IdGroupUnload (&assetIds);
    DrawSmallButton (ui->font, cancel, "取消", false, true);
}

static void DrawSmallButton (Font font, Rectangle rect, const char *text, bool danger, bool enabled) {
    Vector2 mouse = GetMousePosition ();
    bool hot = enabled && CheckCollisionPointRec (mouse, rect);

    Color background = !enabled ? (Color){30, 33, 41, 255}
                       : hot    ? (Color){55, 61, 76, 255}
                                : (Color){38, 42, 52, 255};
    Color foreground = !enabled ? (Color){90, 96, 112, 255}
                       : danger ? (Color){240, 120, 120, 255}
                                : (Color){225, 228, 236, 255};

    DrawRectangleRec (rect, background);
    DrawRectangleLinesEx (rect, 1, (Color){70, 76, 92, 255});

    float width = MeasureTextEx (font, text, UI_FONT_SIZE, 0).x;
    DrawTextEx (font, text, (Vector2){rect.x + (rect.width - width) / 2.0f,
                                      rect.y + (rect.height - UI_FONT_SIZE) / 2.0f},
                UI_FONT_SIZE, 0, foreground);
}

static void DrawManager (const Ui *ui) {
    if (!ui->managerOpen) return;

    Vector2 mouse = GetMousePosition ();
    IdGroup assetIds = AssetIds ();
    int assetCount = assetIds.count;
    Rectangle panel = ManagerPanel ();
    Rectangle list = ManagerListRect (panel);
    Rectangle exit = ManagerExitRect (panel);

    DrawDialogBase (ui, panel, "素材管理");

    if (assetCount == 0) {
        DrawTextEx (ui->font, "没有素材", (Vector2){panel.x + 24.0f, list.y + 24.0f}, UI_FONT_SIZE, 0,
                    (Color){225, 228, 236, 255});
    } else {
        BeginScissorMode ((int)list.x, (int)list.y, (int)list.width, (int)list.height);

        for (int i = 0; i < assetCount; i++) {
            float rowY = list.y + i * UI_MANAGER_ROW_HEIGHT - ui->managerScroll;
            if (rowY + UI_MANAGER_ROW_HEIGHT < list.y || rowY > list.y + list.height) continue;

            Rectangle row = {list.x, rowY, list.width, UI_MANAGER_ROW_HEIGHT};
            Asset *asset = EntityFind (assetIds.ids[i]);
            if (asset == NULL) continue;

            if (asset == ui->selectedAsset) {
                DrawRectangleRec (row, (Color){45, 60, 88, 255});
                DrawRectangle ((int)list.x, (int)rowY, 3, UI_MANAGER_ROW_HEIGHT,
                                (Color){84, 158, 255, 255});
            } else if (CheckCollisionPointRec (mouse, list) && CheckCollisionPointRec (mouse, row)) {
                DrawRectangleRec (row, (Color){38, 42, 52, 255});
            }

            DrawTextureFit (asset->tex, (Rectangle){list.x + 16.0f, rowY + 10.0f, 80.0f, 60.0f});

            DrawTextEx (ui->font, asset->name, (Vector2){list.x + 112.0f, rowY + 32.0f}, UI_FONT_SIZE, 0,
                        (Color){225, 228, 236, 255});

            DrawRectangle ((int)list.x, (int)(rowY + UI_MANAGER_ROW_HEIGHT - 1.0f),
                           (int)list.width, 1, (Color){45, 49, 60, 255});
        }

        EndScissorMode ();

        DrawListScrollbar (list, assetCount, UI_MANAGER_ROW_HEIGHT, ui->managerScroll);
    }

    IdGroupUnload (&assetIds);
    Rectangle renameBtn = ManagerRenameRect (panel);
    Rectangle replaceBtn = ManagerReplaceRect (panel);
    Rectangle deleteBtn = ManagerDeleteRect (panel);
    Rectangle importBtn = ManagerImportRect (panel);
    bool hasSelection = (ui->selectedAsset != NULL);

    DrawRectangle ((int)(panel.x + 24.0f), (int)(panel.y + panel.height - UI_MANAGER_FOOTER),
                   (int)(panel.width - 48.0f), 1, (Color){58, 63, 76, 255});
    DrawSmallButton (ui->font, renameBtn, "重命名", false, hasSelection);
    DrawSmallButton (ui->font, replaceBtn, "更换", false, hasSelection);
    DrawSmallButton (ui->font, deleteBtn, "删除", true, hasSelection);
    DrawSmallButton (ui->font, importBtn, "导入", false, true);
    DrawSmallButton (ui->font, exit, "确定", false, true);
}

static void DrawObjectManager (const Ui *ui) {
    if (!ui->objectManagerOpen) return;

    Vector2 mouse = GetMousePosition ();
    IdGroup objectIds = ObjectIds ();
    int objectCount = objectIds.count;
    Rectangle panel = ManagerPanel ();
    Rectangle list = ManagerListRect (panel);

    DrawDialogBase (ui, panel, "物件管理");

    if (objectCount == 0) {
        DrawTextEx (ui->font, "没有物件", (Vector2){panel.x + 24.0f, list.y + 24.0f}, UI_FONT_SIZE, 0,
                    (Color){225, 228, 236, 255});
    } else {
        BeginScissorMode ((int)list.x, (int)list.y, (int)list.width, (int)list.height);

        for (int i = 0; i < objectCount; i++) {
            float rowY = list.y + i * UI_MANAGER_ROW_HEIGHT - ui->objectScroll;
            if (rowY + UI_MANAGER_ROW_HEIGHT < list.y || rowY > list.y + list.height) continue;

            Rectangle row = {list.x, rowY, list.width, UI_MANAGER_ROW_HEIGHT};
            Object *object = EntityFind (objectIds.ids[i]);
            if (object == NULL) continue;

            bool unavailable = object->lockOwnerId != 0 &&
                               object->lockOwnerId != NetClientUserId() &&
                               object != ui->selectedObject;
            if (object == ui->selectedObject) {
                DrawRectangleRec (row, (Color){45, 60, 88, 255});
                DrawRectangle ((int)list.x, (int)rowY, 3, UI_MANAGER_ROW_HEIGHT,
                                (Color){84, 158, 255, 255});
            } else if (unavailable) {
                DrawRectangleRec (row, (Color){58, 61, 68, 255});
                DrawRectangle ((int)list.x, (int)rowY, 4, UI_MANAGER_ROW_HEIGHT,
                               (Color){120, 124, 134, 255});
            } else if (CheckCollisionPointRec (mouse, list) && CheckCollisionPointRec (mouse, row)) {
                DrawRectangleRec (row, (Color){38, 42, 52, 255});
            }

            Rectangle thumb = {list.x + 16.0f, rowY + 10.0f, 80.0f, 60.0f};
            Asset *firstAsset = ObjectDisplayedAsset (object);
            if (firstAsset != NULL) {
                DrawTextureFit (firstAsset->tex, thumb);
            } else {
                DrawRectangleRec (thumb, (Color){20, 22, 28, 255});
                DrawRectangleLinesEx (thumb, 1, (Color){60, 65, 80, 255});
                DrawTextEx (ui->font, "空", (Vector2){thumb.x + 32.0f, thumb.y + 22.0f}, UI_FONT_SIZE, 0,
                            (Color){110, 116, 132, 255});
            }

            Color nameColor = unavailable ? (Color){215, 218, 225, 255}
                                          : (Color){225, 228, 236, 255};
            Color layerColor = unavailable ? (Color){160, 165, 175, 255}
                                           : (Color){150, 158, 172, 255};
            DrawTextEx (ui->font, object->name, (Vector2){list.x + 112.0f, rowY + 18.0f}, UI_FONT_SIZE, 0,
                        nameColor);
            DrawTextEx (ui->font, TextFormat ("层数 %d", object->layer),
                        (Vector2){list.x + 112.0f, rowY + 44.0f}, UI_FONT_SIZE, 0, layerColor);

            if (unavailable) {
                User *owner = EntityFind (object->lockOwnerId);
                const char *ownerText = owner ? owner->name : "";
                float ownerWidth = MeasureTextEx (ui->font, ownerText, UI_FONT_SIZE, 0).x;
                Color ownerColor = owner != NULL
                                       ? UserColor (owner)
                                       : (Color){205, 209, 218, 255};
                DrawTextEx (ui->font, ownerText,
                            (Vector2){list.x + list.width - ownerWidth - 16.0f, rowY + 32.0f},
                            UI_FONT_SIZE, 0, ownerColor);
            }

            DrawRectangle ((int)list.x, (int)(rowY + UI_MANAGER_ROW_HEIGHT - 1.0f),
                           (int)list.width, 1, (Color){45, 49, 60, 255});
        }

        EndScissorMode ();

        DrawListScrollbar (list, objectCount, UI_MANAGER_ROW_HEIGHT, ui->objectScroll);
    }

    IdGroupUnload (&objectIds);
    bool hasSelection = (ui->selectedObject != NULL);
    DrawRectangle ((int)(panel.x + 24.0f), (int)(panel.y + panel.height - UI_MANAGER_FOOTER),
                   (int)(panel.width - 48.0f), 1, (Color){58, 63, 76, 255});
    DrawSmallButton (ui->font, ObjectRenameRect (panel), "重命名", false, hasSelection);
    DrawSmallButton (ui->font, ObjectManageRect (panel), "管理", false, hasSelection);
    DrawSmallButton (ui->font, ObjectLayerRect (panel), "浮动", false, hasSelection);
    DrawSmallButton (ui->font, ObjectDeleteRect (panel), "删除", true, hasSelection);
    DrawSmallButton (ui->font, ObjectCreateRect (panel), "新建", false, true);
    DrawSmallButton (ui->font, ObjectDoneRect (panel), "确定", false, true);
}

static void DrawObjectAssets (const Ui *ui) {
    if (!ui->objectAssetsOpen || ui->assetsObject == NULL) return;

    Object *object = ui->assetsObject;
    int count = object->assets.count;
    Vector2 mouse = GetMousePosition ();
    Rectangle panel = ObjectAssetsPanel ();
    Rectangle list = ObjectAssetsListRect (panel);

    DrawDialogBase (ui, panel, TextFormat ("物件素材: %s", object->name));

    if (count == 0) {
        DrawTextEx (ui->font, "没有素材", (Vector2){panel.x + 24.0f, list.y + 24.0f}, UI_FONT_SIZE, 0,
                    (Color){225, 228, 236, 255});
    } else {
        BeginScissorMode ((int)list.x, (int)list.y, (int)list.width, (int)list.height);

        for (int i = 0; i < count; i++) {
            float rowY = list.y + i * UI_OBJECT_ASSET_ROW_HEIGHT - ui->assetsObjectScroll;
            if (rowY + UI_OBJECT_ASSET_ROW_HEIGHT < list.y || rowY > list.y + list.height) continue;

            Rectangle row = {list.x, rowY, list.width, UI_OBJECT_ASSET_ROW_HEIGHT};
            Asset *asset = EntityFind (object->assets.ids[i]);
            if (asset == NULL) continue;

            int usedIndex = object->assetIndex;
            if (usedIndex < 0 || usedIndex >= count) usedIndex = 0;
            bool used = (i == usedIndex);

            if (i == ui->assetsObjectSelected) {
                DrawRectangleRec (row, (Color){45, 60, 88, 255});
                DrawRectangle ((int)list.x, (int)rowY, 3, UI_OBJECT_ASSET_ROW_HEIGHT,
                                (Color){84, 158, 255, 255});
            } else if (CheckCollisionPointRec (mouse, list) && CheckCollisionPointRec (mouse, row)) {
                DrawRectangleRec (row, (Color){38, 42, 52, 255});
            }

            Color indexColor = used ? (Color){110, 220, 135, 255}
                                     : (Color){150, 158, 172, 255};
            DrawTextEx (ui->font, TextFormat ("#%d", i), (Vector2){list.x + 16.0f, rowY + 24.0f}, UI_FONT_SIZE, 0,
                        indexColor);

            DrawTextureFit (asset->tex, (Rectangle){list.x + 64.0f, rowY + 8.0f, 60.0f, 48.0f});

            Color nameColor = used ? (Color){110, 220, 135, 255}
                                   : (Color){225, 228, 236, 255};
            DrawTextEx (ui->font, asset->name, (Vector2){list.x + 136.0f, rowY + 24.0f}, UI_FONT_SIZE, 0,
                        nameColor);

            DrawRectangle ((int)list.x, (int)(rowY + UI_OBJECT_ASSET_ROW_HEIGHT - 1.0f),
                           (int)list.width, 1, (Color){45, 49, 60, 255});
        }

        EndScissorMode ();

        DrawListScrollbar (list, count, UI_OBJECT_ASSET_ROW_HEIGHT, ui->assetsObjectScroll);
    }

    bool hasSelection = (ui->assetsObjectSelected >= 0 && ui->assetsObjectSelected < count);
    bool canMoveUp = hasSelection && ui->assetsObjectSelected > 0;
    bool canMoveDown = hasSelection && ui->assetsObjectSelected < count - 1;

    DrawRectangle ((int)(panel.x + 24.0f),
                   (int)(panel.y + panel.height - 24.0f - UI_MANAGER_FOOTER),
                   (int)(panel.width - 48.0f), 1, (Color){58, 63, 76, 255});
    DrawSmallButton (ui->font, ObjectAssetUseRect (panel), "使用", false, hasSelection);
    DrawSmallButton (ui->font, ObjectAssetUpRect (panel), "上移", false, canMoveUp);
    DrawSmallButton (ui->font, ObjectAssetDownRect (panel), "下移", false, canMoveDown);
    DrawSmallButton (ui->font, ObjectAssetRemoveRect (panel), "移除", true, hasSelection);
    DrawSmallButton (ui->font, ObjectAssetAddRect (panel), "添加", false, true);
    DrawSmallButton (ui->font, ObjectAssetDoneRect (panel), "确定", false, true);
}

static void DrawConnectField (const Ui *ui, Rectangle input, const char *text,
                             const char *placeholder, bool active) {
    DrawRectangleRec (input, (Color){20, 22, 28, 255});
    DrawRectangleLinesEx (input, 1, active ? (Color){70, 130, 200, 255}
                                            : (Color){70, 76, 92, 255});

    BeginScissorMode ((int)input.x + 1, (int)input.y + 1, (int)input.width - 2, (int)input.height - 2);
    if (text[0] != '\0') {
        DrawTextEx (ui->font, text, (Vector2){input.x + 8.0f, input.y + 5.0f}, UI_FONT_SIZE, 0,
                    (Color){235, 238, 245, 255});
    } else {
        DrawTextEx (ui->font, placeholder, (Vector2){input.x + 8.0f, input.y + 5.0f}, UI_FONT_SIZE, 0,
                    (Color){112, 120, 136, 255});
    }

    if (active) {
        float textWidth = MeasureTextEx (ui->font, text, UI_FONT_SIZE, 0).x;
        float caretX = input.x + 8.0f + textWidth;
        if (caretX < input.x + input.width - 2.0f)
            DrawRectangle ((int)caretX, (int)(input.y + 5.0f), 1, 24,
                           (Color){235, 238, 245, 255});
    }
    EndScissorMode ();
}

static void DrawTextInputDialog (const Ui *ui, const char *title, const char *text) {
    Rectangle panel = RenamePanel ();
    Rectangle input = RenameInputRect (panel);
    Rectangle ok = RenameOkRect (panel);
    Rectangle cancel = RenameCancelRect (panel);

    DrawRectangle (0, 0, GetScreenWidth (), GetScreenHeight (), Fade (BLACK, 0.45f));
    DrawRectangleRec (panel, (Color){30, 33, 41, 255});
    DrawRectangleLinesEx (panel, 1, (Color){80, 86, 104, 255});
    DrawTextEx (ui->font, title, (Vector2){panel.x + 24.0f, panel.y + 18.0f}, UI_FONT_SIZE, 0,
                (Color){240, 242, 248, 255});

    DrawRectangleRec (input, (Color){20, 22, 28, 255});
    DrawRectangleLinesEx (input, 1, (Color){70, 76, 92, 255});

    BeginScissorMode ((int)input.x + 1, (int)input.y + 1, (int)input.width - 2, (int)input.height - 2);
    DrawTextEx (ui->font, text, (Vector2){input.x + 8.0f, input.y + 5.0f}, UI_FONT_SIZE, 0,
                (Color){235, 238, 245, 255});

    float textWidth = MeasureTextEx (ui->font, text, UI_FONT_SIZE, 0).x;
    float caretX = input.x + 8.0f + textWidth;
    if (caretX < input.x + input.width - 2.0f)
        DrawRectangle ((int)caretX, (int)(input.y + 5.0f), 1, 24, (Color){235, 238, 245, 255});
    EndScissorMode ();

    DrawSmallButton (ui->font, ok, "确定", false, true);
    DrawSmallButton (ui->font, cancel, "取消", false, true);
}

static void DrawRename (const Ui *ui) {
    if (!ui->renameOpen) return;
    DrawTextInputDialog (ui, ui->renameIsObject ? "重命名物件" : "重命名素材", ui->renameText);
}

static void DrawLayer (const Ui *ui) {
    if (!ui->layerOpen) return;
    DrawTextInputDialog (ui, "修改层数", ui->layerText);
}

static void DrawConnect (const Ui *ui) {
    if (!ui->connectOpen) return;

    Rectangle panel = ConnectPanel ();
    Rectangle user = ConnectUserRect (panel);
    Rectangle host = ConnectHostRect (panel);
    Rectangle port = ConnectPortRect (panel);
    Rectangle ok = RenameOkRect (panel);
    Rectangle cancel = RenameCancelRect (panel);

    DrawRectangle (0, 0, GetScreenWidth (), GetScreenHeight (), Fade (BLACK, 0.55f));
    DrawRectangleRec (panel, (Color){26, 29, 36, 255});
    DrawRectangleLinesEx (panel, 1, (Color){70, 76, 92, 255});
    DrawTextEx (ui->font, "连接服务器", (Vector2){panel.x + 24.0f, panel.y + 16.0f}, UI_FONT_SIZE, 0,
                (Color){235, 238, 245, 255});
    DrawRectangle ((int)(panel.x + 24.0f), (int)(panel.y + 43.0f),
                   (int)(panel.width - 48.0f), 1, (Color){58, 63, 76, 255});

    DrawTextEx (ui->font, "用户名", (Vector2){panel.x + 24.0f, panel.y + 54.0f}, UI_FONT_SIZE, 0,
                (Color){190, 196, 208, 255});
    DrawConnectField (ui, user, ui->connectUser, "user", ui->connectField == 0);

    DrawTextEx (ui->font, "IP", (Vector2){panel.x + 24.0f, panel.y + 120.0f}, UI_FONT_SIZE, 0,
                (Color){190, 196, 208, 255});
    DrawConnectField (ui, host, ui->connectHost, "127.0.0.1", ui->connectField == 1);

    DrawTextEx (ui->font, "端口", (Vector2){panel.x + 24.0f, panel.y + 186.0f}, UI_FONT_SIZE, 0,
                (Color){190, 196, 208, 255});
    DrawConnectField (ui, port, ui->connectPort, "7777", ui->connectField == 2);

    DrawSmallButton (ui->font, ok, "连接", false, true);
    DrawSmallButton (ui->font, cancel, "取消", false, true);
}

static void DrawObjectContext (const Ui *ui) {
    if (!ui->objectContextOpen) return;

    Vector2 mouse = GetMousePosition ();
    Rectangle menu = ObjectContextRect (ui);
    Object *object = ui->contextObject;
    int menuCount = MenuCount (object != NULL ? object->id : 0);
    static const char *labels[] = {"查看", "脚本", "删除"};
    static const char *viewLabels[] = {"回到原点", "缩放", "背景", "脚本"};

    DrawRectangle ((int)menu.x + 3, (int)menu.y + 4, (int)menu.width, (int)menu.height,
                   Fade (BLACK, 0.28f));
    DrawRectangleRec (menu, (Color){28, 31, 39, 255});

    for (int i = 0; i < menuCount; i++) {
        Rectangle item = {menu.x, menu.y + i * UI_ITEM_HEIGHT, menu.width, UI_ITEM_HEIGHT};
        if (CheckCollisionPointRec (mouse, item))
            DrawRectangleRec (item, (Color){45, 49, 61, 255});
        const char *name = MenuName (object != NULL ? object->id : 0, i);
        const char *nameText = name != NULL ? name : "";
        DrawTextEx (ui->font, nameText,
                    (Vector2){item.x + 12.0f, MenuTextY (ui->font, nameText, item)},
                    UI_FONT_SIZE, 0, (Color){225, 228, 236, 255});
    }

    float separatorY = menu.y + menuCount * UI_ITEM_HEIGHT;
    if (menuCount > 0)
        DrawRectangle ((int)menu.x, (int)(separatorY + 3.0f), (int)menu.width, 1,
                       (Color){60, 65, 80, 255});
    float actionY = separatorY + (menuCount > 0 ? UI_CONTEXT_SEPARATOR_HEIGHT : 0.0f);

    if (object != NULL) {
        for (int i = 0; i < 3; i++) {
            Rectangle item = {menu.x, actionY + i * UI_ITEM_HEIGHT, menu.width, UI_ITEM_HEIGHT};
            if (CheckCollisionPointRec (mouse, item))
                DrawRectangleRec (item, (Color){45, 49, 61, 255});
            Color color = (i == 2) ? (Color){240, 120, 120, 255}
                                   : (Color){225, 228, 236, 255};
            DrawTextEx (ui->font, labels[i],
                        (Vector2){item.x + 12.0f, MenuTextY (ui->font, labels[i], item)},
                        UI_FONT_SIZE, 0, color);
        }
    } else {
        for (int i = 0; i < 4; i++) {
            Rectangle item = {menu.x, actionY + i * UI_ITEM_HEIGHT, menu.width, UI_ITEM_HEIGHT};
            if (CheckCollisionPointRec (mouse, item))
                DrawRectangleRec (item, (Color){45, 49, 61, 255});
            DrawTextEx (ui->font, viewLabels[i],
                        (Vector2){item.x + 12.0f,
                                  MenuTextY (ui->font, viewLabels[i], item)},
                        UI_FONT_SIZE, 0, (Color){225, 228, 236, 255});
            if (i == 1 || i == 2)
                DrawTextEx (ui->font, ">",
                            (Vector2){item.x + item.width - 20.0f,
                                      MenuTextY (ui->font, ">", item)},
                            UI_FONT_SIZE, 0, (Color){150, 158, 172, 255});
        }

        if (ui->submenuOpen >= 0) {
            Rectangle submenu = ViewContextSubmenuRect (ui);
            const char **items = (ui->submenuOpen == VIEW_SUBMENU_ZOOM) ? viewZoomLabels
                                                                        : viewBackgroundLabels;
            int count = (int)(submenu.height / UI_ITEM_HEIGHT);
            DrawRectangleRec (submenu, (Color){28, 31, 39, 255});
            for (int i = 0; i < count; i++) {
                Rectangle item = {submenu.x, submenu.y + i * UI_ITEM_HEIGHT, submenu.width,
                                  UI_ITEM_HEIGHT};
                if (CheckCollisionPointRec (mouse, item))
                    DrawRectangleRec (item, (Color){45, 49, 61, 255});
                DrawTextEx (ui->font, items[i],
                            (Vector2){item.x + 12.0f,
                                      MenuTextY (ui->font, items[i], item)},
                            UI_FONT_SIZE, 0, (Color){225, 228, 236, 255});
            }
            DrawRectangleLinesEx (submenu, 1, (Color){60, 65, 80, 255});
        }
    }

    DrawRectangleLinesEx (menu, 1, (Color){60, 65, 80, 255});
}

static void DrawMessage (const Ui *ui) {
    float height = 44.0f;
    float gap = 6.0f;
    int slot = 0;

    for (int i = ui->messageCount - 1; i >= 0; i--) {
        float time = ui->messageTimes[i];
        if (time <= 0.0f) continue;

        float textWidth = MeasureTextEx (ui->font, ui->messages[i], UI_FONT_SIZE, 0).x;
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
        if (ui->messageKinds[i] == UI_MESSAGE_ERROR)
            background = (Color){180, 52, 58, 255};
        else if (ui->messageKinds[i] == UI_MESSAGE_SCRIPT)
            background = (Color){86, 91, 99, 255};

        DrawRectangleRounded ((Rectangle){x + 2.0f, y + 4.0f, width, height}, 0.5f, 8,
                              Fade (BLACK, 0.25f * k));
        DrawRectangleRounded ((Rectangle){x, y, width, height}, 0.5f, 8,
                              Fade (background, 0.95f * k));
        DrawTextEx (ui->font, ui->messages[i], (Vector2){x + 24.0f, y + 10.0f},
                    UI_FONT_SIZE, 0.0f, Fade (WHITE, k));
        slot++;
    }
}

static void BuildScriptLine (const TextBuffer *text, size_t start, size_t end,
                              TextBuffer *display) {
    for (size_t i = start; i < end; i++) {
        if (text->data[i] == '\t')
            TextBufferAppend (display, "    ");
        else
            TextBufferAppendChar (display, text->data[i]);
    }
}

static void DrawScript (const Ui *ui) {
    if (!ui->scriptOpen) return;

    Rectangle panel = ScriptPanel ();
    Rectangle code = ScriptCodeRect (panel);
    DrawDialogBase (ui, panel, "脚本");
    DrawRectangleRec (code, (Color){20, 22, 28, 255});
    DrawRectangleLinesEx (code, 1, (Color){70, 76, 92, 255});
    BeginScissorMode ((int)code.x + 1, (int)code.y + 1,
                      (int)code.width - 2, (int)code.height - 2);

    int total = ScriptLineCount (&ui->scriptText);
    int visible = ScriptVisibleLines ();
    int start = (int)ui->scriptScroll;
    if (start < 0) start = 0;
    if (start > total - 1) start = total - 1;

    size_t pos = 0;
    int line = 0;
    while (line < start && pos < ui->scriptText.length) {
        if (ui->scriptText.data[pos] == '\n') line++;
        pos++;
    }

    for (int i = start; i < total && i < start + visible; i++) {
        size_t end = pos;
        while (end < ui->scriptText.length && ui->scriptText.data[end] != '\n') end++;

        TextBuffer display = {0};
        BuildScriptLine (&ui->scriptText, pos, end, &display);
        DrawTextEx (ui->monoFont, display.data != NULL ? display.data : "",
                    (Vector2){code.x + 8.0f, code.y + 6.0f + (i - start) * UI_CODE_LINE_HEIGHT},
                    UI_CODE_FONT_SIZE, 0.0f, (Color){225, 228, 236, 255});
        TextBufferFree (&display);

        if (end < ui->scriptText.length)
            pos = end + 1;
        else
            pos = ui->scriptText.length;
    }

    int cursorLine = ScriptLineOfOffset (&ui->scriptText, ui->scriptCursor);
    if (cursorLine >= start && cursorLine < start + visible) {
        size_t cursorStart = ScriptLineStart (&ui->scriptText, cursorLine);
        size_t cursorEnd = ScriptLineEnd (&ui->scriptText, cursorStart);
        size_t prefixEnd = ui->scriptCursor;
        if (prefixEnd < cursorStart) prefixEnd = cursorStart;
        if (prefixEnd > cursorEnd) prefixEnd = cursorEnd;

        TextBuffer display = {0};
        BuildScriptLine (&ui->scriptText, cursorStart, prefixEnd, &display);
        if (fmod (GetTime (), 1.0) < 0.5) {
            float x = code.x + 8.0f + MeasureTextEx (ui->monoFont,
                                                     display.data != NULL ? display.data : "",
                                                     UI_CODE_FONT_SIZE, 0.0f).x;
            float y = code.y + 6.0f + (cursorLine - start) * UI_CODE_LINE_HEIGHT;
            DrawRectangle ((int)x, (int)y, 1, UI_CODE_FONT_SIZE, (Color){225, 228, 236, 255});
        }
        TextBufferFree (&display);
    }

    if (ui->scriptText.length == 0) {
        DrawTextEx (ui->monoFont, "在此输入脚本", (Vector2){code.x + 8.0f, code.y + 6.0f},
                    UI_CODE_FONT_SIZE, 0.0f, (Color){110, 116, 132, 255});
    }

    EndScissorMode ();
    DrawListScrollbar (code, ScriptLineCount (&ui->scriptText), UI_CODE_LINE_HEIGHT, ui->scriptScroll);

    Rectangle self = ScriptSelfRect (panel);
    DrawRectangleRec (self, (Color){20, 22, 28, 255});
    DrawRectangleLinesEx (self, 1, ui->scriptSelfFocused ? (Color){70, 130, 200, 255}
                                                        : (Color){70, 76, 92, 255});
    BeginScissorMode ((int)self.x + 1, (int)self.y + 1,
                      (int)self.width - 2, (int)self.height - 2);
    if (ui->scriptSelf[0] != '\0') {
        DrawTextEx (ui->monoFont, ui->scriptSelf, (Vector2){self.x + 8.0f, self.y + 9.0f},
                    UI_CODE_FONT_SIZE, 0.0f, (Color){235, 238, 245, 255});
    } else {
        DrawTextEx (ui->monoFont, "self 表达式", (Vector2){self.x + 8.0f, self.y + 9.0f},
                    UI_CODE_FONT_SIZE, 0.0f, (Color){112, 120, 136, 255});
    }
    if (ui->scriptSelfFocused && fmod (GetTime (), 1.0) < 0.5) {
        float width = MeasureTextEx (ui->monoFont, ui->scriptSelf, UI_CODE_FONT_SIZE, 0.0f).x;
        DrawRectangle ((int)(self.x + 8.0f + width), (int)(self.y + 8.0f), 1, 18,
                       (Color){235, 238, 245, 255});
    }
    EndScissorMode ();

    DrawSmallButton (ui->font, ScriptSubmitRect (panel), "提交", false, true);
    DrawSmallButton (ui->font, ScriptCancelRect (panel), "取消", false, true);
    DrawTextEx (ui->font, "Ctrl+Enter 提交",
                (Vector2){panel.x + 24.0f, panel.y + panel.height - 48.0f},
                20.0f, 0.0f, (Color){150, 158, 172, 255});
}

static void DrawUserMenuButtons (const Ui *ui) {
    u64 userId = NetClientUserId ();
    if (userId == 0) return;
    int count = MenuCount (userId);
    for (int i = 0; i < count; i++) {
        const char *name = MenuName (userId, i);
        DrawSmallButton (ui->font, UserMenuButtonRect (i), name != NULL ? name : "",
                         false, true);
    }
}

void UiDraw (const Ui *ui) {
    DrawMenuBar (ui);
    DrawUserMenuButtons (ui);
    DrawManager (ui);
    DrawObjectManager (ui);
    DrawObjectAssets (ui);
    DrawPicker (ui);
    DrawRename (ui);
    DrawLayer (ui);
    DrawConnect (ui);
    DrawObjectContext (ui);
    DrawScript (ui);
    DrawMessage (ui);
}
