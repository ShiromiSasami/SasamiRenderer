#include "AppFramework/Debug/IsoSurfaceOverlayDraw.h"

#include "AppFramework/Debug/WindOverlayProjection.h"
#include "Foundation/Jobs/ParallelFor.h"
#include "Renderer/Fluid/IsoSurfaceMesh.h"
#include "Renderer/Voxel/SignedDistanceGrid.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace SasamiEngine::App::Debug
{
    namespace
    {
        constexpr uint32_t VERTEX_CHUNK = 2048;
        constexpr uint32_t TRIANGLE_CHUNK = 4096;

        enum VertexFlags : uint8_t
        {
            VERTEX_VALID = 1,
            VERTEX_OCCLUDED = 2
        };

        struct ScreenVertex
        {
            ImVec2 position;
            float depth = 0.0f;
            ImU32 color = 0;
            uint8_t flags = 0;
        };

        float Clamp01(float value)
        {
            return (std::min)((std::max)(value, 0.0f), 1.0f);
        }

        ImU32 ShadeColor(const IsoSurfaceDrawStyle& style, float attribute, const float normal[3],
                         const float viewDir[3])
        {
            float rgb[3] = { 255.0f, 255.0f, 255.0f };
            float alpha = 1.0f;
            style.color(attribute, rgb, alpha);

            const float nDotL = normal[0] * style.lightDirection[0] + normal[1] * style.lightDirection[1] +
                                normal[2] * style.lightDirection[2];
            const float shade = style.ambient + (1.0f - style.ambient) * (std::max)(nDotL, 0.0f) * style.diffuse;
            const float nDotV = normal[0] * viewDir[0] + normal[1] * viewDir[1] + normal[2] * viewDir[2];
            const float rimBase = 1.0f - std::fabs(nDotV);
            const float rim = style.rim * rimBase * rimBase;
            const float emissive = Clamp01(style.emissive);
            const float factor = (shade + rim) * (1.0f - emissive) + (1.0f + rim) * emissive;

            const auto channel = [factor](float value) {
                return static_cast<int>((std::min)((std::max)(value * factor, 0.0f), 255.0f) + 0.5f);
            };
            const int a = static_cast<int>(Clamp01(alpha) * 255.0f + 0.5f);
            return IM_COL32(channel(rgb[0]), channel(rgb[1]), channel(rgb[2]), a);
        }
    }

    uint32_t DrawIsoSurfaceOverlay(const Render::Fluid::IsoSurfaceMesh& mesh, const IsoSurfaceDrawStyle& style,
                                   const Render::RenderCameraProxy& camera, const ImVec2& origin, const ImVec2& size,
                                   ImDrawList* drawList, const Render::Voxel::SignedDistanceGrid* sdf,
                                   const Render::Voxel::VoxelGridDesc& desc)
    {
        const uint32_t triangleCount = mesh.TriangleCount();
        if (triangleCount == 0 || drawList == nullptr || style.color == nullptr) {
            return 0;
        }

        const uint32_t vertexCount = mesh.VertexCount();
        std::vector<ScreenVertex> vertices(vertexCount);
        Foundation::ParallelFor((vertexCount + VERTEX_CHUNK - 1) / VERTEX_CHUNK, [&](uint32_t chunk) {
            const uint32_t begin = chunk * VERTEX_CHUNK;
            const uint32_t chunkEnd = (std::min)(begin + VERTEX_CHUNK, vertexCount);
            for (uint32_t i = begin; i < chunkEnd; ++i) {
                ScreenVertex& vertex = vertices[i];
                const float* p = &mesh.positions[static_cast<size_t>(i) * 3];
                if (!ProjectToOverlay(camera, origin, size, p, vertex.position, &vertex.depth)) {
                    continue;
                }
                vertex.flags = VERTEX_VALID;
                if (sdf != nullptr && IsOccludedBySdf(*sdf, desc, camera.cameraPosition, p)) {
                    vertex.flags |= VERTEX_OCCLUDED;
                }

                const float viewRaw[3] = { camera.cameraPosition[0] - p[0], camera.cameraPosition[1] - p[1],
                                           camera.cameraPosition[2] - p[2] };
                const float viewLength = std::sqrt(viewRaw[0] * viewRaw[0] + viewRaw[1] * viewRaw[1] +
                                                   viewRaw[2] * viewRaw[2]);
                const float inv = viewLength > 1e-12f ? 1.0f / viewLength : 0.0f;
                const float viewDir[3] = { viewRaw[0] * inv, viewRaw[1] * inv, viewRaw[2] * inv };
                vertex.color = ShadeColor(style, mesh.attributes[i], &mesh.normals[static_cast<size_t>(i) * 3],
                                          viewDir);
            }
        });

        // Per chunk lists of kept triangles, concatenated in chunk order so the result is deterministic.
        const uint32_t chunkCount = (triangleCount + TRIANGLE_CHUNK - 1) / TRIANGLE_CHUNK;
        std::vector<std::vector<std::pair<float, uint32_t>>> chunkKept(chunkCount);
        Foundation::ParallelFor(chunkCount, [&](uint32_t chunk) {
            const uint32_t begin = chunk * TRIANGLE_CHUNK;
            const uint32_t chunkEnd = (std::min)(begin + TRIANGLE_CHUNK, triangleCount);
            std::vector<std::pair<float, uint32_t>>& kept = chunkKept[chunk];
            for (uint32_t t = begin; t < chunkEnd; ++t) {
                const ScreenVertex& a = vertices[mesh.indices[t * 3]];
                const ScreenVertex& b = vertices[mesh.indices[t * 3 + 1]];
                const ScreenVertex& c = vertices[mesh.indices[t * 3 + 2]];
                if (!((a.flags & b.flags & c.flags) & VERTEX_VALID)) {
                    continue;
                }
                if ((a.flags & b.flags & c.flags) & VERTEX_OCCLUDED) {
                    continue;
                }
                if (style.cullBackFaces) {
                    // Front faces wind counter-clockwise in a y-up frame; ImGui's y points down, which
                    // mirrors the winding, so a front face has a negative 2D cross product here.
                    const float cross = (b.position.x - a.position.x) * (c.position.y - a.position.y) -
                                        (b.position.y - a.position.y) * (c.position.x - a.position.x);
                    if (cross >= 0.0f) {
                        continue;
                    }
                }
                kept.emplace_back((a.depth + b.depth + c.depth) * (1.0f / 3.0f), t);
            }
        });

        std::vector<std::pair<float, uint32_t>> order;
        for (const auto& kept : chunkKept) {
            order.insert(order.end(), kept.begin(), kept.end());
        }
        // Far first; equal depths fall back to the triangle index.
        std::sort(order.begin(), order.end(), [](const auto& lhs, const auto& rhs) {
            if (lhs.first != rhs.first) {
                return lhs.first > rhs.first;
            }
            return lhs.second < rhs.second;
        });

        const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
        for (const auto& entry : order) {
            const uint32_t* tri = &mesh.indices[entry.second * 3];
            drawList->PrimReserve(3, 3);
            const ImDrawIdx base = static_cast<ImDrawIdx>(drawList->_VtxCurrentIdx);
            drawList->PrimWriteIdx(base);
            drawList->PrimWriteIdx(static_cast<ImDrawIdx>(base + 1));
            drawList->PrimWriteIdx(static_cast<ImDrawIdx>(base + 2));
            for (int k = 0; k < 3; ++k) {
                const ScreenVertex& vertex = vertices[tri[k]];
                drawList->PrimWriteVtx(vertex.position, uv, vertex.color);
            }
        }
        return static_cast<uint32_t>(order.size());
    }
}
