#include "Renderer/Voxel/BrickOccupancySelfTest.h"

#include "Renderer/Voxel/BrickOccupancyGrid.h"

#include <bitset>
#include <chrono>
#include <iomanip>
#include <random>
#include <sstream>

namespace SasamiEngine::Render::Voxel
{
    bool CheckBrickRoundTrip(const OccupancyBitGrid& dense, std::string& out)
    {
        const auto buildStart = std::chrono::steady_clock::now();
        BrickOccupancyGrid bricks;
        bricks.Build(dense);
        const double buildMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - buildStart).count();

        OccupancyBitGrid decoded;
        bricks.Decode(decoded);
        uint64_t diffBits = 0;
        const std::vector<uint32_t>& a = dense.Words();
        const std::vector<uint32_t>& b = decoded.Words();
        const bool sizeMatch = a.size() == b.size();
        if (sizeMatch)
        {
            for (size_t i = 0; i < a.size(); ++i)
            {
                diffBits += std::bitset<32>(a[i] ^ b[i]).count();
            }
        }

        const uint64_t emptyBricks = bricks.BrickCount() - bricks.FullBrickCount() - bricks.MixedBrickCount();
        const double ratio = bricks.SizeInBytes() > 0
            ? static_cast<double>(dense.SizeInBytes()) / static_cast<double>(bricks.SizeInBytes())
            : 0.0;
        std::ostringstream line;
        line << "grid=" << dense.DimX() << "x" << dense.DimY() << "x" << dense.DimZ()
             << " occupied=" << dense.CountOccupied()
             << " bricks=" << bricks.BrickCountX() << "x" << bricks.BrickCountY() << "x" << bricks.BrickCountZ()
             << " full=" << bricks.FullBrickCount() << " mixed=" << bricks.MixedBrickCount() << " empty=" << emptyBricks
             << " dense=" << dense.SizeInBytes() << "B sparse=" << bricks.SizeInBytes()
             << "B (top=" << bricks.TopLevelBytes() << " payload=" << bricks.PayloadBytes() << ")"
             << std::fixed << std::setprecision(2) << " ratio=" << ratio << "x"
             << " build=" << buildMs << "ms diffBits=" << (sizeMatch ? diffBits : ~0ull);
        out += line.str();
        return sizeMatch && diffBits == 0;
    }

    std::string RunBrickOccupancySelfTest()
    {
        struct Case
        {
            const char* name;
            OccupancyBitGrid grid;
        };

        std::vector<Case> cases;
        cases.push_back({ "empty", OccupancyBitGrid(13, 9, 17) });

        OccupancyBitGrid full(21, 16, 11);
        for (uint32_t z = 0; z < full.DimZ(); ++z)
            for (uint32_t y = 0; y < full.DimY(); ++y)
                for (uint32_t x = 0; x < full.DimX(); ++x)
                    full.Set(x, y, z);
        cases.push_back({ "full", full });

        OccupancyBitGrid random(37, 29, 45);
        std::mt19937 rng(12345u);
        std::uniform_int_distribution<uint32_t> pick(0, 99);
        for (uint32_t z = 0; z < random.DimZ(); ++z)
            for (uint32_t y = 0; y < random.DimY(); ++y)
                for (uint32_t x = 0; x < random.DimX(); ++x)
                    if (pick(rng) < 3)
                        random.Set(x, y, z);
        cases.push_back({ "random3%", random });

        OccupancyBitGrid sphere(50, 50, 50);
        for (uint32_t z = 0; z < sphere.DimZ(); ++z)
            for (uint32_t y = 0; y < sphere.DimY(); ++y)
                for (uint32_t x = 0; x < sphere.DimX(); ++x)
                {
                    const int dx = static_cast<int>(x) - 24;
                    const int dy = static_cast<int>(y) - 24;
                    const int dz = static_cast<int>(z) - 24;
                    if (dx * dx + dy * dy + dz * dz <= 20 * 20)
                        sphere.Set(x, y, z);
                }
        cases.push_back({ "sphere", sphere });

        bool allOk = true;
        std::string body;
        for (const Case& c : cases)
        {
            std::string line;
            const bool ok = CheckBrickRoundTrip(c.grid, line);
            allOk = allOk && ok;
            body += std::string("  ") + (ok ? "OK " : "ERR ") + c.name + ": " + line;
        }
        return std::string(allOk ? "OK" : "ERR") + " brick round-trip (" + std::to_string(cases.size()) + " cases)" + body;
    }
}
