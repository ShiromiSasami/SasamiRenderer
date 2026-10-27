#include "Renderer/VirtualGeometry/ClusterResidencyManager.h"

#include <algorithm>
#include <bit>
#include <cstdio>

namespace SasamiEngine::Render
{
    namespace
    {
        constexpr uint32_t kBitsPerWord = 32u;
        constexpr uint32_t kDefaultMaxLoadsPerFrame = 256u;

        uint32_t WordCountForGroups(uint32_t totalGroups)
        {
            return (totalGroups + kBitsPerWord - 1u) / kBitsPerWord;
        }

        bool GetBit(const std::vector<uint32_t>& bits, uint32_t index)
        {
            const uint32_t word = index / kBitsPerWord;
            if (word >= bits.size()) {
                return false;
            }
            return (bits[word] & (1u << (index % kBitsPerWord))) != 0u;
        }

        void SetBit(std::vector<uint32_t>& bits, uint32_t index)
        {
            const uint32_t word = index / kBitsPerWord;
            if (word < bits.size()) {
                bits[word] |= (1u << (index % kBitsPerWord));
            }
        }

        void ClearBit(std::vector<uint32_t>& bits, uint32_t index)
        {
            const uint32_t word = index / kBitsPerWord;
            if (word < bits.size()) {
                bits[word] &= ~(1u << (index % kBitsPerWord));
            }
        }

        // Zeroes the bits at index >= totalGroups in the trailing word so popcount/fill operations stay exact.
        void MaskTrailingBits(std::vector<uint32_t>& bits, uint32_t totalGroups)
        {
            if (bits.empty()) {
                return;
            }
            const uint32_t validBitsInLastWord = totalGroups % kBitsPerWord;
            if (validBitsInLastWord == 0u) {
                return;
            }
            const uint32_t mask = (1u << validBitsInLastWord) - 1u;
            bits.back() &= mask;
        }

        uint32_t PopCount(const std::vector<uint32_t>& bits)
        {
            uint32_t count = 0;
            for (uint32_t word : bits) {
                count += static_cast<uint32_t>(std::popcount(word));
            }
            return count;
        }

        // Finds the non-pinned resident group with the smallest last-wanted stamp, never picking a group wanted
        // this frame. One linear pass over the words, starting from scanStartWord to spread repeated evictions
        // within the same Update() call across the bitset instead of always rescanning from the front.
        int64_t FindEvictionVictim(const std::vector<uint32_t>& resident, const std::vector<uint32_t>& pinned,
            const std::vector<uint64_t>& lastWanted, uint32_t totalGroups, uint64_t currentFrame,
            uint32_t retentionFrames, uint32_t& scanStartWord)
        {
            const uint32_t wordCount = static_cast<uint32_t>(resident.size());
            if (wordCount == 0u) {
                return -1;
            }

            int64_t bestGroup = -1;
            uint64_t bestWanted = 0;
            uint32_t bestWord = scanStartWord;

            for (uint32_t offset = 0; offset < wordCount; ++offset) {
                const uint32_t w = (scanStartWord + offset) % wordCount;
                uint32_t candidates = resident[w] & ~pinned[w];
                while (candidates != 0u) {
                    const uint32_t bitIndex = static_cast<uint32_t>(std::countr_zero(candidates));
                    candidates &= candidates - 1u;
                    const uint32_t group = w * kBitsPerWord + bitIndex;
                    if (group >= totalGroups) {
                        continue;
                    }
                    // Groups wanted within the retention window stay put; evicting them is what
                    // drives the request set to oscillate (see SetRetentionFrames). lastWanted == 0 means
                    // never requested, so it is always a candidate.
                    if (lastWanted[group] != 0ull && retentionFrames != 0u &&
                        currentFrame >= lastWanted[group] &&
                        (currentFrame - lastWanted[group]) < static_cast<uint64_t>(retentionFrames)) {
                        continue;
                    }
                    const uint64_t wanted = lastWanted[group];
                    if (bestGroup < 0 || wanted < bestWanted) {
                        bestGroup = static_cast<int64_t>(group);
                        bestWanted = wanted;
                        bestWord = w;
                    }
                }
            }

            if (bestGroup >= 0) {
                scanStartWord = bestWord;
            }
            return bestGroup;
        }
    }

    void ClusterResidencyManager::Reset(uint32_t totalGroups, const std::vector<uint32_t>& pinnedBits)
    {
        const uint32_t wordCount = WordCountForGroups(totalGroups);

        m_pinned.assign(wordCount, 0u);
        const size_t copyCount = std::min<size_t>(wordCount, pinnedBits.size());
        for (size_t i = 0; i < copyCount; ++i) {
            m_pinned[i] = pinnedBits[i];
        }
        MaskTrailingBits(m_pinned, totalGroups);

        m_lastWanted.assign(totalGroups, 0ull);
        m_pendingSince.assign(totalGroups, 0ull);

        const uint32_t budget = m_stats.budgetGroups;
        m_stats = ClusterResidencyStats{};
        m_stats.totalGroups = totalGroups;
        m_stats.budgetGroups = budget;
        m_stats.maxLoadsPerFrame = kDefaultMaxLoadsPerFrame;
        m_stats.retentionFrames = m_retentionFrames;
        m_stats.pinnedGroups = PopCount(m_pinned);

        if (budget == 0u) {
            m_resident.assign(wordCount, 0xFFFFFFFFu);
            MaskTrailingBits(m_resident, totalGroups);
        } else {
            m_resident = m_pinned;
        }

        m_residentCount = PopCount(m_resident);
        m_stats.residentGroups = m_residentCount;
    }

