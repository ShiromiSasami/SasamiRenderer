#include "Renderer/Culling/GpuHzbSelfTest.h"

#include "Renderer/Culling/GpuHzbBuilder.h"
#include "Renderer/Culling/HzbPyramid.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <vector>

namespace SasamiEngine::Render::Culling
{
    namespace
    {
        struct Lcg
        {
            uint32_t state;

            uint32_t Next()
            {
                state = state * 1664525u + 1013904223u;
                return state >> 8;
            }

            float NextFloat() { return static_cast<float>(Next()) / 16777216.0f; }
            uint32_t NextBelow(uint32_t n) { return Next() % n; }
        };

        uint32_t FloatBits(float value)
        {
            uint32_t bits = 0;
            std::memcpy(&bits, &value, sizeof(bits));
            return bits;
        }

        std::string HexBits(float value)
        {
            std::ostringstream s;
            s << "0x" << std::hex << std::setw(8) << std::setfill('0') << FloatBits(value);
            return s.str();
        }
    }

    std::string RunGpuHzbSelfTest(IRhiDevice& device)
    {
        struct Size { uint32_t w; uint32_t h; };
        const Size sizes[] = {
            { 1, 1 }, { 1, 7 }, { 7, 1 }, { 3, 3 }, { 5, 4 }, { 641, 359 }, { 1920, 1080 },
        };

        GpuHzbBuilder builder;
        std::string error;
        if (!builder.Initialize(device, error)) {
            return "ERR " + error;
        }

        uint64_t totalLevels = 0;
        uint64_t totalTexels = 0;
        double gpuMs = 0.0;
        uint32_t sizeIndex = 0;
        for (const Size& size : sizes) {
            const uint32_t w = size.w;
            const uint32_t h = size.h;
            const std::string where = "size=" + std::to_string(w) + "x" + std::to_string(h);

            Lcg rng{ 777u + sizeIndex * 7919u };
            const bool withSpecials = (sizeIndex % 2) == 1;
            ++sizeIndex;

            std::vector<float> depth(static_cast<size_t>(w) * h);
            for (float& d : depth) {
                d = rng.NextFloat();
                if (withSpecials) {
                    const uint32_t roll = rng.NextBelow(100);
                    if (roll < 10) {
                        d = 0.0f;
                    } else if (roll < 15) {
                        d = 1.0f;
                    }
                }
            }

            HzbPyramid cpu;
            cpu.Build(depth.data(), w, h);

            std::vector<float> gpuTexels;
            std::vector<GpuHzbLevel> gpuLevels;
            const auto begin = std::chrono::steady_clock::now();
            const bool built = builder.BuildAndReadback(depth.data(), w, h, gpuTexels, gpuLevels, error);
            const auto end = std::chrono::steady_clock::now();
            if (!built) {
                return "ERR " + where + " " + error;
            }
            if (w == 1920 && h == 1080) {
                gpuMs = std::chrono::duration<double, std::milli>(end - begin).count();
            }

            if (gpuLevels.size() != cpu.LevelCount()) {
                return "ERR " + where + " level count cpu=" + std::to_string(cpu.LevelCount())
                    + " gpu=" + std::to_string(gpuLevels.size());
            }

            for (uint32_t level = 0; level < cpu.LevelCount(); ++level) {
                const GpuHzbLevel& gl = gpuLevels[level];
                const uint32_t lw = cpu.LevelWidth(level);
                const uint32_t lh = cpu.LevelHeight(level);
                if (gl.width != lw || gl.height != lh) {
                    std::ostringstream s;
                    s << "ERR " << where << " level=" << level << " dims cpu=" << lw << "x" << lh
                      << " gpu=" << gl.width << "x" << gl.height;
                    return s.str();
                }
                if (static_cast<size_t>(gl.offset) + static_cast<size_t>(lw) * lh > gpuTexels.size()) {
                    return "ERR " + where + " level=" + std::to_string(level) + " offset out of readback range";
                }

                for (uint32_t y = 0; y < lh; ++y) {
                    for (uint32_t x = 0; x < lw; ++x) {
                        const float c = cpu.Texel(level, x, y);
                        const float g = gpuTexels[gl.offset + static_cast<size_t>(y) * lw + x];
                        if (FloatBits(c) != FloatBits(g)) {
                            std::ostringstream s;
                            s << "ERR " << where << " level=" << level << " x=" << x << " y=" << y
                              << " cpu=" << HexBits(c) << " gpu=" << HexBits(g);
                            return s.str();
                        }
                    }
                }
                totalTexels += static_cast<uint64_t>(lw) * lh;
            }
            totalLevels += cpu.LevelCount();
        }

        std::ostringstream out;
        out << "OK sizes=" << (sizeof(sizes) / sizeof(sizes[0])) << " levels=" << totalLevels
            << " texels=" << totalTexels << " mismatch=0 gpuMs=" << std::fixed << std::setprecision(3) << gpuMs;
        return out.str();
    }
}
