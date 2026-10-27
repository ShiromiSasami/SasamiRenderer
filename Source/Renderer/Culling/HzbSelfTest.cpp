#include "Renderer/Culling/HzbSelfTest.h"

#include "Renderer/Culling/HzbPyramid.h"

#include <algorithm>
#include <cstdint>
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

        // Level-0 inclusive range covered by each texel of each level, along one axis.
        // Derived by expanding the per-level read range [2i, 2i+1 (+2i+2 on an odd edge)].
        struct AxisFootprint
        {
            std::vector<std::vector<uint32_t>> lo;
            std::vector<std::vector<uint32_t>> hi;
        };

        AxisFootprint MakeAxisFootprint(uint32_t size)
        {
            AxisFootprint fp;
            fp.lo.emplace_back();
            fp.hi.emplace_back();
            for (uint32_t i = 0; i < size; ++i) {
                fp.lo[0].push_back(i);
                fp.hi[0].push_back(i);
            }

            uint32_t prev = size;
            while (prev > 1) {
                const uint32_t dim = std::max(1u, prev / 2);
                const std::vector<uint32_t>& pl = fp.lo.back();
                const std::vector<uint32_t>& ph = fp.hi.back();
                std::vector<uint32_t> lo;
                std::vector<uint32_t> hi;
                for (uint32_t i = 0; i < dim; ++i) {
                    uint32_t last = 2 * i + 1;
                    if ((prev & 1u) && i == dim - 1) {
                        last = 2 * i + 2;
                    }
                    last = std::min(last, prev - 1);
                    lo.push_back(pl[2 * i]);
                    hi.push_back(ph[last]);
                }
                fp.lo.push_back(std::move(lo));
                fp.hi.push_back(std::move(hi));
                prev = dim;
            }
            return fp;
        }

        // Closed-form span of a level-0 range [a, b] mapped to `level` (dim = that level's size).
        uint32_t MappedLo(uint32_t a, uint32_t level, uint32_t dim) { return std::min(a >> level, dim - 1); }
        uint32_t MappedHi(uint32_t b, uint32_t level, uint32_t dim) { return std::min(b >> level, dim - 1); }

        struct Checker
        {
            uint64_t checks = 0;
            uint64_t texels = 0;
            uint64_t rects = 0;
            uint64_t levelSum = 0;
            std::string error;

            bool Fail(const std::string& message)
            {
                if (error.empty()) {
                    error = message;
                }
                return false;
            }
        };

        std::string Describe(uint32_t w, uint32_t h)
        {
            std::ostringstream s;
            s << w << "x" << h;
            return s.str();
        }

        bool CheckLevels(Checker& c, const HzbPyramid& pyr, const std::vector<float>& depth,
                         uint32_t w, uint32_t h, const AxisFootprint& fx, const AxisFootprint& fy, uint32_t stride)
        {
            for (uint32_t level = 0; level < pyr.LevelCount(); ++level) {
                const uint32_t lw = pyr.LevelWidth(level);
                const uint32_t lh = pyr.LevelHeight(level);
                if (level >= fx.lo.size() || lw != fx.lo[level].size() || lh != fy.lo[level].size()) {
                    return c.Fail("ERR level dims size=" + Describe(w, h) + " level=" + std::to_string(level));
                }

                const uint32_t step = (stride > 1 && lw * lh > 1024) ? stride : 1;
                uint32_t counter = 0;
                for (uint32_t y = 0; y < lh; ++y) {
                    for (uint32_t x = 0; x < lw; ++x) {
                        if (step > 1 && (counter++ % step) != 0) {
                            continue;
                        }
                        float expected = depth[fy.lo[level][y] * w + fx.lo[level][x]];
                        for (uint32_t yy = fy.lo[level][y]; yy <= fy.hi[level][y]; ++yy) {
                            for (uint32_t xx = fx.lo[level][x]; xx <= fx.hi[level][x]; ++xx) {
                                expected = std::min(expected, depth[yy * w + xx]);
                            }
                        }
                        ++c.checks;
                        ++c.texels;
                        if (pyr.Texel(level, x, y) != expected) {
                            std::ostringstream s;
                            s << "ERR texel mismatch size=" << w << "x" << h << " level=" << level << " x=" << x << " y=" << y
                              << " got=" << pyr.Texel(level, x, y) << " expected=" << expected;
                            return c.Fail(s.str());
                        }
                    }
                }
            }
            return true;
        }

        // Every level-0 texel is covered by exactly one texel per level (per axis, hence in 2D).
        bool CheckCoverage(Checker& c, const AxisFootprint& f, uint32_t size, const char* axis)
        {
            for (size_t level = 0; level < f.lo.size(); ++level) {
                std::vector<uint32_t> count(size, 0);
                for (size_t i = 0; i < f.lo[level].size(); ++i) {
                    for (uint32_t p = f.lo[level][i]; p <= f.hi[level][i]; ++p) {
                        ++count[p];
                    }
                }
                for (uint32_t p = 0; p < size; ++p) {
                    ++c.checks;
                    if (count[p] != 1) {
                        std::ostringstream s;
                        s << "ERR coverage axis=" << axis << " size=" << size << " level=" << level << " texel=" << p << " count=" << count[p];
                        return c.Fail(s.str());
                    }
                }
            }
            return true;
        }

        bool CheckRects(Checker& c, const HzbPyramid& pyr, const std::vector<float>& depth,
                        uint32_t w, uint32_t h, uint32_t count, Lcg& rng)
        {
            for (uint32_t i = 0; i < count; ++i) {
                HzbRect rect;
                const uint32_t kind = rng.NextBelow(4);
                const uint32_t maxW = (kind == 0) ? 1u : (kind == 1 ? 3u : w);
                const uint32_t maxH = (kind == 0) ? 1u : (kind == 1 ? 3u : h);
                rect.x0 = rng.NextBelow(w);
                rect.y0 = rng.NextBelow(h);
                rect.x1 = std::min(w - 1, rect.x0 + rng.NextBelow(std::min(maxW, w)));
                rect.y1 = std::min(h - 1, rect.y0 + rng.NextBelow(std::min(maxH, h)));

                float trueMin = depth[rect.y0 * w + rect.x0];
                for (uint32_t y = rect.y0; y <= rect.y1; ++y) {
                    for (uint32_t x = rect.x0; x <= rect.x1; ++x) {
                        trueMin = std::min(trueMin, depth[y * w + x]);
                    }
                }

                uint32_t level = 0;
                const float hzbMin = pyr.FootprintMin(rect, &level);
                ++c.rects;
                c.levelSum += level;

                std::ostringstream where;
                where << " size=" << w << "x" << h << " rect=(" << rect.x0 << "," << rect.y0 << ")-(" << rect.x1 << "," << rect.y1 << ")";

                ++c.checks;
                if (hzbMin > trueMin) {
                    return c.Fail("ERR not conservative" + where.str());
                }

                // Occlusion: closest depths around the true min and random values.
                const float closestDepths[3] = { trueMin, rng.NextFloat(), hzbMin };
                for (const float closest : closestDepths) {
                    ++c.checks;
                    if (trueMin <= closest && pyr.IsOccluded(rect, closest)) {
                        return c.Fail("ERR false occlusion" + where.str());
                    }
                }

                // Level choice: first level whose mapped rect spans at most 2x2 texels.
                ++c.checks;
                if (level >= pyr.LevelCount()) {
                    return c.Fail("ERR level out of range" + where.str());
                }
                auto span = [&](uint32_t lvl) {
                    const uint32_t sx = MappedHi(rect.x1, lvl, pyr.LevelWidth(lvl)) - MappedLo(rect.x0, lvl, pyr.LevelWidth(lvl));
                    const uint32_t sy = MappedHi(rect.y1, lvl, pyr.LevelHeight(lvl)) - MappedLo(rect.y0, lvl, pyr.LevelHeight(lvl));
                    return std::max(sx, sy);
                };
                if (span(level) > 1) {
                    return c.Fail("ERR chosen level spans more than 2x2" + where.str());
                }
                if (level > 0 && span(level - 1) <= 1) {
                    return c.Fail("ERR chosen level is not the first fitting level" + where.str());
                }
                if (rect.x0 == rect.x1 && rect.y0 == rect.y1) {
                    ++c.checks;
                    if (level != 0) {
                        return c.Fail("ERR 1-texel rect not on level 0" + where.str());
                    }
                }
            }
            return true;
        }

        bool CheckRectFromUv(Checker& c)
        {
            const uint32_t w = 64;
            const uint32_t h = 32;
            const HzbRect full = HzbPyramid::RectFromUv(0.0f, 0.0f, 1.0f, 1.0f, w, h);
            const HzbRect over = HzbPyramid::RectFromUv(-0.5f, -2.0f, 1.5f, 3.0f, w, h);
            const HzbRect mid = HzbPyramid::RectFromUv(0.5f, 0.25f, 0.5f, 0.25f, w, h);
            c.checks += 3;
            if (full.x0 != 0 || full.y0 != 0 || full.x1 != w - 1 || full.y1 != h - 1) {
                return c.Fail("ERR RectFromUv u=1 must map to width-1");
            }
            if (over.x0 != 0 || over.y0 != 0 || over.x1 != w - 1 || over.y1 != h - 1) {
                return c.Fail("ERR RectFromUv out-of-range uv must clamp");
            }
            if (mid.x0 != 32 || mid.x1 != 32 || mid.y0 != 8 || mid.y1 != 8) {
                return c.Fail("ERR RectFromUv mid-point mapping");
            }
            return true;
        }
    }

    std::string RunHzbSelfTest()
    {
        struct Size { uint32_t w; uint32_t h; };
        const Size sizes[] = {
            { 1, 1 }, { 1, 7 }, { 7, 1 }, { 5, 5 }, { 13, 9 }, { 64, 64 }, { 127, 65 }, { 641, 359 },
        };

        Checker c;
        Lcg rng{ 12345u };
        for (const Size& size : sizes) {
            const uint32_t w = size.w;
            const uint32_t h = size.h;
            std::vector<float> depth(static_cast<size_t>(w) * h);
            for (float& d : depth) {
                d = rng.NextFloat();
            }

            HzbPyramid pyr;
            pyr.Build(depth.data(), w, h);
            const AxisFootprint fx = MakeAxisFootprint(w);
            const AxisFootprint fy = MakeAxisFootprint(h);
            const bool big = static_cast<uint64_t>(w) * h > 100000;

            ++c.checks;
            if (pyr.LevelCount() != fx.lo.size() || pyr.LevelCount() != fy.lo.size()) {
                // Axes reach size 1 at different levels; the pyramid runs until both are 1.
                if (pyr.LevelCount() != std::max(fx.lo.size(), fy.lo.size())) {
                    return "ERR level count size=" + Describe(w, h);
                }
            }
            if (pyr.LevelWidth(pyr.LevelCount() - 1) != 1 || pyr.LevelHeight(pyr.LevelCount() - 1) != 1) {
                return "ERR top level is not 1x1 size=" + Describe(w, h);
            }

            // Pad the shorter axis table with its 1-wide top so both reach the same depth.
            AxisFootprint px = fx;
            AxisFootprint py = fy;
            while (px.lo.size() < pyr.LevelCount()) {
                px.lo.push_back(px.lo.back());
                px.hi.push_back(px.hi.back());
            }
            while (py.lo.size() < pyr.LevelCount()) {
                py.lo.push_back(py.lo.back());
                py.hi.push_back(py.hi.back());
            }

            if (!CheckLevels(c, pyr, depth, w, h, px, py, big ? 7u : 1u)) {
                return c.error;
            }
            if (!CheckCoverage(c, px, w, "x") || !CheckCoverage(c, py, h, "y")) {
                return c.error;
            }
            if (!CheckRects(c, pyr, depth, w, h, big ? 3000u : 4000u, rng)) {
                return c.error;
            }
        }

        if (!CheckRectFromUv(c)) {
            return c.error;
        }

        std::ostringstream out;
        out << "OK checks=" << c.checks << " texels=" << c.texels << " rects=" << c.rects
            << " meanLevel=" << (c.rects ? static_cast<double>(c.levelSum) / static_cast<double>(c.rects) : 0.0);
        return out.str();
    }
}
