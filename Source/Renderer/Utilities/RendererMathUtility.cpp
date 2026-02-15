#include "Renderer/Utilities/RendererMathUtility.h"

#include "Renderer/Structures/Float3.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace
{
    using SasamiRenderer::Math::Float3;
    using SasamiRenderer::Math::Cross;
    using SasamiRenderer::Math::Dot;
    using SasamiRenderer::Math::Normalize;

    static float ClampFloat(float x, float minV, float maxV)
    {
        if (x < minV) return minV;
        if (x > maxV) return maxV;
        return x;
    }

    static float MaxFloat(float a, float b)
    {
        return (a > b) ? a : b;
    }

    static float RadicalInverseVdC(std::uint32_t bits)
    {
        // Van der Corput radical inverse (base-2) using bit reversal.
        // Maps integer index i to low-discrepancy value in [0,1).
        bits = (bits << 16) | (bits >> 16);
        bits = ((bits & 0x55555555u) << 1) | ((bits & 0xAAAAAAAAu) >> 1);
        bits = ((bits & 0x33333333u) << 2) | ((bits & 0xCCCCCCCCu) >> 2);
        bits = ((bits & 0x0F0F0F0Fu) << 4) | ((bits & 0xF0F0F0F0u) >> 4);
        bits = ((bits & 0x00FF00FFu) << 8) | ((bits & 0xFF00FF00u) >> 8);
        return static_cast<float>(bits) * 2.3283064365386963e-10f;
    }

    static std::array<float, 2> Hammersley(std::uint32_t i, std::uint32_t n)
    {
        // 2D Hammersley sequence:
        // xi = (i / n, radicalInverseVdC(i))
        return { static_cast<float>(i) / static_cast<float>(n), RadicalInverseVdC(i) };
    }

    static Float3 CubemapTexelDirection(std::uint32_t face,
                                        std::uint32_t x,
                                        std::uint32_t y,
                                        std::uint32_t faceSize)
    {
        // Convert cubemap texel center to normalized 3D direction.
        // s,t are face-local coordinates in [-1,1].
        const float s = ((static_cast<float>(x) + 0.5f) / static_cast<float>(faceSize)) * 2.0f - 1.0f;
        const float t = ((static_cast<float>(y) + 0.5f) / static_cast<float>(faceSize)) * 2.0f - 1.0f;

        switch (face) {
        case 0: return Normalize(Float3{ 1.0f, -t, -s }); // +X
        case 1: return Normalize(Float3{ -1.0f, -t, s }); // -X
        case 2: return Normalize(Float3{ s, 1.0f, t }); // +Y
        case 3: return Normalize(Float3{ s, -1.0f, -t }); // -Y
        case 4: return Normalize(Float3{ s, -t, 1.0f }); // +Z
        default:return Normalize(Float3{ -s, -t, -1.0f }); // -Z
        }
    }

    static Float3 SampleEquirect(const std::vector<float>& hdrPixels,
                                 std::uint32_t width,
                                 std::uint32_t height,
                                 const Float3& dir)
    {
        const float pi = 3.1415926535f;
        // Direction -> equirectangular UV mapping:
        // u = atan2(z, x) / (2*pi) + 0.5
        // v = acos(y) / pi
        const float u = std::atan2(dir.z, dir.x) / (2.0f * pi) + 0.5f;
        const float v = std::acos(ClampFloat(dir.y, -1.0f, 1.0f)) / pi;

        float fx = u * static_cast<float>(width) - 0.5f;
        float fy = v * static_cast<float>(height) - 0.5f;
        int x0 = static_cast<int>(std::floor(fx));
        int y0 = static_cast<int>(std::floor(fy));
        const float tx = fx - static_cast<float>(x0);
        const float ty = fy - static_cast<float>(y0);
        int x1 = x0 + 1;
        int y1 = y0 + 1;

        auto wrapX = [width](int x) -> int {
            int w = static_cast<int>(width);
            x %= w;
            if (x < 0) x += w;
            return x;
        };
        auto clampY = [height](int y) -> int {
            if (y < 0) return 0;
            int h = static_cast<int>(height);
            if (y >= h) return h - 1;
            return y;
        };

        x0 = wrapX(x0);
        x1 = wrapX(x1);
        y0 = clampY(y0);
        y1 = clampY(y1);

        auto fetch = [&](int x, int y) -> Float3 {
            const std::size_t idx = (static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x)) * 3u;
            return { hdrPixels[idx + 0], hdrPixels[idx + 1], hdrPixels[idx + 2] };
        };

        const Float3 c00 = fetch(x0, y0);
        const Float3 c10 = fetch(x1, y0);
        const Float3 c01 = fetch(x0, y1);
        const Float3 c11 = fetch(x1, y1);

        // Bilinear filtering in equirect space.
        const Float3 c0 = c00 * (1.0f - tx) + c10 * tx;
        const Float3 c1 = c01 * (1.0f - tx) + c11 * tx;
        return c0 * (1.0f - ty) + c1 * ty;
    }

    static Float3 CosineSampleHemisphere(float u1, float u2)
    {
        // Cosine-weighted hemisphere sampling:
        // r = sqrt(u1), phi = 2*pi*u2
        // (x, y, z) = (r*cos(phi), r*sin(phi), sqrt(1-u1))
        const float r = std::sqrt(u1);
        const float phi = 2.0f * 3.1415926535f * u2;
        const float x = r * std::cos(phi);
        const float y = r * std::sin(phi);
        const float z = std::sqrt(MaxFloat(0.0f, 1.0f - u1));
        return { x, y, z };
    }

    static void BuildOrthonormalBasis(const Float3& n, Float3& t, Float3& b)
    {
        // Build tangent-space basis (t, b, n).
        // Choose a non-parallel up vector to avoid degeneracy.
        const Float3 up = (std::fabs(n.y) < 0.999f) ? Float3{ 0.0f, 1.0f, 0.0f } : Float3{ 1.0f, 0.0f, 0.0f };
        t = Normalize(Cross(up, n));
        b = Cross(n, t);
    }

    static Float3 ToWorldFromLocal(const Float3& local, const Float3& n)
    {
        // Local tangent-space vector -> world-space vector:
        // world = local.x * T + local.y * B + local.z * N
        Float3 t, b;
        BuildOrthonormalBasis(n, t, b);
        return Normalize(t * local.x + b * local.y + n * local.z);
    }

    static Float3 ImportanceSampleGGX(const std::array<float, 2>& xi, float roughness, const Float3& n)
    {
        // GGX importance sample of half vector H:
        // alpha = roughness^2
        // cosTheta = sqrt((1 - xi.y) / (1 + (alpha^2 - 1) * xi.y))
        const float a = roughness * roughness;
        const float phi = 2.0f * 3.1415926535f * xi[0];
        const float cosTheta = std::sqrt((1.0f - xi[1]) / (1.0f + (a * a - 1.0f) * xi[1]));
        const float sinTheta = std::sqrt(MaxFloat(0.0f, 1.0f - cosTheta * cosTheta));

        Float3 hLocal = {
            sinTheta * std::cos(phi),
            sinTheta * std::sin(phi),
            cosTheta
        };
        return ToWorldFromLocal(hLocal, n);
    }

    static float GeometrySchlickGGX(float nDotV, float roughness)
    {
        // Schlick-GGX G1 with Disney remapping:
        // k = (roughness + 1)^2 / 8
        // G1 = N.V / (N.V * (1-k) + k)
        const float r = roughness + 1.0f;
        const float k = (r * r) / 8.0f;
        return nDotV / MaxFloat(nDotV * (1.0f - k) + k, 1e-6f);
    }

    static std::array<float, 2> IntegrateBrdf(float nDotV, float roughness)
    {
        const std::uint32_t sampleCount = 128;
        const Float3 v = { std::sqrt(MaxFloat(0.0f, 1.0f - nDotV * nDotV)), 0.0f, nDotV };

        float a = 0.0f;
        float b = 0.0f;
        const Float3 n = { 0.0f, 0.0f, 1.0f };
        for (std::uint32_t i = 0; i < sampleCount; ++i) {
            const auto xi = Hammersley(i, sampleCount);
            const Float3 h = ImportanceSampleGGX(xi, MaxFloat(roughness, 0.04f), n);
            const Float3 l = Normalize(h * (2.0f * Dot(v, h)) - v);

            const float nDotL = MaxFloat(l.z, 0.0f);
            const float nDotH = MaxFloat(h.z, 0.0f);
            const float vDotH = MaxFloat(Dot(v, h), 0.0f);
            if (nDotL > 0.0f) {
                const float gV = GeometrySchlickGGX(MaxFloat(nDotV, 0.0f), roughness);
                const float gL = GeometrySchlickGGX(nDotL, roughness);
                const float g = gV * gL;
                // Visibility term used by split-sum BRDF LUT.
                // gVis = (G * V.H) / (N.H * N.V)
                const float gVis = (g * vDotH) / MaxFloat(nDotH * nDotV, 1e-6f);
                // Schlick Fresnel weight split:
                // F = F0 * (1 - fc) + 1 * fc, where fc = (1 - V.H)^5
                const float fc = std::pow(1.0f - vDotH, 5.0f);
                a += (1.0f - fc) * gVis;
                b += fc * gVis;
            }
        }
        return { a / static_cast<float>(sampleCount), b / static_cast<float>(sampleCount) };
    }
}

