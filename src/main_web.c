#if defined(__EMSCRIPTEN__)

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <emscripten.h>
#include <emscripten/html5.h>

#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"
#include "assets.h"
#include "entity.h"
#include "client.h"
#include "cutils.h"
#include "netclient.h"
#include "object.h"
#include "ui.h"
#include "user.h"
#include "view.h"

#define WEB_DEFAULT_SERVER "wss://dsk.cwinf.top/dsk/"

static const char *webError;

static void WebLocationPart (const char *script, char *text, size_t size) {
    const char *value = emscripten_run_script_string (script);
    snprintf (text, size, "%s", value != NULL ? value : "");
}

static void WebUrlDecode (char *text) {
    char *read = text;
    char *write = text;
    while (*read != '\0') {
        if (*read == '%' && read[1] != '\0' && read[2] != '\0') {
            int high = read[1] >= '0' && read[1] <= '9' ? read[1] - '0'
                     : read[1] >= 'A' && read[1] <= 'F' ? read[1] - 'A' + 10
                     : read[1] >= 'a' && read[1] <= 'f' ? read[1] - 'a' + 10
                     : -1;
            int low = read[2] >= '0' && read[2] <= '9' ? read[2] - '0'
                    : read[2] >= 'A' && read[2] <= 'F' ? read[2] - 'A' + 10
                    : read[2] >= 'a' && read[2] <= 'f' ? read[2] - 'a' + 10
                    : -1;
            if (high >= 0 && low >= 0) {
                *write++ = (char)((high << 4) | low);
                read += 3;
                continue;
            }
        }
        *write++ = *read++;
    }
    *write = '\0';
}

static bool WebQueryValue (const char *query, const char *key,
                           char *text, size_t size) {
    size_t keyLength = strlen (key);
    const char *cursor = query;
    while (cursor != NULL && *cursor != '\0') {
        if (*cursor == '?' || *cursor == '&') cursor++;
        if (strncmp (cursor, key, keyLength) == 0 && cursor[keyLength] == '=') {
            const char *value = cursor + keyLength + 1;
            const char *end = strchr (value, '&');
            size_t length = end != NULL ? (size_t)(end - value) : strlen (value);
            if (length >= size) length = size - 1;
            memcpy (text, value, length);
            text[length] = '\0';
            return text[0] != '\0';
        }
        cursor = strchr (cursor, '&');
    }
    return false;
}

static bool webFontLoaded;
static bool webFontFailed;

EMSCRIPTEN_KEEPALIVE
void WebFontReady (int ok) {
    webFontLoaded = true;
    webFontFailed = !ok;
}

static void WebLoadFont (void) {
    webFontLoaded = false;
    webFontFailed = false;
    emscripten_run_script ("imageCanvasFetchFont()");
    while (!webFontLoaded) emscripten_sleep (10);
    if (webFontFailed) fprintf (stderr, "Web font fetch failed\n");
}

static void WebAskName (char *name, size_t size) {
    const char *value = emscripten_run_script_string (
        "(function(){var n=window.prompt('请输入用户名','');"
        "if(n===null||n===undefined)return '';"
        "return n.trim();})()");
    if (value != NULL && value[0] != '\0') {
        snprintf (name, size, "%s", value);
    } else {
        snprintf (name, size, "web-%d",
                  EM_ASM_INT({ return (int)(Math.random () * 2147483647.0); }));
    }
}

static void WebErrorFrame (void) {
    BeginDrawing ();
    ClearBackground (RAYWHITE);
    DrawText ("Image Canvas Web", 40, 40, 30, DARKGRAY);
    if (webError != NULL)
        DrawText (webError, 40, 90, 18, RED);
    EndDrawing ();
}


static float WebDpr (void) {
    return (float)EM_ASM_DOUBLE({ return window.devicePixelRatio || 1.0; });
}

static int webCssWidth;
static int webCssHeight;
static float webDpr = 1.0f;

static void WebApplyProjection (int width, int height) {
    rlViewport (0, 0, (int)(webCssWidth*webDpr), (int)(webCssHeight*webDpr));
    rlMatrixMode (RL_PROJECTION);
    rlLoadIdentity ();
    rlOrtho (0, width, height, 0, 0, 1);
    rlMatrixMode (RL_MODELVIEW);
    rlLoadIdentity ();
}

