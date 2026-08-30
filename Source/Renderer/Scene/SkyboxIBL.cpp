// SkyboxIBL.cpp
// IBL texture upload entry point — delegates to IblSystem.
#include "Renderer/Scene/Skybox.h"

namespace SasamiRenderer
{
    void Skybox::EnsureIblTexturesUploaded(CommandList* cmdList)
    {
        const bool equirectLoaded = EnsureHdrEnvironmentLoaded();
        m_iblSystem.EnsureTexturesUploaded(cmdList, equirectLoaded,
                                           m_hdrEquirectPixels,
                                           m_hdrEquirectWidth,
                                           m_hdrEquirectHeight);
    }

} // namespace SasamiRenderer
