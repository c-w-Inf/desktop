#include "cutils.h"
#include "rlgl.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#include <emscripten/html5.h>
#else
#include <uv.h>
#endif

#ifndef S_ISDIR
#define S_ISDIR(mode) (((mode) & S_IFMT) == S_IFDIR)
#endif

static CutilsFileChosenFn fileHandler;

#if defined(__EMSCRIPTEN__)
static bool webPressedFrame[3];
static bool webReleasedFrame[3];
static bool webClickedFrame[3];
static float webWheelFrame;

void CutilsInitInput (void) {}

bool CutilsMousePressed (int button) {
    return button >= 0 && button < 3 && webPressedFrame[button];
}

bool CutilsMouseClicked (int button) {
    return button >= 0 && button < 3 && webClickedFrame[button];
}

bool CutilsMouseReleased (int button) {
    return button >= 0 && button < 3 && webReleasedFrame[button];
}

void CutilsMouseReset (void) {
    for (int i = 0; i < 3; i++) {
        webPressedFrame[i] = EM_ASM_INT({
            var value = window.__desktopMousePressed[$0] || 0;
            window.__desktopMousePressed[$0] = 0;
            return value;
        }, i) != 0;
        webReleasedFrame[i] = EM_ASM_INT({
            var value = window.__desktopMouseReleased[$0] || 0;
            window.__desktopMouseReleased[$0] = 0;
            return value;
        }, i) != 0;
        webClickedFrame[i] = EM_ASM_INT({
            var value = window.__desktopMouseClicked[$0] || 0;
            window.__desktopMouseClicked[$0] = 0;
            return value;
        }, i) != 0;
    }
    webWheelFrame = (float)EM_ASM_DOUBLE({
        var value = window.__desktopWheel || 0;
        window.__desktopWheel = 0;
        return value;
    });
}

float CutilsWheelMove (void) {
    return webWheelFrame;
}

#define CUTILS_KEY_QUEUE 128

static int keyFrame[CUTILS_KEY_QUEUE];
static int keyFrameCount;
static int charFrame[CUTILS_KEY_QUEUE];
static int charFrameCount;
static int charFrameRead;

void CutilsKeysReset (void) {
    keyFrameCount = EM_ASM_INT({
        var queue = window.__desktopKeyEvents;
        var count = queue.length;
        if (count > $1) count = $1;
        for (var i = 0; i < count; i++) HEAP32[($0 >> 2) + i] = queue[i];
        queue.splice(0, count);
        return count;
    }, keyFrame, CUTILS_KEY_QUEUE);
    charFrameCount = EM_ASM_INT({
        var queue = window.__desktopChars;
        var count = queue.length;
        if (count > $1) count = $1;
        for (var i = 0; i < count; i++) HEAP32[($0 >> 2) + i] = queue[i];
        queue.splice(0, count);
        return count;
    }, charFrame, CUTILS_KEY_QUEUE);
    charFrameRead = 0;
}

int CutilsCharPressed (void) {
    return charFrameRead < charFrameCount ? charFrame[charFrameRead++] : 0;
}

bool CutilsKeyPressed (int key) {
    for (int i = 0; i < keyFrameCount; i++) {
        if (keyFrame[i] == key) return true;
    }
    return false;
}

bool CutilsKeyDown (int key) {
    return EM_ASM_INT({
        var state = window.__desktopKeyState;
        if ($0 === 341 || $0 === 345) return state[341] || state[345] ? 1 : 0;
        if ($0 === 340 || $0 === 344) return state[340] || state[344] ? 1 : 0;
        return state[$0] ? 1 : 0;
    }, key) != 0;
}

void CutilsBeginScissor (int x, int y, int width, int height) {
    rlDrawRenderBatchActive ();
    rlEnableScissorTest ();
    float scale = GetWindowScaleDPI ().x;
    int screenHeight = (int)(GetScreenHeight ()*scale);
    rlScissor ((int)(x*scale), screenHeight - (int)((y + height)*scale),
               (int)(width*scale), (int)(height*scale));
}

void CutilsEndScissor (void) {
    rlDrawRenderBatchActive ();
    rlDisableScissorTest ();
}
#else
void CutilsInitInput (void) {}
static bool mouseWasDown[3];
static bool mousePressedFrame[3];
static bool mouseClickedFrame[3];
static bool mouseReleasedFrame[3];
bool CutilsMousePressed (int button) {
    return button >= 0 && button < 3 && mousePressedFrame[button];
}
bool CutilsMouseClicked (int button) {
    return button >= 0 && button < 3 && mouseClickedFrame[button];
}
bool CutilsMouseReleased (int button) {
    return button >= 0 && button < 3 && mouseReleasedFrame[button];
}
void CutilsMouseReset (void) {
    for (int i = 0; i < 3; i++) {
        bool down = IsMouseButtonDown (i);
        mousePressedFrame[i] = IsMouseButtonPressed (i) || (down && !mouseWasDown[i]);
        mouseClickedFrame[i] = IsMouseButtonPressed (i);
        mouseReleasedFrame[i] = IsMouseButtonReleased (i);
        mouseWasDown[i] = down;
    }
}

