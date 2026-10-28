#include "view.h"
#include "cutils.h"

#include <stddef.h>
#include <stdlib.h>

#include "object.h"
#include "entity.h"
#include "user.h"
#include "netclient.h"
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

Camera2D viewCam;
BackgroundStyle viewBackground;
Shader viewShadowShader;
Object **viewDrawOrder;
int viewDrawOrderCapacity;
const Object *viewDragPreview;
Vector2 viewDragPreviewPos;
void ViewInit () {

    viewCam = (Camera2D){0};
    viewCam.offset = (Vector2){GetScreenWidth () / 2.0f, GetScreenHeight () / 2.0f};
    viewCam.zoom = 1.0f;
    viewBackground = BACKGROUND_WHITE;

    viewDrawOrder = NULL;
    viewDrawOrderCapacity = 0;
    viewDragPreview = NULL;
    viewDragPreviewPos = (Vector2){0};

    viewShadowShader = LoadShaderFromMemory (shadowVertexShader, shadowFragmentShader);
    if (viewShadowShader.id != 0) {
        int shadowColorLoc = GetShaderLocation (viewShadowShader, "shadowColor");
        Vector4 shadowColor = {0.01f, 0.01f, 0.02f, 0.12f};
        SetShaderValue (viewShadowShader, shadowColorLoc, &shadowColor, SHADER_UNIFORM_VEC4);
    }
}

void ViewSetBackground (BackgroundStyle background) {
    viewBackground = background;
}

void ViewUpdateViewport () {

    viewCam.offset = (Vector2){GetScreenWidth () / 2.0f, GetScreenHeight () / 2.0f};
}

void ViewSetOrigin () {
    viewCam.target = (Vector2){0.0f, 0.0f};
}

void ViewSetZoom (float zoom) {
    float newZoom = Clamp (zoom, MIN_ZOOM, MAX_ZOOM);
    Vector2 screenCenter = {(float)GetScreenWidth () / 2.0f, (float)GetScreenHeight () / 2.0f};
    Vector2 worldBefore = GetScreenToWorld2D (screenCenter, viewCam);
    viewCam.zoom = newZoom;
    Vector2 worldAfter = GetScreenToWorld2D (screenCenter, viewCam);
    viewCam.target = Vector2Add (viewCam.target, Vector2Subtract (worldBefore, worldAfter));
}

void ViewUnload () {
    free (viewDrawOrder);
    viewDrawOrder = NULL;
    viewDrawOrderCapacity = 0;
    if (viewShadowShader.id != 0) UnloadShader (viewShadowShader);
}


void ViewZoomAt (Vector2 screenPoint, float wheel) {
    Vector2 before = GetScreenToWorld2D (screenPoint, viewCam);
    viewCam.zoom = Clamp (viewCam.zoom * powf (1.1f, wheel), MIN_ZOOM, MAX_ZOOM);
    Vector2 after = GetScreenToWorld2D (screenPoint, viewCam);
    viewCam.target = Vector2Add (viewCam.target, Vector2Subtract (before, after));
}

void ViewPan (Vector2 mouseDelta) {
    viewCam.target = Vector2Subtract (viewCam.target, Vector2Scale (mouseDelta, 1.0f / viewCam.zoom));
}

Vector2 ViewScreenToWorld (Vector2 screenPoint) {
    return GetScreenToWorld2D (screenPoint, viewCam);
}

static float BackgroundStep (float base, float zoom, float minScreen, float maxScreen) {
    float step = base;
    while (step * zoom < minScreen) step *= 2.0f;
    while (step * zoom > maxScreen) step *= 0.5f;
    return step;
}

