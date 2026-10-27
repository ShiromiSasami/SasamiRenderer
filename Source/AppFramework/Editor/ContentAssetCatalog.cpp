#include "AppFramework/Editor/ContentAssetCatalog.h"

#include "AppFramework/ApplicationResourcePaths.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <system_error>

namespace SasamiEngine::App
{
    namespace
    {
        ContentFolder& FindOrAddFolder(ContentFolder& parent, const std::string& name)
        {
            for (ContentFolder& folder : parent.folders)
            {
                if (folder.name == name)
                {
                    return folder;
                }
            }
            parent.folders.push_back(ContentFolder{ name, {}, {} });
            return parent.folders.back();
        }

        void SortTree(ContentFolder& folder)
        {
            std::sort(folder.folders.begin(), folder.folders.end(),
                [](const ContentFolder& a, const ContentFolder& b) { return a.name < b.name; });
            std::sort(folder.files.begin(), folder.files.end(),
                [](const ContentAssetEntry& a, const ContentAssetEntry& b) { return a.relPath < b.relPath; });
            for (ContentFolder& child : folder.folders)
            {
                SortTree(child);
            }
        }
    }

    bool ContentAssetCatalog::FormatForExtension(const std::string& extension, MeshComponent::ModelFormat& format)
    {
        std::string ext = extension;
        std::transform(ext.begin(), ext.end(), ext.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext == ".obj")
        {
            format = MeshComponent::ModelFormat::Obj;
            return true;
        }
        if (ext == ".gltf" || ext == ".glb")
        {
            format = MeshComponent::ModelFormat::Gltf;
            return true;
        }
        if (ext == ".fbx")
        {
            format = MeshComponent::ModelFormat::Fbx;
            return true;
        }
        return false;
    }

    void ContentAssetCatalog::Scan()
    {
        namespace fs = std::filesystem;
        m_entries.clear();
        m_scanned = true;

        const fs::path root = fs::path(ApplicationResourcePaths::ResolveAssetPathWide("")).lexically_normal();
        std::error_code ec;
        fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
        const fs::recursive_directory_iterator end;
        for (; !ec && it != end; it.increment(ec))
        {
            if (!it->is_regular_file(ec))
            {
                continue;
            }
            MeshComponent::ModelFormat format = MeshComponent::ModelFormat::Obj;
            if (!FormatForExtension(it->path().extension().string(), format))
            {
                continue;
            }
            ContentAssetEntry entry;
            entry.relPath = it->path().lexically_relative(root).generic_string();
            entry.format = format;
            entry.sizeBytes = it->file_size(ec);
            m_entries.push_back(entry);
        }

        std::sort(m_entries.begin(), m_entries.end(),
            [](const ContentAssetEntry& a, const ContentAssetEntry& b) { return a.relPath < b.relPath; });
        m_root = BuildContentTree(m_entries);
    }

    const ContentAssetEntry* ContentAssetCatalog::Find(const std::string& relPath) const
    {
        for (const ContentAssetEntry& entry : m_entries)
        {
            if (entry.relPath == relPath)
            {
                return &entry;
            }
        }
        return nullptr;
    }

    ContentFolder BuildContentTree(const std::vector<ContentAssetEntry>& entries)
    {
        ContentFolder root;
        for (const ContentAssetEntry& entry : entries)
        {
            ContentFolder* folder = &root;
            size_t start = 0;
            for (size_t slash = entry.relPath.find('/'); slash != std::string::npos;
                 slash = entry.relPath.find('/', start))
            {
                folder = &FindOrAddFolder(*folder, entry.relPath.substr(start, slash - start));
                start = slash + 1;
            }
            folder->files.push_back(entry);
        }
        SortTree(root);
        return root;
    }

    int RunContentAssetCatalogSelfTest(int& checks)
    {
        int failures = 0;
        auto check = [&](bool ok) { ++checks; failures += ok ? 0 : 1; };

        MeshComponent::ModelFormat format = MeshComponent::ModelFormat::Obj;
        check(ContentAssetCatalog::FormatForExtension(".FBX", format) && format == MeshComponent::ModelFormat::Fbx);
        check(ContentAssetCatalog::FormatForExtension(".glb", format) && format == MeshComponent::ModelFormat::Gltf);
        check(ContentAssetCatalog::FormatForExtension(".Obj", format) && format == MeshComponent::ModelFormat::Obj);
        check(!ContentAssetCatalog::FormatForExtension(".png", format));

        const std::vector<ContentAssetEntry> entries = {
            { "Models/Fox/Fox.gltf", MeshComponent::ModelFormat::Gltf, 0 },
            { "Models/Bunny.obj", MeshComponent::ModelFormat::Obj, 0 },
            { "Top.fbx", MeshComponent::ModelFormat::Fbx, 0 },
            { "Models/Bistro/A.fbx", MeshComponent::ModelFormat::Fbx, 0 },
        };
        const ContentFolder root = BuildContentTree(entries);
        check(root.files.size() == 1 && root.files[0].relPath == "Top.fbx");
        check(root.folders.size() == 1 && root.folders[0].name == "Models");
        if (root.folders.size() == 1)
        {
            const ContentFolder& models = root.folders[0];
            check(models.files.size() == 1 && models.files[0].relPath == "Models/Bunny.obj");
            check(models.folders.size() == 2 && models.folders[0].name == "Bistro" && models.folders[1].name == "Fox");
        }
        return failures;
    }
}