void CutilsKeysReset (void) {}
int CutilsCharPressed (void) { return GetCharPressed (); }
float CutilsWheelMove (void) { return GetMouseWheelMove (); }
bool CutilsKeyPressed (int key) { return IsKeyPressed (key); }
bool CutilsKeyDown (int key) { return IsKeyDown (key); }

void CutilsBeginScissor (int x, int y, int width, int height) {
    BeginScissorMode (x, y, width, height);
}

void CutilsEndScissor (void) {
    EndScissorMode ();
}
#endif


static bool cutilsCustomFont;
static bool cutilsCustomMonoFont;

Font CutilsFont;
Font CutilsMonoFont;

static int *BuildFontCodepoints (int *count) {
    static const int ranges[][2] = {
        {0x0020, 0x007E}, {0x00A0, 0x024F}, {0x2000, 0x206F},
        {0x3000, 0x303F}, {0x4E00, 0x9FFF}, {0xFF00, 0xFFEF}
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
    for (size_t i = 0; i < sizeof (ranges) / sizeof (ranges[0]); i++)
        for (int cp = ranges[i][0]; cp <= ranges[i][1]; cp++)
            codepoints[total++] = cp;
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

void CutilsLoadFonts (void) {
    int atlasSize = 24;
    int monoAtlasSize = 24;
#if defined(__EMSCRIPTEN__)
    float dpr = (float)EM_ASM_DOUBLE({ return window.devicePixelRatio || 1.0; });
    if (dpr < 1.0f) dpr = 1.0f;
    if (dpr > 4.0f) dpr = 4.0f;
    atlasSize = (int)(24.0f*dpr);
    monoAtlasSize = (int)(22.0f*dpr);
#endif
    Font sans = LoadAppFont ("WenQuanYiMicroHei.ttf", atlasSize);
    if (!IsLoadedFont (sans)) sans = LoadAppFont ("unifont.otf", atlasSize);

    if (IsLoadedFont (sans)) {
        if (cutilsCustomFont && CutilsFont.texture.id != sans.texture.id)
            UnloadFont (CutilsFont);
        CutilsFont = sans;
        cutilsCustomFont = true;
    } else if (!cutilsCustomFont) {
        CutilsFont = GetFontDefault ();
    }

    Font mono = LoadAppFont ("WenQuanYiMicroHeiMono.ttf", monoAtlasSize);
    if (IsLoadedFont (mono)) {
        if (cutilsCustomMonoFont && CutilsMonoFont.texture.id != mono.texture.id)
            UnloadFont (CutilsMonoFont);
        CutilsMonoFont = mono;
        cutilsCustomMonoFont = true;
    } else {
        if (cutilsCustomMonoFont && CutilsMonoFont.texture.id != CutilsFont.texture.id)
            UnloadFont (CutilsMonoFont);
        CutilsMonoFont = CutilsFont;
        cutilsCustomMonoFont = false;
    }

    SetTextureFilter (CutilsFont.texture, TEXTURE_FILTER_POINT);
    SetTextureFilter (CutilsMonoFont.texture, TEXTURE_FILTER_POINT);
}

void CutilsUnloadFonts (void) {
    if (cutilsCustomMonoFont && CutilsMonoFont.texture.id != CutilsFont.texture.id)
        UnloadFont (CutilsMonoFont);
    if (cutilsCustomFont) UnloadFont (CutilsFont);
}


#if defined(__EMSCRIPTEN__)
void CutilsPump(void) {
}

bool CutilsOpenRemote (const char *host, int port, const char *username) {
    char endpoint[512];
    snprintf (endpoint, sizeof (endpoint), "ws://%s:%d", host, port);
    EM_ASM({
        const endpoint = UTF8ToString($0);
        const name = UTF8ToString($1);
        window.open("?server=" + encodeURIComponent(endpoint) +
                    "&name=" + encodeURIComponent(name), "_blank");
    }, endpoint, username);
    return true;
}
#else
void CutilsPump (void) {
    uv_run (uv_default_loop (), UV_RUN_NOWAIT);
}

static void CutilsRemoteClosed (uv_handle_t *handle) {
    free (handle);
}

static void CutilsRemoteExited (uv_process_t *process, int64_t status,
                                int signal) {
    (void)status;
    (void)signal;
    uv_close ((uv_handle_t *)process, CutilsRemoteClosed);
}

bool CutilsOpenRemote (const char *host, int port, const char *username) {
    char portText[16];
    snprintf (portText, sizeof (portText), "%d", port);

    char executable[1024];
    size_t size = sizeof (executable);
    if (uv_exepath (executable, &size) != 0 || size >= sizeof (executable))
        return false;
    executable[size] = '\0';

    uv_process_t *process = calloc (1, sizeof (*process));
    if (process == NULL) return false;

    char *arguments[] = {executable, (char *)host, portText,
                         "--name", (char *)username, NULL};
    uv_process_options_t options;
    memset (&options, 0, sizeof (options));
    options.exit_cb = CutilsRemoteExited;
    options.file = executable;
    options.args = arguments;
#ifdef _WIN32
    options.flags |= UV_PROCESS_WINDOWS_HIDE;
#endif
    if (uv_spawn (uv_default_loop (), process, &options) != 0) {
        free (process);
        return false;
    }
    return true;
}
#endif

void CutilsSetFileHandler (CutilsFileChosenFn handler) {
    fileHandler = handler;
}

#if !defined(__EMSCRIPTEN__)
#include "tinyfiledialogs.h"

static const char *imagePatterns[] = {
    "*.png", "*.jpg", "*.jpeg", "*.bmp", "*.tga",
    "*.gif", "*.qoi", "*.psd"
};
static const char *importPatterns[] = {
    "*.dsk", "*.png", "*.jpg", "*.jpeg", "*.bmp",
    "*.tga", "*.gif", "*.qoi", "*.psd"
};

static CutilsDialogResult CopySelected (const char *selected, char *path,
                                        size_t size) {
    if (selected == NULL || selected[0] == '\0')
        return CUTILS_DIALOG_CANCELLED;
    size_t length = strlen (selected);
    if (length >= size) return CUTILS_DIALOG_TOO_LONG;
    memcpy (path, selected, length + 1);
    return CUTILS_DIALOG_OK;
}

static void PrepareDialogs (void) {
#ifdef _WIN32
    tinyfd_winUtf8 = 1;
#endif
}
#endif

#if defined(__EMSCRIPTEN__)
EM_JS(void, CutilsPickImportFile, (void), {
    const input = document.createElement("input");
    input.type = "file";
    input.accept = ".dsk,image/*";
    input.style.display = "none";
    document.body.appendChild(input);
    input.onchange = () => {
        const file = input.files[0];
        if (!file) {
            document.body.removeChild(input);
            return;
        }
        const reader = new FileReader();
        reader.onload = () => {
            const path = "/tmp/import-" + Date.now() + "-" + file.name;
            try {
                try { FS.mkdir("/tmp"); } catch (error) {}
                FS.writeFile(path, new Uint8Array(reader.result));
                ccall("CutilsWebFileSelected", null, ["string"], [path]);
            } catch (error) {
                console.error(error);
            }
            document.body.removeChild(input);
        };
        reader.readAsArrayBuffer(file);
    };
    input.click();
});

EMSCRIPTEN_KEEPALIVE
void CutilsWebFileSelected (const char *path) {
    if (fileHandler != NULL) fileHandler (path);
}
#endif

CutilsDialogResult CutilsOpenImage (char *path, size_t size) {
#if defined(__EMSCRIPTEN__)
    (void)path;
    (void)size;
    if (fileHandler == NULL) return CUTILS_DIALOG_FAILED;
    CutilsPickImportFile ();
    return CUTILS_DIALOG_PENDING;
#else
    PrepareDialogs ();
    const char *selected = tinyfd_openFileDialog (
        "选择图片", "", 8, imagePatterns, "图片", 0);
    return CopySelected (selected, path, size);
#endif
}

CutilsDialogResult CutilsOpenImport (char *path, size_t size) {
#if defined(__EMSCRIPTEN__)
    (void)path;
    (void)size;
    if (fileHandler == NULL) return CUTILS_DIALOG_FAILED;
    CutilsPickImportFile ();
    return CUTILS_DIALOG_PENDING;
#else
    PrepareDialogs ();
    const char *selected = tinyfd_openFileDialog (
        "导入", "", 9, importPatterns, "支持的文件", 0);
    return CopySelected (selected, path, size);
#endif
}

CutilsDialogResult CutilsOpenFolder (char *path, size_t size) {
#if defined(__EMSCRIPTEN__)
    (void)path;
    (void)size;
    return CUTILS_DIALOG_FAILED;
#else
    PrepareDialogs ();
    const char *selected = tinyfd_selectFolderDialog ("导入文件夹", "");
    return CopySelected (selected, path, size);
#endif
}

CutilsDialogResult CutilsSaveDsk (char *path, size_t size) {
#if defined(__EMSCRIPTEN__)
    (void)path;
    (void)size;
    return CUTILS_DIALOG_FAILED;
#else
    PrepareDialogs ();
    const char *selected = tinyfd_saveFileDialog (
        "导出存档", "desktop.dsk", 1,
        (const char *[]){ "*.dsk" }, "存档");
    if (selected == NULL || selected[0] == '\0')
        return CUTILS_DIALOG_CANCELLED;
    size_t length = strlen (selected);
    if (length + 4 >= size) return CUTILS_DIALOG_TOO_LONG;
    memcpy (path, selected, length);
    if (length < 4 || strcmp (path + length - 4, ".dsk") != 0) {
        memcpy (path + length, ".dsk", 4);
        length += 4;
    }
    path[length] = '\0';
    return CUTILS_DIALOG_OK;
#endif
}

bool PathIsDirectory (const char *path) {
    if (path == NULL || path[0] == '\0') return false;
    struct stat info;
    return stat (path, &info) == 0 && S_ISDIR (info.st_mode);
}
