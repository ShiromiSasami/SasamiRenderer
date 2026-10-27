#pragma once

#include "AppFramework/Component/MeshComponent.h"

#include <cstdint>
#include <string>
#include <vector>

namespace SasamiEngine::App
{
    // One model file under Assets/. relPath is Assets/-relative with '/' separators, the form
    // StaticModel::LoadModel takes (e.g. "Models/Bunny.obj").
    struct ContentAssetEntry
    {
        std::string relPath;
        MeshComponent::ModelFormat format = MeshComponent::ModelFormat::Obj;
        uint64_t sizeBytes = 0;
    };

    // Folder of the Content Browser tree; folders and files are sorted by name.
    struct ContentFolder
    {
        std::string name;  // last path segment; empty for the root
        std::vector<ContentFolder> folders;
        std::vector<ContentAssetEntry> files;
    };

    // Lists the model files (.obj, .gltf, .glb, .fbx) under Assets/. Scanning walks the disk, so the
    // Content Browser keeps the result and rescans only on Refresh.
    class ContentAssetCatalog
    {
    public:
        // Maps a file extension (any case, with the dot) to the loader format. False for non-models.
        static bool FormatForExtension(const std::string& extension, MeshComponent::ModelFormat& format);

        // Recursive scan of the Assets root. Unreadable directories are skipped.
        void Scan();
        const std::vector<ContentAssetEntry>& Entries() const { return m_entries; }
        const ContentFolder& Root() const { return m_root; }
        bool HasScanned() const { return m_scanned; }

        // Finds an entry by its relPath, or null.
        const ContentAssetEntry* Find(const std::string& relPath) const;

    private:
        std::vector<ContentAssetEntry> m_entries;  // sorted by relPath
        ContentFolder m_root;
        bool m_scanned = false;
    };

    // Builds the folder tree from entries (relPath split on '/'). Pure, so the selftest can call it.
    ContentFolder BuildContentTree(const std::vector<ContentAssetEntry>& entries);

    // Extension mapping and tree building on synthetic paths. Adds to `checks`, returns failures.
    int RunContentAssetCatalogSelfTest(int& checks);
}
