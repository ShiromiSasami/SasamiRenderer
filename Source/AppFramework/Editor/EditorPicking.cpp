#include "AppFramework/Editor/EditorPicking.h"

#include "AppFramework/ApplicationCore.h"
#include "AppFramework/Object/PointLight.h"
#include "AppFramework/Object/SkinnedModel.h"
#include "AppFramework/Object/SpotLight.h"
#include "AppFramework/Object/StaticModel.h"
#include "Foundation/Math/MathUtil.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

namespace SasamiEngine::App
{
    namespace
    {
        constexpr float MIN_LIGHT_DEPTH = 0.05f;

        using Foundation::Math::Dot3;

        struct ModelCandidate
        {
            float rank = 0.0f;    // tEnter, or tExit when the camera is inside the box
            float volume = 0.0f;  // tie-break: the smaller box wins
        };

        bool BetterModel(const ModelCandidate& a, const ModelCandidate& b)
        {
            const float tolerance = 1e-4f * (std::max)(1.0f, b.rank);
            if (std::fabs(a.rank - b.rank) > tolerance)
            {
                return a.rank < b.rank;
            }
            return a.volume < b.volume;
        }

        bool WorldBox(const float localMin[3], const float localMax[3], const float model[16], float outMin[3], float outMax[3])
        {
            float corners[8][3];
            TransformBoxCorners(localMin, localMax, model, corners);
            for (int axis = 0; axis < 3; ++axis)
            {
                outMin[axis] = corners[0][axis];
                outMax[axis] = corners[0][axis];
                for (int i = 1; i < 8; ++i)
                {
                    outMin[axis] = (std::min)(outMin[axis], corners[i][axis]);
                    outMax[axis] = (std::max)(outMax[axis], corners[i][axis]);
                }
            }
            return true;
        }

        // Light pick in display pixels; returns the view depth on a hit.
        bool PickLightPoint(const Render::RenderCameraProxy& camera, const float position[3], float x, float y,
                            float width, float height, float radiusPixels, float& outDepth)
        {
            const float rel[3] = { position[0] - camera.cameraPosition[0], position[1] - camera.cameraPosition[1],
                                   position[2] - camera.cameraPosition[2] };
            const float z = Dot3(rel, camera.cameraForward);
            if (z < MIN_LIGHT_DEPTH)
            {
                return false;
            }
            const float sx = Dot3(rel, camera.cameraRight) / (z * camera.tanHalfFovY * camera.aspectRatio);
            const float sy = Dot3(rel, camera.cameraUp) / (z * camera.tanHalfFovY);
            const float px = (sx + 1.0f) * 0.5f * width;
            const float py = (1.0f - sy) * 0.5f * height;
            const float dx = px - x;
            const float dy = py - y;
            if (dx * dx + dy * dy > radiusPixels * radiusPixels)
            {
                return false;
            }
            outDepth = z;
            return true;
        }
    }

    EditorRay MakeEditorPickRay(const Render::RenderCameraProxy& camera, float x, float y, float width, float height)
    {
        EditorRay ray;
        const float sx = 2.0f * x / width - 1.0f;
        const float sy = 1.0f - 2.0f * y / height;
        float length = 0.0f;
        for (int i = 0; i < 3; ++i)
        {
            ray.origin[i] = camera.cameraPosition[i];
            ray.direction[i] = camera.cameraForward[i] + sx * camera.tanHalfFovY * camera.aspectRatio * camera.cameraRight[i]
                             + sy * camera.tanHalfFovY * camera.cameraUp[i];
            length += ray.direction[i] * ray.direction[i];
        }
        length = std::sqrt(length);
        for (float& d : ray.direction)
        {
            d /= length;
        }
        return ray;
    }

    bool IntersectRayAabb(const EditorRay& ray, const float boxMin[3], const float boxMax[3], float& tEnter, float& tExit)
    {
        tEnter = -std::numeric_limits<float>::infinity();
        tExit = std::numeric_limits<float>::infinity();
        for (int axis = 0; axis < 3; ++axis)
        {
            if (ray.direction[axis] == 0.0f)
            {
                if (ray.origin[axis] < boxMin[axis] || ray.origin[axis] > boxMax[axis])
                {
                    return false;
                }
                continue;
            }
            const float inv = 1.0f / ray.direction[axis];
            float t0 = (boxMin[axis] - ray.origin[axis]) * inv;
            float t1 = (boxMax[axis] - ray.origin[axis]) * inv;
            if (t0 > t1)
            {
                std::swap(t0, t1);
            }
            tEnter = (std::max)(tEnter, t0);
            tExit = (std::min)(tExit, t1);
        }
        return tEnter <= tExit && tExit >= 0.0f;
    }

