#include "QuadtreeColumnLayout.h"

#include <algorithm>
#include <string>

namespace SasamiEngine::Sample::Wave
{
    namespace
    {
        constexpr uint32_t NO_LEAF = 0xFFFFFFFFu;

        struct Candidate
        {
            uint32_t x;
            uint32_t z;
            uint32_t level;
        };

        bool Fail(std::string* reason, const std::string& message)
        {
            if (reason)
                *reason = message;
            return false;
        }

        std::string Coord(uint32_t x, uint32_t z)
        {
            return "(" + std::to_string(x) + "," + std::to_string(z) + ")";
        }

        uint32_t DesiredAt(const std::vector<uint8_t>& desiredLog2, uint32_t nx, uint32_t nz, uint32_t x, uint32_t z,
                           uint32_t maxLog2)
        {
            if (desiredLog2.size() != size_t(nx) * nz)
                return 0;
            return (std::min)(uint32_t(desiredLog2[x + size_t(nx) * z]), maxLog2);
        }

        uint32_t MinDesiredInLeaf(const std::vector<uint8_t>& desiredLog2, uint32_t nx, uint32_t nz,
                                  const Candidate& c, uint32_t maxLog2)
        {
            const uint32_t size = 1u << c.level;
            const uint32_t x1 = (std::min)(c.x + size, nx);
            const uint32_t z1 = (std::min)(c.z + size, nz);
            uint32_t m = maxLog2;
            for (uint32_t z = c.z; z < z1; ++z)
                for (uint32_t x = c.x; x < x1; ++x)
                    m = (std::min)(m, DesiredAt(desiredLog2, nx, nz, x, z, maxLog2));
            return m;
        }

        void AppendChildren(const Candidate& c, uint32_t nx, uint32_t nz, std::vector<Candidate>& out)
        {
            const uint32_t half = 1u << (c.level - 1);
            for (uint32_t j = 0; j < 2; ++j)
            {
                for (uint32_t i = 0; i < 2; ++i)
                {
                    const uint32_t cx = c.x + i * half;
                    const uint32_t cz = c.z + j * half;
                    if (cx >= nx || cz >= nz)
                        continue;
                    out.push_back({cx, cz, c.level - 1});
                }
            }
        }

        void FillLevelMap(const std::vector<Candidate>& leaves, uint32_t nx, uint32_t nz, std::vector<uint8_t>& levelMap)
        {
            levelMap.assign(size_t(nx) * nz, 0);
            for (const Candidate& c : leaves)
            {
                const uint32_t size = 1u << c.level;
                const uint32_t x1 = (std::min)(c.x + size, nx);
                const uint32_t z1 = (std::min)(c.z + size, nz);
                for (uint32_t z = c.z; z < z1; ++z)
                    for (uint32_t x = c.x; x < x1; ++x)
                        levelMap[x + size_t(nx) * z] = uint8_t(c.level);
            }
        }

        bool HasCoarseNeighbourGap(const Candidate& c, uint32_t nx, uint32_t nz, const std::vector<uint8_t>& levelMap)
        {
            const uint32_t size = 1u << c.level;
            const uint32_t x1 = (std::min)(c.x + size, nx);
            const uint32_t z1 = (std::min)(c.z + size, nz);
            auto tooFine = [&](uint32_t x, uint32_t z) {
                return uint32_t(levelMap[x + size_t(nx) * z]) + 1 < c.level;
            };
            for (uint32_t z = c.z; z < z1; ++z)
            {
                if (c.x > 0 && tooFine(c.x - 1, z))
                    return true;
                if (x1 < nx && tooFine(x1, z))
                    return true;
            }
            for (uint32_t x = c.x; x < x1; ++x)
            {
                if (c.z > 0 && tooFine(x, c.z - 1))
                    return true;
                if (z1 < nz && tooFine(x, z1))
                    return true;
            }
            return false;
        }
    }

