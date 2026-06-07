#pragma once

#include "Renderer/RHI/GraphicsDevice.h"

#include <cstdint>

namespace SasamiRenderer
{
    namespace ResourceUploadUtility
    {
        bool CreateUploadBuffer(IRHIDevice& device,
                                std::uint64_t size,
                                Resource& outResource,
                                void** outMappedPtr);

        bool CreateTexture2DFromRgba8(IRHIDevice& device,
                                      CommandList* cmdList,
                                      const std::uint8_t* pixels,
                                      UINT width,
                                      UINT height,
                                      Resource& outTexture,
                                      Resource& outUpload);
    }
}
