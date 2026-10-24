#include "view.h"

#include <stddef.h>
#include <stdlib.h>

#include "object.h"
#include "entity.h"
#include "user.h"
#include "net.h"
#include "raymath.h"

#define MIN_ZOOM 0.2f
#define MAX_ZOOM 5.0f

static const char *shadowVertexShader =
#if defined(__EMSCRIPTEN__)
    "attribute vec3 vertexPosition;\n"
    "attribute vec2 vertexTexCoord;\n"
    "uniform mat4 mvp;\n"
    "varying vec2 fragTexCoord;\n"
    "void main()\n"
    "{\n"
    "    fragTexCoord = vertexTexCoord;\n"
    "    gl_Position = mvp*vec4(vertexPosition, 1.0);\n"
    "}\n";
#else
    "#version 330\n"
    "in vec3 vertexPosition;\n"
    "in vec2 vertexTexCoord;\n"
    "uniform mat4 mvp;\n"
    "out vec2 fragTexCoord;\n"
    "void main()\n"
    "{\n"
    "    fragTexCoord = vertexTexCoord;\n"
    "    gl_Position = mvp*vec4(vertexPosition, 1.0);\n"
    "}\n";
#endif

static const char *shadowFragmentShader =
#if defined(__EMSCRIPTEN__)
    "precision mediump float;\n"
    "varying vec2 fragTexCoord;\n"
    "uniform sampler2D texture0;\n"
    "uniform vec4 shadowColor;\n"
    "void main()\n"
    "{\n"
    "    float alpha = texture2D(texture0, fragTexCoord).a;\n"
    "    float full = (alpha >= 1.0) ? 1.0 : 0.0;\n"
    "    gl_FragColor = vec4(shadowColor.rgb, full*shadowColor.a);\n"
    "}\n";
#else
    "#version 330\n"
    "in vec2 fragTexCoord;\n"
    "uniform sampler2D texture0;\n"
    "uniform vec4 shadowColor;\n"
    "out vec4 finalColor;\n"
    "void main()\n"
    "{\n"
    "    float alpha = texture(texture0, fragTexCoord).a;\n"
    "    float full = (alpha >= 1.0) ? 1.0 : 0.0;\n"
    "    finalColor = vec4(shadowColor.rgb, full*shadowColor.a);\n"
    "}\n";
#endif

void ViewInit (View *view) {
    // offset 固定在窗口中心，target 表示窗口中心对应的世界坐标
    view->cam = (Camera2D){0};
    view->cam.offset = (Vector2){GetScreenWidth () / 2.0f, GetScreenHeight () / 2.0f};
    view->cam.zoom = 1.0f;
    view->background = BACKGROUND_WHITE;

    view->drawOrder = NULL;
    view->drawOrderCapacity = 0;

    view->shadowShader = LoadShaderFromMemory (shadowVertexShader, shadowFragmentShader);
    if (view->shadowShader.id != 0) {
        int shadowColorLoc = GetShaderLocation (view->shadowShader, "shadowColor");
        Vector4 shadowColor = {0.01f, 0.01f, 0.02f, 0.12f};
        SetShaderValue (view->shadowShader, shadowColorLoc, &shadowColor, SHADER_UNIFORM_VEC4);
    }
}

void ViewSetBackground (View *view, BackgroundStyle background) {
    view->background = background;
}

void ViewUpdateViewport (View *view) {
    // 窗口初始化/缩放后保持世界原点与窗口中心对齐
    view->cam.offset = (Vector2){GetScreenWidth () / 2.0f, GetScreenHeight () / 2.0f};
}

void ViewSetOrigin (View *view) {
    view->cam.target = (Vector2){0.0f, 0.0f};
}

void ViewSetZoom (View *view, float zoom) {
    float newZoom = Clamp (zoom, MIN_ZOOM, MAX_ZOOM);
    Vector2 screenCenter = {(float)GetScreenWidth () / 2.0f, (float)GetScreenHeight () / 2.0f};
    Vector2 worldBefore = GetScreenToWorld2D (screenCenter, view->cam);
    view->cam.zoom = newZoom;
    Vector2 worldAfter = GetScreenToWorld2D (screenCenter, view->cam);
    view->cam.target = Vector2Add (view->cam.target, Vector2Subtract (worldBefore, worldAfter));
}

