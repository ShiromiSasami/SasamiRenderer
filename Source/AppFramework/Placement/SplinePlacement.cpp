#include "AppFramework/Placement/SplinePlacement.h"

#include "AppFramework/Placement/PlacementRandom.h"

#include <algorithm>
#include <cmath>

namespace SasamiEngine::App
{
    namespace
    {
        using Point = SplinePlacementDesc::Point;

        // Samples per segment for the arc-length table. Spacing is measured along the resulting
        // polyline, so the error is the chord-vs-arc difference of 1/64 of a segment.
        constexpr int kSamplesPerSegment = 64;

        Point Lerp(const Point& a, const Point& b, float t)
        {
            return { a.x + (b.x - a.x) * t, a.z + (b.z - a.z) * t };
        }

        float Distance(const Point& a, const Point& b)
        {
            return std::hypot(b.x - a.x, b.z - a.z);
        }

        // Knot step of the centripetal parameterization: |Pi+1 - Pi|^alpha with alpha = 0.5.
        // Clamped so coincident phantom/control points cannot divide by zero below.
        float KnotStep(const Point& a, const Point& b)
        {
            return std::max(std::sqrt(Distance(a, b)), 1.0e-4f);
        }

        // Barry-Goldman pyramidal evaluation of the segment p1 -> p2 at u in [0, 1].
        Point EvaluateSegment(const Point& p0, const Point& p1, const Point& p2, const Point& p3, float u)
        {
            const float t0 = 0.0f;
            const float t1 = t0 + KnotStep(p0, p1);
            const float t2 = t1 + KnotStep(p1, p2);
            const float t3 = t2 + KnotStep(p2, p3);
            const float t = t1 + (t2 - t1) * u;

            const Point a1 = Lerp(p0, p1, (t - t0) / (t1 - t0));
            const Point a2 = Lerp(p1, p2, (t - t1) / (t2 - t1));
            const Point a3 = Lerp(p2, p3, (t - t2) / (t3 - t2));
            const Point b1 = Lerp(a1, a2, (t - t0) / (t2 - t0));
            const Point b2 = Lerp(a2, a3, (t - t1) / (t3 - t1));
            return Lerp(b1, b2, (t - t1) / (t2 - t1));
        }

        // The curve as a dense polyline. Consecutive duplicate control points are dropped first:
        // a zero-length segment carries no direction to orient instances by.
        std::vector<Point> Tessellate(const std::vector<Point>& input)
        {
            std::vector<Point> points;
            for (const Point& p : input) {
                if (points.empty() || Distance(points.back(), p) > 1.0e-6f) {
                    points.push_back(p);
                }
            }
            if (points.size() < 2) {
                return {};
            }

            const size_t n = points.size();
            const auto at = [&](ptrdiff_t i) -> Point {
                if (i < 0) {
                    return { 2.0f * points[0].x - points[1].x, 2.0f * points[0].z - points[1].z };
                }
                if (i >= static_cast<ptrdiff_t>(n)) {
                    return { 2.0f * points[n - 1].x - points[n - 2].x, 2.0f * points[n - 1].z - points[n - 2].z };
                }
                return points[static_cast<size_t>(i)];
            };

            std::vector<Point> polyline;
            polyline.reserve((n - 1) * kSamplesPerSegment + 1);
            for (size_t seg = 0; seg + 1 < n; ++seg) {
                const ptrdiff_t i = static_cast<ptrdiff_t>(seg);
                for (int s = 0; s < kSamplesPerSegment; ++s) {
                    const float u = static_cast<float>(s) / kSamplesPerSegment;
                    polyline.push_back(EvaluateSegment(at(i - 1), at(i), at(i + 1), at(i + 2), u));
                }
            }
            polyline.push_back(points.back());
            return polyline;
        }
    }

    SplinePlacementResult PlaceAlongSpline(const SplinePlacementDesc& desc)
    {
        SplinePlacementResult result;
        if (!(desc.spacing > 0.0f)) {
            return result;
        }

        const std::vector<Point> polyline = Tessellate(desc.points);
        if (polyline.size() < 2) {
            return result;
        }

        PlacementRandom rng(desc.seed);
        float nextAt = 0.0f;  // arc length of the next instance
        float walked = 0.0f;  // arc length at the start of the current polyline edge
        for (size_t i = 0; i + 1 < polyline.size(); ++i) {
            const Point& a = polyline[i];
            const Point& b = polyline[i + 1];
            const float edge = Distance(a, b);
            if (edge <= 0.0f) {
                continue;
            }

            const float dirX = (b.x - a.x) / edge;
            const float dirZ = (b.z - a.z) / edge;
            while (nextAt <= walked + edge && result.transforms.size() < kMaxSplineInstances) {
                const Point p = Lerp(a, b, (nextAt - walked) / edge);

                // Drawn unconditionally so the stream does not depend on which options are zero.
                const float sideUnit = rng.NextUnit();
                const float yawUnit = rng.NextUnit();
                const float scaleUnit = rng.NextUnit();

                // Local +Z goes to (sin yaw, 0, cos yaw) (PlacementTransform.h), local +X to
                // (cos yaw, 0, -sin yaw): the right-hand side of the heading.
                const float heading = std::atan2(dirX, dirZ);
                const float side = (sideUnit - 0.5f) * desc.width;
                const float yaw = heading + (yawUnit - 0.5f) * desc.yawJitter;
                const float scale = desc.scaleMin + (desc.scaleMax - desc.scaleMin) * scaleUnit;
                result.transforms.push_back(
                    MakeYawScaleTranslation(yaw, scale, p.x + dirZ * side, desc.y, p.z - dirX * side));
                nextAt += desc.spacing;
            }
            walked += edge;
        }

        result.length = walked;
        return result;
    }
}
