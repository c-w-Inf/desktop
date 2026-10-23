#ifndef VIEW_H
#define VIEW_H

#include "raylib.h"
#include "ui.h"
#include "object.h"

typedef struct View {
    Camera2D cam;
    BackgroundStyle background;
    Shader shadowShader;
    Object **drawOrder;
    int drawOrderCapacity;
} View;

void ViewInit (View *view);
void ViewUnload (View *view);
void ViewUpdateViewport (View *view);
void ViewSetBackground (View *view, BackgroundStyle background);
void ViewSetOrigin (View *view);
void ViewSetZoom (View *view, float zoom);
void ViewZoomAt (View *view, Vector2 screenPoint, float wheel);
void ViewPan (View *view, Vector2 mouseDelta);
Vector2 ViewScreenToWorld (const View *view, Vector2 screenPoint);
void ViewDraw (View *view, const Object *chosen, Font font);

#endif
