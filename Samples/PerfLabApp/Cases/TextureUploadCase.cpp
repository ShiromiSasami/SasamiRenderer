#include "Cases/TextureUploadCase.h"

#include "Cases/PerfMeshSource.h"
#include "ApplicationCore.h"
#include "ApplicationResourcePaths.h"
#include "Renderer/Runtime/Renderer.h"
#include "Renderer/RHI/GraphicsDevice.h"
#include "Foundation/Tools/ScopedPerfTimer.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <new>

namespace SasamiEngine::Sample::PerfLab
{
    namespace
    {
        using App::DdsTextureLoader::DdsFormat;

        constexpr const char* kKinds[] = { "_BaseColor.dds", "_Normal.dds", "_Specular.dds" };

        bool EndsWith(const std::string& text, const char* suffix)
        {
            const size_t length = std::char_traits<char>::length(suffix);
            return text.size() >= length && text.compare(text.size() - length, length, suffix) == 0;
        }

        // RhiFormat of the BC blocks as stored; Unknown for formats the RHI does not take (BC2, BC6H).
        Render::RhiFormat BlockFormat(DdsFormat format, bool srgb)
        {
            using Render::RhiFormat;
            switch (format) {
            case DdsFormat::BC1: return srgb ? RhiFormat::BC1UNormSrgb : RhiFormat::BC1UNorm;
            case DdsFormat::BC3: return srgb ? RhiFormat::BC3UNormSrgb : RhiFormat::BC3UNorm;
            case DdsFormat::BC4: return RhiFormat::BC4UNorm;
            case DdsFormat::BC5: return RhiFormat::BC5UNorm;
            case DdsFormat::BC7: return srgb ? RhiFormat::BC7UNormSrgb : RhiFormat::BC7UNorm;
            default: return RhiFormat::Unknown;
            }
        }

        bool ReadFile(const std::filesystem::path& path, std::vector<uint8_t>& out)
        {
            std::ifstream file(path, std::ios::binary | std::ios::ate);
            if (!file.is_open()) {
                return false;
            }
            const std::streamoff size = file.tellg();
            if (size <= 0) {
                return false;
            }
            out.resize(static_cast<size_t>(size));
            file.seekg(0);
            file.read(reinterpret_cast<char*>(out.data()), size);
            return static_cast<bool>(file);
        }
    }

    TextureUploadCase::TextureUploadCase(std::string name, App::ApplicationCore& app, std::string textureDir)
        : m_name(std::move(name))
        , m_app(app)
        , m_textureDir(std::move(textureDir))
    {
        m_description = m_textureDir + ": RGBA8 decode + upload vs BC upload";
    }

    bool TextureUploadCase::Prepare(std::string& error)
    {
        Release();
        if (!m_app.GetRenderer().GetRhiDevice()) {
            error = "no RHI device";
            return false;
        }
        const std::filesystem::path dir(App::ApplicationResourcePaths::ResolveAssetPathWide(m_textureDir));
        std::vector<std::filesystem::path> files;
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (entry.is_regular_file()) {
                files.push_back(entry.path());
            }
        }
        if (ec || files.empty()) {
            error = "no files in " + dir.string();
            return false;
        }
        std::sort(files.begin(), files.end());

        uint64_t diskBytes = 0;
        try {
            for (const char* kind : kKinds) {
                uint32_t taken = 0;
                for (const auto& path : files) {
                    if (taken >= kPerKind) {
                        break;
                    }
                    if (!EndsWith(path.filename().string(), kind)) {
                        continue;
                    }
                    SourceTexture texture;
                    texture.path = path.wstring();
                    if (!ReadFile(path, texture.file)
                        || !App::DdsTextureLoader::ParseDds(texture.file.data(), texture.file.size(),
                                                            texture.info, texture.path)) {
                        continue;
                    }
                    const auto& info = texture.info;
                    if (BlockFormat(info.format, info.srgb) == Render::RhiFormat::Unknown
                        || info.width % 4 != 0 || info.height % 4 != 0 || info.mips.empty()) {
                        continue;
                    }
                    for (const auto& mip : info.mips) {
                        if (mip.offset + mip.byteSize > texture.file.size() || texture.mipCount >= 16) {
                            break;
                        }
                        ++texture.mipCount;
                    }
                    if (texture.mipCount == 0) {
                        continue;
                    }
                    diskBytes += texture.file.size();
                    m_textures.push_back(std::move(texture));
                    ++taken;
                }
            }
        } catch (const std::bad_alloc&) {
            Release();
            error = "out of memory reading textures";
            return false;
        }
        if (m_textures.empty()) {
            error = "no BC textures with a 4-aligned top mip in " + dir.string();
            return false;
        }