namespace SasamiRenderer
{
    namespace RendererMathUtility
    {
        void GenerateSkyCubemapFromEquirect(const std::vector<float>& hdrPixels,
                                            std::uint32_t hdrWidth,
                                            std::uint32_t hdrHeight,
                                            std::uint32_t faceSize,
                                            std::vector<std::vector<float>>& outFaces)
        {
            // Skybox conversion:
            // for each cubemap texel direction D, sample HDR equirectangular radiance L(D).
            outFaces.assign(6, std::vector<float>(static_cast<std::size_t>(faceSize) * faceSize * 4u, 0.0f));
            for (std::uint32_t face = 0; face < 6; ++face) {
                auto& dst = outFaces[face];
                for (std::uint32_t y = 0; y < faceSize; ++y) {
                    for (std::uint32_t x = 0; x < faceSize; ++x) {
                        const Float3 dir = CubemapTexelDirection(face, x, y, faceSize);
                        const Float3 c = SampleEquirect(hdrPixels, hdrWidth, hdrHeight, dir);
                        const std::size_t idx = (static_cast<std::size_t>(y) * faceSize + static_cast<std::size_t>(x)) * 4u;
                        dst[idx + 0] = c.x;
                        dst[idx + 1] = c.y;
                        dst[idx + 2] = c.z;
                        dst[idx + 3] = 1.0f;
                    }
                }
            }
        }

