#pragma once

#include "PerfCase.h"
#include "Loader/DdsTextureLoader.h"

#include <cstdint>
#include <string>
#include <vector>

namespace SasamiEngine::App
{
    class ApplicationCore;
}

namespace SasamiEngine::Sample::PerfLab
{
    // Bistro's BC-compressed DDS textures uploaded two ways: Baseline decodes every mip to RGBA8
    // on the CPU (bcdec, as LoadRgba8FromDds does) and uploads that; Optimized uploads the BC blocks
    // as they are (IRhiDevice::CreateRhiTexture2DFromData, DX12 only). Prepare reads kPerKind files
    // of each kind (BaseColor / Normal / Specular, name order) whose top mip is a multiple of 4, so
    // disk reads stay out of the timing. cpuMs = decode + create/upload (each upload waits for the
    // GPU), gpuBytes = the sum of the mip payloads (not the allocation size the driver picks),
    // diskBytes = the DDS file sizes,
    // cpuBytes = the largest RGBA8 staging of one texture (Baseline; 0 for Optimized). Not measured yet: drawing time and the image difference.
    // Check: every texture was created in both variants.
    class TextureUploadCase : public IPerfCase
    {
    public:
        TextureUploadCase(std::string name, App::ApplicationCore& app, std::string textureDir);

        const char* Name() const override { return m_name.c_str(); }
        const char* Description() const override { return m_description.c_str(); }

        bool Prepare(std::string& error) override;
        PerfMetrics RunVariant(PerfVariant variant) override;
        bool Check(std::string& detail) override;
        void Release() override;

    private:
        static constexpr uint32_t kPerKind = 4;

        struct SourceTexture
        {
            std::wstring path;
            std::vector<uint8_t> file;
            App::DdsTextureLoader::DdsImageInfo info;
            uint32_t mipCount = 0; // mips whose data lies inside the file
        };

        std::string m_name;
        App::ApplicationCore& m_app;
        std::string m_textureDir;
        std::string m_description;
        std::vector<SourceTexture> m_textures;
        uint32_t m_failed[2] = {};
        uint32_t m_runs[2] = {};
    };
}
