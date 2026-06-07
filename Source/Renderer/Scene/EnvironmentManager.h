#pragma once
#include "Renderer/Scene/Skybox.h"
#include "Renderer/RayTracing/SWRTExecutor.h"
#include "Renderer/Structures/RendererEnums.h"
#include "Renderer/RHI/GraphicsDevice.h"
#include <vector>
#include <cstdint>

namespace SasamiRenderer
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
        void RefreshEnvironmentAssets();
        void EnsureTexturesUploaded(CommandList* cmdList);

    private:
        Skybox& m_skybox;
        SWRTExecutor& m_swrtExecutor;
    };
}