        void GenerateIrradianceCubemapFromEquirect(const std::vector<float>& hdrPixels,
                                                   std::uint32_t hdrWidth,
                                                   std::uint32_t hdrHeight,
                                                   std::uint32_t faceSize,
                                                   std::vector<std::vector<float>>& outFaces)
        {
            const std::uint32_t sampleCount = 64;
            // Diffuse irradiance approximation:
            // E(N) ~= (1 / sampleCount) * sum L(wi), wi sampled with cosine-weighted hemisphere about N.
            // (The cosine term is baked by the sampling PDF.)
            outFaces.assign(6, std::vector<float>(static_cast<std::size_t>(faceSize) * faceSize * 4u, 0.0f));
            for (std::uint32_t face = 0; face < 6; ++face) {
                auto& dst = outFaces[face];
                for (std::uint32_t y = 0; y < faceSize; ++y) {
                    for (std::uint32_t x = 0; x < faceSize; ++x) {
                        const Float3 n = CubemapTexelDirection(face, x, y, faceSize);
                        Float3 acc = { 0.0f, 0.0f, 0.0f };
                        for (std::uint32_t i = 0; i < sampleCount; ++i) {
                            const auto xi = Hammersley(i, sampleCount);
                            const Float3 lLocal = CosineSampleHemisphere(xi[0], xi[1]);
                            const Float3 lWorld = ToWorldFromLocal(lLocal, n);
                            const Float3 s = SampleEquirect(hdrPixels, hdrWidth, hdrHeight, lWorld);
                            acc += s;
                        }
                        acc *= 1.0f / static_cast<float>(sampleCount);
                        const std::size_t idx = (static_cast<std::size_t>(y) * faceSize + static_cast<std::size_t>(x)) * 4u;
                        dst[idx + 0] = acc.x;
                        dst[idx + 1] = acc.y;
                        dst[idx + 2] = acc.z;
                        dst[idx + 3] = 1.0f;
                    }
                }
            }
        }

