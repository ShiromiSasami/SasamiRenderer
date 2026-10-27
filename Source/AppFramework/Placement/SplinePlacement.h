#pragma once

#include "AppFramework/Placement/PlacementTransform.h"

#include <cstdint>
#include <vector>

namespace SasamiEngine::App
{
    // Places instances at a fixed arc-length spacing along a curve through control points on the
    // XZ plane. Like ScatterByDensity it is pure data in, matrices out, for
    // StaticModel::SetInstanceTransforms and an [instance] section via scene.save.
    constexpr uint32_t kMaxSplineInstances = 10000000;

    struct SplinePlacementDesc
    {
        struct Point
        {
            float x = 0.0f;
            float z = 0.0f;
        };

        std::vector<Point> points; // at least 2; the curve passes through every one
        float y       = 0.0f;      // fixed height; there is no terrain projection
        float spacing = 1.0f;      // arc length between consecutive instances, first one at points[0]
        float width   = 0.0f;      // instances are offset sideways by up to +-width/2
        uint32_t seed = 0;
        float scaleMin = 1.0f;
        float scaleMax = 1.0f;
        float yawJitter = 0.0f;    // radians, added as +-yawJitter/2 to the along-curve heading
    };

    struct SplinePlacementResult
    {
        std::vector<PlacementMatrix> transforms;
        float length = 0.0f; // approximate arc length of the whole curve
    };

    // Centripetal Catmull-Rom (alpha = 0.5) through the points -- Yuksel, Schaefer & Keyser,
    // "Parameterization and applications of Catmull-Rom curves", CAD 43(7), 2011: unlike the
    // uniform form it never forms cusps or self-intersections inside a segment, which matters for
    // hand-placed points with uneven gaps. End tangents come from mirrored phantom points.
    // Instances face along the curve: the model's local +Z follows the tangent.
    // Returns nothing for fewer than 2 distinct points or a non-positive spacing; at most
    // kMaxSplineInstances placements are produced.
    SplinePlacementResult PlaceAlongSpline(const SplinePlacementDesc& desc);
}