    QuadtreeColumnLayout BuildQuadtreeColumnLayout(uint32_t nx, uint32_t nz, const std::vector<uint8_t>& desiredLog2,
                                                   uint32_t maxLog2)
    {
        QuadtreeColumnLayout layout;
        layout.nx = nx;
        layout.nz = nz;
        if (nx == 0 || nz == 0)
            return layout;

        maxLog2 = (std::min)(maxLog2, QuadtreeColumnLayout::MAX_LEVELS - 1);
        layout.maxLog2 = maxLog2;

        const uint32_t blockSize = 1u << maxLog2;
        std::vector<Candidate> pending;
        for (uint32_t z = 0; z < nz; z += blockSize)
            for (uint32_t x = 0; x < nx; x += blockSize)
                pending.push_back({x, z, maxLog2});

        std::vector<Candidate> leaves;
        while (!pending.empty())
        {
            const Candidate c = pending.back();
            pending.pop_back();
            if (c.level > 0 && MinDesiredInLeaf(desiredLog2, nx, nz, c, maxLog2) < c.level)
                AppendChildren(c, nx, nz, pending);
            else
                leaves.push_back(c);
        }

        std::vector<uint8_t> levelMap;
        for (;;)
        {
            FillLevelMap(leaves, nx, nz, levelMap);
            std::vector<Candidate> next;
            next.reserve(leaves.size());
            bool changed = false;
            for (const Candidate& c : leaves)
            {
                if (c.level > 1 && HasCoarseNeighbourGap(c, nx, nz, levelMap))
                {
                    AppendChildren(c, nx, nz, next);
                    changed = true;
                }
                else
                {
                    next.push_back(c);
                }
            }
            leaves.swap(next);
            if (!changed)
                break;
        }

        std::sort(leaves.begin(), leaves.end(), [](const Candidate& a, const Candidate& b) {
            return a.z != b.z ? a.z < b.z : a.x < b.x;
        });

        layout.leafOf.assign(size_t(nx) * nz, NO_LEAF);
        layout.leaves.reserve(leaves.size());
        for (const Candidate& c : leaves)
        {
            const uint32_t index = uint32_t(layout.leaves.size());
            QuadtreeLeaf leaf;
            leaf.x = c.x;
            leaf.z = c.z;
            leaf.level = c.level;
            leaf.size = 1u << c.level;
            layout.leaves.push_back(leaf);
            layout.leavesPerLevel[c.level]++;

            const uint32_t x1 = (std::min)(c.x + leaf.size, nx);
            const uint32_t z1 = (std::min)(c.z + leaf.size, nz);
            for (uint32_t z = c.z; z < z1; ++z)
                for (uint32_t x = c.x; x < x1; ++x)
                    layout.leafOf[x + size_t(nx) * z] = index;
        }
        return layout;
    }