        void GeneratePrefilterCubemapFromEquirect(const std::vector<float>& hdrPixels,
                                                  std::uint32_t hdrWidth,
                                                  std::uint32_t hdrHeight,
                                                  std::uint32_t baseFaceSize,
                                                  std::uint32_t mipLevels,
                                                  std::vector<std::vector<float>>& outSubresources)
        {
            outSubresources.assign(static_cast<std::size_t>(mipLevels) * 6u, {});
            const std::uint32_t sampleCount = 128;
            // Specular prefilter (split-sum first term):
            // prefiltered(R, roughness) ~= sum L(wi) * (N.wi) / sum (N.wi)
            // wi are generated via GGX importance sampling per roughness(mip).
            for (std::uint32_t face = 0; face < 6; ++face) {
                for (std::uint32_t mip = 0; mip < mipLevels; ++mip) {
                    const std::uint32_t size = (baseFaceSize >> mip) > 0 ? (baseFaceSize >> mip) : 1u;
                    auto& dst = outSubresources[mip + face * mipLevels];
                    dst.resize(static_cast<std::size_t>(size) * size * 4u, 0.0f);
                    const float roughness = (mipLevels > 1) ? static_cast<float>(mip) / static_cast<float>(mipLevels - 1) : 0.0f;

                    for (std::uint32_t y = 0; y < size; ++y) {
                        for (std::uint32_t x = 0; x < size; ++x) {
                            const Float3 r = CubemapTexelDirection(face, x, y, size);
                            const Float3 v = r;
                            Float3 acc = { 0.0f, 0.0f, 0.0f };
                            float weight = 0.0f;
                            for (std::uint32_t i = 0; i < sampleCount; ++i) {
                                const auto xi = Hammersley(i, sampleCount);
                                const Float3 h = ImportanceSampleGGX(xi, MaxFloat(roughness, 0.04f), r);
                                const float vDotH = MaxFloat(Dot(v, h), 0.0f);
                                const Float3 l = Normalize(h * (2.0f * vDotH) - v);
                                const float nDotL = MaxFloat(Dot(r, l), 0.0f);
                                if (nDotL > 0.0f) {
                                    const Float3 s = SampleEquirect(hdrPixels, hdrWidth, hdrHeight, l);
                                    acc += s * nDotL;
                                    weight += nDotL;
                                }
                            }
                            if (weight > 1e-6f) {
                                acc *= 1.0f / weight;
                            }

                            const std::size_t idx = (static_cast<std::size_t>(y) * size + static_cast<std::size_t>(x)) * 4u;
                            dst[idx + 0] = acc.x;
                            dst[idx + 1] = acc.y;
                            dst[idx + 2] = acc.z;
                            dst[idx + 3] = 1.0f;
                        }
                    }
                }
            }
        }

        void GenerateBrdfLut(std::uint32_t width,
                             std::uint32_t height,
                             std::vector<float>& outPixels)
        {
            // BRDF integration LUT (split-sum second term):
            // stores (A, B) such that specIBL ~= prefilteredEnv * (F0 * A + B)
            outPixels.assign(static_cast<std::size_t>(width) * height * 4u, 0.0f);
            for (std::uint32_t y = 0; y < height; ++y) {
                const float roughness = (static_cast<float>(y) + 0.5f) / static_cast<float>(height);
                for (std::uint32_t x = 0; x < width; ++x) {
                    const float nDotV = (static_cast<float>(x) + 0.5f) / static_cast<float>(width);
                    const auto brdf = IntegrateBrdf(nDotV, roughness);
                    const std::size_t idx = (static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x)) * 4u;
                    outPixels[idx + 0] = brdf[0];
                    outPixels[idx + 1] = brdf[1];
                    outPixels[idx + 2] = 0.0f;
                    outPixels[idx + 3] = 1.0f;
                }
            }
        }
    }
}
