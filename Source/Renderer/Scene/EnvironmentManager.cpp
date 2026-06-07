#include "Renderer/Scene/EnvironmentManager.h"

namespace SasamiRenderer
{
    EnvironmentManager::EnvironmentManager(Skybox& skybox, SWRTExecutor& swrtExecutor)
        : m_skybox(skybox)
        , m_swrtExecutor(swrtExecutor)
    {
    }

    void EnvironmentManager::SetSkyboxHdrEquirectData(std::vector<float> pixels, UINT width, UINT height)
    {
        m_skybox.SetHdrEquirectData(std::move(pixels), width, height);
        m_swrtExecutor.OnReflectionResourcesReallocated();
    }

    void EnvironmentManager::SetSkyboxLdrEquirectData(std::vector<uint8_t> pixels, UINT width, UINT height)
    {
        m_skybox.SetLdrEquirectData(std::move(pixels), width, height);
        m_swrtExecutor.OnReflectionResourcesReallocated();
    }

    void EnvironmentManager::SetSkyboxLdrCubemapFacesData(std::vector<std::vector<uint8_t>> facePixels, UINT width, UINT height)
    {
        m_skybox.SetLdrCubemapFaceData(std::move(facePixels), width, height);
        m_swrtExecutor.OnReflectionResourcesReallocated();
    }

    void EnvironmentManager::SetSkyboxLoadFormat(SkyboxLoadFormat format)
    {
        m_skybox.SetLoadFormat(format);
        m_swrtExecutor.OnReflectionResourcesReallocated();
    }

    void EnvironmentManager::RefreshEnvironmentAssets()
    {
        m_skybox.RefreshEnvironmentAssets();
        m_swrtExecutor.InvalidateCache();
    }

    void EnvironmentManager::EnsureTexturesUploaded(CommandList* cmdList)
    {
        if (!m_skybox.IsSkyboxTextureUploaded() && !m_skybox.HasSkyboxUploadAttempted()) {
            m_skybox.EnsureSkyboxTextureUploaded(cmdList);
            return;
        }
        if (!m_skybox.IsIblUploaded() && !m_skybox.HasIblUploadAttempted()) {
            m_skybox.EnsureIblTexturesUploaded(cmdList);
        }
    }
}