    void TransformBoxCorners(const float boxMin[3], const float boxMax[3], const float model[16], float outCorners[8][3])
    {
        for (int i = 0; i < 8; ++i)
        {
            const float p[3] = { (i & 1) ? boxMax[0] : boxMin[0], (i & 2) ? boxMax[1] : boxMin[1],
                                 (i & 4) ? boxMax[2] : boxMin[2] };
            for (int axis = 0; axis < 3; ++axis)
            {
                outCorners[i][axis] = p[0] * model[axis] + p[1] * model[4 + axis] + p[2] * model[8 + axis] + model[12 + axis];
            }
        }
    }

    bool GetSelectionBox(const EditorSelection& selection, float boxMin[3], float boxMax[3], const float*& model)
    {
        if (selection.kind == EditorObjectKind::StaticModel && selection.object != nullptr)
        {
            const MeshComponent& mesh = static_cast<StaticModel*>(selection.object)->MeshComponentRef();
            model = mesh.GetModelMatrix();
            return mesh.ComputeLocalBounds(boxMin, boxMax);
        }
        if (selection.kind == EditorObjectKind::SkinnedModel && selection.object != nullptr)
        {
            const SkinnedMeshComponent& mesh = static_cast<SkinnedModel*>(selection.object)->SkinnedMeshComponentRef();
            model = mesh.GetModelMatrix();
            return mesh.ComputeLocalBounds(boxMin, boxMax);
        }
        return false;
    }

    const float* GetSelectionModelMatrix(const EditorSelection& selection)
    {
        if (selection.kind == EditorObjectKind::StaticModel && selection.object != nullptr)
        {
            return static_cast<StaticModel*>(selection.object)->MeshComponentRef().GetModelMatrix();
        }
        if (selection.kind == EditorObjectKind::SkinnedModel && selection.object != nullptr)
        {
            return static_cast<SkinnedModel*>(selection.object)->SkinnedMeshComponentRef().GetModelMatrix();
        }
        return nullptr;
    }

    EditorPickResult PickEditorObject(ApplicationCore& app, const Render::RenderCameraProxy& camera, float x, float y,
                                      float width, float height, float lightRadiusPixels)
    {
        EditorPickResult result;

        // Lights first: they are drawn as small markers and would otherwise always lose to the boxes around them.
        float bestLight = std::numeric_limits<float>::infinity();
        for (PointLight* light : app.GetPointLightObjects())
        {
            const Vector3& p = light->Transform().position;
            const float position[3] = { p.x, p.y, p.z };
            float depth = 0.0f;
            if (PickLightPoint(camera, position, x, y, width, height, lightRadiusPixels, depth) && depth < bestLight)
            {
                bestLight = depth;
                result = { EditorObjectKind::PointLight, light, depth };
            }
        }
        for (SpotLight* light : app.GetSpotLightObjects())
        {
            const Vector3& p = light->Transform().position;
            const float position[3] = { p.x, p.y, p.z };
            float depth = 0.0f;
            if (PickLightPoint(camera, position, x, y, width, height, lightRadiusPixels, depth) && depth < bestLight)
            {
                bestLight = depth;
                result = { EditorObjectKind::SpotLight, light, depth };
            }
        }
        if (result.kind != EditorObjectKind::None)
        {
            return result;
        }

        const EditorRay ray = MakeEditorPickRay(camera, x, y, width, height);
        bool found = false;
        ModelCandidate best;
        auto consider = [&](EditorObjectKind kind, SObject* object)
        {
            EditorSelection probe;
            probe.Select(kind, object, 0);
            float localMin[3], localMax[3], worldMin[3], worldMax[3];
            const float* model = nullptr;
            if (!GetSelectionBox(probe, localMin, localMax, model))
            {
                return;
            }
            WorldBox(localMin, localMax, model, worldMin, worldMax);
            float tEnter = 0.0f, tExit = 0.0f;
            if (!IntersectRayAabb(ray, worldMin, worldMax, tEnter, tExit))
            {
                return;
            }
            ModelCandidate candidate;
            candidate.rank = tEnter >= 0.0f ? tEnter : tExit;
            candidate.volume = (worldMax[0] - worldMin[0]) * (worldMax[1] - worldMin[1]) * (worldMax[2] - worldMin[2]);
            if (!found || BetterModel(candidate, best))
            {
                found = true;
                best = candidate;
                result = { kind, object, (std::max)(tEnter, 0.0f) };
            }
        };
        for (StaticModel* model : app.GetStaticModels())
        {
            consider(EditorObjectKind::StaticModel, model);
        }
        for (SkinnedModel* model : app.GetSkinnedModels())
        {
            consider(EditorObjectKind::SkinnedModel, model);
        }
        return result;
    }

