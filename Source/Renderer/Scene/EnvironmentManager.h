#pragma once
#include "Renderer/Scene/Skybox.h"
#include "Renderer/RayTracing/SWRTExecutor.h"
#include "Renderer/Structures/RendererEnums.h"
#include "Renderer/RHI/GraphicsDevice.h"
#include <vector>
#include <cstdint>

namespace SasamiEngine::Render
{
    class EnvironmentManager
    {
    public:
        using SkyboxLoadFormat = RendererEnums::SkyboxLoadFormat;

        EnvironmentManager(Skybox& skybox, SWRTExecutor& swrtExecutor);

        void SetSkyboxHdrEquirectData(std::vector<float> pixels, UINT width, UINT height);
        void SetSkyboxLdrEquirectData(std::vector<uint8_t> pixels, UINT width, UINT height);
        void SetSkyboxLdrCubemapFacesData(std::vector<std::vector<uint8_t>> facePixels, UINT width, UINT height);
        void SetSkyboxLoadFormat(SkyboxLoadFormat format);
        // Worker-pregenerated IBL data (see IblSystem::AdoptPregeneratedIblData).
        void AdoptPregeneratedIblData(IblSystem::GeneratedIblData&& data);
        void RefreshEnvironmentAssets();
        void EnsureTexturesUploaded(CommandList* cmdList);

    private:
        Skybox& m_skybox;
        SWRTExecutor& m_swrtExecutor;
    };
}

// Same reason as the Foundation headers: the directive sits on
// SasamiEngine so that anything nested under it finds these names by enclosing-namespace lookup.
namespace SasamiEngine { using namespace Render; }