void ViewUnload (View *view) {
    free (view->drawOrder);
    view->drawOrder = NULL;
    view->drawOrderCapacity = 0;
    if (view->shadowShader.id != 0) UnloadShader (view->shadowShader);
}

// 以光标为中心缩放
void ViewZoomAt (View *view, Vector2 screenPoint, float wheel) {
    Vector2 before = GetScreenToWorld2D (screenPoint, view->cam);
    view->cam.zoom = Clamp (view->cam.zoom * powf (1.1f, wheel), MIN_ZOOM, MAX_ZOOM);
    Vector2 after = GetScreenToWorld2D (screenPoint, view->cam);
    view->cam.target = Vector2Add (view->cam.target, Vector2Subtract (before, after));
}

void ViewPan (View *view, Vector2 mouseDelta) {
    view->cam.target = Vector2Subtract (view->cam.target, Vector2Scale (mouseDelta, 1.0f / view->cam.zoom));
}

Vector2 ViewScreenToWorld (const View *view, Vector2 screenPoint) {
    return GetScreenToWorld2D (screenPoint, view->cam);
}

static float BackgroundStep (float base, float zoom, float minScreen, float maxScreen) {
    float step = base;
    while (step * zoom < minScreen) step *= 2.0f;
    while (step * zoom > maxScreen) step *= 0.5f;
    return step;
}

static void DrawBackground (const View *view) {
    if (view->background == BACKGROUND_WHITE) return;

    Vector2 topLeft = GetScreenToWorld2D ((Vector2){0.0f, 0.0f}, view->cam);
    Vector2 bottomRight = GetScreenToWorld2D (
        (Vector2){(float)GetScreenWidth (), (float)GetScreenHeight ()}, view->cam);

    if (view->background == BACKGROUND_DOTS) {
        float step = BackgroundStep (40.0f, view->cam.zoom, 22.0f, 120.0f);
        float radius = 1.6f / view->cam.zoom;
        float startX = floorf (topLeft.x / step) * step;
        float startY = floorf (topLeft.y / step) * step;
        Color color = {175, 175, 175, 255};

        for (float y = startY; y <= bottomRight.y + step; y += step) {
            for (float x = startX; x <= bottomRight.x + step; x += step) {
                DrawCircleV ((Vector2){x, y}, radius, color);
            }
        }
    } else if (view->background == BACKGROUND_GRID) {
        float step = BackgroundStep (48.0f, view->cam.zoom, 24.0f, 160.0f);
        float thickness = 1.0f / view->cam.zoom;
        float startX = floorf (topLeft.x / step) * step;
        float startY = floorf (topLeft.y / step) * step;
        Color color = {205, 205, 205, 255};

        for (float x = startX; x <= bottomRight.x + step; x += step) {
            DrawRectangleRec ((Rectangle){x - thickness / 2.0f, topLeft.y, thickness,
                                          bottomRight.y - topLeft.y}, color);
        }
        for (float y = startY; y <= bottomRight.y + step; y += step) {
            DrawRectangleRec ((Rectangle){topLeft.x, y - thickness / 2.0f,
                                          bottomRight.x - topLeft.x, thickness}, color);
        }
    } else if (view->background == BACKGROUND_CHECKER) {
        float tile = BackgroundStep (32.0f, view->cam.zoom, 18.0f, 120.0f);
        int startX = (int)floorf (topLeft.x / tile);
        int endX = (int)floorf (bottomRight.x / tile);
        int startY = (int)floorf (topLeft.y / tile);
        int endY = (int)floorf (bottomRight.y / tile);
        Color color = {230, 230, 230, 255};

        for (int y = startY; y <= endY; y++) {
            for (int x = startX; x <= endX; x++) {
                int parityX = ((x % 2) + 2) % 2;
                int parityY = ((y % 2) + 2) % 2;
                if (parityX != parityY)
                    DrawRectangleRec ((Rectangle){x * tile, y * tile, tile, tile}, color);
            }
        }
    }
}