    bool ValidateQuadtreeColumnLayout(const QuadtreeColumnLayout& layout, const std::vector<uint8_t>* desiredLog2,
                                      std::string* reason)
    {
        const uint32_t nx = layout.nx;
        const uint32_t nz = layout.nz;
        const size_t columns = size_t(nx) * nz;

        if (layout.leafOf.size() != columns)
            return Fail(reason, "leafOf size " + std::to_string(layout.leafOf.size()) + " != nx*nz " +
                                    std::to_string(columns));
        if (layout.maxLog2 >= QuadtreeColumnLayout::MAX_LEVELS)
            return Fail(reason, "maxLog2 " + std::to_string(layout.maxLog2) + " out of range");

        for (size_t i = 0; i < columns; ++i)
        {
            if (layout.leafOf[i] >= layout.leaves.size())
                return Fail(reason, "leafOf index out of range at column " +
                                        Coord(uint32_t(i % nx), uint32_t(i / nx)));
        }

        uint32_t counts[QuadtreeColumnLayout::MAX_LEVELS] = {};
        for (size_t i = 0; i < layout.leaves.size(); ++i)
        {
            const QuadtreeLeaf& leaf = layout.leaves[i];
            const std::string where = "leaf " + std::to_string(i) + " at " + Coord(leaf.x, leaf.z);
            if (leaf.level >= QuadtreeColumnLayout::MAX_LEVELS)
                return Fail(reason, where + ": level " + std::to_string(leaf.level) + " out of range");
            if (leaf.level > layout.maxLog2)
                return Fail(reason, where + ": level " + std::to_string(leaf.level) + " > maxLog2");
            if (leaf.size != (1u << leaf.level))
                return Fail(reason, where + ": size " + std::to_string(leaf.size) + " != 1<<level");
            if (leaf.x >= nx || leaf.z >= nz)
                return Fail(reason, where + ": corner outside grid");
            if (leaf.x % leaf.size != 0 || leaf.z % leaf.size != 0)
                return Fail(reason, where + ": corner not aligned to size " + std::to_string(leaf.size));
            counts[leaf.level]++;
        }

        std::vector<uint32_t> coverage(columns, 0);
        for (size_t i = 0; i < layout.leaves.size(); ++i)
        {
            const QuadtreeLeaf& leaf = layout.leaves[i];
            const uint32_t x1 = (std::min)(leaf.x + leaf.size, nx);
            const uint32_t z1 = (std::min)(leaf.z + leaf.size, nz);
            for (uint32_t z = leaf.z; z < z1; ++z)
            {
                for (uint32_t x = leaf.x; x < x1; ++x)
                {
                    const size_t col = x + size_t(nx) * z;
                    if (++coverage[col] > 1)
                        return Fail(reason, "column " + Coord(x, z) + " covered by more than one leaf");
                    if (layout.leafOf[col] != i)
                        return Fail(reason, "leafOf at " + Coord(x, z) + " does not name the covering leaf " +
                                                std::to_string(i));
                }
            }
        }
        for (size_t i = 0; i < columns; ++i)
        {
            if (coverage[i] == 0)
                return Fail(reason, "column " + Coord(uint32_t(i % nx), uint32_t(i / nx)) + " not covered by any leaf");
        }

        for (uint32_t z = 0; z < nz; ++z)
        {
            for (uint32_t x = 0; x < nx; ++x)
            {
                const uint32_t level = layout.leaves[layout.LeafAt(x, z)].level;
                if (x + 1 < nx)
                {
                    const uint32_t other = layout.leaves[layout.LeafAt(x + 1, z)].level;
                    if (level + 1 < other || other + 1 < level)
                        return Fail(reason, "levels " + std::to_string(level) + " and " + std::to_string(other) +
                                                " differ by more than 1 between " + Coord(x, z) + " and " +
                                                Coord(x + 1, z));
                }
                if (z + 1 < nz)
                {
                    const uint32_t other = layout.leaves[layout.LeafAt(x, z + 1)].level;
                    if (level + 1 < other || other + 1 < level)
                        return Fail(reason, "levels " + std::to_string(level) + " and " + std::to_string(other) +
                                                " differ by more than 1 between " + Coord(x, z) + " and " +
                                                Coord(x, z + 1));
                }
                if (desiredLog2 && desiredLog2->size() == columns)
                {
                    const uint32_t allowed = (std::min)(uint32_t((*desiredLog2)[x + size_t(nx) * z]), layout.maxLog2);
                    if (level > allowed)
                        return Fail(reason, "column " + Coord(x, z) + " leaf level " + std::to_string(level) +
                                                " > allowed " + std::to_string(allowed));
                }
            }
        }

        for (uint32_t l = 0; l < QuadtreeColumnLayout::MAX_LEVELS; ++l)
        {
            if (layout.leavesPerLevel[l] != counts[l])
                return Fail(reason, "leavesPerLevel[" + std::to_string(l) + "] = " +
                                        std::to_string(layout.leavesPerLevel[l]) + " != " + std::to_string(counts[l]));
        }
        return true;
    }
}