    int RunEditorPickingSelfTest(int& checks)
    {
        int failures = 0;
        auto expect = [&](bool condition)
        {
            ++checks;
            if (!condition)
            {
                ++failures;
            }
        };
        auto nearlyEqual = [](float a, float b) { return std::fabs(a - b) < 1e-4f; };

        const float boxMin[3] = { -1.0f, -1.0f, -1.0f };
        const float boxMax[3] = { 1.0f, 1.0f, 1.0f };
        float tEnter = 0.0f, tExit = 0.0f;

        EditorRay ray;
        ray.origin[2] = -5.0f;  // looking down +z at the unit box
        expect(IntersectRayAabb(ray, boxMin, boxMax, tEnter, tExit) && nearlyEqual(tEnter, 4.0f) && nearlyEqual(tExit, 6.0f));

        ray.origin[0] = 1.5f;   // parallel to z, outside the x slab
        expect(!IntersectRayAabb(ray, boxMin, boxMax, tEnter, tExit));

        ray.origin[0] = 0.0f;
        ray.origin[2] = 0.0f;   // inside: enter behind, exit ahead
        expect(IntersectRayAabb(ray, boxMin, boxMax, tEnter, tExit) && tEnter < 0.0f && nearlyEqual(tExit, 1.0f));

        ray.origin[2] = 5.0f;   // box behind the ray
        expect(!IntersectRayAabb(ray, boxMin, boxMax, tEnter, tExit));

        const float s = std::sqrt(0.5f);
        EditorRay diagonal;
        diagonal.origin[0] = -3.0f;
        diagonal.origin[2] = -3.0f;
        diagonal.direction[0] = s;
        diagonal.direction[2] = s;
        expect(IntersectRayAabb(diagonal, boxMin, boxMax, tEnter, tExit) && nearlyEqual(tEnter, 2.0f / s));

        // Translate (10, 0, 0) and scale 2: corner 7 lands on (12, 2, 2), corner 0 on (8, -2, -2).
        const float model[16] = { 2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 2, 0, 10, 0, 0, 1 };
        float corners[8][3];
        TransformBoxCorners(boxMin, boxMax, model, corners);
        expect(nearlyEqual(corners[7][0], 12.0f) && nearlyEqual(corners[7][1], 2.0f) && nearlyEqual(corners[7][2], 2.0f));
        expect(nearlyEqual(corners[0][0], 8.0f) && nearlyEqual(corners[0][1], -2.0f) && nearlyEqual(corners[0][2], -2.0f));

        // The centre pixel ray is the camera forward.
        Render::RenderCameraProxy camera;
        camera.cameraPosition[2] = -5.0f;
        camera.aspectRatio = 2.0f;
        const EditorRay centre = MakeEditorPickRay(camera, 100.0f, 50.0f, 200.0f, 100.0f);
        expect(nearlyEqual(centre.direction[0], 0.0f) && nearlyEqual(centre.direction[1], 0.0f) && nearlyEqual(centre.direction[2], 1.0f));
        // The right edge leans by atan(tanHalfFovY * aspect).
        const EditorRay right = MakeEditorPickRay(camera, 200.0f, 50.0f, 200.0f, 100.0f);
        expect(nearlyEqual(right.direction[0] / right.direction[2], camera.tanHalfFovY * camera.aspectRatio));

        // Ranking: a box containing the camera is ranked by its exit, so a small box in front wins.
        ModelCandidate enclosing{ 50.0f, 1e6f };
        ModelCandidate inFront{ 3.0f, 1.0f };
        expect(BetterModel(inFront, enclosing) && !BetterModel(enclosing, inFront));
        ModelCandidate sameSmall{ 3.0f, 0.5f };
        expect(BetterModel(sameSmall, inFront));

        return failures;
    }
}