static void DrawObjectShadow (const View *view, const Object *object) {
    if (view->shadowShader.id == 0 || object->assets.count == 0) return;

    static const Vector2 offsets[] = {
        { 3.0f,  0.0f}, {-3.0f,  0.0f}, { 0.0f,  3.0f}, { 0.0f, -3.0f},
        { 2.0f,  2.0f}, {-2.0f,  2.0f}, { 2.0f, -2.0f}, {-2.0f, -2.0f},
        { 7.0f,  0.0f}, {-7.0f,  0.0f}, { 0.0f,  7.0f}, { 0.0f, -7.0f},
        { 5.0f,  5.0f}, {-5.0f,  5.0f}, { 5.0f, -5.0f}, {-5.0f, -5.0f}
    };
    const int offsetCount = (int)(sizeof (offsets) / sizeof (offsets[0]));
    const float inverseZoom = 1.0f / view->cam.zoom;

    BeginShaderMode (view->shadowShader);

    if (object->assets.count > 0) {
        const Asset *asset = ObjectDisplayedAsset (object);
        if (asset != NULL) {
            Vector2 base = {
                object->pos.x - asset->tex.width / 2.0f,
                object->pos.y - asset->tex.height / 2.0f};
            for (int i = 0; i < offsetCount; i++) {
                Vector2 position = Vector2Add (base, Vector2Scale (offsets[i], inverseZoom));
                DrawTextureV (asset->tex, position, WHITE);
            }
        }
    }

    EndShaderMode ();
}

static void DrawObjectLockLabel (const View *view, const Object *object, Font font) {
    if (object->lockOwnerId == 0 || object->lockOwnerId == NetClientUserId () ||
        object->assets.count == 0) return;

    const Asset *asset = ObjectDisplayedAsset (object);
    const User *owner = UserFind (object->lockOwnerId);
    if (asset == NULL || owner == NULL) return;

    Vector2 corner = GetWorldToScreen2D (
        (Vector2){object->pos.x + asset->tex.width / 2.0f,
                  object->pos.y + asset->tex.height / 2.0f}, view->cam);

    // 标签属于物件绘制顺序的一部分，后续物件可以遮挡它。
    EndMode2D ();
    DrawTextEx (font, owner->name,
                (Vector2){corner.x + 5.0f, corner.y + 2.0f},
                20.0f, 0.0f, UserColor (owner));
    BeginMode2D (view->cam);
}

static int CompareObjectsForDraw (const void *a, const void *b) {
    const Object *objectA = *(const Object *const *)a;
    const Object *objectB = *(const Object *const *)b;
    return ObjectCompareDrawOrder (objectA, objectB);
}

void ViewDraw (View *view, const Object *chosen, Font font) {
    BeginMode2D (view->cam);

    DrawBackground (view);

    IdGroup objectIds = ObjectIds ();
    int objectCount = objectIds.count;

    if (objectCount > view->drawOrderCapacity) {
        Object **order = realloc (view->drawOrder, (size_t)objectCount * sizeof (Object *));
        if (order != NULL) {
            view->drawOrder = order;
            view->drawOrderCapacity = objectCount;
        }
    }

    bool sorted = (objectCount <= view->drawOrderCapacity);
    if (sorted) {
        for (int i = 0; i < objectCount; i++)
            view->drawOrder[i] = EntityFind (objectIds.ids[i]);
        if (objectCount > 1)
            qsort (view->drawOrder, (size_t)objectCount, sizeof (Object *),
                   CompareObjectsForDraw);
    }

    for (int i = 0; i < objectCount; i++) {
        const Object *object = sorted ? view->drawOrder[i] : EntityFind (objectIds.ids[i]);
        if (object == NULL || object->hidden) continue;

        if (object == chosen || object->lockOwnerId != 0)
            DrawObjectShadow (view, object);

        if (object->assets.count > 0) {
            const Asset *asset = ObjectDisplayedAsset (object);
            if (asset != NULL) {
                Vector2 position = {
                    object->pos.x - asset->tex.width / 2.0f,
                    object->pos.y - asset->tex.height / 2.0f};
                DrawTextureV (asset->tex, position, WHITE);
            }
        }
        DrawObjectLockLabel (view, object, font);
    }

    IdGroupUnload (&objectIds);
    EndMode2D ();
}
