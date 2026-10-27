#include "Renderer/VirtualGeometry/ClusterDagSerializer.h"

#include <cstring>
#include <limits>
#include <type_traits>

namespace SasamiEngine::Render
{
    namespace
    {
        constexpr uint32_t kMagic = 0x47414453u; // "SDAG" little-endian

        // Packed-by-construction header: uint32/uint64 fields only, no padding, so the
        // bytes written here match the bytes read back regardless of struct alignment.
        struct FileHeader
        {
            uint32_t magic = 0;
            uint32_t formatVersion = 0;
            uint32_t clusterStructSize = 0;
            uint32_t groupStructSize = 0;
            uint32_t nodeStructSize = 0;
            uint64_t indexCount = 0;
            uint64_t clusterCount = 0;
            uint64_t groupCount = 0;
            uint64_t nodeCount = 0;
            uint32_t levelCount = 0;
            uint32_t terminalGroupCount = 0;
            uint32_t sourceTriangleCount = 0;
            uint32_t sourceVertexCount = 0;
        };

        static_assert(std::is_trivially_copyable_v<ClusterDagCluster>,
                      "ClusterDagCluster must stay POD for bulk serialization");
        static_assert(std::is_trivially_copyable_v<ClusterDagGroup>,
                      "ClusterDagGroup must stay POD for bulk serialization");
        static_assert(std::is_trivially_copyable_v<ClusterDagNode>,
                      "ClusterDagNode must stay POD for bulk serialization");

        template <typename T>
        void AppendBytes(std::vector<std::byte>& out, const T& value)
        {
            const auto* src = reinterpret_cast<const std::byte*>(&value);
            out.insert(out.end(), src, src + sizeof(T));
        }

        template <typename T>
        void AppendArray(std::vector<std::byte>& out, const std::vector<T>& values)
        {
            if (values.empty()) {
                return;
            }
            const auto* src = reinterpret_cast<const std::byte*>(values.data());
            out.insert(out.end(), src, src + values.size() * sizeof(T));
        }

        // Overflow-safe: computes count * elemSize and checks it fits in the bytes that
        // remain after `offset`, without ever letting count * elemSize wrap uint64.
        bool ArrayByteSizeFits(uint64_t count, uint64_t elemSize, uint64_t offset, uint64_t totalBytes,
                               uint64_t& outByteSize)
        {
            if (elemSize != 0 && count > (std::numeric_limits<uint64_t>::max)() / elemSize) {
                return false;
            }
            outByteSize = count * elemSize;
            if (offset > totalBytes || outByteSize > totalBytes - offset) {
                return false;
            }
            return true;
        }

        template <typename T>
        bool ReadArray(std::span<const std::byte> bytes, uint64_t& offset, uint64_t count,
                       std::vector<T>& out)
        {
            uint64_t byteSize = 0;
            if (!ArrayByteSizeFits(count, sizeof(T), offset, static_cast<uint64_t>(bytes.size()), byteSize)) {
                return false;
            }
            out.resize(static_cast<size_t>(count));
            if (byteSize > 0) {
                std::memcpy(out.data(), bytes.data() + offset, static_cast<size_t>(byteSize));
            }
            offset += byteSize;
            return true;
        }
    }

    std::vector<std::byte> ClusterDagSerializer::Serialize(const ClusterDag& dag)
    {
        FileHeader header{};
        header.magic = kMagic;
        header.formatVersion = kFormatVersion;
        header.clusterStructSize = static_cast<uint32_t>(sizeof(ClusterDagCluster));
        header.groupStructSize = static_cast<uint32_t>(sizeof(ClusterDagGroup));
        header.nodeStructSize = static_cast<uint32_t>(sizeof(ClusterDagNode));
        header.indexCount = static_cast<uint64_t>(dag.indices.size());
        header.clusterCount = static_cast<uint64_t>(dag.clusters.size());
        header.groupCount = static_cast<uint64_t>(dag.groups.size());
        header.nodeCount = static_cast<uint64_t>(dag.nodes.size());
        header.levelCount = dag.levelCount;
        header.terminalGroupCount = dag.terminalGroupCount;
        header.sourceTriangleCount = dag.sourceTriangleCount;
        header.sourceVertexCount = dag.sourceVertexCount;

        std::vector<std::byte> out;
        out.reserve(sizeof(FileHeader) +
                    dag.indices.size() * sizeof(uint32_t) +
                    dag.clusters.size() * sizeof(ClusterDagCluster) +
                    dag.groups.size() * sizeof(ClusterDagGroup) +
                    dag.nodes.size() * sizeof(ClusterDagNode));

        AppendBytes(out, header);
        AppendArray(out, dag.indices);
        AppendArray(out, dag.clusters);
        AppendArray(out, dag.groups);
        AppendArray(out, dag.nodes);
        return out;
    }

    bool ClusterDagSerializer::Deserialize(std::span<const std::byte> bytes, ClusterDag& outDag)
    {
        outDag = ClusterDag{};

        if (bytes.size() < sizeof(FileHeader)) {
            return false;
        }

        FileHeader header{};
        std::memcpy(&header, bytes.data(), sizeof(FileHeader));

        if (header.magic != kMagic || header.formatVersion != kFormatVersion) {
            return false;
        }
        if (header.clusterStructSize != sizeof(ClusterDagCluster) ||
            header.groupStructSize != sizeof(ClusterDagGroup) ||
            header.nodeStructSize != sizeof(ClusterDagNode)) {
            return false;
        }

        uint64_t offset = sizeof(FileHeader);

        std::vector<uint32_t> indices;
        std::vector<ClusterDagCluster> clusters;
        std::vector<ClusterDagGroup> groups;
        std::vector<ClusterDagNode> nodes;

        if (!ReadArray(bytes, offset, header.indexCount, indices) ||
            !ReadArray(bytes, offset, header.clusterCount, clusters) ||
            !ReadArray(bytes, offset, header.groupCount, groups) ||
            !ReadArray(bytes, offset, header.nodeCount, nodes)) {
            return false;
        }

        for (const uint32_t index : indices) {
            if (index >= header.sourceVertexCount) {
                return false;
            }
        }

        const uint64_t indexCount = indices.size();
        for (const ClusterDagCluster& cluster : clusters) {
            const uint64_t begin = cluster.indexOffset;
            const uint64_t end = begin + static_cast<uint64_t>(cluster.indexCount);
            if (end < begin || end > indexCount) {
                return false;
            }
        }

        const uint64_t clusterCount = clusters.size();
        for (const ClusterDagGroup& group : groups) {
            const uint64_t begin = group.firstCluster;
            const uint64_t end = begin + static_cast<uint64_t>(group.clusterCount);
            if (end < begin || end > clusterCount) {
                return false;
            }
        }

        const uint64_t groupCount = groups.size();
        const uint64_t nodeCount = nodes.size();
        for (const ClusterDagNode& node : nodes) {
            if (node.group != ClusterDag::kInvalidIndex && node.group >= groupCount) {
                return false;
            }
            const uint64_t begin = node.childOffset;
            const uint64_t end = begin + static_cast<uint64_t>(node.childCount);
            if (end < begin || end > nodeCount) {
                return false;
            }
        }

        outDag.indices = std::move(indices);
        outDag.clusters = std::move(clusters);
        outDag.groups = std::move(groups);
        outDag.nodes = std::move(nodes);
        outDag.levelCount = header.levelCount;
        outDag.terminalGroupCount = header.terminalGroupCount;
        outDag.sourceTriangleCount = header.sourceTriangleCount;
        outDag.sourceVertexCount = header.sourceVertexCount;
        return true;
    }
}