    void ClusterResidencyManager::SetBudget(uint32_t maxResidentGroups)
    {
        m_stats.budgetGroups = maxResidentGroups;

        if (maxResidentGroups == 0u) {
            std::fill(m_resident.begin(), m_resident.end(), 0xFFFFFFFFu);
            MaskTrailingBits(m_resident, m_stats.totalGroups);
            m_residentCount = m_stats.totalGroups;
            m_stats.residentGroups = m_residentCount;
            return;
        }

        uint32_t scanStartWord = 0;
        while (m_residentCount > maxResidentGroups) {
            // Shrinking the budget is a configuration change, not a frame: evict freely.
            const int64_t victim = FindEvictionVictim(m_resident, m_pinned, m_lastWanted, m_stats.totalGroups,
                0ull, 0u, scanStartWord);
            if (victim < 0) {
                break; // nothing left to evict (remaining resident groups are all pinned)
            }
            ClearBit(m_resident, static_cast<uint32_t>(victim));
            --m_residentCount;
            ++m_stats.totalEvictions;
        }

        m_stats.residentGroups = m_residentCount;
    }

    void ClusterResidencyManager::SetMaxLoadsPerFrame(uint32_t loads)
    {
        m_stats.maxLoadsPerFrame = loads;
    }

    void ClusterResidencyManager::SetRetentionFrames(uint32_t frames)
    {
        m_retentionFrames = frames;
        m_stats.retentionFrames = frames;
    }

    void ClusterResidencyManager::Update(const std::vector<uint32_t>& wantedBits, uint64_t frameIndex)
    {
        m_stats.loadsThisFrame = 0;
        m_stats.evictionsThisFrame = 0;
        m_stats.missingWanted = 0;

        const uint32_t totalGroups = m_stats.totalGroups;
        const uint32_t wordCount = WordCountForGroups(totalGroups);
        const size_t wantedWordCount = std::min<size_t>(wordCount, wantedBits.size());
        const uint32_t cap = m_stats.maxLoadsPerFrame;
        const uint32_t budget = m_stats.budgetGroups;
        uint32_t scanStartWord = 0;

        for (size_t w = 0; w < wantedWordCount; ++w) {
            uint32_t word = wantedBits[w];
            while (word != 0u) {
                const uint32_t bitIndex = static_cast<uint32_t>(std::countr_zero(word));
                word &= word - 1u;
                const uint32_t group = static_cast<uint32_t>(w) * kBitsPerWord + bitIndex;
                if (group >= totalGroups) {
                    continue;
                }

                m_lastWanted[group] = frameIndex;

                if (GetBit(m_resident, group)) {
                    m_pendingSince[group] = 0; // already resident: stamp refreshed above
                    continue;
                }

                if (m_pendingSince[group] == 0) {
                    m_pendingSince[group] = frameIndex;
                }

                if (cap != 0u && m_stats.loadsThisFrame >= cap) {
                    ++m_stats.missingWanted;
                    continue;
                }

                bool canLoad = (budget == 0u) || (m_residentCount < budget);
                if (!canLoad) {
                    const int64_t victim = FindEvictionVictim(m_resident, m_pinned, m_lastWanted, totalGroups,
                        frameIndex, m_retentionFrames, scanStartWord);
                    if (victim >= 0) {
                        ClearBit(m_resident, static_cast<uint32_t>(victim));
                        m_pendingSince[static_cast<uint32_t>(victim)] = 0;
                        --m_residentCount;
                        ++m_stats.evictionsThisFrame;
                        ++m_stats.totalEvictions;
                        canLoad = true;
                    }
                }

                if (canLoad) {
                    SetBit(m_resident, group);
                    ++m_residentCount;
                    ++m_stats.loadsThisFrame;
                    ++m_stats.totalLoads;

                    const uint32_t wait = static_cast<uint32_t>(frameIndex - m_pendingSince[group]);
                    m_stats.waitFramesLast = wait;
                    if (wait > m_stats.waitFramesMax) {
                        m_stats.waitFramesMax = wait;
                    }
                    m_stats.waitFramesTotal += wait;
                    ++m_stats.waitSamples;
                    m_pendingSince[group] = 0;
                } else {
                    ++m_stats.missingWanted;
                }
            }
        }

        m_stats.residentGroups = m_residentCount;
    }

    bool ClusterResidencyManager::IsResident(uint32_t group) const
    {
        return GetBit(m_resident, group);
    }

    std::string ClusterResidencyManager::FormatReport() const
    {
        char buffer[384];
        std::snprintf(buffer, sizeof(buffer),
            "resident=%u/%u pinned=%u budget=%u loads=%u(+%llu) evictions=%u(+%llu) missing=%u retention=%u"
            " wait=%u/%u/%.2f(last/max/avg frames over %llu loads)",
            m_stats.residentGroups, m_stats.totalGroups, m_stats.pinnedGroups, m_stats.budgetGroups,
            m_stats.loadsThisFrame, static_cast<unsigned long long>(m_stats.totalLoads),
            m_stats.evictionsThisFrame, static_cast<unsigned long long>(m_stats.totalEvictions),
            m_stats.missingWanted, m_stats.retentionFrames,
            m_stats.waitFramesLast, m_stats.waitFramesMax,
            m_stats.waitSamples != 0 ? static_cast<double>(m_stats.waitFramesTotal) / static_cast<double>(m_stats.waitSamples) : 0.0,
            static_cast<unsigned long long>(m_stats.waitSamples));
        return buffer;
    }
}
