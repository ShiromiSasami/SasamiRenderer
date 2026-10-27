#pragma once

#include "AppFramework/Editor/EditorSelection.h"
#include "Renderer/Scene/RenderCameraProxy.h"

namespace SasamiEngine::App
{
    class ApplicationCore;

    struct EditorRay
    {
        float origin[3] = { 0.0f, 0.0f, 0.0f };
        float direction[3] = { 0.0f, 0.0f, 1.0f };  // unit length
    };

    // Pinhole ray through display pixel (x, y) of a width x height view (the inverse of
    // Debug::ProjectToOverlay).
    EditorRay MakeEditorPickRay(const Render::RenderCameraProxy& camera, float x, float y, float width, float height);

    // Slab test (Kay & Kajiya 1986). On a hit, tEnter/tExit are the ray parameters where the ray enters
    // and leaves the box; tEnter is negative when the origin is inside.
    bool IntersectRayAabb(const EditorRay& ray, const float boxMin[3], const float boxMax[3], float& tEnter, float& tExit);

    // The 8 corners of a local box mapped by a row-major, row-vector model matrix; corner i takes
    // max on x/y/z when bit 0/1/2 of i is set.
    void TransformBoxCorners(const float boxMin[3], const float boxMax[3], const float model[16], float outCorners[8][3]);

    // Local bounds (component space) and model matrix of a selected model; false for other kinds or empty meshes.
    bool GetSelectionBox(const EditorSelection& selection, float boxMin[3], float boxMax[3], const float*& model);
    // Model matrix of a selected model (cheap, unlike the bounds); null for other kinds.
    const float* GetSelectionModelMatrix(const EditorSelection& selection);

    struct EditorPickResult
    {
        EditorObjectKind kind = EditorObjectKind::None;
        SObject* object = nullptr;
        float distance = 0.0f;
    };

    // Picks what lies under display pixel (x, y). Point/spot lights win when their projected position
    // is within `lightRadiusPixels` (closest to the camera first). Models are tested against the world
    // AABB of their transformed local bounds, so the pick is box-accurate, not triangle-accurate. A box
    // that contains the camera is ranked by where the ray leaves it, so a huge enclosing model (a whole
    // town) does not hide the objects in front of the camera.
    EditorPickResult PickEditorObject(ApplicationCore& app, const Render::RenderCameraProxy& camera, float x, float y,
                                      float width, float height, float lightRadiusPixels = 12.0f);

    // Ray/box checks; returns failures, `checks` receives the number run.
    int RunEditorPickingSelfTest(int& checks);
}