static void WebConfigureCanvas (int cssWidth, int cssHeight, float dpr) {
    int pixelWidth = (int)(cssWidth*dpr);
    int pixelHeight = (int)(cssHeight*dpr);
    EM_ASM({
        var canvas = document.getElementById("canvas");
        if (!canvas) return;
        canvas.width = $0;
        canvas.height = $1;
        canvas.style.setProperty("width", $2 + "px", "important");
        canvas.style.setProperty("height", $3 + "px", "important");
        canvas.style.position = "fixed";
        canvas.style.left = "0";
        canvas.style.top = "0";
        canvas.style.margin = "0";
        canvas.style.zIndex = "10";
        canvas.style.imageRendering = "auto";
    }, pixelWidth, pixelHeight, cssWidth, cssHeight);
    webCssWidth = cssWidth;
    webCssHeight = cssHeight;
    webDpr = dpr;
    WebApplyProjection (cssWidth, cssHeight);
}

static void WebSyncCanvas (void) {
    int cssWidth = EM_ASM_INT({ return window.innerWidth; });
    int cssHeight = EM_ASM_INT({ return window.innerHeight; });
    EM_ASM({
        var canvas = document.getElementById("canvas");
        if (canvas) {
            canvas.style.setProperty("width", $0 + "px", "important");
            canvas.style.setProperty("height", $1 + "px", "important");
        }
    }, cssWidth, cssHeight);
    int canvasWidth = EM_ASM_INT({
        var canvas = document.getElementById("canvas");
        return canvas ? canvas.width : 0;
    });
    int canvasHeight = EM_ASM_INT({
        var canvas = document.getElementById("canvas");
        return canvas ? canvas.height : 0;
    });
    float dpr = WebDpr ();
    int pixelWidth = (int)(cssWidth*dpr);
    int pixelHeight = (int)(cssHeight*dpr);
    if (cssWidth != webCssWidth || cssHeight != webCssHeight || dpr != webDpr ||
        pixelWidth != canvasWidth || pixelHeight != canvasHeight) {
        WebConfigureCanvas (cssWidth, cssHeight, dpr);
    }
}

static void WebFrame (void) {
    WebSyncCanvas ();
    WebApplyProjection (webCssWidth, webCssHeight);
    if (!ClientFrame ()) {
        NetClientDisconnect ();
        emscripten_cancel_main_loop ();
    }
}

int main (void) {
    SetConfigFlags (FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT);
    int initialWidth = EM_ASM_INT({ return window.innerWidth; });
    int initialHeight = EM_ASM_INT({ return window.innerHeight; });
    InitWindow (initialWidth, initialHeight, "desktop");
    if (!IsWindowReady ()) return 1;
    WebConfigureCanvas (initialWidth, initialHeight, WebDpr ());
    CutilsInitInput ();
    EM_ASM({
        var element = document.getElementById("status");
        if (element) element.style.display = "none";
        element = document.getElementById("spinner");
        if (element) element.style.display = "none";
        element = document.getElementById("progress");
        if (element) element.style.display = "none";
        element = document.getElementById("controls");
        if (element) element.style.display = "none";
        element = document.getElementById("output");
        if (element) element.style.display = "none";
        var links = document.querySelectorAll("a[href*=emscripten]");
        for (var index = 0; index < links.length; index++) links[index].style.display = "none";
        var pointer = document.getElementById("pointerLock");
        if (pointer) pointer.checked = false;
        document.exitPointerLock();
    });

    AssetInit ();
    ObjectInit ();
    UserInit ();

    WebLoadFont ();

    UiInit ();
    ViewInit ();

    char serverText[512];
    char name[128];
    char query[1024];
    WebLocationPart ("location.search", query, sizeof (query));
    if (WebQueryValue (query, "server", serverText, sizeof (serverText)))
        WebUrlDecode (serverText);
    else {
        snprintf (serverText, sizeof (serverText), "%s", WEB_DEFAULT_SERVER);
    }
    if (WebQueryValue (query, "name", name, sizeof (name)))
        WebUrlDecode (name);
    else
        WebAskName (name, sizeof (name));

    if (!NetClientConnect (serverText, 0, name)) {
        webError = "Cannot connect to server";
        emscripten_set_main_loop (WebErrorFrame, 30, 0);
        return 0;
    }

    ClientInit (false, 0, "");
    emscripten_set_main_loop (WebFrame, 0, 1);
    return 0;
}

#endif