static void DrawBackground () {
    if (viewBackground == BACKGROUND_WHITE) return;

    Vector2 topLeft = GetScreenToWorld2D ((Vector2){0.0f, 0.0f}, viewCam);
    Vector2 bottomRight = GetScreenToWorld2D (
        (Vector2){(float)GetScreenWidth (), (float)GetScreenHeight ()}, viewCam);

    if (viewBackground == BACKGROUND_DOTS) {
        float step = BackgroundStep (40.0f, viewCam.zoom, 22.0f, 120.0f);
        float radius = 1.6f / viewCam.zoom;
        float startX = floorf (topLeft.x / step) * step;
        float startY = floorf (topLeft.y / step) * step;
        Color color = {175, 175, 175, 255};

        for (float y = startY; y <= bottomRight.y + step; y += step) {
            for (float x = startX; x <= bottomRight.x + step; x += step) {
                DrawCircleV ((Vector2){x, y}, radius, color);
            }
        }
    } else if (viewBackground == BACKGROUND_GRID) {
        float step = BackgroundStep (48.0f, viewCam.zoom, 24.0f, 160.0f);
        float thickness = 1.0f / viewCam.zoom;
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
    } else if (viewBackground == BACKGROUND_CHECKER) {
        float tile = BackgroundStep (32.0f, viewCam.zoom, 18.0f, 120.0f);
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

static Vector2 ObjectDrawPos (const Object *object) {
    return object == viewDragPreview ? viewDragPreviewPos : object->pos;
}

static void DrawObjectShadow (const Object *object) {
    if (viewShadowShader.id == 0 || object->assets.count == 0) return;

    static const Vector2 offsets[] = {
        { 3.0f,  0.0f}, {-3.0f,  0.0f}, { 0.0f,  3.0f}, { 0.0f, -3.0f},
        { 2.0f,  2.0f}, {-2.0f,  2.0f}, { 2.0f, -2.0f}, {-2.0f, -2.0f},
        { 7.0f,  0.0f}, {-7.0f,  0.0f}, { 0.0f,  7.0f}, { 0.0f, -7.0f},
        { 5.0f,  5.0f}, {-5.0f,  5.0f}, { 5.0f, -5.0f}, {-5.0f, -5.0f}
    };
    const int offsetCount = (int)(sizeof (offsets) / sizeof (offsets[0]));
    const float inverseZoom = 1.0f / viewCam.zoom;

    BeginShaderMode (viewShadowShader);

    if (object->assets.count > 0) {
        const Asset *asset = ObjectDisplayedAsset (object);
        if (asset != NULL) {
            Vector2 drawPos = ObjectDrawPos (object);
            Vector2 base = {
                drawPos.x - AssetTexture(asset).width / 2.0f,
                drawPos.y - AssetTexture(asset).height / 2.0f};
            Rectangle source = {0, 0, (float)AssetTexture(asset).width, (float)AssetTexture(asset).height};
            for (int i = 0; i < offsetCount; i++) {
                Vector2 position = Vector2Add (base, Vector2Scale (offsets[i], inverseZoom));
                DrawTextureRec (AssetTexture(asset), source, position, WHITE);
            }
        }
    }

    EndShaderMode ();
}

static void DrawObjectLockLabel (const Object *object) {
    if (object->lockOwnerId == 0 || object->lockOwnerId == NetClientUserId () ||
        object->assets.count == 0) return;

    const Asset *asset = ObjectDisplayedAsset (object);
    const User *owner = EntityKindOf (object->lockOwnerId) == ENTITY_KIND_USER
                            ? (User *)EntityFind (object->lockOwnerId)
                            : NULL;
    if (asset == NULL || owner == NULL) return;

    Vector2 drawPos = ObjectDrawPos (object);
    Vector2 corner = GetWorldToScreen2D (
        (Vector2){drawPos.x + AssetTexture(asset).width / 2.0f,
                  drawPos.y + AssetTexture(asset).height / 2.0f}, viewCam);


    EndMode2D ();
    DrawTextEx (CutilsFont, owner->name,
                (Vector2){corner.x + 5.0f, corner.y + 2.0f},
                20.0f, 0.0f, UserColor (owner));
    BeginMode2D (viewCam);
}

static int CompareObjectsForDraw (const void *a, const void *b) {
    const Object *objectA = *(const Object *const *)a;
    const Object *objectB = *(const Object *const *)b;
    return ObjectCompareDrawOrder (objectA, objectB);
}

void ViewSetDragPreview (const Object *object, Vector2 position) {
    viewDragPreview = object;
    viewDragPreviewPos = position;
}

void ViewClearDragPreview () {
    viewDragPreview = NULL;
}

void ViewDraw (const Object *chosen) {
    BeginMode2D (viewCam);

    DrawBackground ();

    IdGroup objectIds = ObjectIds ();
    int objectCount = objectIds.count;

    if (objectCount > viewDrawOrderCapacity) {
        Object **order = realloc (viewDrawOrder, (size_t)objectCount * sizeof (Object *));
        if (order != NULL) {
            viewDrawOrder = order;
            viewDrawOrderCapacity = objectCount;
        }
    }

    bool sorted = (objectCount <= viewDrawOrderCapacity);
    if (sorted) {
        for (int i = 0; i < objectCount; i++)
            viewDrawOrder[i] = EntityFind (objectIds.ids[i]);
        if (objectCount > 1)
            qsort (viewDrawOrder, (size_t)objectCount, sizeof (Object *),
                   CompareObjectsForDraw);
    }

    for (int i = 0; i < objectCount; i++) {
        const Object *object = sorted ? viewDrawOrder[i] : EntityFind (objectIds.ids[i]);
        if (object == NULL || object->hidden) continue;

        if (object == chosen || object->lockOwnerId != 0)
            DrawObjectShadow (object);

        if (object->assets.count > 0) {
            const Asset *asset = ObjectDisplayedAsset (object);
            if (asset != NULL) {
                Vector2 drawPos = ObjectDrawPos (object);
                Vector2 position = {
                    drawPos.x - AssetTexture(asset).width / 2.0f,
                    drawPos.y - AssetTexture(asset).height / 2.0f};
                Rectangle source = {0, 0, (float)AssetTexture(asset).width, (float)AssetTexture(asset).height};
                DrawTextureRec (AssetTexture(asset), source, position, WHITE);
            }
        }
        DrawObjectLockLabel (object);
    }

    IdGroupUnload (&objectIds);
    EndMode2D ();
}