        uint32_t maxSide = 0;
        for (const auto& texture : m_textures) {
            maxSide = (std::max)(maxSide, (std::max)(texture.info.width, texture.info.height));
        }
        char text[160];
        std::snprintf(text, sizeof(text), ": %zu textures (max %u px), disk %s",
                      m_textures.size(), maxSide, FormatKiB(static_cast<size_t>(diskBytes)).c_str());
        m_description = m_textureDir + text;
        return true;
    }

    PerfMetrics TextureUploadCase::RunVariant(PerfVariant variant)
    {
        PerfMetrics metrics;
        Render::IRHIDevice* device = m_app.GetRenderer().GetRhiDevice();
        const uint32_t v = static_cast<uint32_t>(variant);
        ++m_runs[v];
        if (!device) {
            m_failed[v] += static_cast<uint32_t>(m_textures.size());
            return metrics;
        }

        double decodeMs = 0.0;
        double uploadMs = 0.0;
        std::vector<std::vector<uint8_t>> decoded;
        std::vector<const void*> mipData;
        std::vector<uint32_t> rowPitch;
        const auto start = Foundation::ScopedPerfTimer::Now();
        for (const auto& texture : m_textures) {
            const auto& info = texture.info;
            metrics.diskBytes += texture.file.size();
            mipData.clear();
            rowPitch.clear();
            Render::RhiFormat format = BlockFormat(info.format, info.srgb);
            if (variant == PerfVariant::Baseline) {
                const auto decodeStart = Foundation::ScopedPerfTimer::Now();
                decoded.resize(texture.mipCount);
                bool ok = true;
                for (uint32_t mip = 0; mip < texture.mipCount && ok; ++mip) {
                    ok = App::DdsTextureLoader::DecodeDdsMipToRgba8(texture.file.data(), texture.file.size(),
                                                                    info, mip, decoded[mip], texture.path);
                    mipData.push_back(decoded[mip].data());
                    rowPitch.push_back(info.mips[mip].width * 4u);
                    metrics.gpuBytes += decoded[mip].size();
                }
                decodeMs += Foundation::ScopedPerfTimer::ElapsedMilliseconds(decodeStart, Foundation::ScopedPerfTimer::Now());
                uint64_t staged = 0;
                for (uint32_t mip = 0; mip < texture.mipCount; ++mip) {
                    staged += decoded[mip].size();
                }
                metrics.cpuBytes = (std::max)(metrics.cpuBytes, staged);
                if (!ok) {
                    ++m_failed[v];
                    continue;
                }
                format = info.srgb ? Render::RhiFormat::R8G8B8A8UNormSrgb : Render::RhiFormat::R8G8B8A8UNorm;
            } else {
                for (uint32_t mip = 0; mip < texture.mipCount; ++mip) {
                    mipData.push_back(texture.file.data() + info.mips[mip].offset);
                    rowPitch.push_back(info.mips[mip].rowPitch);
                    metrics.gpuBytes += info.mips[mip].byteSize;
                }
            }
            const auto uploadStart = Foundation::ScopedPerfTimer::Now();
            const Render::RhiTextureHandle handle = device->CreateRhiTexture2DFromData(
                info.width, info.height, texture.mipCount, mipData.data(), rowPitch.data(), format);
            uploadMs += Foundation::ScopedPerfTimer::ElapsedMilliseconds(uploadStart, Foundation::ScopedPerfTimer::Now());
            if (!handle.IsValid()) {
                ++m_failed[v];
                continue;
            }
            device->DestroyRhiResource(handle);
        }
        metrics.cpuMs = Foundation::ScopedPerfTimer::ElapsedMilliseconds(start, Foundation::ScopedPerfTimer::Now());
        metrics.extras.push_back({ "decodeMs", decodeMs });
        metrics.extras.push_back({ "uploadMs", uploadMs });
        return metrics;
    }

    bool TextureUploadCase::Check(std::string& detail)
    {
        char text[160];
        std::snprintf(text, sizeof(text), " textures=%zu failedBaseline=%u failedOptimized=%u",
                      m_textures.size(), m_failed[0], m_failed[1]);
        detail += text;
        return !m_textures.empty() && m_failed[0] == 0 && m_failed[1] == 0 && m_runs[0] > 0 && m_runs[1] > 0;
    }

    void TextureUploadCase::Release()
    {
        m_textures.clear();
        m_textures.shrink_to_fit();
        m_failed[0] = m_failed[1] = 0;
        m_runs[0] = m_runs[1] = 0;
    }
}
