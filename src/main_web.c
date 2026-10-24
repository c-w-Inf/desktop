#if defined(__EMSCRIPTEN__)

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <emscripten.h>

#include "raylib.h"
#include "raymath.h"
#include "assets.h"
#include "client.h"
#include "entity.h"
#include "net.h"
#include "object.h"
#include "ui.h"
#include "user.h"
#include "view.h"

#define WEB_DEFAULT_SERVER "wss://dsk.cwinf.top/dsk/"

static ClientFrameState webFrame;
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

static void WebPump (void) {
}

static bool WebOpenRemote (const char *program, const char *host, int port,
                           const char *username) {
    (void)program;
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

static bool webFontLoaded;
static bool webFontFailed;

static void WebFontOnLoad (const char *file) {
    printf ("WEBFONT loaded: %s\n", file);
    fflush (stdout);
    webFontLoaded = true;
}

static void WebFontOnError (const char *file) {
    printf ("WEBFONT failed: %s\n", file);
    fflush (stdout);
    webFontLoaded = true;
    webFontFailed = true;
}

static void WebLoadFont (void) {
    EM_ASM({
        try { FS.mkdir("/assets"); } catch (error) {}
    });
    webFontLoaded = false;
    webFontFailed = false;
    printf ("WEBFONT start\n");
    fflush (stdout);
    emscripten_async_wget ("/assets/WenQuanYiMicroHei.ttf",
                           "/assets/WenQuanYiMicroHei.ttf",
                           WebFontOnLoad, WebFontOnError);
    while (!webFontLoaded) emscripten_sleep (10);
}

static void WebErrorFrame (void) {
    BeginDrawing ();
    ClearBackground (RAYWHITE);
    DrawText ("Image Canvas Web", 40, 40, 30, DARKGRAY);
    if (webError != NULL)
        DrawText (webError, 40, 90, 18, RED);
    EndDrawing ();
}

static void WebClientFrame (void) {
    if (!ClientFrame (&webFrame)) {
        NetClientDisconnect ();
        ClientFrameUnload (&webFrame);
        emscripten_cancel_main_loop ();
    }
}

int main (void) {
    SetConfigFlags (FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT);
    InitWindow (1000, 700, "Image Canvas");
    if (!IsWindowReady ()) return 1;
    SetWindowSize (1000, 700);
    EM_ASM({
        const canvas = document.getElementById("canvas");
        if (canvas) {
            canvas.style.width = canvas.width + "px";
            canvas.style.height = canvas.height + "px";
        }
        document.body.style.margin = "0";
        document.body.style.background = "#ffffff";
        document.body.style.overflow = "hidden";
    });

    AssetInit ();
    ObjectInit ();
    UserInit ();

    WebLoadFont ();

    Ui ui;
    UiInit (&ui);
    View view;
    ViewInit (&view);

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
        snprintf (name, sizeof (name), "web");

    if (!NetClientConnect (serverText, 0, name)) {
        webError = "Cannot connect to server";
        emscripten_set_main_loop (WebErrorFrame, 30, 0);
        return 0;
    }

    ClientFrameInit (&webFrame, &ui, &view, false, 0, "",
                     WebPump, WebOpenRemote, "");
    emscripten_set_main_loop (WebClientFrame, 0, 0);
    return 0;
}

#endif
